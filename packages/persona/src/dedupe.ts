/**
 * 幂等去重环：同一事件 id 只处理一次。
 *
 * 为什么必须有：每个驱动源都会重投——飞书在 ACK 前断连会重推同一 `event_id`，
 * 调度器重启会重新触发同一时刻的任务，webhook 按自身策略重试。重复事件 = 重复副作用
 * （重复发消息、重复提交单据），这是真实事故形态而非理论风险。
 *
 * 实现取向：有界 LRU（容量可配，默认 1024）。不落盘——进程重启后允许重放，
 * 因为「重启」本身意味着上一轮未完成，重放比静默丢失更安全；跨重启的严格幂等
 * 应由驱动源自己的 id + 下游操作幂等保证，不靠这一层。
 */
export class EventDedupe {
  private seen = new Map<string, number>()

  constructor(private readonly capacity = 1024) {
    if (!Number.isInteger(capacity) || capacity < 1) throw new Error(`EventDedupe capacity 必须是正整数: ${capacity}`)
  }

  /**
   * 登记并判定：首次见到返回 `true`（应处理），重复返回 `false`（应丢弃）。
   *
   * 命中已有 id 时**刷新其位置**（Map 迭代序即插入序，删后再插 = 移到最新），
   * 这样真正热的 id 不会被一轮冷事件冲掉，而长期不来的自然淘汰。
   */
  admit(id: string): boolean {
    if (!id) throw new Error("EventDedupe.admit 需要非空 id（幂等键必填）")
    if (this.seen.has(id)) {
      this.seen.delete(id)
      this.seen.set(id, Date.now())
      return false
    }
    if (this.seen.size >= this.capacity) {
      const oldest = this.seen.keys().next().value
      if (oldest !== undefined) this.seen.delete(oldest)
    }
    this.seen.set(id, Date.now())
    return true
  }

  /** 已登记 id 数（诊断用）。 */
  size(): number {
    return this.seen.size
  }

  /** 清空（测试/人格重载用）。 */
  clear(): void {
    this.seen.clear()
  }
}
