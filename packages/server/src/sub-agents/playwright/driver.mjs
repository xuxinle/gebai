/**
 * playwright 子Agent 的 node 桥接驱动（常驻进程，由 Bun 侧 playwright_tools.ts spawn）。
 *
 * 为什么需要桥接：Bun 运行时与 playwright 的 driver 子进程 pipe 通信存在兼容问题
 * （实测 chromium.launch 超时），而 node 环境完全正常。因此真实浏览器操作全部在
 * 本进程（node）内执行，与宿主编程通过 stdin/stdout 行分隔 JSON-RPC 通信。
 *
 * 协议：
 *   请求  ->  {"id": number, "op": string, "args": object}
 *   响应  <-  {"id": number, "ok": true, "result": any}
 *             {"id": number, "ok": false, "error": string}
 * 每行一个 JSON；stdout 仅输出响应行，日志一律走 stderr。
 *
 * 生命周期：Browser 进程内单例（惰性启动、断开自动重建）；BrowserContext 按
 * sessionId 隔离（多用户/多会话互不串扰），空闲超时（惰性检查）自动关闭。
 * 宿主编程退出（或 kill）时本进程随之结束，浏览器由 OS 回收。
 */
import { createInterface } from "node:readline"
import { fileURLToPath } from "node:url"
import { existsSync, mkdirSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"

const IDLE_TIMEOUT_MS = 10 * 60 * 1000 // 会话浏览器上下文空闲回收阈值
const RESULT_LIMIT = 200_000 // 单次结果最大字符数（超出截断并标记 truncated）
const WS_MAX_FRAMES = 300 // 单会话 WebSocket 帧记录上限（超出丢弃最旧）
const WS_FRAME_PREVIEW = 4_000 // 单帧 payload 预览上限（字符）
const ROUTES_MAX = 32 // 单会话请求拦截规则上限
const DIALOGS_MAX = 100 // 单会话对话框记录上限
const DOWNLOADS_MAX = 100 // 单会话下载记录上限（文件保留在磁盘，仅记录淘汰）
const REPLAY_BODY_PREVIEW = 50_000 // network_replay 响应体预览上限（字符）
const BODY_FILE_MAX = 20_000_000 // network_body 响应体落盘上限（字节）

/* ---------------- 网络录制（reverse_site 子Agent 接口逆向用） ---------------- */

const NETWORK_MAX_ENTRIES = 500 // 单会话记录条数上限（超出丢弃最旧）
const NETWORK_BODY_CAP = 20_000 // 单条响应体预览上限（字符）
const NETWORK_BODY_MAX = 200_000 // 超过此大小的响应体不捕获
/** 录制时脱敏的敏感请求/响应头（值一律替换为 ***）。 */
const SENSITIVE_HEADERS = new Set([
  "authorization", "cookie", "proxy-authorization", "x-api-key", "x-auth-token",
  "x-csrf-token", "x-access-token", "set-cookie", "www-authenticate",
])
/** postData 中按键名脱敏的参数（值替换为 ***）。 */
const SENSITIVE_PARAM_RE = /token|password|passwd|secret|api[-_]?key|authorization|auth/i

function redactHeaders(headers) {
  const out = {}
  for (const [k, v] of Object.entries(headers)) out[k] = SENSITIVE_HEADERS.has(k.toLowerCase()) ? "***" : v
  return out
}

/** postData 脱敏：JSON 对象 / urlencoded 按键名替换敏感值；其余原样。 */
function redactPostData(data) {
  if (!data) return ""
  const redact = (v) => {
    if (Array.isArray(v)) return v.map(redact)
    if (v && typeof v === "object") {
      const o = {}
      for (const [k, val] of Object.entries(v)) o[k] = SENSITIVE_PARAM_RE.test(k) ? "***" : redact(val)
      return o
    }
    return v
  }
  try {
    return JSON.stringify(redact(JSON.parse(data)))
  } catch {
    if (/[=]/.test(data) && /[&=]/.test(data)) {
      try {
        const sp = new URLSearchParams(data)
        const out = []
        for (const [k, v] of sp) out.push(`${k}=${SENSITIVE_PARAM_RE.test(k) ? "***" : v}`)
        return out.join("&")
      } catch { /* 解析失败按原样 */ }
    }
    return data
  }
}

/** 响应体捕获（异步，随响应到达补齐）：仅文本类且大小受限，预览上限截断。 */
async function captureResponseBody(entry, resp) {
  try {
    const ct = resp.headers()["content-type"] || ""
    if (!/json|text|xml|javascript|html|form/i.test(ct)) return
    const reader = resp.body().getReader()
    const chunks = []
    let total = 0
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      total += value.length
      if (total > NETWORK_BODY_MAX) {
        entry.body = "…[响应体过大，未捕获]"
        return
      }
      chunks.push(value)
    }
    const text = Buffer.concat(chunks).toString("utf8")
    entry.body = text.length > NETWORK_BODY_CAP ? text.slice(0, NETWORK_BODY_CAP) + "\n…[响应体已截断]" : text
  } catch { /* 捕获失败忽略 */ }
}

