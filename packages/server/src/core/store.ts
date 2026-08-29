import { mkdir, writeFile, readFile, rm, readdir, stat, unlink, rename } from "node:fs/promises"
import { join, resolve, sep, isAbsolute, relative } from "node:path"
import type { FileEntry, Message, SessionInfo, TodoItem } from "@gebai/sdk"
import { assertNoSymlinkEscape, isValidSessionId, resolveInSandbox, sessionPath, walkDir } from "./paths"
import { randomUUID } from "node:crypto"

const MAX_CACHE_MESSAGES = 300
const MAX_CACHE_SESSIONS = 10
/** 会话 env 内存缓存上限（LRU）：仅活跃会话驻留，防长生命周期进程无界增长。 */
const MAX_ENV_CACHE_SESSIONS = 256

export interface SessionStoreOptions {
  home: string
}

export interface SessionFile {
  session: SessionData
  env: Record<string, string>
}

export interface SessionData {
  id: string
  name: string
  userId: string
  messages: Message[]
  todos: TodoItem[]
  createdAt: number
  updatedAt: number
  /** 会话已装载子Agent 名单（chat.json 持久化）：恢复历史会话时据此重新注册工具；
   *  未定义 = 新会话/旧格式，首次运行按启动预载名单（GEBAI_PRELOAD_SUB_AGENTS）初始化。 */
  loadedSubAgents?: string[]
  /** 上下文 token 估算（chars/4）：任务结束时持久化，会话列表展示用（单位 k）。
   *  有 usage 真值时 = 最近一次调用的真实 input tokens + 未发送增量估算。 */
  ctxTokens?: number
  /** 最近一次模型调用的真实 input tokens（服务端 usage 真值，含 system 提示词与工具 schema）：
   *  跨 run 上下文压缩判定基线；未定义 = 无真值（老会话/接口不返回 usage/压缩后锚点失效），压缩判定与显示走估算兜底。 */
  ctxInputTokens?: number
  /** 同一次调用的提示词缓存命中 tokens（ctxInputTokens 的一部分，接口返回 cached_tokens/cache_read 才有值）：
   *  纯展示口径（前端上下文圆环悬浮命中率），随 usage 基线同点位建立/清除。 */
  ctxCachedTokens?: number
  /** 建立 ctxInputTokens 基线那次调用已覆盖的历史消息条数（loadHistory 坐标）：下次 run 以
   *  history.slice(ctxAtMessage) 估算基线之后的增量（下一次真实调用会用真值接管并重建基线）。 */
  ctxAtMessage?: number
  /** 会话级运行时定义工具清单（js defineTool 注册，chat.json 持久化、重启恢复）：序列化定义。 */
  dynamicTools?: import("./types").DynamicToolDef[]
}

/** 会话公开信息（列表/详情接口统一序列化，REST 与 WS 共用）。 */
export function toSessionInfo(s: SessionData): SessionInfo {
  // ctxTokens 兜底：旧会话（新版本运行前创建）无持久化值时按持久化消息即时估算，保证列表全部有值
  return {
    id: s.id,
    name: s.name,
    userId: s.userId,
    createdAt: s.createdAt,
    updatedAt: s.updatedAt,
    ctxTokens: s.ctxTokens ?? (s.messages?.length ? estimateCtxTokens(s.messages) : undefined),
    ctxCachedTokens: s.ctxCachedTokens,
  }
}

/** CJK 字符判定（统一表意文字/假名/谚文/全角符号区段；charCode 范围判断比正则逐字符快）。 */
function isCjkChar(ch: string): boolean {
  const c = ch.charCodeAt(0)
  return (
    (c >= 0x3000 && c <= 0x303f) ||
    (c >= 0x3040 && c <= 0x30ff) ||
    (c >= 0x3400 && c <= 0x4dbf) ||
    (c >= 0x4e00 && c <= 0x9fff) ||
    (c >= 0xf900 && c <= 0xfaff) ||
    (c >= 0xff00 && c <= 0xffef)
  )
}

