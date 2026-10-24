/**
 * LSP · 单个（根目录 × 服务器）会话：子进程生命周期 + JSON-RPC 请求/通知 + 文本文档同步。
 *
 * 为什么自己实现而不引 `vscode-languageclient`：前端 Monaco 是 AMD 版（原因见
 * `packages/web/src/files/editor.ts` 顶部），官方客户端绑定 ESM 打包版 monaco；服务端这侧
 * 只需要「帧 + 请求关联 + 文档同步」三件事，自实现更小、无新依赖，也便于注入假进程做单测。
 *
 * **反向请求必须应答**：服务器会主动发请求（`workspace/configuration`、
 * `client/registerCapability`、`window/workDoneProgress/create` …），不应答会一直阻塞其初始化。
 * 这里按方法名给最小可用答复，未知方法回 `null`（不打断会话）。
 *
 * 文档标识：前端不认识绝对路径，`didOpen` 发给服务器的 uri 由本层用 `rootAbs + 相对路径` 生成，
 * 对外（前端）只用 `docId`；服务器回来的 `publishDiagnostics.uri` 反向换回 `docId` 再上行。
 */

import { spawn } from "node:child_process"
import { basename } from "node:path"
import { FrameReader, encodeFrame } from "./protocol"
import type { LspServerPick } from "./registry"

/** 子进程抽象（测试注入替身，不真起进程）。 */
export interface LspProc {
  pid: number | null
  write(data: string): void
  kill(): void
}

export interface LspSpawnInput {
  cmd: string[]
  cwd: string
  env: Record<string, string>
  onStdout: (chunk: string) => void
  onStderr: (text: string) => void
  onExit: (code: number | null) => void
}

export type LspSpawner = (input: LspSpawnInput) => LspProc

/** 进程树终止（Windows 用 taskkill /T：cmd / node 包装层会派生孙进程）。 */
export function killProcessTree(pid: number | null): void {
  if (pid == null) return
  if (process.platform === "win32") {
    try {
      spawn("taskkill", ["/pid", String(pid), "/T", "/F"], { stdio: "ignore" }).on("error", () => {})
    } catch {
      /* taskkill 不可用：下面 SIGKILL 兜底 */
    }
  }
  try {
    process.kill(pid, "SIGKILL")
  } catch {
    /* 已退出 */
  }
}

/** 默认 spawner：node 子进程 + 流式 UTF-8 解码。Windows 上 `.cmd`/`.bat` 包装器须经 cmd.exe。 */
export const defaultLspSpawner: LspSpawner = (input) => {
  let file = input.cmd[0] ?? ""
  let args = input.cmd.slice(1)
  if (process.platform === "win32" && /\.(cmd|bat)$/i.test(file)) {
    args = ["/d", "/s", "/c", file, ...args]
    file = process.env.ComSpec || "cmd.exe"
  }
  const child = spawn(file, args, {
    cwd: input.cwd,
    env: input.env,
    stdio: ["pipe", "pipe", "pipe"],
    windowsHide: true,
  })
  const outDec = new TextDecoder("utf-8")
  const errDec = new TextDecoder("utf-8")
  child.stdout?.on("data", (b: Buffer) => input.onStdout(outDec.decode(b, { stream: true })))
  child.stderr?.on("data", (b: Buffer) => input.onStderr(errDec.decode(b, { stream: true })))
  // stdin 的错误（EPIPE：语言服务器提前退出）必须有监听者，否则流上的 error 事件会抛到进程级
  child.stdin?.on("error", () => {})
  let exited = false
  child.on("exit", (code) => {
    if (exited) return
    exited = true
    input.onExit(code)
  })
  child.on("error", (err) => {
    input.onStderr(`${(err as Error).message}\n`)
    if (exited) return
    exited = true
    input.onExit(null)
  })
  return {
    pid: child.pid ?? null,
    write: (data) => {
      try {
        child.stdin?.write(data)
      } catch {
        /* 进程已退出：由 exit 事件收敛状态 */
      }
    },
    kill: () => killProcessTree(child.pid ?? null),
  }
}

