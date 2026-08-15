import type { ToolContext, ToolResult, ToolSet } from "../../core/types"
import { artifactBlocks, truncate } from "../../core/tools"
import type { ToolSchema } from "@gebai/sdk"
import { pathToFileURL } from "node:url"
import { isAbsolute, join, normalize, sep } from "node:path"
import { existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs"
import { isBinaryMode, resolveGebaiHome } from "../../core/config"

/**
 * playwright 子Agent 工具集：浏览器自动化（打开页面/读取内容/截图/交互/JS 执行）。
 *
 * 架构：Bun 运行时与 playwright driver 的 pipe 通信存在兼容问题（chromium 启动超时），
 * 因此不直接 import playwright，而是经常驻 node 子进程桥接——本文件 spawn
 * `node driver.mjs`，通过 stdin/stdout 行分隔 JSON-RPC 通信（见 driver.mjs 顶部协议说明）。
 * 浏览器在 node 进程内单例，BrowserContext 按会话隔离，空闲自动回收。
 *
 * 服务端部署（sandboxed）可用：浏览器是隔离环境，与 desktop（操控宿主机桌面）不同；
 * 但导航/交互/JS 执行类操作默认需审批，防 SSRF 与任意脚本滥用。
 */

const DRIVER_FILE = "driver.mjs"
const REQUEST_TIMEOUT_MS = 180_000 // 单个请求超时（超时即杀进程重启，浏览器状态丢失）
const STDOUT_CHUNK_LIMIT = 1 << 20 // 响应行上限 1MB（超出判定协议异常）

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

function num(v: unknown, dflt: number): number {
  const n = Number(v)
  return Number.isFinite(n) && n > 0 ? n : dflt
}

/** driver 脚本绝对路径：源码模式与本文件同目录（dist 非编译形态则与入口同目录）。 */
export function driverPath(): string {
  return join(import.meta.dir, DRIVER_FILE)
}

/**
 * 解析 driver 脚本路径：源码模式直接取同目录文件；二进制（bun --compile）模式从内嵌产物
 * （`core/driver.embedded.generated.json`，构建脚本 `scripts/build-driver-embed.ts` 生成，gzip base64）
 * 物化到 `{GEBAI_HOME}/vendor/playwright/driver.mjs`（与 d2js 同思路的打包闭环）。
 */
export async function resolveDriverFile(): Promise<string> {
  if (!isBinaryMode()) return driverPath()
  const dir = join(resolveGebaiHome(), "vendor", "playwright")
  const file = join(dir, DRIVER_FILE)
  if (!existsSync(file)) {
    const embedded = await import("../../core/driver.embedded.generated.json")
      .then((m) => m.default as { gzip: true; driver: string })
      .catch(() => null)
    if (!embedded) {
      throw new Error("playwright 桥接驱动内嵌产物缺失（构建时请先运行 scripts/build-driver-embed.ts）")
    }
    mkdirSync(dir, { recursive: true })
    writeFileSync(file, Bun.gunzipSync(Buffer.from(embedded.driver, "base64")))
  }
  return file
}

/** playwright 包入口的 file:// URL（Bun 侧解析真实路径，node 进程按此动态 import）。 */
function playwrightModuleUrl(): string {
  // 拼接规避 bun bundler 对字面量的静态解析（playwright 不打包进产物，运行时按需加载）
  const name = "play" + "wright"
  const resolved = Bun.resolveSync(name, import.meta.dir)
  return pathToFileURL(resolved).href
}

/* ---------------- 桥接进程 ---------------- */

export interface BridgeLike {
  request: (op: string, args: Record<string, unknown>, timeoutMs?: number) => Promise<unknown>
}

export class Bridge implements BridgeLike {
  private readonly opts: { driverPath?: string; playwrightModule: string; requestTimeoutMs: number }
  private proc: Bun.Subprocess | null = null
  private stdin: Bun.FileSink | null = null
  private pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void }>()
  private nextId = 1
  private started = false
  private stderrTail = ""

  constructor(opts: { driverPath?: string; playwrightModule?: string; requestTimeoutMs?: number } = {}) {
    this.opts = {
      driverPath: opts.driverPath,
      playwrightModule: opts.playwrightModule ?? playwrightModuleUrl(),
      requestTimeoutMs: opts.requestTimeoutMs ?? REQUEST_TIMEOUT_MS,
    }
  }

  async request(op: string, args: Record<string, unknown>, timeoutMs = this.opts.requestTimeoutMs): Promise<unknown> {
    await this.ensureStarted()
    const id = this.nextId++
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        this.kill("请求超时")
        reject(new Error(`playwright 桥接请求超时（${Math.round(timeoutMs / 1000)}s）${op}`))
      }, timeoutMs)
      this.pending.set(id, {
        resolve: (v) => { clearTimeout(timer); resolve(v) },
        reject: (e) => { clearTimeout(timer); reject(e) },
      })
      const line = JSON.stringify({ id, op, args }) + "\n"
      this.stdin?.write(line)
    })
  }

  private async ensureStarted(): Promise<void> {
    if (this.started && this.proc && !this.proc.killed) return
    const path = this.opts.driverPath ?? (await resolveDriverFile())
    const { existsSync } = await import("node:fs")
    if (!existsSync(path)) {
      throw new Error(
        `playwright 桥接驱动缺失: ${path}（dist 构建需将 driver.mjs 与入口同目录；二进制形态需构建时运行 scripts/build-driver-embed.ts 生成内嵌产物）`,
      )
    }
    const proc = Bun.spawn(["node", path], {
      stdout: "pipe",
      stderr: "pipe",
      stdin: "pipe",
    })
    this.proc = proc
    this.started = true
    this.stderrTail = ""
    this.stdin = proc.stdin as Bun.FileSink
    // stdout 行缓冲：按 id 分发响应。注意：进程退出事件（reader done）异步到达，
    // 期间可能已重启新进程——只有事件属于当前 proc 才清理状态，防误杀新进程。
    const procLocal = proc
    let buf = ""
    const reader = proc.stdout.getReader()
    void (async () => {
      try {
        for (;;) {
          const { done, value } = await reader.read()
          if (done) break
          buf += new TextDecoder().decode(value)
          let nl: number
          while ((nl = buf.indexOf("\n")) >= 0) {
            const line = buf.slice(0, nl)
            buf = buf.slice(nl + 1)
            if (line.length > STDOUT_CHUNK_LIMIT) continue // 异常大行：协议损坏，丢弃
            this.dispatch(line)
          }
        }
      } catch { /* 进程退出 */ }
      if (this.proc === procLocal) this.onExit()
    })()
    // stderr 环形缓冲（保留末尾，用于报错上下文）
    const errReader = proc.stderr.getReader()
    void (async () => {
      try {
        for (;;) {
          const { done, value } = await errReader.read()
          if (done) break
          const text = new TextDecoder().decode(value)
          this.stderrTail = (this.stderrTail + text).slice(-64_000)
        }
      } catch { /* 进程退出 */ }
    })()
    // 首次启动注入 playwright 模块路径
    await this.request("init", { playwrightModule: this.opts.playwrightModule }, 30_000)
  }

  private dispatch(line: string): void {
    let msg: { id?: number; ok?: boolean; result?: unknown; error?: string }
    try {
      msg = JSON.parse(line)
    } catch {
      return
    }
    const p = this.pending.get(msg.id!)
    if (!p) return
    this.pending.delete(msg.id!)
    if (msg.ok) p.resolve(msg.result)
    else p.reject(new Error(msg.error || "playwright 桥接错误"))
  }

  private onExit(): void {
    // 错误信息只回传 stderr 最后一行并截断（driver 日志含路径/URL 等内部细节，避免整体泄漏）
    const lastLine = this.stderrTail.split("\n").filter(Boolean).pop()?.slice(0, 500) ?? ""
    const err = new Error(`playwright 桥接进程退出${lastLine ? `：${lastLine}` : "（无错误输出）"}`)
    for (const p of this.pending.values()) p.reject(err)
    this.pending.clear()
    this.started = false
    this.proc = null
  }

  kill(reason?: string): void {
    if (reason) logBridge(reason)
    const proc = this.proc
    this.onExit() // 先清状态，再杀进程；reader-done 回调因 proc 引用不匹配而不再重复清理
    try {
      proc?.kill()
    } catch { /* 已退出 */ }
  }
}