/** 文本 token 估算（CJK 感知）：CJK 字符约 1 token/字，其余（含 ASCII）约 4 字符/token。
 *  中文场景下 chars/4 会系统性低估 2~4 倍，导致压缩阈值触发过晚、真实窗口提前打满。
 *  引擎（增量估算）与会话列表展示（estimateCtxTokens）共用同一口径。 */
export function estimateCharsTokens(text: string): number {
  let cjk = 0
  let other = 0
  for (const ch of text) {
    if (isCjkChar(ch)) cjk++
    else other++
  }
  return Math.ceil(cjk + other / 4)
}

/**
 * 上下文 token 估算（CJK 感知，会话列表展示兜底口径，单位 k）：忽略 system 角色（system 为运行时拼装、
 * 不随会话持久化，统一口径避免显示值在运行结束前后回跳）。仅在无 usage 真值（老会话/接口不返回 usage）时
 * 由 toSessionInfo/任务结束持久化使用；有真值时的口径见 engine（真实 input tokens + 未发送增量估算）。
 */
export function estimateCtxTokens(msgs: Array<{ role?: string; content?: unknown; toolCalls?: Array<{ name?: string; arguments?: unknown }>; session?: boolean; subAgent?: boolean }>): number {
  let tokens = 0
  for (const m of msgs) {
    if (m.role === "system") continue
    // 新会话执行过程消息不进主 LLM 上下文：不计入上下文占用（会话列表展示口径与引擎一致；subAgent 为旧版兼容）
    if (m.session || m.subAgent) continue
    tokens += estimateCharsTokens(Array.isArray(m.content) ? JSON.stringify(m.content) : String(m.content ?? ""))
    if (m.toolCalls) {
      for (const tc of m.toolCalls) tokens += estimateCharsTokens((tc.name ?? "") + JSON.stringify(tc.arguments ?? {}))
    }
  }
  return tokens
}

/**
 * 上下文保护消息（不压缩、不改变）：系统提示词（含 loadedAgent 装载提示词/compacted 摘要/旧格式 system）
 * 与用户输入原样保留，新会话执行存档（session/sessionRun/subAgent/subAgentRun）同理——压缩不选进区间、
 * 不进摘要输入、超限截断不丢弃、区间夹带时原位保留。
 */
export function isProtectedMessage(m: { role?: string; session?: boolean; sessionRun?: unknown; subAgent?: boolean; subAgentRun?: unknown }): boolean {
  if (m.role === "user" || m.role === "system") return true
  return !!(m.session || m.sessionRun || m.subAgent || m.subAgentRun)
}

/** repairToolPairing 的结构化入参（Message 与 provider 层 MessageLike 均可赋入）。 */
export interface PairingRepairMessage {
  id?: string
  role: "user" | "assistant" | "tool" | "system"
  content?: unknown
  name?: string
  toolCallId?: string
  toolCalls?: Array<{ id: string; name: string; arguments?: unknown }>
  createdAt?: number
  session?: boolean
  sessionRun?: unknown
  subAgent?: boolean
  subAgentRun?: unknown
}

/**
 * assistant(toolCalls)/tool 配对完整性修复（上下文保护的兜底）：历史因任务取消中断、压缩/截断边界或
 * 旧版本缺陷产生「孤儿 tool 结果」（发起 assistant 已被删）或「未应答 toolCalls」（tool 结果缺失）时，
 * 严格校验的 LLM 接口（OpenAI 要求 tool 消息跟随对应 tool_calls、Anthropic 要求每个 tool_use 有
 * tool_result）会拒绝整个请求——会话自此每次运行都被 400 卡死，本函数使历史自愈：
 * - 孤儿 tool 消息：受保护的（agent_run 存档等）补一条最小 assistant(toolCalls) 桩保持配对可见，普通的丢弃；
 * - 中途出现未应答 toolCalls（其后已有后续消息）时补占位 tool 结果；
 * - 尾部未应答 toolCalls **只在 flushTail 时补占位**（llm 序列化前调用——发送时刻不可能存在在途批次）；
 *   存储/装载路径不 flush 尾部——正常执行流 assistant(toolCalls) 先落盘、结果随后到达，提前 flush 会
 *   让真实结果落盘时反被判孤儿丢弃。
 * 不重排顺序、不改动已配对消息。
 */