export interface LspSessionEventNotify {
  type: "notify"
  method: string
  params: Record<string, unknown>
}
export interface LspSessionEventLog {
  type: "log"
  text: string
}
export interface LspSessionEventExit {
  type: "exit"
  code: number | null
}
export type LspSessionEvent = LspSessionEventNotify | LspSessionEventLog | LspSessionEventExit

/** 已同步的文档（docId ↔ 绝对路径 ↔ 服务器 uri）。 */
export interface LspDocumentInfo {
  docId: string
  uri: string
  path: string
  language: string
  version: number
}

export interface LspSessionOptions {
  rootAbs: string
  server: LspServerPick
  spawner?: LspSpawner
  env?: Record<string, string>
  now?: () => number
  onEvent?: (evt: LspSessionEvent) => void
  /** 普通请求超时（默认 15s）。 */
  requestTimeoutMs?: number
  /** initialize 握手超时（默认 30s：rust-analyzer / clangd 首次索引较慢）。 */
  initTimeoutMs?: number
}

/** 空进程环境基准（process.env 过滤 undefined + 会话附加项）。 */
function baseEnv(extra?: Record<string, string>): Record<string, string> {
  const out: Record<string, string> = {}
  for (const [k, v] of Object.entries(process.env)) if (v !== undefined) out[k] = v
  return { ...out, ...(extra ?? {}) }
}

/** Windows 盘符与空格/中文路径 → file URI（`C:\a b\c.ts` → `file:///C:/a%20b/c.ts`）。 */
export function pathToFileUri(abs: string): string {
  let p = String(abs ?? "").replace(/\\/g, "/")
  if (!p.startsWith("/")) p = `/${p}`
  const enc = p
    .split("/")
    .map((seg) => encodeURIComponent(seg).replace(/%3A/gi, ":"))
    .join("/")
  return `file://${enc}`
}

/**
 * 归一化 uri 键：`file:///C:/a/b` 与 `file:///c%3A/a/b` 必须命中同一条登记（服务器会按自己的规范改写盘符大小写与编码）。
 *
 * `platform` 可注入（默认当前平台）：Windows 盘符/路径大小写不敏感、POSIX 必须保持敏感——把这条差异
 * 变成显式参数，用例不必随宿主平台漂移。
 */
