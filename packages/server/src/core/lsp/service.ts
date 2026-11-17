/**
 * LSP · 服务层：会话池（按 用户 × **工程根** × 服务器 复用进程）+ 文档归属 + 事件订阅 + 空闲回收。
 *
 * 语义与终端 PTY 服务同构（`core/exec/pty-session.ts`）：会话表 + 订阅推送 + idle 回收 +
 * `detach(key)` 随 WS 断开退订。差别在于：
 * - LSP 的「输出」不是字节流而是**语义事件**（诊断 / 服务器退出 / stderr 日志），按用户过滤后推给订阅者；
 * - 会话按 (用户, **工程根**, 服务器) 复用——复用键用一个**探测出来的工程根**（`project-root.ts`）
 *   而不是工作台根：同一 Go 工程的多个文件（哪怕它们来自不同的工作台根）共用一个 gopls，
 *   而工作台根落在模块子目录里时也不会再出现「gopls 找不到 module」的静默降级；
 * - 有打开文档的会话用更长空闲阈值，避免「文件开着不动一会儿补全就失效」。
 *
 * 前端只认 `docId`（它不知道绝对路径）：`didOpen` 的 file uri 由会话层生成，请求参数里的
 * `uri === docId` 在转发前替换为真实 uri，服务器回来的诊断 uri 反向换回 docId。
 */

import { randomUUID } from "node:crypto"
import { LspSession, type LspSessionEvent, type LspSpawner } from "./session"
import { detectProjectRoot, type ProjectRootInfo } from "./project-root"
import { resolveRegistry, serverForLanguage, type LspMissing, type LspRegistry, type LspServerPick } from "./registry"

/** 空闲回收阈值（无打开文档；有文档时按此值的 6 倍）。 */
export const LSP_IDLE_MS = 10 * 60 * 1000
/** 并发语言服务器进程上限（每个服务器都是一份常驻索引，超标即拒绝）。 */
export const LSP_MAX_SESSIONS = 4

export interface LspServiceOptions {
  /** GEBAI_LSP_SERVERS 原文（JSON 字符串；覆盖内置表）。 */
  overrides?: string
  spawner?: LspSpawner
  which?: (cmd: string) => string | null
  now?: () => number
  idleMs?: number
  maxSessions?: number
  requestTimeoutMs?: number
  initTimeoutMs?: number
  /** 工程根探测的标记存在性探测（测试注入，避免依赖真实磁盘）。 */
  projectMarkerExists?: (absFile: string) => boolean
  /** 工程根探测的标记文件读取（工作区根细化用；测试注入）。 */
  projectMarkerRead?: (absFile: string) => string | null
  /** 工程根探测的目录/文件名拼接（测试注入；与 `projectMarkerExists` 配对使用同一套路径语义）。 */
  projectRootJoin?: (dir: string, name: string) => string
}

export interface LspOpenInput {
  user: string
  rootId: string
  rootAbs: string
  /** root 内相对路径（展示与审计用）。 */
  path: string
  /** 绝对路径（会话层据此生成 file uri）。 */
  absPath: string
  language: string
  text: string
  version: number
  /**
   * 可选的「跳转来源文档」docId：实现文档**复用它的服务器会话**（跳到工作区外的库文件时用）。
   * 服务端会校验「同用户 + 同一服务器二进制」，不满足则退回正常建会话路径。
   */
  attachTo?: string
}

export interface LspOpenOk {
  available: true
  docId: string
  session: string
  server: { id: string; command: string }
  /** 文档同步模式：0=不同步 / 1=全量 / 2=增量（前端按此决定是否发变更）。 */
  sync: 0 | 1 | 2
  capabilities: Record<string, unknown>
  /** 该文档发给服务器的 `file://` uri（前端判定「结果是不是本文」用）。 */
  uri: string
  /** 根信息：前端把服务器返回的 file:// uri 折算回「本根内相对路径」用。 */
  root: { id: string; abs: string }
  /** 语言服务器实际的**工程根**（探测结果；前端状态栏显示，便于判断语义能力为何有效/失效）。 */
  projectRoot: string
  /** 命中的工程标记文件（`go.mod` / `Cargo.toml`…）；未探测到为空串。 */
  projectMarker: string
  /** 本次是否新拉起了服务器进程（审计与状态栏提示用）。 */
  created: boolean
}

export interface LspOpenUnavailable {
  available: false
  reason: string
}

export type LspOpenResult = LspOpenOk | LspOpenUnavailable

/** 服务端 → 前端推送（ws 层包成 `lsp.notify` / `lsp.exit` / `lsp.log`）。 */
export type LspPush =
  | { type: "notify"; docId: string; method: string; params: Record<string, unknown> }
  | { type: "exit"; session: string; server: string; code: number | null }
  | { type: "log"; session: string; text: string }