export function repairToolPairing<T extends PairingRepairMessage>(messages: T[], opts: { flushTail?: boolean } = {}): T[] {
  const out: T[] = []
  const pending = new Map<string, string>() // 未应答 toolCallId → 工具名
  const lastTs = () => (out.length && typeof out[out.length - 1].createdAt === "number" ? (out[out.length - 1].createdAt as number) : Date.now())
  const flush = () => {
    for (const [id, name] of pending) {
      out.push({
        id: randomUUID().replace(/-/g, ""),
        role: "tool",
        content: "（占位结果：该工具调用无结果记录——会话中断或历史边界调整丢失，已自动补齐以保持消息配对）",
        toolCallId: id,
        name,
        createdAt: lastTs(),
      } as T)
    }
    pending.clear()
  }
  for (const m of messages) {
    if (m.role === "tool") {
      if (m.toolCallId && pending.has(m.toolCallId)) {
        pending.delete(m.toolCallId)
        out.push(m)
      } else if (isProtectedMessage(m) && m.toolCallId) {
        // 受保护 tool（agent_run 存档）：补最小 assistant 桩而非丢弃——存档内容保持可见
        out.push({
          id: randomUUID().replace(/-/g, ""),
          role: "assistant",
          content: "",
          toolCalls: [{ id: m.toolCallId, name: m.name ?? "tool", arguments: {} }],
          createdAt: typeof m.createdAt === "number" ? m.createdAt : lastTs(),
        } as T)
        out.push(m)
      }
      // 普通孤儿 tool：丢弃（无发起 assistant 的结果会被接口拒绝，对模型亦无意义）
      continue
    }
    flush()
    out.push(m)
    if (m.role === "assistant" && m.toolCalls?.length) {
      for (const tc of m.toolCalls) pending.set(tc.id, tc.name)
    }
  }
  if (opts.flushTail) flush()
  return out
}

export class SessionStore {
  private cache = new Map<string, SessionData>()
  private envCache = new Map<string, Record<string, string>>()
  /** 按用户索引的会话路径缓存（listSessions 填充；load 的归属回退仅能命中本用户的列表）。 */
  private indexedPathsByUser = new Map<string, string[]>()
  /** 会话 id → 所有者 user id（无 user 上下文的归属查询用，如 webhook 事件过滤）。 */
  private owners = new Map<string, string>()

  constructor(private opts: SessionStoreOptions) {}

  private dir(user: string, id: string) {
    // 防御纵深：即使调用方漏校验，sessionPath 的格式白名单也会拒绝穿越串
    return sessionPath(this.opts.home, user, id)
  }

  private async ensureDir(p: string) {
    await mkdir(p, { recursive: true })
  }

  async createSession(userId: string, name?: string): Promise<SessionData> {
    const id = randomUUID().replace(/-/g, "")
    const now = Date.now()
    const session: SessionData = {
      id,
      name: name || "新会话",
      userId,
      messages: [],
      todos: [],
      createdAt: now,
      updatedAt: now,
    }
    const dir = this.dir(userId, id)
    await this.ensureDir(join(dir, "tmp"))
    await this.save(session)
    return session
  }