/** 按录制 id 查找 entry（找不到给出可操作提示）。 */
function findEntry(s, id) {
  const n = Number(id)
  if (!s || !s.network || s.network.length === 0) throw new Error("当前会话没有录制数据，请先 capture_start 并浏览页面")
  const entry = s.network.find((e) => e.id === n)
  if (!entry) throw new Error(`找不到录制记录 id=${id}，请用 capture_list 查看现有 id`)
  return entry
}

/** 为会话上下文挂接网络录制监听（request/response/requestfailed）。 */
function attachNetworkListeners(s) {
  s.context.on("request", (req) => {
    if (!s.recording) return
    if (s.network.length >= NETWORK_MAX_ENTRIES) {
      const dropped = s.network.shift()
      if (dropped) s.netMap.delete(dropped.req)
    }
    const entry = {
      req,
      id: s.netId++,
      time: Date.now(),
      method: req.method(),
      url: req.url(),
      resourceType: req.resourceType(),
      requestHeaders: redactHeaders(req.headers()),
      postData: redactPostData(req.postData() ?? ""),
      status: 0,
      responseHeaders: null,
      body: "",
      error: null,
    }
    s.network.push(entry)
    s.netMap.set(req, entry)
  })
  s.context.on("requestfailed", (req) => {
    if (!s.recording) return
    const entry = s.netMap.get(req)
    if (entry) entry.error = "请求失败"
  })
  s.context.on("response", (resp) => {
    if (!s.recording) return
    const entry = s.netMap.get(resp.request())
    if (!entry) return
    entry.status = resp.status()
    entry.responseHeaders = redactHeaders(resp.headers())
    entry.resp = resp // 保留响应对象（network_body 完整响应体用）
    captureResponseBody(entry, resp)
  })
}

/** 为页面挂接对话框/下载/WebSocket 帧监听（context.on("page") 时对新页面调用）。 */
function attachPageListeners(s, page) {
  // 对话框（alert/confirm/prompt/beforeunload）：有监听器时必须 accept/dismiss，否则页面冻结；
  // 无自动应答配置时默认 dismiss 并记录，供 dialog_list 查看
  page.on("dialog", (d) => {
    void (async () => {
      const rec = { type: d.type(), message: d.message(), defaultText: d.defaultValue() ?? "", time: Date.now() }
      if (s.dialogs.length >= DIALOGS_MAX) s.dialogs.shift()
      s.dialogs.push(rec)
      try {
        const auto = s.dialogAuto
        if (auto && auto.mode === "accept") await d.accept(d.type() === "prompt" ? auto.promptText : undefined)
        else await d.dismiss()
        rec.handled = auto && auto.mode === "accept" ? "accept" : "dismiss"
      } catch { try { await d.dismiss() } catch { /* 已处理 */ } }
    })()
  })
  // 下载：保存到系统临时目录（宿主侧 downloads 工具负责复制进会话沙箱）
  page.on("download", (dl) => {
    void (async () => {
      try {
        const dir = join(tmpdir(), "gebai-dl", s.id)
        mkdirSync(dir, { recursive: true })
        const base = dl.suggestedFilename() || `download-${Date.now()}`
        let name = base
        let n = 1
        while (existsSync(join(dir, name))) name = `${n++}-${base}`
        const path = join(dir, name)
        await dl.saveAs(path)
        if (s.downloads.length >= DOWNLOADS_MAX) s.downloads.shift()
        s.downloads.push({ filename: name, path, url: dl.url(), time: Date.now() })
      } catch { /* 保存失败忽略（不出现在列表中） */ }
    })()
  })
  // WebSocket 帧录制（随网络录制开关 s.recording 联动）
  page.on("websocket", (ws) => {
    const url = ws.url()
    const push = (dir, payload) => {
      if (!s.recording) return
      if (s.ws.length >= WS_MAX_FRAMES) s.ws.shift()
      let text = typeof payload === "string" ? payload : Buffer.from(payload).toString("utf8")
      if (text.length > WS_FRAME_PREVIEW) text = text.slice(0, WS_FRAME_PREVIEW) + "…[帧已截断]"
      s.ws.push({ id: s.wsId++, time: Date.now(), dir, url, payload: text })
    }
    ws.on("framesent", (f) => push("sent", f.payload))
    ws.on("framereceived", (f) => push("recv", f.payload))
  })
}