interface PooledSession {
  id: string
  key: string
  user: string
  rootId: string
  /** 语言服务器实际的工作根（探测到的工程根；无则工作台根）。 */
  rootAbs: string
  project: ProjectRootInfo
  server: LspServerPick
  session: LspSession
}

interface DocRef {
  user: string
  key: string
}

/** 转发前把参数里的 `uri === docId` 换成真实 file uri（递归；只认 docId 本身，不认任意字符串）。 */
export function rewriteDocUris(value: unknown, docId: string, uri: string): unknown {
  if (Array.isArray(value)) return value.map((v) => rewriteDocUris(v, docId, uri))
  if (value && typeof value === "object") {
    const out: Record<string, unknown> = {}
    for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
      out[k] = k === "uri" && v === docId ? uri : rewriteDocUris(v, docId, uri)
    }
    return out
  }
  return value
}

/**
 * `textDocument/*` 类请求补上 `textDocument.uri`（值是 docId，随后由 `rewriteDocUris` 换成真实 uri）。
 *
 * 为什么在服务端兜：`textDocument` 是这批方法的必填参数，缺了它服务器会报“no package metadata for
 * file ”这类看不出所以然的错（实测 gopls 0.21 就是这样），而这类“少了一层壳”的调用在多端接入时很容易
 * 出现。补全后行为与前端一致（前端一直在 `lspRequest` 里注入它）。
 */
export function withDocumentUri(method: string, params: unknown, docId: string): Record<string, unknown> {
  const out: Record<string, unknown> = params && typeof params === "object" && !Array.isArray(params) ? { ...(params as Record<string, unknown>) } : {}
  if (!method.startsWith("textDocument/")) return out
  const td = out.textDocument
  if (!td || typeof td !== "object" || Array.isArray(td) || (td as { uri?: unknown }).uri === undefined) {
    out.textDocument = { ...(td && typeof td === "object" ? (td as Record<string, unknown>) : {}), uri: docId }
  }
  return out
}

export class LspService {
  private opts: LspServiceOptions
  /** 探测结果（**惰性**：首次用到时才掃 PATH，见 `registry()`）。 */
  private reg: LspRegistry | null = null
  private sessions = new Map<string, PooledSession>()
  private docs = new Map<string, DocRef>()
  private subscribers = new Map<object, { user: string; sink: (evt: LspPush) => void }>()
  private now: () => number
  private idleMs: number
  private maxSessions: number
  private seq = 0

  constructor(opts: LspServiceOptions = {}) {
    this.opts = opts
    this.now = opts.now ?? Date.now
    this.idleMs = opts.idleMs ?? LSP_IDLE_MS
    this.maxSessions = opts.maxSessions ?? LSP_MAX_SESSIONS
  }

  /**
   * 探测可用语言服务器（**惰性**，首次真正需要时才做）。
   *
   * ## 为何不能放在构造函数里（实测代价）
   *
   * `resolveRegistry` 要对着 PATH 里每个目录 × 每个扩展名做 `statSync` 探测，
   * 而 `DEFAULT_SERVER_DEFS` 有 17 个候选命令。本机实测：**1484ms**（Windows，
   * PATH 条目多且部分在网络/重定向点）。它曾经在 `composeServer` 里随服务装配
   * 同步跑，于是这 1.5 秒完全落在用户看到的“启动中”里——而 LSP 是**文件工作台
   * 按需打开某文件时**才需要的能力，启动时没人用得到它。
   *
   * 惰性化不改变任何可观察语义：`servers()` / `open()` 仍然拿得到同一份登记表，
   * 只是把探测推到第一次真问的时候。前端拉 `/api/v1/lsp/servers` 才会触发它。
   */
  private registry(): LspRegistry {
    this.reg ??= resolveRegistry({ overrides: this.opts.overrides, which: this.opts.which })
    return this.reg
  }

  /** 可用服务器清单（REST/前端启动时拉取）。 */
  servers(): { enabled: boolean; servers: LspServerPick[]; missing: LspMissing[]; errors: string[] } {
    const reg = this.registry()
    return { enabled: true, servers: reg.picks, missing: reg.missing, errors: reg.errors }
  }

  /** 语言 → 可用服务器（无则 undefined：调用方静默降级）。 */
  serverFor(language: string): LspServerPick | undefined {
    return serverForLanguage(this.registry(), language)
  }