  async load(id: string, user?: string): Promise<SessionData | null> {
    if (!isValidSessionId(id)) return null // 格式白名单：穿越串/畸形 id 一律视为不存在
    const cached = this.cache.get(id)
    if (cached) {
      // 缓存命中仍须校验归属：session id 可能被跨用户猜中/引用，
      // 按 id 直接返回会形成跨用户读取与实时事件泄漏。
      // 旧版无 userId 会话在 load 时已按查找目录补全归属（见下），缓存内必有归属
      // 命中刷新位置（LRU）：运行中会话不因其他会话活跃而被挤出（挤出后无 user 的
      // appendMessage/getTodos 装载失败会中断运行中任务）
      if (!user || cached.userId === user) {
        this.touchCache(cached)
        return cached
      }
      return null
    }
    if (user) {
      // Deterministic path lookup when the owning user is known (sharded by id hash).
      const direct = join(sessionPath(this.opts.home, user, id), "chat.json")
      const s = await this.readFileByPath(direct)
      if (s) {
        // 归属校验与缓存分支一致：路径命中但 chat.json 内 userId 属于他人时拒绝
        // （正常会话的 userId 即所在目录用户；不一致仅可能因目录被手工移动/复制）
        if (s.userId && s.userId !== user) return null
        // 旧版会话缺 userId 字段：所在目录即归属，读时补全（防缓存/索引跨用户命中旧数据）
        if (!s.userId) s.userId = user
        this.touchCache(s)
        return s
      }
      // Fallback: index populated by a prior listSessions(user) —— 仅搜索该用户自己的索引，
      // 避免「跨用户索引命中无 userId 的旧版会话」导致会话泄漏
      const viaIndex = await this.readFileByPath(this.findByList(id, this.indexedPathsByUser.get(user) ?? []))
      if (viaIndex && (!viaIndex.userId || viaIndex.userId === user)) {
        if (!viaIndex.userId) viaIndex.userId = user
        this.touchCache(viaIndex)
        return viaIndex
      }
    }
    return null
  }

  private findByList(id: string, index: string[]): string | null {
    // resolved from listSessions paths cache
    const tail = `${id}${sep}chat.json`
    for (const p of index) {
      if (p.endsWith(tail)) return p
    }
    return null
  }

  async listSessions(userId: string): Promise<SessionData[]> {
    const base = join(this.opts.home, "users", userId, "sessions")
    const out: SessionData[] = []
    const index: string[] = []
    await walkDir(base, 3, async (p) => {
      if (!p.endsWith("chat.json")) return
      try {
        const raw = await readFile(p, "utf8")
        const session = JSON.parse(raw) as SessionData
        if (session.id && (session.userId === userId || !session.userId)) {
          out.push(session)
          index.push(p)
        }
      } catch {
        /* skip corrupt */
      }
    })
    this.indexedPathsByUser.set(userId, index)
    return out.sort((a, b) => b.updatedAt - a.updatedAt)
  }

  /**
   * 会话归属查询（无 user 上下文的调用方使用，如 webhook 事件过滤）：命中内存缓存
   * 或最近加载/创建记录；未命中返回 null（表示归属未知，调用方按未过滤处理）。
   */
  ownerOf(sessionId: string): string | null {
    const cached = this.cache.get(sessionId)
    if (cached?.userId) return cached.userId
    return this.owners.get(sessionId) ?? null
  }

  private async readFileByPath(p: string | null): Promise<SessionData | null> {
    if (!p) return null
    try {
      const raw = await readFile(p, "utf8")
      const s = JSON.parse(raw) as SessionData
      // 旧版本/中断产生的历史配对缺陷在装载时自愈（下次 save 落盘），否则会话将无法继续请求模型
      s.messages = repairToolPairing(s.messages)
      return s
    } catch {
      return null
    }
  }

  async save(session: SessionData): Promise<void> {
    session.updatedAt = Date.now()
    const dir = this.dir(session.userId, session.id)
    await this.ensureDir(dir)
    // 原子写（tmp + rename）：每条消息都全量重写 chat.json，进程崩溃/断电/磁盘满中断在写入中途
    // 会留下截断 JSON——装载侧 parse 失败被静默吞掉（会话「消失」且无法自愈）。先写临时文件、
    // rename 原子替换（Bun/libuv 在 Windows 上等价 MoveFileExW REPLACE_EXISTING），最坏情况
    // 只残留无害的 .tmp 文件
    const target = join(dir, "chat.json")
    const tmpPath = `${target}.tmp`
    await writeFile(tmpPath, JSON.stringify(session, null, 2))
    try {
      await rename(tmpPath, target)
    } catch {
      // Windows 上目标正被并发读取（readFile 句柄未关）时 MoveFileEx 替换失败（EPERM）：
      // 回退直接覆写（原子性降级为最佳努力）——rename 竞态不应中断整个任务
      await writeFile(target, JSON.stringify(session, null, 2))
    }
    this.touchCache(session)
  }

