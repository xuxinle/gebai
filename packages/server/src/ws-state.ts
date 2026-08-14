import { appendFileSync, mkdirSync, readFileSync, writeFileSync } from "node:fs"
import { join, dirname } from "node:path"
import type { AgentEvent, SessionInfo } from "@gebai/sdk"
import type { AppDeps } from "./app"
import type { WsConn, WsSink } from "./ws"
import { toSessionInfo } from "./core/store"

/**
 * WS 状态服务（MVC 模型层）：把连接级状态从传输层（ws.data）提升为
 * 服务端权威状态，断线/重连/重启后自动恢复，客户端重连后经快照/重放收敛：
 *
 * - 每用户事件日志：全局递增 seq + 有界环形缓冲，断线期间的事件不丢失，
 *   重连后按 seq 重放（sync.request → sync.replay / sync.overrun）；
 *   尾部条目持久化到 users/{user}/ws-journal.jsonl（服务重启后 seq 连续、
 *   断线×重启重叠窗口内的事件仍可重放）
 * - 文本增量（event.message.delta）按消息合并后入日志（50ms 窗口，非增量事件立即冲刷），
 *   避免逐 chunk 高频事件冲掉日志缓冲（稍长断线即 overrun 降级全量重同步）
 * - 每用户连接状态（当前会话等）：持久化到 {GEBAI_HOME}/conn-state.json，
 *   新连接/重启后自动恢复
 * - 状态快照（state.snapshot）：连接建立/登录后自动推送，客户端作为模型基线
 */

/** 单条日志事件（发送到线格式：type/seq/sessionId/payload/timestamp）。 */
export interface JournalEntry {
  seq: number
  type: string
  sessionId: string
  payload: Record<string, unknown>
  timestamp: number
}

/** 每用户事件日志容量：超出后最旧事件被丢弃（重放出现缺口 → 客户端走全量重同步）。 */
const JOURNAL_CAP = 1000
/** delta 合并窗口（毫秒）：同一消息的连续文本增量合并为一条日志事件。 */
const DELTA_MERGE_MS = 50
/** 合并中的 delta 累计文本上限（字符）：防止极端流式输出单条事件无限膨胀。 */
const DELTA_MERGE_MAX_CHARS = 200_000
/** 持久化文件重写周期（追加条数）：定期裁剪回尾部 cap 条，防文件无限增长。 */
const JOURNAL_REWRITE_EVERY = 2000

/** 每用户事件日志：分配递增 seq 的有界环形缓冲（日志为 seq 权威来源，与在线推送同源）。
 *  file 提供时尾部条目持久化（JSONL 追加），构造时同步加载尾部并延续 seq（服务重启不掉线补偿）。 */
export class UserJournal {
  private entries: JournalEntry[] = []
  private next = 1
  private file?: string
  private appended = 0
  private persisted = false

  constructor(private cap = JOURNAL_CAP, file?: string) {
    if (!file) return
    this.file = file
    this.persisted = true // 无论加载成败都启用持久化（首次运行文件不存在仍要写）
    try {
      const lines = readFileSync(file, "utf8").split("\n").filter(Boolean)
      const tail: JournalEntry[] = []
      for (const line of lines) {
        try {
          const e = JSON.parse(line) as JournalEntry
          if (e && typeof e.seq === "number" && typeof e.type === "string" && e.payload && typeof e.sessionId === "string") tail.push(e)
        } catch {
          /* 损坏行跳过 */
        }
      }
      const kept = tail.slice(-cap)
      this.entries = kept
      this.next = (kept[kept.length - 1]?.seq ?? 0) + 1
      // 加载即重写裁剪（长时间运行后的文件回归 cap 条）
      this.rewriteFile(kept)
    } catch {
      /* 首次运行/文件不可读：空日志 */
    }
  }

  /** 记录事件并返回带 seq 的条目（payload 已注入 sessionId，与在线推送一致）。 */
  append(e: AgentEvent): JournalEntry {
    const entry: JournalEntry = {
      seq: this.next++,
      type: e.type,
      sessionId: e.sessionId,
      payload: { ...e.payload, sessionId: e.sessionId },
      timestamp: e.timestamp,
    }
    this.entries.push(entry)
    if (this.entries.length > this.cap) this.entries.splice(0, this.entries.length - this.cap)
    this.persist(entry)
    return entry
  }

  /** 已发放的最大 seq（快照基线/新连接起点）。 */
  lastSeq(): number {
    return this.next - 1
  }

  /**
   * 重放 afterSeq 之后的事件（客户端离线期间错过的）。
   * 返回 null 表示缺口（缓冲溢出，最旧事件已被丢弃），调用方应走全量重同步。
   */
  replay(afterSeq: number): { entries: JournalEntry[]; lastSeq: number } | null {
    const start = this.entries[0]?.seq ?? this.next
    if (afterSeq < start - 1) return null
    return { entries: this.entries.filter((x) => x.seq > afterSeq), lastSeq: this.next - 1 }
  }

