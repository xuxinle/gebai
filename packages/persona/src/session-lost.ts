/**
 * 会话丢失（"这个 sessionId 在存储里已经不存在了"）。
 *
 * ## 为什么需要单独的错误类型
 *
 * 事件投递链上有三层各自持有一份 `sessionId`：运行时的解析缓存（TTL 30 分钟）、
 * 调度器的队列键、引擎的 `store.load`。三层中只有最后一层会**真正**校验存活，
 * 前两层只是把上一次解析到的 id 记着。
 *
 * 于是「会话被删除 / 被 GC 归档 / 磁盘上 chat.json 丢了」这件事会以一条普通
 * `Error("会话不存在: xxx")` 的形式从引擎抛回来，落到调度器的通用 catch 分支里——
 * 那里把它当成**毒事件**处理：按原 id 放回队首、烧掉一次重试、三次之后丢弃。
 * 结果是缓存过期前（最长 30 分钟）这个分身的每一条事件都这样死一遍，
 * 表面症状是「这个分身从此哑了」，日志里只有几条 warn。
 *
 * 用类型把这件事标出来，调度器才能走**自愈**路径：让宿主持有的那两层重新解析，
 * 把在途与待跑事件改投新会话，且**不消耗重试次数**（它不是毒事件）。
 */

/** 会话已不在存储中（删除 / GC 归档 / chat.json 丢失）。 */
export class SessionLostError extends Error {
  /** 丢失的会话 id。 */
  readonly sessionId: string
  /** 底层原因（引擎侧的原始错误或说明）。 */
  readonly reason?: string

  constructor(sessionId: string, opts: { reason?: string; cause?: unknown } = {}) {
    super(`会话不存在: ${sessionId}${opts.reason ? `（${opts.reason}）` : ""}`, opts.cause !== undefined ? { cause: opts.cause } : undefined)
    this.name = "SessionLostError"
    this.sessionId = sessionId
    if (opts.reason) this.reason = opts.reason
  }
}

/** 判定是否会话丢失错误（跨包/跨 realm 也认：先看 name，再看 instanceof）。 */
export function isSessionLostError(err: unknown): err is SessionLostError {
  if (err instanceof SessionLostError) return true
  if (!err || typeof err !== "object") return false
  const e = err as { name?: unknown; sessionId?: unknown }
  return e.name === "SessionLostError" && typeof e.sessionId === "string"
}

/**
 * 把「引擎侧说会话没了」归一成 `SessionLostError`。
 *
 * 引擎不认识本包（它是下层），所以只能给出 `code` 或一句中文文案；
 * 这里把两种都认下来，避免为了传达一个语义而让引擎反向依赖 persona。
 *
 * **只认开启轮次时的那一种**（引擎 `run()` 准备阶段的 `会话不存在: <id>`）。
 * 不认存储层运行中那句 `session not found: <id>`：那是**跑到一半会话被删**，
 * 此时模型可能已经发过消息/改过文件，重跑一轮会重复对外动作。
 * 那种情况让它按普通失败处理（下一轮准备阶段就会报会话不存在，届时再自愈），
 * 自愈仍会收敛，但不会把已完成的对外动作做第二遍。
 */
export function toSessionLostError(sessionId: string, err: unknown): SessionLostError | null {
  if (isSessionLostError(err)) return err
  const e = (err ?? {}) as { code?: unknown; message?: unknown }
  const code = typeof e.code === "string" ? e.code : ""
  const message = typeof e.message === "string" ? e.message : String(err ?? "")
  const hit = code === "SESSION_NOT_FOUND" || /会话不存在/.test(message)
  return hit ? new SessionLostError(sessionId, { reason: message || undefined, cause: err }) : null
}
