/**
 * 自发送消息记账（outbox registry）：登记「本进程刚发出去的消息 id」，
 * 供入站侧（长连接回调）过滤——精确挡掉自己发出的消息回灌，不误伤他人。
 *
 * ## 为什么需要（背景）
 *
 * 人格以 `act(verb:"send", as_user:true)` 发消息时，飞书群里会出现一条
 * **以用户名义**发出的消息，长连接会把它当普通入站推回来（`sender_type=user`，
 * 与用户本人发言无法从事件字段区分——机器人名义的 `sender_type=app` 才能被
 * `IGNORE_SELF` 挡住）。结果：自己说的话又被人格当「新消息」处理一轮
 * （典型现象是人格对「复述」再做一次无意义回应，浪费轮次与 token）。
 *
 * 精确解法：act 成功后把返回的 `message_id` 记进本表；入站回调先查表，
 * 命中即丢弃。只挡**自己发的那条**，用户本人的同名消息不受影响。
 *
 * ## 为什么是通道层而不是会话/引擎层
 *
 * 这是「外部渠道收发的自指问题」，不涉及会话上下文/引擎历史——放通道层即可，
 * 引擎与会话零改动。
 *
 * ## 实现取舍
 *
 * - **有界 LRU + TTL 双保险**：容量上限防进程长跑内存膨胀；TTL 让过期的
 *   id 自然失效（飞书重投/重连补推通常发生在秒~分钟级，10 分钟足够覆盖）。
 * - **不落盘**：进程重启后清空是安全的——重启后旧的入站事件（若有）也早已
 *   不在投递窗口；且「关掉进程自己发的消息仍不被过滤」的可能性远小于
 *   「落盘增加复杂性与 I/O」。
 * - 线程安全（单进程事件循环天然串行，无需锁）。
 */
export class SentMessageRegistry {
  /** id → 登记时刻（Map 迭代序 = 插入序，LRU 靠「删后重插」维护）。 */
  private readonly seen = new Map<string, number>()

  constructor(
    private readonly capacity = 1024,
    private readonly ttlMs = 10 * 60_000,
  ) {
    if (!Number.isInteger(capacity) || capacity < 1) throw new Error(`SentMessageRegistry capacity 必须是正整数: ${capacity}`)
    if (!Number.isFinite(ttlMs) || ttlMs <= 0) throw new Error(`SentMessageRegistry ttlMs 必须是正数: ${ttlMs}`)
  }

  /** 登记一条已发送消息 id（幂等：重复登记只刷新时间戳）。 */
  record(id: string, now = Date.now()): void {
    if (!id) return
    // 先删后插：刷新 LRU 位置（同 dedupe.ts 的手法）
    if (this.seen.has(id)) this.seen.delete(id)
    this.seen.set(id, now)
    // 容量上限：驱逐最旧
    if (this.seen.size > this.capacity) {
      const oldest = this.seen.keys().next().value
      if (oldest !== undefined) this.seen.delete(oldest)
    }
  }

  /**
   * 命中判定：近期发过 → true（入站应丢弃）。
   *
   * 顺带惰性清理过期条目（只在查询路径上清，避免单独定时器）。
   */
  hit(id: string, now = Date.now()): boolean {
    if (!id) return false
    const t = this.seen.get(id)
    if (t === undefined) return false
    if (now - t > this.ttlMs) {
      this.seen.delete(id)
      return false
    }
    return true
  }

  /** 当前登记的 id 数（诊断用）。 */
  size(): number {
    return this.seen.size
  }

  /** 清空（测试用）。 */
  clear(): void {
    this.seen.clear()
  }
}