  /**
   * 超限截断（顺序保留）：受保护消息（isProtectedMessage：系统提示词/用户输入/压缩摘要/新会话执行存档）
   * 原位保留，从最早的其他消息（assistant/tool）开始丢弃直至长度不超上限——不重排消息顺序（append 语义
   * 装载的提示词消息保持在末尾，前端渲染与缓存引用顺序稳定），避免长会话中上下文压缩机制被静默破坏、
   * 避免装载提示词（会话恢复关键记录）与用户输入丢失。受保护消息本身超过上限时按原样保留（软上限，
   * 用户输入与系统提示词不改变优先）。丢弃按 tool_call 配对原子执行——assistant(toolCalls) 被丢弃时
   * 连带其后紧邻的 tool 结果（拆散配对会产生孤儿 tool 消息，严格校验的 LLM 接口会拒绝整个请求），
   * 实际保留条数可略低于上限（配对完整性优先）。
   */
  private trimToCacheLimit(messages: Message[]): Message[] {
    if (messages.length <= MAX_CACHE_MESSAGES) return messages
    const over = messages.length - MAX_CACHE_MESSAGES
    const out: Message[] = []
    let dropped = 0
    for (let i = 0; i < messages.length; i++) {
      const m = messages[i]
      if (isProtectedMessage(m) || dropped >= over) {
        out.push(m)
        continue
      }
      dropped++
      if (m.role === "assistant" && m.toolCalls?.length) {
        while (i + 1 < messages.length && messages[i + 1].role === "tool" && !isProtectedMessage(messages[i + 1])) i++
      }
    }
    return out
  }

  /** userId 可选：任务流程内的持久化调用传归属用户，缓存被挤出后仍可从磁盘确定性装载（LRU 双保险）。 */
  async appendMessage(sessionId: string, msg: Message, userId?: string): Promise<void> {
    const session = await this.load(sessionId, userId)
    if (!session) throw new Error(`session not found: ${sessionId}`)
    session.messages.push(msg)
    session.messages = this.trimToCacheLimit(session.messages)
    await this.save(session)
  }

  /** 截断会话消息：删除 beforeMsgId 及之后的所有消息（撤回该消息及其后续，用户/助手消息均可）。
   *  后缀删除不破坏 assistant(toolCalls)/tool 配对（工具结果恒在其发起消息之后，随之一并删除）。 */
  async truncateMessages(sessionId: string, userId: string, beforeMsgId: string): Promise<void> {
    const session = await this.load(sessionId, userId)
    if (!session) throw new Error(`session not found: ${sessionId}`)
    const idx = session.messages.findIndex((m) => m.id === beforeMsgId)
    if (idx === -1) throw new Error(`message not found: ${beforeMsgId}`)
    session.messages = session.messages.slice(0, idx)
    // 真实 usage 基线的索引锚点随删除错位（与压缩同因）：清除基线回退估算，下次真实调用重建
    session.ctxInputTokens = undefined
    session.ctxCachedTokens = undefined
    session.ctxAtMessage = undefined
    session.ctxTokens = estimateCtxTokens(session.messages)
    await this.save(session)
  }