/** 请求拦截规则处理函数（block=中止 / mock=伪造响应 / modify=改写请求头后放行）。 */
function makeRouteHandler(spec) {
  return async (route) => {
    try {
      if (spec.mode === "block") return await route.abort()
      if (spec.mode === "mock") {
        return await route.fulfill({
          status: spec.status ?? 200,
          contentType: spec.contentType ?? "application/json",
          body: spec.body ?? "",
        })
      }
      const headers = { ...route.request().headers(), ...(spec.headers || {}) }
      return await route.continue({ headers })
    } catch { try { await route.continue() } catch { /* 路由已处理 */ } }
  }
}

/** 重建上下文后恢复拦截规则（规则挂在 context 上，随上下文销毁丢失）。 */
async function applyRoutes(s) {
  for (const spec of s.routes) {
    try { await s.context.route(spec.pattern, makeRouteHandler(spec)) } catch { /* 规则失效忽略 */ }
  }
}

/** 选择器目标解析：支持 `iframe选择器 >> 子iframe选择器 >> 目标选择器` 逐级穿透 iframe
 *  （Locator.contentFrame 链）；无 `>>` 时等价 page.locator。CSS 引擎天然穿透开放 shadow DOM。 */
function resolveTarget(page, selector) {
  const raw = String(selector)
  const parts = raw.split(">>").map((p) => p.trim()).filter(Boolean)
  if (parts.length <= 1) return page.locator(raw)
  let frame = null
  for (let i = 0; i < parts.length - 1; i++) {
    const loc = frame ? frame.locator(parts[i]) : page.locator(parts[i])
    frame = loc.contentFrame()
  }
  return frame.locator(parts[parts.length - 1])
}

/** 惰性加载的 playwright 模块（init 时注入路径，规避 node 侧模块解析问题）。 */
let pw = null
let browser = null
let channel = "" // 浏览器启动 channel（init 注入；空 = 不指定，用默认 chromium）
/** sessionId -> { context, lastUsed, activePageIndex } */
const sessions = new Map()
let lastOp = null // 最近一次请求上下文（错误信息补充）

/* ---------------- 基础工具 ---------------- */

function log(...parts) {
  console.error(`[pw-driver]`, ...parts)
}

function truncateJson(v) {
  const json = JSON.stringify(v)
  if (json === undefined) return { value: "undefined" }
  if (json.length <= RESULT_LIMIT) return { value: json, truncated: false }
  return { value: json.slice(0, RESULT_LIMIT), truncated: true }
}

/** 序列化任意 evaluate 返回值（循环引用/函数等兜底）。 */
function serialize(value) {
  try {
    return truncateJson(value)
  } catch {
    return truncateJson(String(value))
  }
}

function num(v, dflt) {
  const n = Number(v)
  return Number.isFinite(n) && n > 0 ? n : dflt
}

/** 单次操作超时：默认 30s，上限 120s（低于桥接进程 180s 请求超时，防单请求拖死整个桥接进程）。 */
const OP_TIMEOUT_MAX = 120_000
function opTimeout(v, dflt = 30_000) {
  return Math.min(num(v, dflt), OP_TIMEOUT_MAX)
}

function str(v, dflt = "") {
  return v === undefined || v === null ? dflt : String(v)
}

/* ---------------- 浏览器/会话管理 ---------------- */

async function ensureBrowser() {
  if (!pw) throw new Error("driver 未初始化（init 未调用或失败）")
  if (browser && browser.isConnected()) return browser
  log("launching chromium" + (channel ? ` (${channel})` : "") + "...")
  browser = await pw.chromium.launch({ headless: true, ...(channel ? { channel } : {}) })
  browser.on("disconnected", () => {
    log("browser disconnected")
    browser = null
    // 浏览器进程死亡：清空所有会话状态，下次请求惰性重建
    for (const s of sessions.values()) s.context = null
  })
  log("chromium launched")
  return browser
}

async function ensureSession(sessionId) {
  const b = await ensureBrowser()
  let s = sessions.get(sessionId)
  if (!s) {
    s = {
      id: sessionId,
      context: null,
      lastUsed: Date.now(),
      activePageIndex: 0,
      recording: false,
      network: [],
      netMap: new Map(),
      netId: 0,
      emu: null, // 仿真档案（userAgent/locale/timezoneId/viewport/isMobile/hasTouch），newContext 时生效
      ws: [], // WebSocket 帧记录（随录制开关）
      wsId: 0,
      routes: [], // 请求拦截规则（重建上下文后重挂）
      dialogs: [], // 对话框记录
      dialogAuto: null, // 自动应答配置 { mode, promptText }
      downloads: [], // 已保存的下载记录
    }
    sessions.set(sessionId, s)
  }
  if (!s.context) {
    s.context = await b.newContext({ acceptDownloads: true, ...(s.emu || {}) })
    s.activePageIndex = 0
    // 上下文重建时重置网络录制与 WS 帧状态（旧状态随旧上下文销毁）；拦截规则/对话框/下载记录保留
    s.recording = false
    s.network = []
    s.netMap = new Map()
    s.netId = 0
    s.ws = []
    s.wsId = 0
    attachNetworkListeners(s)
    s.context.on("page", (p) => attachPageListeners(s, p))
    for (const p of s.context.pages()) attachPageListeners(s, p)
    await applyRoutes(s)
  }
  s.lastUsed = Date.now()
  return s
}