  /**
   * 打开文档：无可用服务器返回 `{ available:false }`（正常路径，不是错误——没有 LSP 照旧用）；
   * 起进程失败同样以 available:false 回，reason 带原因供前端与审计。
   */
  async open(input: LspOpenInput): Promise<LspOpenResult> {
    const server = this.serverFor(input.language)
    if (!server) {
      return { available: false, reason: `未检测到 ${input.language} 的语言服务器` }
    }
    // ① 跳到**工作区外的库文件**时，优先挂到“跳转来源那份文档”的会话上（同一服务器二进制才复用）：
    //    它已把该文件纳入索引、手里有编译参数；另起一份既没这两样，还会白吃一个并发槽位
    const reused = this.reusable(input.attachTo, input.user, server.id)
    if (reused) {
      const docId = `d${(this.seq += 1).toString(36)}${randomUUID().replace(/-/g, "").slice(0, 6)}`
      reused.session.openDocument({ docId, absPath: input.absPath, language: input.language, text: input.text, version: input.version })
      this.docs.set(docId, { user: input.user, key: reused.key })
      return {
        available: true,
        docId,
        session: reused.id,
        server: { id: reused.server.id, command: reused.server.command },
        sync: reused.session.sync,
        capabilities: reused.session.serverCapabilities,
        uri: reused.session.document(docId)?.uri ?? "",
        root: { id: input.rootId, abs: input.rootAbs },
        projectRoot: reused.rootAbs,
        projectMarker: reused.project.marker,
        created: false,
      }
    }
    // 工程根探测：服务器进程的 cwd / rootUri 用它，而不是工作台根（见 project-root.ts 的说明）
    const project = detectProjectRoot({
      fileAbs: input.absPath,
      rootAbs: input.rootAbs,
      language: input.language,
      exists: this.opts.projectMarkerExists,
      read: this.opts.projectMarkerRead,
      join: this.opts.projectRootJoin,
      now: this.now,
    })
    // 复用键用**工程根**：同一工程的多个文件共用一个服务器，跨工作台根也复用
    const key = `${input.user}|${project.abs}|${server.id}`
    let pooled = this.sessions.get(key)
    if (pooled && !pooled.session.alive) {
      this.drop(key)
      pooled = undefined
    }
    let created = false
    if (!pooled) {
      const newSession = await this.create(input, server, key, project)
      if ("available" in newSession) return newSession
      pooled = newSession
      created = true
    }
    const docId = `d${(this.seq += 1).toString(36)}${randomUUID().replace(/-/g, "").slice(0, 6)}`
    pooled.session.openDocument({ docId, absPath: input.absPath, language: input.language, text: input.text, version: input.version })
    this.docs.set(docId, { user: input.user, key })
    return {
      available: true,
      docId,
      session: pooled.id,
      server: { id: pooled.server.id, command: pooled.server.command },
      sync: pooled.session.sync,
      capabilities: pooled.session.serverCapabilities,
      uri: pooled.session.document(docId)?.uri ?? "",
      root: { id: input.rootId, abs: input.rootAbs },
      projectRoot: pooled.rootAbs,
      projectMarker: pooled.project.marker,
      created,
    }
  }

  /** 请求转发（补全 / 悬停 / 定义 / 引用 / 重命名 / 格式化 …）。 */
  async request(user: string, docId: string, method: string, params: unknown): Promise<unknown> {
    const { session, doc } = this.locate(user, docId)
    const out = await session.session.request(method, rewriteDocUris(withDocumentUri(method, params, docId), docId, doc.uri))
    return out
  }

  /** 内容变更（前端发全文；服务端按会话同步模式决定是否下发 didChange）。 */
  change(user: string, docId: string, version: number, text: string): void {
    const { session } = this.locate(user, docId)
    session.session.changeDocument(docId, version, text)
  }

  save(user: string, docId: string, text?: string): void {
    const { session } = this.locate(user, docId)
    session.session.saveDocument(docId, text)
  }

  close(user: string, docId: string): void {
    const ref = this.docs.get(docId)
    if (!ref || ref.user !== user) return
    this.docs.delete(docId)
    this.sessions.get(ref.key)?.session.closeDocument(docId)
  }

  /** 订阅事件（连接级 key：同一 WS 上的多次 open 只订阅一次）。 */
  subscribe(key: object, user: string, sink: (evt: LspPush) => void): () => void {
    this.subscribers.set(key, { user, sink })
    return () => {
      if (this.subscribers.get(key)?.sink === sink) this.subscribers.delete(key)
    }
  }

  /** 连接断开：按连接级 key 退订（会话本身保留，等空闲回收）。 */
  detach(key: object): void {
    this.subscribers.delete(key)
  }