/** 桥接进程日志（stderr），仅诊断用。 */
function logBridge(msg: string): void {
  console.error(`[playwright-bridge] ${msg}`)
}

/* ---------------- 会话串行化 ---------------- */

/** 同一会话的浏览器操作串行执行，避免并发操作同一页面互相干扰。 */
const sessionLocks = new Map<string, Promise<unknown>>()
function withSessionLock<T>(sessionId: string, fn: () => Promise<T>): Promise<T> {
  const prev = sessionLocks.get(sessionId) ?? Promise.resolve()
  const next = prev.then(fn, fn)
  sessionLocks.set(sessionId, next.catch(() => {}))
  return next
}

/* ---------------- 内置静态文件服务器（B1：本地 HTML 免手工起服务） ---------------- */

/** 已启动的静态服务器（按根目录复用）。 */
const staticServers = new Map<string, { url: string; stop: () => void }>()

function serveMime(p: string): string {
  const ext = p.split(".").pop()?.toLowerCase() ?? ""
  const map: Record<string, string> = {
    html: "text/html; charset=utf-8", htm: "text/html; charset=utf-8", js: "text/javascript; charset=utf-8",
    mjs: "text/javascript; charset=utf-8", css: "text/css; charset=utf-8", json: "application/json; charset=utf-8",
    png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", svg: "image/svg+xml",
    webp: "image/webp", ico: "image/x-icon", txt: "text/plain; charset=utf-8", md: "text/markdown; charset=utf-8",
    pdf: "application/pdf", wasm: "application/wasm", woff: "font/woff", woff2: "font/woff2", ttf: "font/ttf",
  }
  return map[ext] ?? "application/octet-stream"
}