  private persist(entry: JournalEntry): void {
    if (!this.file || !this.persisted) return
    try {
      mkdirSync(dirname(this.file), { recursive: true })
      appendFileSync(this.file, `${JSON.stringify(entry)}\n`)
      this.appended++
      if (this.appended >= JOURNAL_REWRITE_EVERY) {
        this.appended = 0
        this.rewriteFile(this.entries)
      }
    } catch {
      /* 写失败静默：内存日志仍可用，仅重启丢失 */
    }
  }

  private rewriteFile(list: JournalEntry[]): void {
    if (!this.file) return
    try {
      mkdirSync(dirname(this.file), { recursive: true })
      writeFileSync(this.file, list.map((e) => JSON.stringify(e)).join("\n") + (list.length ? "\n" : ""), "utf8")
    } catch {
      /* 写失败静默 */
    }
  }
}

/** 每用户连接状态：跨连接/重启保持，重连后自动恢复（当前会话等）。 */
export class ConnectionState {
  private state = new Map<string, { currentSessionId?: string }>()
  private saveTimer: ReturnType<typeof setTimeout> | null = null

  constructor(private file: string) {
    try {
      const raw = JSON.parse(readFileSync(file, "utf8")) as Record<string, { currentSessionId?: string }>
      for (const [uid, v] of Object.entries(raw)) {
        if (v && typeof v === "object") this.state.set(uid, v)
      }
    } catch {
      /* 首次运行/文件损坏：空状态 */
    }
  }

  getCurrent(userId: string): string | undefined {
    return this.state.get(userId)?.currentSessionId
  }

  setCurrent(userId: string, id: string | undefined): void {
    const cur = this.state.get(userId) ?? {}
    if (id === undefined) delete cur.currentSessionId
    else cur.currentSessionId = id
    this.state.set(userId, cur)
    this.scheduleSave()
  }

  /** 立即落盘（进程退出/测试断言用）。 */
  flush(): void {
    if (this.saveTimer) {
      clearTimeout(this.saveTimer)
      this.saveTimer = null
    }
    try {
      mkdirSync(dirname(this.file), { recursive: true })
      writeFileSync(this.file, JSON.stringify(Object.fromEntries(this.state)), "utf8")
    } catch {
      /* 写失败静默：状态仍存内存，仅重启丢失 */
    }
  }

  private scheduleSave(): void {
    if (this.saveTimer) return
    this.saveTimer = setTimeout(() => {
      this.saveTimer = null
      this.flush()
    }, 300)
  }
}

/** WS 状态快照（state.snapshot payload）：重连后客户端模型收敛基线。 */
export interface WsSnapshot {
  currentSessionId: string | null
  sessions: SessionInfo[]
  running: string[]
  lastSeq: number
  /** 模型上下文窗口（token）：0=未知/未配置，标题栏上下文占比显示用。 */
  maxContextTokens?: number
}

/**
 * WS 状态服务：唯一的事件总线订阅点（按会话归属路由到所属用户的日志），
 * 各连接注册监听回调接收带 seq 的日志条目（在线推送 = 日志条目的实时投递）。
 */
interface PendingDelta {
  sessionId: string
  messageId?: string
  sessionRunId?: string
  sessionMark: boolean
  text: string
  timestamp: number
  timer: ReturnType<typeof setTimeout>
}

export class WsStateService {
  private journals = new Map<string, UserJournal>()
  private listeners = new Map<string, Set<(e: JournalEntry) => void>>()
  private chains = new Map<string, Promise<void>>()
  /** 每用户合并中的文本增量（delta 合并窗口内的暂存，冲刷时作为单条日志事件入列）。 */
  private pendingDeltas = new Map<string, PendingDelta>()
  readonly connState: ConnectionState

  constructor(private home: string, private d: AppDeps) {
    this.connState = new ConnectionState(join(home, "conn-state.json"))
    // 服务端级单点订阅：与连接数无关，断线期间事件照样入日志（重连后可重放）
    d.events.subscribe((ev) => this.onEvent(ev))
  }

  journal(userId: string): UserJournal {
    let j = this.journals.get(userId)
    if (!j) {
      // 持久化日志：尾部条目落盘 users/{user}/ws-journal.jsonl，重启后 seq 连续可重放
      j = new UserJournal(JOURNAL_CAP, join(this.home, "users", userId, "ws-journal.jsonl"))
      this.journals.set(userId, j)
    }
    return j
  }