  /** 会话清单（诊断/列表用）。 */
  list(): Array<{ id: string; server: string; root: string; docs: number; alive: boolean }> {
    return [...this.sessions.values()].map((p) => ({
      id: p.id,
      server: p.server.id,
      root: p.rootAbs,
      docs: p.session.documents.length,
      alive: p.session.alive,
    }))
  }

  /** 空闲回收：无文档会话按 idleMs，有文档会话按 6 倍阈值（文件开着不动不该丢能力）。 */
  sweep(): void {
    const t = this.now()
    for (const [key, p] of [...this.sessions]) {
      const docs = p.session.documents.length
      const limit = docs > 0 ? this.idleMs * 6 : this.idleMs
      if (t - p.session.lastActive > limit) this.drop(key)
    }
  }

  dispose(): void {
    for (const key of [...this.sessions.keys()]) this.drop(key)
    this.subscribers.clear()
  }

  /* --------------------------- 内部 --------------------------- */

  /**
   * 可复用的会话（指向的文档属于同一用户、服务器二进制也相同），否则 null。
   *
   * 三条门槛都是必须的：**跨用户**不能复用（会话按用户隔离）；**跨服务器**不能复用（拿 gopls 去答一个
   * C 头文件是错配）；**会话已死**不能复用（回退到正常建会话路径）。
   */
  private reusable(attachTo: string | undefined, user: string, serverId: string): PooledSession | null {
    if (!attachTo) return null
    const ref = this.docs.get(attachTo)
    if (!ref || ref.user !== user) return null
    const pooled = this.sessions.get(ref.key)
    if (!pooled || !pooled.session.alive) return null
    if (pooled.server.id !== serverId) return null
    return pooled
  }

  private locate(user: string, docId: string): { session: PooledSession; doc: { uri: string; path: string } } {
    const ref = this.docs.get(docId)
    if (!ref || ref.user !== user) throw new Error("LSP 文档未打开或已关闭")
    const pooled = this.sessions.get(ref.key)
    const doc = pooled?.session.document(docId)
    if (!pooled || !doc) throw new Error("语言服务器已关闭：请重新打开该文件")
    pooled.session.touch()
    return { session: pooled, doc }
  }

  private async create(input: LspOpenInput, server: LspServerPick, key: string, project: ProjectRootInfo): Promise<PooledSession | LspOpenUnavailable> {
    this.sweep()
    if (this.sessions.size >= this.maxSessions) {
      // 先回收「无文档」的空闲会话，仍满则拒绝（避免无上限拉起常驻索引进程）
      for (const [k, p] of [...this.sessions]) if (p.session.documents.length === 0) this.drop(k)
    }
    if (this.sessions.size >= this.maxSessions) {
      return { available: false, reason: `语言服务器进程数已达上限（${this.maxSessions}）：请关闭部分文件或稍后再试` }
    }
    const id = `s${(this.seq += 1).toString(36)}${randomUUID().replace(/-/g, "").slice(0, 6)}`
    const session = new LspSession({
      // 工程根而非工作台根：进程 cwd 与 rootUri/workspaceFolders 都用它
      rootAbs: project.abs,
      server,
      spawner: this.opts.spawner,
      requestTimeoutMs: this.opts.requestTimeoutMs,
      initTimeoutMs: this.opts.initTimeoutMs,
      now: this.now,
      onEvent: (evt) => this.publish(id, server.id, input.user, evt),
    })
    try {
      await session.start()
    } catch (err) {
      session.dispose()
      return { available: false, reason: `启动 ${server.id} 失败：${(err as Error).message}` }
    }
    const pooled: PooledSession = { id, key, user: input.user, rootId: input.rootId, rootAbs: project.abs, project, server, session }
    this.sessions.set(key, pooled)
    return pooled
  }

  private drop(key: string): void {
    const p = this.sessions.get(key)
    if (!p) return
    this.sessions.delete(key)
    for (const doc of p.session.documents) this.docs.delete(doc.docId)
    p.session.dispose()
  }

  /** 会话事件 → 该用户的订阅者（按会长级订阅过滤用户，避免串号）。 */
  private publish(sessionId: string, server: string, user: string, evt: LspSessionEvent): void {
    let push: LspPush
    if (evt.type === "notify") {
      const docId = typeof evt.params?.uri === "string" ? evt.params.uri : ""
      push = { type: "notify", docId, method: evt.method, params: evt.params }
    } else if (evt.type === "exit") {
      push = { type: "exit", session: sessionId, server, code: evt.code }
    } else {
      push = { type: "log", session: sessionId, text: evt.text }
    }
    for (const [, sub] of this.subscribers) {
      if (sub.user !== user) continue
      try {
        sub.sink(push)
      } catch {
        /* 单个订阅者异常不影响其它 */
      }
    }
  }
}