/* ---------------- 工具集 ---------------- */

export function createPlaywrightTools(deps: { bridge?: BridgeLike } = {}): ToolSet {
  const bridge: BridgeLike = deps.bridge ?? new Bridge()
  const request = (sessionId: string, op: string, args: Record<string, unknown>): Promise<unknown> =>
    withSessionLock(sessionId, () => bridge.request(op, { sessionId, ...args }))

  /** 会话锁内执行并统一错误文案。 */
  const run = async (
    ctx: ToolContext,
    op: string,
    args: Record<string, unknown>,
    describe: string
  ): Promise<ToolResult> => {
    try {
      await request(ctx.sessionId, op, args)
      return { output: describe }
    } catch (err) {
      return { output: `操作失败: ${err instanceof Error ? err.message : String(err)}` }
    }
  }

  const tools: ToolSet = {
    open: {
      name: "open",
      description:
        "打开 URL（http/https 或 file:// 本地文件），返回页面标题与最终地址。会话内首次调用自动启动无头浏览器；后续调用复用当前页面。参数 waitUntil 可选 load/domcontentloaded/networkidle/commit（默认 load）。本地 HTML 文件可用 file:// 直接打开，或先用 serve_dir 起静态服务器。",
      card: { titleParams: ["url"], args: "none" },
      parameters: schema(
        {
          url: { type: "string", description: "要打开的 http(s) 或 file:// 地址" },
          waitUntil: { type: "string", description: "等待加载完成的条件（默认 load）" },
          timeout: { type: "number", description: "超时毫秒（默认 30000，上限 120000）" },
        },
        ["url"]
      ),
      async execute(args, ctx) {
        const r = await request(ctx.sessionId, "open", {
          url: String(args.url ?? "").trim(),
          waitUntil: String(args.waitUntil ?? "load"),
          timeout: num(args.timeout, 30_000),
        })
        const info = r as { url: string; title: string }
        return { output: `已打开: ${info.url}\n标题: ${info.title || "(无标题)"}` }
      },
    },

    content: {
      name: "content",
      description:
        "读取当前页面（或指定元素）的内容。mode 为 text（可见文本，省 token，默认）/ html（DOM 结构）/ both。selector 省略则作用于整个页面。内容较大时自动截断保存，可用 read 读取全文。",
      parameters: schema({
        mode: { type: "string", description: "text | html | both（默认 text）" },
        selector: { type: "string", description: "可选：CSS 选择器，读取指定元素" },
        index: { type: "number", description: "可选：标签页序号（默认当前页）" },
      }),
      async execute(args, ctx) {
        const mode = String(args.mode ?? "text")
        if (!["text", "html", "both"].includes(mode)) return { output: `mode 必须是 text/html/both: ${mode}` }
        const r = (await request(ctx.sessionId, "content", {
          mode,
          selector: args.selector === undefined ? "" : String(args.selector),
          index: args.index,
        })) as { html?: string; text?: string }
        const out = [r.html ? `【HTML】\n${r.html}` : "", r.text ? `【文本】\n${r.text}` : ""].filter(Boolean).join("\n\n")
        if (!out) return { output: "(无内容)" }
        return truncate(out, "playwright_content", ctx)
      },
    },

    screenshot: {
      name: "screenshot",
      description:
        "对当前页面（或指定元素）截图，保存 PNG 到会话 tmp/ 并返回图片。fullPage=true 截取整个滚动页面；selector 指定元素区域。",
      card: { titleParams: ["selector"], args: "none" },
      parameters: schema({
        fullPage: { type: "boolean", description: "是否整页截图（默认 false）" },
        selector: { type: "string", description: "可选：CSS 选择器，只截该元素" },
        index: { type: "number", description: "可选：标签页序号（默认当前页）" },
      }),
      async execute(args, ctx) {
        const rel = `tmp/playwright_${Date.now()}.png`
        const abs = ctx.resolvePath(rel)
        try {
          await request(ctx.sessionId, "screenshot", {
            path: abs,
            fullPage: !!args.fullPage,
            selector: args.selector === undefined ? "" : String(args.selector),
            index: args.index,
          })
          return { output: `已截图: ${rel}\n绝对路径: ${abs}`, blocks: artifactBlocks(rel) }
        } catch (err) {
          return { output: `截图失败: ${err instanceof Error ? err.message : String(err)}` }
        }
      },
    },

    click: {
      name: "click",
      description: "点击页面元素（CSS 选择器）。点击前建议先用 content/pages 确认目标存在。",
      parameters: schema(
        {
          selector: { type: "string", description: "CSS 选择器" },
          timeout: { type: "number", description: "超时毫秒（默认 30000）" },
        },
        ["selector"]
      ),
      async execute(args, ctx) {
        return run(ctx, "click", { selector: String(args.selector), timeout: num(args.timeout, 30_000) }, `已点击: ${args.selector}`)
      },
    },

    fill: {
      name: "fill",
      description: "向输入框/文本域填入文本（覆盖原值；先自动清空）。textarea 与 contenteditable 均支持。",
      parameters: schema(
        {
          selector: { type: "string", description: "CSS 选择器" },
          value: { type: "string", description: "要填入的文本" },
          timeout: { type: "number", description: "超时毫秒（默认 30000）" },
        },
        ["selector", "value"]
      ),
      async execute(args, ctx) {
        return run(ctx, "fill", { selector: String(args.selector), value: String(args.value ?? ""), timeout: num(args.timeout, 30_000) }, `已填写: ${args.selector}`)
      },
    },

    press: {
      name: "press",
      description:
        "按键：selector 省略时作用于当前焦点（如 Enter 提交表单）；提供 selector 时先聚焦该元素。键名如 Enter/Tab/Escape/ArrowDown/Control+a。",
      parameters: schema({
        key: { type: "string", description: "按键名（如 Enter/Control+a）" },
        selector: { type: "string", description: "可选：先聚焦该元素" },
        timeout: { type: "number", description: "超时毫秒（默认 30000）" },
      }),
      async execute(args, ctx) {
        const key = String(args.key ?? "")
        if (!key) return { output: "缺少 key 参数" }
        return run(ctx, "press", { key, selector: args.selector === undefined ? "" : String(args.selector), timeout: num(args.timeout, 30_000) }, `已按键: ${key}`)
      },
    },

    select: {
      name: "select",
      description: "下拉框（<select>）选择：value（选项值）或 label（选项文本）二选一。",
      parameters: schema(
        {
          selector: { type: "string", description: "CSS 选择器" },
          value: { type: "string", description: "选项的 value" },
          label: { type: "string", description: "选项的可见文本" },
          timeout: { type: "number", description: "超时毫秒（默认 30000）" },
        },
        ["selector"]
      ),
      async execute(args, ctx) {
        return run(ctx, "select", {
          selector: String(args.selector),
          value: args.value === undefined ? "" : String(args.value),
          label: args.label === undefined ? "" : String(args.label),
          timeout: num(args.timeout, 30_000),
        }, `已选择: ${args.selector} (${args.value ?? args.label})`)
      },
    },

    check: {
      name: "check",
      description: "勾选/取消勾选 checkbox 或 radio。checked=false 表示取消勾选。",
      parameters: schema(
        {
          selector: { type: "string", description: "CSS 选择器" },
          checked: { type: "boolean", description: "勾选（默认 true）或取消勾选" },
          timeout: { type: "number", description: "超时毫秒（默认 30000）" },
        },
        ["selector"]
      ),
      async execute(args, ctx) {
        return run(ctx, "check", { selector: String(args.selector), checked: args.checked !== false, timeout: num(args.timeout, 30_000) }, `已${args.checked === false ? "取消勾选" : "勾选"}: ${args.selector}`)
      },
    },

    wait_for: {
      name: "wait_for",
      description:
        "等待页面条件：selector（元素出现/可见，state 可选 visible/attached/hidden/detached）、url（地址匹配，支持 glob）、或 loadState（网络空闲等，默认 networkidle）。适合等待异步渲染完成后再读取/截图。",
      parameters: schema({
        selector: { type: "string", description: "等待出现的 CSS 选择器" },
        state: { type: "string", description: "selector 的等待状态（默认 visible）" },
        url: { type: "string", description: "等待 URL 匹配（glob 模式，如 **/order/*）" },
        loadState: { type: "string", description: "load | domcontentloaded | networkidle" },
        timeout: { type: "number", description: "超时毫秒（默认 30000）" },
      }),
      async execute(args, ctx) {
        return run(ctx, "wait_for", {
          selector: args.selector === undefined ? "" : String(args.selector),
          state: String(args.state ?? "visible"),
          url: args.url === undefined ? "" : String(args.url),
          loadState: args.loadState === undefined ? "" : String(args.loadState),
          timeout: num(args.timeout, 30_000),
        }, "等待完成")
      },
    },

    evaluate: {
      name: "evaluate",
      description:
        "在当前页面执行 JavaScript 表达式并返回结果（JSON 序列化）。适用于读取动态数据、模拟复杂交互。返回结果超长时自动截断。注意：可访问页面内一切数据（含表单值/cookie），请谨慎使用。",
      parameters: schema(
        {
          expression: { type: "string", description: "JavaScript 表达式（如 `document.title` 或 `() => [...document.querySelectorAll('a')].map(a => a.href)`）" },
        },
        ["expression"]
      ),
      async execute(args, ctx) {
        const expression = String(args.expression ?? "").trim()
        if (!expression) return { output: "缺少 expression 参数" }
        try {
          const r = (await request(ctx.sessionId, "evaluate", { expression })) as { value: { value: string; truncated?: boolean } }
          return truncate(r.value.value, "playwright_evaluate", ctx)
        } catch (err) {
          return { output: `执行失败: ${err instanceof Error ? err.message : String(err)}（可改用 content 工具读取页面 text/html 观察结构）` }
        }
      },
    },

    pages: {
      name: "pages",
      description: "列出当前会话浏览器打开的全部标签页（序号、地址、标题），用于多页管理。只读操作。",
      card: { args: "none" },
      parameters: schema({}),
      async execute(_args, ctx) {
        try {
          const r = (await request(ctx.sessionId, "pages", {})) as { pages: Array<{ index: number; url: string; title: string; active: boolean }> }
          if (r.pages.length === 0) return { output: "当前会话没有打开的页面（先 open / new_page）" }
          const lines = r.pages.map((p) => `[${p.index}]${p.active ? " ◀当前" : ""} ${p.title || "(无标题)"}\n    ${p.url}`)
          return { output: `已打开 ${r.pages.length} 个标签页：\n${lines.join("\n")}` }
        } catch (err) {
          return { output: `查询失败: ${err instanceof Error ? err.message : String(err)}` }
        }
      },
    },

    new_page: {
      name: "new_page",
      description: "新开一个标签页（可选直接导航到 url，支持 http(s)/file://），新页成为当前操作页。",
      card: { titleParams: ["url"], args: "none" },
      parameters: schema({
        url: { type: "string", description: "可选：新标签页打开的地址" },
        waitUntil: { type: "string", description: "等待加载完成的条件（默认 load）" },
        timeout: { type: "number", description: "超时毫秒（默认 30000）" },
      }),
      async execute(args, ctx) {
        try {
          const r = (await request(ctx.sessionId, "new_page", {
            url: String(args.url ?? "").trim(),
            waitUntil: String(args.waitUntil ?? "load"),
            timeout: num(args.timeout, 30_000),
          })) as { index: number; url: string; title: string }
          return { output: `新标签页 [${r.index}]: ${r.title || "(无标题)"}\n${r.url || "(空白页)"}` }
        } catch (err) {
          return { output: `开页失败: ${err instanceof Error ? err.message : String(err)}` }
        }
      },
    },

    switch_page: {
      name: "switch_page",
      description: "切换当前操作页到指定标签页序号（见 pages 的 index）。",
      card: { titleParams: ["index"], args: "none" },
      parameters: schema(
        {
          index: { type: "number", description: "标签页序号（pages 工具返回的 index）" },
        },
        ["index"]
      ),
      async execute(args, ctx) {
        try {
          const r = (await request(ctx.sessionId, "switch_page", { index: args.index })) as { index: number; url: string; title: string }
          return { output: `已切换到 [${r.index}]: ${r.title || "(无标题)"}\n${r.url}` }
        } catch (err) {
          return { output: `切换失败: ${err instanceof Error ? err.message : String(err)}` }
        }
      },
    },

    close_page: {
      name: "close_page",
      description: "关闭当前（或指定序号）标签页。",
      card: { titleParams: ["index"], args: "none" },
      parameters: schema({
        index: { type: "number", description: "可选：标签页序号（默认当前页）" },
      }),
      async execute(args, ctx) {
        return run(ctx, "close_page", { index: args.index }, "标签页已关闭")
      },
    },

    close: {
      name: "close",
      description: "关闭当前会话的整个浏览器上下文（清空全部标签页与 Cookie 等站点数据），释放资源。结束浏览器任务时调用。",
      card: { args: "none" },
      parameters: schema({}),
      async execute(_args, ctx) {
        return run(ctx, "close", {}, "浏览器上下文已关闭")
      },
    },

    serve_dir: {
      name: "serve_dir",
      description:
        "启动内置静态文件服务器服务本地目录（解决 file:// 受限场景：本地 HTML/资源用浏览器打开），返回 http://127.0.0.1:<port>/ 地址供 open 使用。同一目录重复调用返回已有地址。服务端部署模式仅可服务预置项目目录。",
      card: { titleParams: ["path"], args: "none" },
      parameters: schema({
        path: { type: "string", description: "本地目录（绝对路径或预置项目名）" },
        port: { type: "number", description: "可选：端口（默认自动分配）" },
      }),
      async execute(args, ctx) {
        const pathArg = String(args.path ?? "").trim()
        if (!pathArg) return { output: "缺少 path 参数" }
        let root: string
        if (isAbsolute(pathArg)) {
          if (ctx.sandboxed) return { output: "服务端部署模式仅可服务预置项目目录（传项目名而非绝对路径）" }
          root = pathArg
        } else {
          try {
            root = ctx.resolveProjectPath(pathArg)
          } catch {
            return { output: `无法解析目录: ${pathArg}` }
          }
        }
        const { stat } = await import("node:fs/promises")
        try {
          const st = await stat(root)
          if (!st.isDirectory()) return { output: `不是目录: ${root}` }
        } catch {
          return { output: `目录不存在: ${root}` }
        }
        const existing = staticServers.get(root)
        if (existing) return { output: `该目录已在服务中: ${existing.url}` }
        const port = num(args.port, 0)
        const server = Bun.serve({
          port,
          fetch(req) {
            try {
              const url = new URL(req.url)
              const p = decodeURIComponent(url.pathname)
              const file = normalize(join(root, p))
              const base = normalize(root)
              if (file !== base && !file.toLowerCase().startsWith(base.toLowerCase() + sep)) {
                return new Response("Forbidden", { status: 403 })
              }
              const target = statSync(file).isDirectory() ? join(file, "index.html") : file
              if (!existsSync(target)) return new Response("Not Found", { status: 404 })
              return new Response(readFileSync(target), { headers: { "content-type": serveMime(target) } })
            } catch {
              return new Response("Bad Request", { status: 400 })
            }
          },
        })
        const url = `http://127.0.0.1:${server.port}`
        staticServers.set(root, { url, stop: () => server.stop() })
        return { output: `已启动静态服务器: ${url}（服务目录 ${root}）\n用 open 打开该地址即可浏览/自动化。` }
      },
    },
  }
  return tools
}