  /**
   * 上下文压缩：将 [from, to) 区间内【可压缩】历史消息替换为一条摘要消息（DESIGN「上下文保护」）。
   * 区间内夹带的受保护消息（isProtectedMessage：系统提示词/用户输入/新会话执行存档）**原样保留**
   * （不参与压缩替换，仅其前后的普通消息合并为摘要）；摘要消息标记 compacted/summary，loadHistory 时作为
   * assistant 角色注入（不污染 system 段）。区间内无可压缩消息时不做任何改动（不创建摘要、不动 usage 基线）。
   * 返回压缩后的消息列表。
   */
  async compactMessages(sessionId: string, userId: string, opts: { from: number; to: number; summary: string }): Promise<Message[]> {
    const session = await this.load(sessionId, userId)
    if (!session) throw new Error(`session not found: ${sessionId}`)
    const messages = session.messages
    const start = Math.max(0, opts.from)
    const end = Math.min(messages.length, opts.to)
    if (start >= end) return messages
    const slice = messages.slice(start, end)
    // 受保护消息（系统提示词/用户输入/存档）原位保留——用户输入与系统提示词不压缩不改变
    const kept = slice.filter(isProtectedMessage)
    const removed = slice.length - kept.length
    if (removed === 0) return messages
    const firstTs = messages[start]?.createdAt
    const lastTs = messages[end - 1]?.createdAt
    const compacted: Message = {
      id: randomUUID().replace(/-/g, ""),
      role: "system",
      content: opts.summary,
      compacted: true,
      summary: removed > 1 ? `已压缩 ${removed} 条历史消息（${firstTs} ~ ${lastTs}）` : "已压缩 1 条历史消息",
      createdAt: Date.now(),
    }
    const next = [...messages.slice(0, start), ...kept, compacted, ...messages.slice(end)]
    // 区间边界可能切在 assistant(toolCalls)/tool 配对中间——压缩后立即修复配对完整性
    session.messages = this.trimToCacheLimit(repairToolPairing(next))
    // 消息被摘要替换后，真实 usage 基线的索引锚点（ctxAtMessage）错位：清除基线，
    // 压缩判定回退估算，直至下一次真实模型调用重建基线（DESIGN「上下文保护」）
    session.ctxInputTokens = undefined
    session.ctxCachedTokens = undefined
    session.ctxAtMessage = undefined
    await this.save(session)
    return session.messages
  }

  async getTodos(sessionId: string, userId?: string): Promise<TodoItem[]> {
    const session = await this.load(sessionId, userId)
    return session?.todos ?? []
  }

  async setTodos(sessionId: string, todos: TodoItem[], userId?: string): Promise<void> {
    const session = await this.load(sessionId, userId)
    if (!session) throw new Error(`session not found: ${sessionId}`)
    session.todos = todos
    await this.save(session)
  }

  async rename(sessionId: string, name: string, userId?: string): Promise<void> {
    const session = await this.load(sessionId, userId)
    if (!session) throw new Error(`session not found: ${sessionId}`)
    session.name = name
    await this.save(session)
  }

  async delete(sessionId: string, userId?: string): Promise<void> {
    this.evict(sessionId)
    this.owners.delete(sessionId)
    const session = await this.load(sessionId, userId)
    if (!session) return
    const dir = this.dir(session.userId, sessionId)
    await rm(dir, { recursive: true, force: true })
  }

  getTmpDir(sessionId: string, userId: string): string {
    return join(this.dir(userId, sessionId), "tmp")
  }

  /** 失效会话缓存（GC 归档/外部移动会话目录后调用，防止缓存命中后 save() 重建目录）。 */
  evict(sessionId: string): void {
    this.cache.delete(sessionId)
    this.envCache.delete(sessionId)
  }

  /** 会话根目录（工具产物与附件统一以它为基准）。 */
  getSessionDir(sessionId: string, userId: string): string {
    return this.dir(userId, sessionId)
  }

  /** 会话 tmp/ 子树文件列表（DESIGN「会话临时文件查看与下载」：文件操作严格限定在会话 tmp/ 内，
   *  chat.json/env.json/todo.json 等会话数据文件不暴露）；保留 `tmp/` 前缀与既有路径语义一致。 */
  async listSessionFiles(sessionId: string, userId: string): Promise<FileEntry[]> {
    const root = join(this.dir(userId, sessionId), "tmp")
    const out: FileEntry[] = []
    await this.walkFiles(root, "tmp", out)
    return out
  }

  /** 文件接口路径解析：以会话 tmp/ 为根（仅所有者可访问，配合沙箱拒绝 ../、绝对路径、符号链接）；
   *  兼容 `tmp/` 前缀路径（旧附件/截断引用路径）。返回安全绝对路径。 */
  resolveSessionTmpFile(sessionId: string, userId: string, path: string, sandboxEnabled: boolean): string {
    const tmp = join(this.dir(userId, sessionId), "tmp")
    const p = path.startsWith("tmp/") ? path.slice(4) : path
    if (sandboxEnabled) return resolveInSandbox(tmp, p)
    return resolve(tmp, p)
  }

