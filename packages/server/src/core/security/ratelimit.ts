/**
 * 每用户消息/请求速率限制（令牌桶）：防单用户高频 prompt 消耗 LLM 配额/资源（DESIGN「限流保护」）。
 * 纯内存实现：容量内突发放行，超出按补充速率拒绝；桶随空闲自动清理防内存增长。
 */
export class TokenBucket {
  private buckets = new Map<string, { tokens: number; last: number }>()

  constructor(
    /** 桶容量（突发上限）。 */
    private capacity: number,
    /** 每秒补充速率。 */
    private refillPerSec: number,
    /** 桶空闲清理阈值（毫秒），超时未活动的桶在下次 allow 时移除。 */
    private idleSweepMs = 10 * 60 * 1000,
  ) {}

  /** 消耗一个令牌；未超限返回 true，超限返回 false（调用方按 429/错误应答处理）。 */
  allow(key: string): boolean {
    const now = Date.now()
    // 顺带清理长时间未活动的桶（防内存增长）
    if (this.buckets.size > 1000) {
      for (const [k, b] of this.buckets) {
        if (now - b.last > this.idleSweepMs) this.buckets.delete(k)
      }
    }
    const b = this.buckets.get(key) ?? { tokens: this.capacity, last: now }
    b.tokens = Math.min(this.capacity, b.tokens + ((now - b.last) / 1000) * this.refillPerSec)
    b.last = now
    if (b.tokens < 1) return false
    b.tokens -= 1
    this.buckets.set(key, b)
    return true
  }
}
