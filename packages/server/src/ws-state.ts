import { mkdirSync, readFileSync, writeFileSync } from "node:fs"
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
 *   重连后按 seq 重放（sync.request → sync.replay / sync.overrun）
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

/** 每用户事件日志：分配递增 seq 的有界环形缓冲（日志为 seq 权威来源，与在线推送同源）。 */
export class UserJournal {
  private entries: JournalEntry[] = []
  private next = 1

  constructor(private cap = JOURNAL_CAP) {}

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
export class WsStateService {
  private journals = new Map<string, UserJournal>()
  private listeners = new Map<string, Set<(e: JournalEntry) => void>>()
  private chains = new Map<string, Promise<void>>()
  readonly connState: ConnectionState

  constructor(home: string, private d: AppDeps) {
    this.connState = new ConnectionState(join(home, "conn-state.json"))
    // 服务端级单点订阅：与连接数无关，断线期间事件照样入日志（重连后可重放）
    d.events.subscribe((ev) => this.onEvent(ev))
  }

  journal(userId: string): UserJournal {
    let j = this.journals.get(userId)
    if (!j) {
      j = new UserJournal()
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

  private onEvent(ev: AgentEvent): void {
    if (!ev.sessionId) return
    // 同会话事件按序处理（异步归属解析不交错）；不同会话相互独立
    const prev = this.chains.get(ev.sessionId) ?? Promise.resolve()
    const p = prev.then(() => this.route(ev)).catch(() => {})
    this.chains.set(ev.sessionId, p)
  }

  private async route(ev: AgentEvent): Promise<void> {
    // 快速路径：内存缓存命中；慢路径：磁盘读取会话归属
    let uid = this.d.store.ownerOf(ev.sessionId)
    if (!uid) {
      const s = await this.d.store.load(ev.sessionId)
      uid = s?.userId ?? null
    }
    if (!uid) return
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