  /** 文件预览路径解析（files/preview 接口，DESIGN「文件链接弹窗查看」）：相对路径（含 `tmp/` 前缀）
   *  与 resolveSessionTmpFile 同规则（会话 tmp/ 为根）；**绝对路径**（code 项目文件等）
   *  按用户隔离边界放行——沙箱用户仅允许本用户数据目录（users/{user}/，与文件工具 project 参数经
   *  resolveInSandbox(root=users/{user}) 的可达范围一致，含符号链接逃逸检查），非沙箱（本地模式操作者
   *  本人，与文件工具能力对齐）放开绝对路径。 */
  resolvePreviewFile(sessionId: string, userId: string, path: string, sandboxEnabled: boolean): string {
    if (isAbsolute(path)) {
      const abs = resolve(path)
      if (!sandboxEnabled) return abs
      const userRoot = join(this.opts.home, "users", userId)
      const rel = relative(userRoot, abs)
      if (!rel || rel.startsWith("..") || isAbsolute(rel)) throw new Error(`path outside user data: ${path}`)
      assertNoSymlinkEscape(userRoot, abs, path)
      return abs
    }
    return this.resolveSessionTmpFile(sessionId, userId, path, sandboxEnabled)
  }

  private async walkFiles(dir: string, prefix: string, out: FileEntry[]) {
    let entries
    try {
      entries = await readdir(dir, { withFileTypes: true })
    } catch {
      return
    }
    for (const e of entries) {
      const full = join(dir, e.name)
      const rel = prefix ? `${prefix}/${e.name}` : e.name
      if (e.isDirectory()) {
        out.push({ path: rel, size: 0, modifiedAt: 0, isDir: true })
        await this.walkFiles(full, rel, out)
      } else {
        try {
          const st = await stat(full)
          out.push({ path: rel, size: st.size, modifiedAt: st.mtimeMs, isDir: false })
        } catch {
          /* ignore */
        }
      }
    }
  }

  /** 会话 env（**纯内存，零落盘**——用户环境变量只存浏览器本地，服务端不留存）：
   *  进程重启即空，前端每次加载会话时自行同步所需键（applyApprovalSkip）；
   *  首次触达惰性清理历史版本落盘的 env.json（迁移，删除失败忽略）。 */
  async getEnv(sessionId: string, userId: string): Promise<Record<string, string>> {
    const dir = this.dir(userId, sessionId)
    if (this.envCache.has(sessionId)) {
      // LRU：命中刷新位置，超限时淘汰最久未用（长生命周期进程防无界增长）
      const hit = this.envCache.get(sessionId)!
      this.envCache.delete(sessionId)
      this.envCache.set(sessionId, hit)
      return hit
    }
    if (this.envCache.size >= MAX_ENV_CACHE_SESSIONS) {
      const oldest = this.envCache.keys().next().value
      if (oldest !== undefined) this.envCache.delete(oldest)
    }
    this.envCache.set(sessionId, {})
    try {
      await unlink(join(dir, "env.json"))
    } catch {
      /* 不存在（常态）或已清理 */
    }
    return {}
  }

  async setEnv(sessionId: string, userId: string, vars: Record<string, string | null>): Promise<Record<string, string>> {
    const current = await this.getEnv(sessionId, userId)
    for (const [k, v] of Object.entries(vars)) {
      if (v === null) delete current[k]
      else current[k] = v
    }
    this.envCache.set(sessionId, current)
    return current
  }

  private touchCache(session: SessionData) {
    this.cache.delete(session.id)
    this.cache.set(session.id, session)
    if (session.userId) {
      this.owners.delete(session.id)
      this.owners.set(session.id, session.userId)
    }
    if (this.cache.size > MAX_CACHE_SESSIONS) {
      const first = this.cache.keys().next().value
      if (first !== undefined) this.cache.delete(first)
    }
    // 归属记录上限保护：防止无界增长（活动会话规模远小于上限，超限仅影响归属兜底命中率）
    if (this.owners.size > 10_000) this.owners.clear()
  }
}
