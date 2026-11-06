/**
 * WS 消息队列（lane）划分：把一条连接上的消息按**领域**拆到独立队列，各队列内部严格保序、
 * 队列之间真并发。
 *
 * ## 为什么需要（原来的单链问题）
 * Bun 对 async `websocket.message` 处理器**不保证串行**——前一条 await 磁盘/网络 I/O 时下一条
 * 即并发进入。最初的修法是「每条连接一条 promise 链，所有消息排成一队」，它解决了认证顺序，
 * 但代价是**全连接串行**：
 * - 终端输入（`term.input`，每次按键一条）与 LSP `change`（每次编辑一条）是**高频小消息**，
 *   它们排在队里时，紧随其后的 `session.get` / `state.snapshot` 只能干等；
 * - 一次 `session.prompt` 的启动要读盘、解析会话（几十~几百毫秒），期间用户在同一条连接上
 *   点开别的会话、拉快照、敲终端，全被堵住——表现为「点哪都没反应，过一会儿一起出来」。
 *
 * ## 现在的语义
 * - **同 lane 严格保序**：认证、会话生命周期、同一条终端的按键顺序，这些语义不能乱；
 * - **跨 lane 真并发**：终端输出/按键不再阻塞会话操作，LSP 编辑不再阻塞终端；
 * - **auth 屏障**：`auth.login` / `auth.logout` 属于 `auth` lane，且**其后所有 lane 提交的消息
 *   都要等它完成**——否则「登录后立刻发的请求」会以未登录身份被拒（这条是当初单链要解决的问题，
 *   拆队时必须原样保住）；
 * - **队列深度护栏**：单 lane 积压超过阈值即拒绝该消息（回 `queue_full`），不无限堆内存。
 *   慢消费只会挡自己的 lane，不会把整条连接拖死。
 *
 * 纯函数部分（`laneOf` / `laneQueue`）独立可测；`WsLanes` 只管调度，不碰协议细节。
 */

/** 消息队列标识（领域）。 */
export type WsLane = "auth" | "session" | "prompt" | "term" | "lsp" | "interaction" | "admin" | "default"

/**
 * 消息类型 → 所属队列。
 *
 * 划分口径（按「乱序会造成什么后果」而非按代码目录）：
 * - `auth`：登录/登出。**必须**先于其后一切消息生效（屏障语义见模块注释）；
 * - `session`：会话生命周期（创建/删除/改名/切换/压缩/读消息/env/文件/附件上传）。同一会话的
 *   「先切后读」必须有序；不同会话之间其实可以并发，但以会话 id 再分裂队列的收益不足以抵消
 *   复杂度（切会话本身很轻），这里保持一个队列，保序优先；
 * - `prompt`：发起任务/取消。与 session 分开——启动任务要读历史+组装上下文（最慢的一步），
 *   不能让它压住用户在别处的会话操作；
 * - `term`：终端按键/尺寸/开关。**每次按键一条**，绝不能被别的域挡住（否则打字卡顿）；
 * - `lsp`：编辑器补全/跳转请求。**每次编辑一条**，同上；
 * - `interaction`：审批/选择/环境变量回答/前端渲染结果。用户点一下就要立刻生效；
 * - `admin`：用户管理/反馈/子Agent 装载。低频、重操作，单独一队避免拖累用户操作；
 * - 其余（`state.snapshot`/`sync.request`/`ping`/`session.list`/未知）→ `default`。
 */
const LANE_BY_TYPE: Record<string, WsLane> = {
  "auth.login": "auth",
  "auth.logout": "auth",

  "session.list": "session",
  "session.create": "session",
  "session.import": "session",
  "session.get": "session",
  "session.delete": "session",
  "session.rename": "session",
  "session.pin": "session",
  "session.model_config.set": "session",
  "session.switch": "session",
  "session.current": "session",
  "session.compact": "session",
  "session.restore": "session",
  "session.attach": "session",
  "session.todo.get": "session",
  "session.env.get": "session",
  "session.env.set": "session",
  "session.tool.get": "session",
  "session.files.list": "session",
  "session.files.get": "session",
  "session.attachment.upload": "session",

  "session.prompt": "prompt",
  "session.cancel": "prompt",

  "term.open": "term",
  "term.attach": "term",
  "term.input": "term",
  "term.resize": "term",
  "term.interrupt": "term",
  "term.close": "term",
  "term.list": "term",

  "lsp.open": "lsp",
  "lsp.change": "lsp",
  "lsp.save": "lsp",
  "lsp.close": "lsp",
  "lsp.request": "lsp",

  "approval.decide": "interaction",
  "choice.decide": "interaction",
  "env.decide": "interaction",
  "draw.result": "interaction",
  "capture.result": "interaction",

  "user.list": "admin",
  "user.create": "admin",
  "user.update": "admin",
  "user.delete": "admin",
  "feedback.submit": "admin",
  "feedback.list": "admin",
  "sub_agent.list": "admin",
  "sub_agent.load": "admin",
  "sub_agent.unload": "admin",
}