/** 惰性回收：清理空闲超时的会话上下文。 */
function gcContexts() {
  const now = Date.now()
  for (const [id, s] of sessions) {
    if (s.context && now - s.lastUsed > IDLE_TIMEOUT_MS) {
      s.context.close().catch(() => {})
      s.context = null
      log("closed idle context", id)
    }
  }
}

/** 会话当前活动页（index 越界自动回退到最后一个）。 */
function activePage(s, index) {
  const pages = s.context.pages()
  if (pages.length === 0) throw new Error("当前会话没有打开的页面，请先 open 或 new_page")
  let i = index === undefined ? s.activePageIndex : Number(index)
  if (!Number.isInteger(i) || i < 0 || i >= pages.length) i = pages.length - 1
  s.activePageIndex = i
  return pages[i]
}

async function pageInfo(page) {
  let title = ""
  try { title = await page.title() } catch { /* 页面可能已关闭 */ }
  return { url: page.url(), title }
}

/** 导航 URL 校验：http(s) 或 file://（本地文件必须存在）。 */
function assertNavUrl(url) {
  if (/^https?:\/\//i.test(url)) return
  if (/^file:\/\//i.test(url)) {
    try {
      const p = fileURLToPath(url)
      if (!existsSync(p)) throw new Error(`本地文件不存在: ${p}`)
      return
    } catch (e) {
      throw new Error(`file:// 地址无效: ${e.message}`)
    }
  }
  throw new Error(`url 必须是 http(s) 或 file 地址: ${url}`)
}

/* ---------------- 操作实现 ---------------- */

const ops = {
  /** 初始化：注入 playwright 模块绝对路径（file:// URL）与浏览器 channel。幂等。 */
  async init(args) {
    if (!pw) {
      const path = str(args.playwrightModule)
      if (!path) throw new Error("缺少 playwrightModule 参数")
      const mod = await import(path)
      // CJS 包经 file:// import 时具名导出可能缺失（cjs-module-lexer 未识别），回退 default
      pw = mod && mod.chromium ? mod : (mod.default ?? mod)
      if (!pw || !pw.chromium) throw new Error(`playwright 模块不可用（未导出 chromium）: ${path}`)
      channel = str(args.channel)
      log("playwright loaded:", path, channel ? `(channel: ${channel})` : "")
    }
    return { ok: true }
  },

  async open(args) {
    const { sessionId } = args
    const s = await ensureSession(sessionId)
    const page = s.context.pages()[0] ?? (await s.context.newPage())
    s.activePageIndex = 0
    const url = str(args.url)
    assertNavUrl(url)
    const waitUntil = str(args.waitUntil, "load")
    const valid = ["load", "domcontentloaded", "networkidle", "commit"]
    if (!valid.includes(waitUntil)) throw new Error(`waitUntil 必须是 ${valid.join("/")}`)
    await page.goto(url, { waitUntil, timeout: opTimeout(args.timeout) })
    return { url: page.url(), ...(await pageInfo(page)) }
  },

  async content(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const mode = str(args.mode, "html")
    const selector = str(args.selector)
    const target = selector ? resolveTarget(page, selector) : page
    const out = {}
    if (mode === "html" || mode === "both") out.html = selector ? await target.innerHTML() : await page.content()
    if (mode === "text" || mode === "both") out.text = selector ? await target.innerText() : await page.evaluate(() => document.body ? document.body.innerText : "")
    return out
  },

  async screenshot(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const path = str(args.path)
    if (!path) throw new Error("缺少 path 参数（截图落盘位置）")
    const selector = str(args.selector)
    if (selector) await resolveTarget(page, selector).screenshot({ path, timeout: opTimeout(args.timeout) })
    else await page.screenshot({ path, fullPage: !!args.fullPage, timeout: opTimeout(args.timeout) })
    return { path }
  },

  async click(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    await resolveTarget(page, selector).click({ timeout: opTimeout(args.timeout) })
    return { clicked: selector }
  },

  async fill(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    await resolveTarget(page, selector).fill(str(args.value), { timeout: opTimeout(args.timeout) })
    return { filled: selector }
  },

  async press(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const key = str(args.key)
    if (!key) throw new Error("缺少 key 参数（如 Enter/Tab/Control+a）")
    if (args.selector) await resolveTarget(page, str(args.selector)).press(key, { timeout: opTimeout(args.timeout) })
    else await page.keyboard.press(key)
    return { pressed: key }
  },

  async select(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    const value = args.value !== undefined && args.value !== null && args.value !== "" ? { value: str(args.value) }
      : args.label ? { label: str(args.label) }
      : null
    if (!value) throw new Error("value 与 label 至少提供一个")
    await resolveTarget(page, selector).selectOption(value, { timeout: opTimeout(args.timeout) })
    return { selected: selector }
  },

  async check(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    if (args.checked === false) await resolveTarget(page, selector).uncheck({ timeout: opTimeout(args.timeout) })
    else await resolveTarget(page, selector).check({ timeout: opTimeout(args.timeout) })
    return { checked: selector, state: args.checked === false ? "unchecked" : "checked" }
  },

  async hover(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    await resolveTarget(page, selector).hover({ timeout: opTimeout(args.timeout) })
    return { hovered: selector }
  },

  async dblclick(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    await resolveTarget(page, selector).dblclick({ timeout: opTimeout(args.timeout) })
    return { dblclicked: selector }
  },

  async drag(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const source = str(args.source)
    const target = str(args.target)
    if (!source || !target) throw new Error("缺少 source/target 参数")
    await resolveTarget(page, source).dragTo(resolveTarget(page, target), { timeout: opTimeout(args.timeout) })
    return { dragged: `${source} -> ${target}` }
  },

  async upload(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    const paths = Array.isArray(args.paths) ? args.paths.map((p) => str(p)).filter(Boolean) : [str(args.paths)].filter(Boolean)
    if (paths.length === 0) throw new Error("缺少 paths 参数（要上传的文件绝对路径）")
    for (const p of paths) {
      if (!existsSync(p)) throw new Error(`文件不存在: ${p}`)
    }
    await resolveTarget(page, selector).setInputFiles(paths, { timeout: opTimeout(args.timeout) })
    return { uploaded: paths }
  },

  async wait_for(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const timeout = opTimeout(args.timeout)
    if (args.selector) {
      await page.waitForSelector(str(args.selector), { state: str(args.state, "visible"), timeout })
      return { waited: `selector ${args.selector} ${str(args.state, "visible")}` }
    }
    if (args.url) {
      await page.waitForURL(str(args.url), { timeout })
      return { waited: `url ${args.url}` }
    }
    await page.waitForLoadState(str(args.loadState ?? "networkidle", "networkidle"), { timeout })
    return { waited: `loadState ${args.loadState ?? "networkidle"}` }
  },

  async evaluate(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const expression = str(args.expression)
    if (!expression) throw new Error("缺少 expression 参数")
    // 表达式无内部超时（可能死循环），用 race 兜底：超时返回错误而非挂死桥接进程
    const value = await Promise.race([
      page.evaluate(expression),
      new Promise((_, reject) => setTimeout(() => reject(new Error("evaluate 超时（120s）")), OP_TIMEOUT_MAX)),
    ])
    return { value: serialize(value) }
  },

  async pages(args) {
    const s = await ensureSession(args.sessionId)
    if (!s.context) return { pages: [] }
    const list = await Promise.all(s.context.pages().map((p) => pageInfo(p)))
    return { pages: list.map((p, i) => ({ index: i, ...p, active: i === s.activePageIndex })) }
  },

  async new_page(args) {
    const s = await ensureSession(args.sessionId)
    const page = await s.context.newPage()
    s.activePageIndex = s.context.pages().length - 1
    const url = str(args.url)
    if (url) {
      assertNavUrl(url)
      await page.goto(url, { waitUntil: str(args.waitUntil, "load"), timeout: opTimeout(args.timeout) })
    }
    return { index: s.activePageIndex, ...(await pageInfo(page)) }
  },

  async switch_page(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    return { index: s.activePageIndex, ...(await pageInfo(page)) }
  },

  async close_page(args) {
    const s = await ensureSession(args.sessionId)
    const pages = s.context.pages()
    if (pages.length === 0) throw new Error("当前会话没有打开的页面")
    const i = args.index === undefined ? s.activePageIndex : Number(args.index)
    const idx = Number.isInteger(i) && i >= 0 && i < pages.length ? i : pages.length - 1
    await pages[idx].close()
    const rest = s.context.pages().length
    s.activePageIndex = rest === 0 ? 0 : Math.max(0, idx - 1)
    return { closed: idx, remaining: rest }
  },

  async close(args) {
    const s = sessions.get(args.sessionId)
    if (s && s.context) {
      await s.context.close().catch(() => {})
      s.context = null
    }
    sessions.delete(args.sessionId)
    return { closed: true }
  },

  /* ---------------- 会话工具（存储/仿真/下载/对话框/导出） ---------------- */

  async pdf(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const path = str(args.path)
    if (!path) throw new Error("缺少 path 参数（PDF 落盘位置）")
    const format = str(args.format, "A4")
    if (!/^(A[0-5]|Legal|Letter|Tabloid)$/.test(format)) throw new Error(`format 必须是 A1-A5/Legal/Letter/Tabloid: ${format}`)
    await page.pdf({ path, format, landscape: !!args.landscape, printBackground: true })
    return { path }
  },

  /** 仿真档案（UA/locale/timezoneId/viewport/isMobile/hasTouch）——上下文级选项只在创建时生效，
   *  存在即重建上下文应用（页面/cookie/录制状态随之清空，拦截规则自动重挂）。reset 清档回默认。 */
  async emulate(args) {
    const s = await ensureSession(args.sessionId)
    if (args.reset) {
      s.emu = null
    } else {
      s.emu = s.emu ? { ...s.emu } : {}
      if (args.userAgent) s.emu.userAgent = str(args.userAgent)
      if (args.locale) s.emu.locale = str(args.locale)
      if (args.timezoneId) s.emu.timezoneId = str(args.timezoneId)
      const w = num(args.width, 0)
      const h = num(args.height, 0)
      if (w && h) s.emu.viewport = { width: Math.round(w), height: Math.round(h) }
      if (args.mobile !== undefined) {
        s.emu.isMobile = !!args.mobile
        s.emu.hasTouch = !!args.mobile
      }
    }
    if (s.context) {
      await s.context.close().catch(() => {})
      s.context = null
      await ensureSession(args.sessionId)
    }
    return { emulated: s.emu ? { ...s.emu } : null }
  },

  async cookies_get(args) {
    const s = await ensureSession(args.sessionId)
    const urls = Array.isArray(args.urls) ? args.urls.map((u) => str(u)) : args.urls ? [str(args.urls)] : undefined
    const cookies = await s.context.cookies(urls)
    return { cookies }
  },

  async cookies_set(args) {
    const s = await ensureSession(args.sessionId)
    const cookies = Array.isArray(args.cookies) ? args.cookies : []
    if (cookies.length === 0) throw new Error("缺少 cookies 参数（cookie 对象数组）")
    await s.context.addCookies(cookies)
    return { set: cookies.length }
  },

  async cookies_clear(args) {
    const s = await ensureSession(args.sessionId)
    await s.context.clearCookies()
    return { cleared: true }
  },

  /** 当前页面 origin 的 localStorage 读写（定式 evaluate，key/value 由参数传入而非任意脚本）。 */
  async local_storage(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const action = str(args.action, "list")
    if (action === "list") return { items: await page.evaluate(() => ({ ...localStorage })) }
    const key = str(args.key)
    if (!key) throw new Error("缺少 key 参数")
    if (action === "get") return { value: await page.evaluate((k) => localStorage.getItem(k), key) }
    if (action === "set") {
      if (args.value === undefined) throw new Error("缺少 value 参数")
      await page.evaluate(([k, v]) => localStorage.setItem(k, v), [key, str(args.value)])
      return { set: key }
    }
    if (action === "remove") {
      await page.evaluate((k) => localStorage.removeItem(k), key)
      return { removed: key }
    }
    if (action === "clear") {
      await page.evaluate(() => localStorage.clear())
      return { cleared: true }
    }
    throw new Error(`action 必须是 list/get/set/remove/clear: ${action}`)
  },

  /** 登录态快照（playwright storageState 格式：cookies + 各 origin 的 localStorage）。 */
  async storage_save(args) {
    const s = await ensureSession(args.sessionId)
    const state = await s.context.storageState()
    return { state, cookies: state.cookies?.length ?? 0, origins: state.origins?.length ?? 0 }
  },

  /** 恢复登录态：cookies 直接注入；localStorage 逐 origin 开临时页导航写入（不可达 origin 告警不中断）。 */
  async storage_apply(args) {
    const s = await ensureSession(args.sessionId)
    const state = args.state
    if (!state || typeof state !== "object" || !Array.isArray(state.cookies)) {
      throw new Error("缺少 state 参数（storageState 对象）")
    }
    const warnings = []
    if (state.cookies.length > 0) await s.context.addCookies(state.cookies)
    for (const o of Array.isArray(state.origins) ? state.origins : []) {
      if (!Array.isArray(o.localStorage) || o.localStorage.length === 0) continue
      let page = null
      try {
        page = await s.context.newPage()
        await page.goto(o.origin, { waitUntil: "domcontentloaded", timeout: 15_000 })
        await page.evaluate((items) => { for (const it of items) localStorage.setItem(it.name, it.value) }, o.localStorage)
      } catch {
        warnings.push(`origin 恢复失败（不可达或写入失败）: ${o.origin}`)
      } finally {
        await page?.close().catch(() => {})
      }
    }
    return { cookies: state.cookies.length, origins: state.origins?.length ?? 0, warnings }
  },

  async downloads_list(args) {
    const s = sessions.get(args.sessionId)
    return { downloads: s ? s.downloads.slice(-100) : [] }
  },

  async dialog_list(args) {
    const s = sessions.get(args.sessionId)
    return { dialogs: s ? s.dialogs.slice(-50) : [], auto: s?.dialogAuto ?? null }
  },

  async dialog_auto(args) {
    const s = await ensureSession(args.sessionId)
    if (args.mode) {
      const mode = str(args.mode)
      if (!["accept", "dismiss"].includes(mode)) throw new Error("mode 必须是 accept/dismiss")
      s.dialogAuto = { mode, promptText: str(args.promptText) }
    } else {
      s.dialogAuto = null
    }
    return { auto: s.dialogAuto }
  },

  async dialog_clear(args) {
    const s = sessions.get(args.sessionId)
    const n = s ? s.dialogs.length : 0
    if (s) s.dialogs = []
    return { cleared: n }
  },

  /* ---------------- 网络录制（接口逆向） ---------------- */

  async network_start(args) {
    const s = await ensureSession(args.sessionId)
    s.recording = true
    return { recording: true, captured: s.network.length }
  },

  async network_stop(args) {
    const s = sessions.get(args.sessionId)
    if (!s) return { recording: false, captured: 0 }
    s.recording = false
    return { recording: false, captured: s.network.length }
  },

  async network_clear(args) {
    const s = sessions.get(args.sessionId)
    if (!s) return { cleared: 0 }
    const n = s.network.length
    s.network = []
    s.netMap = new Map()
    return { cleared: n }
  },

  async network_list(args) {
    const s = sessions.get(args.sessionId)
    if (!s || !s.network) return { entries: [], captured: 0, recording: false }
    let entries = s.network
    const method = str(args.method).toUpperCase()
    if (method) entries = entries.filter((e) => e.method.toUpperCase() === method)
    if (args.url) {
      const pat = str(args.url)
      let re = null
      try { re = new RegExp(pat) } catch { /* 非正则按子串匹配 */ }
      entries = entries.filter((e) => (re ? re.test(e.url) : e.url.includes(pat)))
    }
    const status = Number(args.status)
    if (Number.isFinite(status) && status > 0) entries = entries.filter((e) => e.status === status)
    const detail = !!args.detail
    const limit = Math.min(Math.max(1, num(args.limit, 200)), NETWORK_MAX_ENTRIES)
    const out = entries.slice(-limit).map((e) => {
      const base = { id: e.id, time: e.time, method: e.method, url: e.url, resourceType: e.resourceType, status: e.status, error: e.error }
      if (detail) {
        base.requestHeaders = e.requestHeaders
        base.postData = e.postData
        base.responseHeaders = e.responseHeaders
        base.body = e.body
      }
      return base
    })
    return { entries: out, captured: s.network.length, recording: s.recording }
  },

  /** 指定录制请求的完整响应体：传 path 直接落盘（支持二进制与大文件）；不传则返回文本预览。 */
  async network_body(args) {
    const s = sessions.get(args.sessionId)
    const entry = findEntry(s, args.id)
    if (!entry.resp) throw new Error("该记录没有响应对象（请求失败或上下文已重建），请重新录制")
    const buf = await entry.resp.body()
    if (buf.length > BODY_FILE_MAX) throw new Error(`响应体过大（${buf.length} 字节，上限 ${BODY_FILE_MAX}）`)
    const path = str(args.path)
    if (path) {
      writeFileSync(path, buf)
      return { path, size: buf.length }
    }
    const ct = entry.responseHeaders?.["content-type"] ?? ""
    if (!/json|text|xml|javascript|html|form/i.test(ct)) {
      return { size: buf.length, contentType: ct || "未知类型", hint: "非文本响应体，请传 file 参数保存为文件" }
    }
    const text = buf.toString("utf8")
    return {
      body: text.length > NETWORK_BODY_MAX ? text.slice(0, NETWORK_BODY_MAX) + "\n…[响应体已截断，完整内容用 file 参数落盘]" : text,
      size: buf.length,
      truncated: text.length > NETWORK_BODY_MAX,
    }
  },

  /** 原始（未脱敏）请求信息——重放命令生成用，宿主侧以审批工具暴露。 */
  async network_raw(args) {
    const entry = findEntry(sessions.get(args.sessionId), args.id)
    return {
      method: entry.req.method(),
      url: entry.req.url(),
      headers: entry.req.headers(),
      postData: entry.req.postData() ?? "",
    }
  },

  /** 一键重放录制请求（context.request 与浏览器共享 cookie/存储状态，可重放需登录接口）：
   *  method/url/headers/body/params 为覆盖项（headers 覆盖合并，params 合并进 URL 查询串）；
   *  followRedirects=false 时不自动跟随重定向（宿主侧沙箱模式逐跳校验防 SSRF）。 */
  async network_replay(args) {
    const s = sessions.get(args.sessionId)
    const entry = findEntry(s, args.id)
    if (!s.context) throw new Error("会话上下文不存在（可能已 close），请先 open 页面")
    const method = str(args.method) || entry.req.method()
    let url = str(args.url) || entry.req.url()
    if (args.params && typeof args.params === "object") {
      try {
        const u = new URL(url)
        for (const [k, v] of Object.entries(args.params)) u.searchParams.set(k, String(v))
        url = u.toString()
      } catch { /* URL 无效保持原样，由 fetch 报错 */ }
    }
    const headers = { ...entry.req.headers(), ...(args.headers || {}) }
    const data = args.body !== undefined ? str(args.body) : entry.req.postData() ?? undefined
    const init = { method, headers, data, timeout: opTimeout(args.timeout, 30_000) }
    if (args.followRedirects === false) init.maxRedirects = 0
    const res = await s.context.request.fetch(url, init)
    const ct = res.headers()["content-type"] || ""
    let body = ""
    if (/json|text|xml|javascript|html|form/i.test(ct)) {
      const text = await res.text()
      body = text.length > REPLAY_BODY_PREVIEW ? text.slice(0, REPLAY_BODY_PREVIEW) + "\n…[响应体已截断]" : text
    } else {
      body = `(非文本响应体: ${ct || "未知类型"}，已跳过)`
    }
    return { status: res.status(), headers: redactHeaders(res.headers()), location: res.headers()["location"] || "", body }
  },

  /** WebSocket 帧记录列表（录制开关联动）。 */
  async network_ws_list(args) {
    const s = sessions.get(args.sessionId)
    if (!s) return { frames: [], total: 0, recording: false }
    let frames = s.ws
    if (args.url) {
      const sub = str(args.url)
      frames = frames.filter((f) => f.url.includes(sub))
    }
    const last = Math.min(Math.max(1, num(args.last, 100)), 300)
    return { frames: frames.slice(-last), total: s.ws.length, recording: s.recording }
  },

  async route_add(args) {
    const s = await ensureSession(args.sessionId)
    if (s.routes.length >= ROUTES_MAX) throw new Error(`拦截规则已达上限 ${ROUTES_MAX}，请先 route clear 清理`)
    const pattern = str(args.pattern)
    if (!pattern) throw new Error("缺少 pattern 参数（glob，如 **/api/**）")
    const mode = str(args.mode, "block")
    if (!["block", "mock", "modify"].includes(mode)) throw new Error("mode 必须是 block/mock/modify")
    const spec = {
      pattern,
      mode,
      status: args.status,
      contentType: args.contentType || undefined,
      body: args.body || undefined,
      headers: args.headers && typeof args.headers === "object" ? args.headers : null,
    }
    await s.context.route(pattern, makeRouteHandler(spec))
    s.routes.push(spec)
    return { added: pattern, mode, total: s.routes.length }
  },

  async route_clear(args) {
    const s = sessions.get(args.sessionId)
    if (!s) return { cleared: 0 }
    const n = s.routes.length
    if (s.context) {
      for (const spec of s.routes) await s.context.unroute(spec.pattern).catch(() => {})
    }
    s.routes = []
    return { cleared: n }
  },

  async route_list(args) {
    const s = sessions.get(args.sessionId)
    return { routes: s ? s.routes.map(({ pattern, mode, status, contentType, headers }) => ({ pattern, mode, status, contentType, headers })) : [] }
  },

  async ping() {
    return { pong: true }
  },
}

/* ---------------- 主循环 ---------------- */

const rl = createInterface({ input: process.stdin, crlfDelay: Infinity })

function respond(msg) {
  process.stdout.write(JSON.stringify(msg) + "\n")
}

rl.on("line", async (line) => {
  let req
  try {
    req = JSON.parse(line)
  } catch {
    respond({ id: null, ok: false, error: "请求不是合法 JSON" })
    return
  }
  const { id, op, args = {} } = req
  lastOp = op
  const handler = ops[op]
  if (!handler) {
    respond({ id, ok: false, error: `未知操作: ${op}` })
    return
  }
  try {
    gcContexts()
    const result = await handler(args)
    respond({ id, ok: true, result })
  } catch (err) {
    const message = err && err.message ? err.message : String(err)
    log(`op ${op} failed:`, message)
    respond({ id, ok: false, error: message })
  }
})

rl.on("close", async () => {
  log("stdin closed, shutting down")
  for (const s of sessions) if (s[1].context) await s[1].context.close().catch(() => {})
  if (browser) await browser.close().catch(() => {})
  process.exit(0)
})

log("driver ready (pid", process.pid + ")")