export function uriKey(uri: string, platform: NodeJS.Platform = process.platform): string {
  const raw = String(uri ?? "").trim()
  if (!/^file:/i.test(raw)) return raw
  let path = raw.replace(/^file:\/\//i, "")
  try {
    path = decodeURIComponent(path)
  } catch {
    /* 有非法转义：保留原样 */
  }
  path = path.replace(/\\/g, "/")
  // Windows 盘符/路径大小写不敏感；POSIX 保持敏感
  return platform === "win32" ? path.toLowerCase() : path
}

/** 归一化服务器返回的 textDocumentSync（数字或对象）→ 0=不同步 / 1=全量 / 2=增量。 */
export function normalizeSyncKind(raw: unknown): 0 | 1 | 2 {
  const v = typeof raw === "object" && raw !== null ? (raw as { change?: unknown }).change : raw
  return v === 0 || v === 1 || v === 2 ? v : 1
}

/**
 * 客户端能力声明（与前端实际注册的 provider 对齐）。两处刻意为之：
 * - **不声明 `workspace.workspaceFolders`**：实测 pyright（1.1.407）在客户端声明该能力后会把分析准备挂起——
 *   hover / 补全 / 定义全部不应答（`textDocument/completion` 直到超时）；不声明则一切正常。
 *   initialize 参数里仍带 workspaceFolders（供 clangd / rust-analyzer 这类优先用多根工作区的服务器）；
 * - **不声明 `workspace.configuration`**：免服务器索取配置（我们在反向请求里只能回 null，声明了反招问题）。
 */
const CLIENT_CAPABILITIES = {
  workspace: {
    didChangeConfiguration: { dynamicRegistration: false },
    symbol: { dynamicRegistration: false },
  },
  textDocument: {
    synchronization: { dynamicRegistration: false, didSave: true, willSave: false, willSaveWaitUntil: false },
    publishDiagnostics: { relatedInformation: true, versionSupport: true, codeDescriptionSupport: true },
    completion: {
      dynamicRegistration: false,
      contextSupport: true,
      completionItem: {
        snippetSupport: true,
        documentationFormat: ["markdown", "plaintext"],
        insertReplaceSupport: true,
        resolveSupport: { properties: ["documentation", "detail", "additionalTextEdits"] },
      },
    },
    hover: { dynamicRegistration: false, contentFormat: ["markdown", "plaintext"] },
    definition: { dynamicRegistration: false, linkSupport: true },
    references: { dynamicRegistration: false },
    documentSymbol: { dynamicRegistration: false, hierarchicalDocumentSymbolSupport: true },
    signatureHelp: {
      dynamicRegistration: false,
      contextSupport: true,
      signatureInformation: { documentationFormat: ["markdown", "plaintext"] },
    },
    rename: { dynamicRegistration: false, prepareSupport: true },
    formatting: { dynamicRegistration: false },
    rangeFormatting: { dynamicRegistration: false },
  },
  window: { workDoneProgress: true, showMessage: {} },
} as const

export class LspSession {
  readonly server: LspServerPick
  readonly rootAbs: string
  private spawner: LspSpawner
  private envExtra?: Record<string, string>
  private now: () => number
  private onEvent?: (evt: LspSessionEvent) => void
  private requestTimeoutMs: number
  private initTimeoutMs: number
  private proc: LspProc | null = null
  private reader = new FrameReader()
  private seq = 0
  private pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void; timer: ReturnType<typeof setTimeout> }>()
  private docs = new Map<string, LspDocumentInfo>()
  private byUri = new Map<string, string>()
  private capabilities: Record<string, unknown> = {}
  private syncKind: 0 | 1 | 2 = 1
  private running = false
  private lastActiveAt: number
  private stderrTail = ""

  constructor(opts: LspSessionOptions) {
    this.server = opts.server
    this.rootAbs = opts.rootAbs
    this.spawner = opts.spawner ?? defaultLspSpawner
    this.envExtra = opts.env
    this.now = opts.now ?? Date.now
    this.onEvent = opts.onEvent
    this.requestTimeoutMs = opts.requestTimeoutMs ?? 15000
    this.initTimeoutMs = opts.initTimeoutMs ?? 30000
    this.lastActiveAt = this.now()
  }

  /** 起进程并完成 initialize 握手（失败抛错，由调用方决定是否降级）。 */
  async start(): Promise<void> {
    const cmd = [this.server.command, ...this.server.args]
    this.proc = this.spawner({
      cmd,
      cwd: this.rootAbs,
      env: baseEnv(this.envExtra),
      onStdout: (chunk) => this.onStdout(chunk),
      onStderr: (text) => this.onStderr(text),
      onExit: (code) => this.onExit(code),
    })
    this.running = true
    try {
      const res = await this.request(
        "initialize",
        {
          processId: process.pid,
          clientInfo: { name: "gebai-file-workbench", version: "1" },
          rootUri: pathToFileUri(this.rootAbs),
          rootPath: this.rootAbs,
          workspaceFolders: [{ uri: pathToFileUri(this.rootAbs), name: basename(this.rootAbs) || this.rootAbs }],
          capabilities: CLIENT_CAPABILITIES,
          trace: "off",
        },
        this.initTimeoutMs,
      )
      const caps = (res as { capabilities?: Record<string, unknown> } | null)?.capabilities
      this.capabilities = caps && typeof caps === "object" ? caps : {}
      this.syncKind = normalizeSyncKind(this.capabilities.textDocumentSync)
      this.notify("initialized", {})
    } catch (err) {
      this.dispose()
      throw err
    }
  }

  get alive(): boolean {
    return this.running && !!this.proc
  }

  get lastActive(): number {
    return this.lastActiveAt
  }

  /** 文档同步模式（0=不同步 / 1=全量 / 2=增量）。 */
  get sync(): 0 | 1 | 2 {
    return this.syncKind
  }

  /** 服务器能力（原样回传前端，前端据此决定注册哪些 provider）。 */
  get serverCapabilities(): Record<string, unknown> {
    return this.capabilities
  }

  get documents(): LspDocumentInfo[] {
    return [...this.docs.values()]
  }

  /** 服务器观察到的最近 stderr（诊断/报错提示用）。 */
  get stderr(): string {
    return this.stderrTail
  }

  touch(): void {
    this.lastActiveAt = this.now()
  }

  /* --------------------------- 请求 / 通知 --------------------------- */

  request(method: string, params: unknown, timeoutMs = this.requestTimeoutMs): Promise<unknown> {
    if (!this.alive) return Promise.reject(new Error("语言服务器未运行"))
    const id = ++this.seq
    return new Promise<unknown>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        reject(new Error(`${method} 超时（${Math.round(timeoutMs / 1000)}s）`))
      }, timeoutMs)
      this.pending.set(id, { resolve, reject, timer })
      try {
        this.send({ jsonrpc: "2.0", id, method, params })
      } catch (err) {
        clearTimeout(timer)
        this.pending.delete(id)
        reject(err as Error)
      }
    })
  }

  notify(method: string, params: unknown): void {
    if (!this.alive) return
    try {
      this.send({ jsonrpc: "2.0", method, params })
    } catch {
      /* 进程刚退出：状态由 exit 事件收敛 */
    }
  }

  /* --------------------------- 文档同步 --------------------------- */

  openDocument(input: { docId: string; absPath: string; language: string; text: string; version: number }): LspDocumentInfo {
    const uri = pathToFileUri(input.absPath)
    const doc: LspDocumentInfo = { docId: input.docId, uri, path: input.absPath, language: input.language, version: input.version }
    this.docs.set(input.docId, doc)
    this.byUri.set(uriKey(uri), input.docId)
    this.touch()
    if (this.syncKind === 0) return doc
    this.notify("textDocument/didOpen", {
      textDocument: { uri, languageId: input.language, version: input.version, text: input.text },
    })
    return doc
  }

  /** 内容变更（前端一律发全文：对 Full 语义正确，对 Incremental 也是合法的整篇替换）。 */
  changeDocument(docId: string, version: number, text: string): void {
    const doc = this.docs.get(docId)
    if (!doc || this.syncKind === 0) return
    doc.version = version
    this.touch()
    this.notify("textDocument/didChange", {
      textDocument: { uri: doc.uri, version },
      contentChanges: [{ text }],
    })
  }

  saveDocument(docId: string, text?: string): void {
    const doc = this.docs.get(docId)
    if (!doc) return
    this.touch()
    this.notify("textDocument/didSave", text === undefined ? { textDocument: { uri: doc.uri } } : { textDocument: { uri: doc.uri }, text })
  }

  closeDocument(docId: string): void {
    const doc = this.docs.get(docId)
    if (!doc) return
    this.docs.delete(docId)
    this.byUri.delete(uriKey(doc.uri))
    this.touch()
    this.notify("textDocument/didClose", { textDocument: { uri: doc.uri } })
  }

  /** 前端 uri 标识（docId）→ 文档信息（service 转发请求时用）。 */
  document(docId: string): LspDocumentInfo | undefined {
    return this.docs.get(docId)
  }

  dispose(): void {
    const wasRunning = this.running
    this.running = false
    if (wasRunning) {
      // 尽力优雅退出（不等应答）：服务器无跨会话状态，随后直接杀进程树
      try {
        this.send({ jsonrpc: "2.0", id: ++this.seq, method: "shutdown", params: null })
        this.send({ jsonrpc: "2.0", method: "exit", params: null })
      } catch {
        /* 已退出 */
      }
    }
    const proc = this.proc
    this.proc = null
    try {
      proc?.kill()
    } catch {
      /* 已退出 */
    }
    this.failPending("语言服务器已关闭")
    this.docs.clear()
    this.byUri.clear()
  }

  /* --------------------------- 内部 --------------------------- */

  private send(msg: unknown): void {
    this.proc?.write(encodeFrame(msg).toString("utf8"))
  }

  private onStdout(chunk: string): void {
    for (const body of this.reader.push(chunk)) this.handle(body)
  }

  private onStderr(text: string): void {
    if (!text) return
    this.stderrTail = (this.stderrTail + text).slice(-2000)
    this.onEvent?.({ type: "log", text })
  }

  private onExit(code: number | null): void {
    if (!this.running) return
    this.running = false
    this.proc = null
    this.reader.reset()
    this.failPending("语言服务器已退出")
    this.onEvent?.({ type: "exit", code })
  }

  private failPending(reason: string): void {
    for (const [, p] of this.pending) {
      clearTimeout(p.timer)
      p.reject(new Error(reason))
    }
    this.pending.clear()
  }

  private handle(body: string): void {
    let msg: { id?: number | string; method?: string; params?: unknown; result?: unknown; error?: { code?: number; message?: string } }
    try {
      msg = JSON.parse(body) as typeof msg
    } catch {
      this.onStderr(`[非 JSON 报文] ${body.slice(0, 200)}\n`)
      return
    }
    this.touch()
    if (typeof msg.method === "string") {
      if (msg.id !== undefined) return this.answerServerRequest(msg.id, msg.method, msg.params)
      return this.serverNotification(msg.method, msg.params)
    }
    if (msg.id === undefined) return
    const p = this.pending.get(Number(msg.id))
    if (!p) return
    this.pending.delete(Number(msg.id))
    clearTimeout(p.timer)
    if (msg.error) p.reject(new Error(`${msg.error.message ?? "语言服务器错误"}${msg.error.code ? ` (${msg.error.code})` : ""}`))
    else p.resolve(msg.result)
  }

  private serverNotification(method: string, params: unknown): void {
    if (method === "textDocument/publishDiagnostics") {
      const p = (params ?? {}) as { uri?: unknown; diagnostics?: unknown }
      const docId = typeof p.uri === "string" ? this.byUri.get(uriKey(p.uri)) : undefined
      // 未登记的文档（如服务器主动分析的文件）：丢弃，避免前端拿到不认识的 uri
      if (!docId) return
      this.onEvent?.({ type: "notify", method, params: { ...(p as Record<string, unknown>), uri: docId } })
      return
    }
    if (method === "window/showMessage") {
      const text = typeof (params as { message?: unknown })?.message === "string" ? String((params as { message: string }).message) : ""
      if (text) this.onEvent?.({ type: "log", text: `${text}\n` })
      return
    }
    if (method === "window/logMessage") {
      const text = typeof (params as { message?: unknown })?.message === "string" ? String((params as { message: string }).message) : ""
      if (text) this.onEvent?.({ type: "log", text: `${text}\n` })
    }
  }

  /** 服务器反向请求：最小可用答复（不应答会卡住其初始化/初始化后配置环节）。 */
  private answerServerRequest(id: number | string, method: string, params: unknown): void {
    this.send({ jsonrpc: "2.0", id, result: this.replyFor(method, params) })
  }

  private replyFor(method: string, params: unknown): unknown {
    switch (method) {
      case "workspace/configuration": {
        const items = (params as { items?: unknown })?.items
        return Array.isArray(items) ? items.map(() => null) : []
      }
      case "workspace/workspaceFolders":
        return [{ uri: pathToFileUri(this.rootAbs), name: basename(this.rootAbs) || this.rootAbs }]
      case "window/workDoneProgress/create":
      case "client/registerCapability":
      case "client/unregisterCapability":
      case "window/showMessageRequest":
        return null
      case "workspace/applyEdit":
        return { applied: false }
      case "window/showDocument":
        return { success: false }
      default:
        return null
    }
  }
}