/** 查消息类型所属队列（未知类型归 default：仍按序，不阻塞其余域）。 */
export function laneOf(type: string): WsLane {
  return LANE_BY_TYPE[type] ?? "default"
}

/** 所有 lane（测试与统计用）。 */
export const ALL_LANES: WsLane[] = ["auth", "session", "prompt", "term", "lsp", "interaction", "admin", "default"]

/** 单 lane 队列深度上限：超过即拒绝新消息（回 queue_full），不无限堆内存。
 *  取值权衡：终端按键与 LSP 编辑都是人操作驱动，正常几十条/秒；64 已远超真实输入速率，
 *  超过说明消费者（磁盘/网络）卡了，此时继续收只会让积压更深、延迟更久。 */
export const LANE_MAX_DEPTH = 64

/** 一个 lane 的运行时状态：一条串行链 + 当前排队数。 */
interface LaneState {
  /** 队尾（新任务接在它后面）。 */
  tail: Promise<void>
  /** 已提交未完成的消息数（含正在跑的那条）。 */
  depth: number
}

/**
 * 每连接的 lane 调度器。
 *
 * 用法：`submit(type, fn)` —— 按 type 找到 lane，排在该 lane 队尾；返回的 promise 在该消息
 * 处理完成（或跳过）后 resolve。`waitAll()` 用于测试与优雅关闭（等所有 lane 排空）。
 */
export class WsLanes {
  private lanes = new Map<WsLane, LaneState>()
  /** auth 屏障：认证消息完成后 resolve；后续提交的（非 auth）消息先等它。 */
  private authGate: Promise<void> = Promise.resolve()
  /** 被队列深度护栏拒绝的次数（按 lane 统计，供日志与观测）。 */
  private readonly rejected = new Map<WsLane, number>()

  constructor(private readonly maxDepth = LANE_MAX_DEPTH) {}

  /** 某 lane 当前排队深度（含正在跑的）。 */
  depthOf(lane: WsLane): number {
    return this.lanes.get(lane)?.depth ?? 0
  }

  /** 被拒绝次数（按 lane）。 */
  rejectedOf(lane: WsLane): number {
    return this.rejected.get(lane) ?? 0
  }

  /** 所有 lane 的总排队深度。 */
  totalDepth(): number {
    let n = 0
    for (const s of this.lanes.values()) n += s.depth
    return n
  }

  /**
   * 提交一条消息处理。
   * - 按 `type` 归 lane；同 lane 串行，跨 lane 并发；
   * - 非 auth lane 的消息先等 auth 屏障（保证「刚登录就发的请求」看到已登录身份）；
   * - 该 lane 已积压超过上限 → 不排队，`onRejected` 被调用（调用方回 queue_full）。
   *
   * 返回的 promise 恒 resolve（处理异常由 `run` 自己兜住），便于调用方 `await` 而不炸。
   */
  submit(type: string, run: () => Promise<void> | void, onRejected?: () => void): Promise<void> {
    const lane = laneOf(type)
    const state = this.lanes.get(lane) ?? { tail: Promise.resolve(), depth: 0 }
    if (state.depth >= this.maxDepth) {
      this.rejected.set(lane, (this.rejected.get(lane) ?? 0) + 1)
      onRejected?.()
      return Promise.resolve()
    }
    state.depth++
    // **同 lane 串行链**：接在该 lane 的队尾后面（不是接在屏障后面）——
    // 只接屏障的话，同一 lane 的多条消息会并行跑，“保序”就成了空话。
    // 屏障只用于**跨越 auth 边界**：非 auth lane 的队首要先等认证落地。
    const gate = lane === "auth" ? state.tail : Promise.all([state.tail, this.authGate]).then(() => {})
    const task = gate
      .then(() => run())
      .catch(() => {})
      .finally(() => {
        state.depth--
      })
    state.tail = task
    this.lanes.set(lane, state)
    if (lane === "auth") this.authGate = task
    return task
  }

  /** 等待全部 lane 排空（测试/关闭用）。 */
  async waitAll(): Promise<void> {
    // 反复等一轮：等待期间可能又提交了新消息（tail 换了新的 promise）
    for (let i = 0; i < 1000; i++) {
      const tails = [...this.lanes.values()].map((s) => s.tail)
      if (!tails.length) return
      await Promise.all(tails)
      if (this.totalDepth() === 0 && [...this.lanes.values()].every((s) => s.depth === 0)) return
    }
  }
}