  /** 订阅某用户的事件（在线推送）；返回退订函数。 */
  subscribe(userId: string, cb: (e: JournalEntry) => void): () => void {
    let set = this.listeners.get(userId)
    if (!set) {
      set = new Set()
      this.listeners.set(userId, set)
    }
    set.add(cb)
    return () => {
      set?.delete(cb)
    }
  }

  /** 处理中的会话链数量（泄漏回归测试用）。 */
  pendingChainCount(): number {
    return this.chains.size
  }

  private onEvent(ev: AgentEvent): void {
    if (!ev.sessionId) return
    // 同会话事件按序处理（异步归属解析不交错）；不同会话相互独立
    const prev = this.chains.get(ev.sessionId) ?? Promise.resolve()
    const p = prev.then(() => this.route(ev)).catch(() => {})
    this.chains.set(ev.sessionId, p)
    // 链终结后清理（会话事件不是常态流量，Map 常驻即泄漏：闭包引用事件与上下文）
    void p.finally(() => {
      if (this.chains.get(ev.sessionId) === p) this.chains.delete(ev.sessionId)
    })
  }

  private async route(ev: AgentEvent): Promise<void> {
    // 快速路径：内存缓存命中；慢路径：磁盘读取会话归属
    let uid = this.d.store.ownerOf(ev.sessionId)
    if (!uid) {
      const s = await this.d.store.load(ev.sessionId)
      uid = s?.userId ?? null
    }
    if (!uid) return
    // 文本增量合并：同一消息的连续 delta 在窗口内合并为一条（下一个非增量事件先冲刷再入列，
    // 保证用户视角顺序不变）；其余事件直接入列
    if (ev.type === "event.message.delta") {
      const cur = this.pendingDeltas.get(uid)
      const sameStream =
        cur &&
        cur.sessionId === ev.sessionId &&
        cur.messageId === ev.payload.messageId &&
        cur.sessionRunId === ev.payload.sessionRunId &&
        cur.sessionMark === (ev.payload.session === true)
      if (sameStream && cur) {
        cur.text = (cur.text + String(ev.payload.text ?? "")).slice(0, DELTA_MERGE_MAX_CHARS)
        return
      }
      this.flushDelta(uid)
      const pending: PendingDelta = {
        sessionId: ev.sessionId,
        messageId: ev.payload.messageId as string | undefined,
        sessionRunId: ev.payload.sessionRunId as string | undefined,
        sessionMark: ev.payload.session === true,
        text: String(ev.payload.text ?? ""),
        timestamp: ev.timestamp,
        timer: setTimeout(() => this.flushDelta(uid), DELTA_MERGE_MS),
      }
      this.pendingDeltas.set(uid, pending)
      return
    }
    this.flushDelta(uid)
    this.appendAndNotify(uid, ev)
  }

  /** 冲刷合并中的文本增量（作为单条 delta 事件入日志并推送）。 */
  private flushDelta(uid: string): void {
    const pending = this.pendingDeltas.get(uid)
    if (!pending) return
    this.pendingDeltas.delete(uid)
    clearTimeout(pending.timer)
    if (!pending.text) return
    const payload: Record<string, unknown> = { text: pending.text }
    if (pending.messageId !== undefined) payload.messageId = pending.messageId
    if (pending.sessionMark) payload.session = true
    if (pending.sessionRunId !== undefined) payload.sessionRunId = pending.sessionRunId
    this.appendAndNotify(uid, { type: "event.message.delta", sessionId: pending.sessionId, payload, timestamp: pending.timestamp })
  }

  private appendAndNotify(uid: string, ev: AgentEvent): void {
    const entry = this.journal(uid).append(ev)
    const ls = this.listeners.get(uid)
    if (ls) {
      for (const cb of ls) {
        try {
          cb(entry)
        } catch {
          /* 单个连接异常不影响其余连接 */
        }
      }
    }
  }

  /** 构建状态快照（连接级当前会话 + 会话列表 + 运行中会话 + 日志基线 seq）。 */
  async buildSnapshot(conn: WsConn): Promise<WsSnapshot> {
    const user = conn.get()
    const [sessions, running] = await Promise.all([this.d.store.listSessions(user.id), this.d.engine.runningIds(user.id)])
    return {
      currentSessionId: conn.getCurrent() ?? null,
      sessions: sessions.map(toSessionInfo),
      running,
      lastSeq: this.journal(user.id).lastSeq(),
      maxContextTokens: this.d.engine.contextWindow(),
    }
  }

  /** 推送状态快照到指定连接（连接建立/登录/登出后调用）。 */
  async pushSnapshot(ws: WsSink, conn: WsConn): Promise<void> {
    const snap = await this.buildSnapshot(conn)
    ws.send(JSON.stringify({ type: "state.snapshot", payload: snap }))
  }
}
