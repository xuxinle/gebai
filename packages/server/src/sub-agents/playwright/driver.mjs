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
import { existsSync } from "node:fs"

const IDLE_TIMEOUT_MS = 10 * 60 * 1000 // 会话浏览器上下文空闲回收阈值
const RESULT_LIMIT = 200_000 // 单次结果最大字符数（超出截断并标记 truncated）

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
    captureResponseBody(entry, resp)
  })
}

/** 惰性加载的 playwright 模块（init 时注入路径，规避 node 侧模块解析问题）。 */
let pw = null
let browser = null
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
  log("launching chromium...")
  browser = await pw.chromium.launch({ headless: true })
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
    s = { context: null, lastUsed: Date.now(), activePageIndex: 0, recording: false, network: [], netMap: new Map(), netId: 0 }
    sessions.set(sessionId, s)
  }
  if (!s.context) {
    s.context = await b.newContext()
    s.activePageIndex = 0
    // 上下文重建时重置网络录制状态（旧状态随旧上下文销毁）
    s.recording = false
    s.network = []
    s.netMap = new Map()
    s.netId = 0
    attachNetworkListeners(s)
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
  /** 初始化：注入 playwright 模块绝对路径（file:// URL）。幂等。 */
  async init(args) {
    if (!pw) {
      const path = str(args.playwrightModule)
      if (!path) throw new Error("缺少 playwrightModule 参数")
      pw = await import(path)
      log("playwright loaded:", path)
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
    const target = selector ? page.locator(selector) : page
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
    if (selector) await page.locator(selector).screenshot({ path, timeout: opTimeout(args.timeout) })
    else await page.screenshot({ path, fullPage: !!args.fullPage, timeout: opTimeout(args.timeout) })
    return { path }
  },

  async click(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    await page.click(selector, { timeout: opTimeout(args.timeout) })
    return { clicked: selector }
  },

  async fill(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    await page.fill(selector, str(args.value), { timeout: opTimeout(args.timeout) })
    return { filled: selector }
  },

  async press(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const key = str(args.key)
    if (!key) throw new Error("缺少 key 参数（如 Enter/Tab/Control+a）")
    if (args.selector) await page.press(str(args.selector), key, { timeout: opTimeout(args.timeout) })
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
    await page.selectOption(selector, value, { timeout: opTimeout(args.timeout) })
    return { selected: selector }
  },

  async check(args) {
    const s = await ensureSession(args.sessionId)
    const page = activePage(s, args.index)
    const selector = str(args.selector)
    if (!selector) throw new Error("缺少 selector 参数")
    if (args.checked === false) await page.uncheck(selector, { timeout: opTimeout(args.timeout) })
    else await page.check(selector, { timeout: opTimeout(args.timeout) })
    return { checked: selector, state: args.checked === false ? "unchecked" : "checked" }
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
    const out = entries.slice(-200).map((e) => {
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
