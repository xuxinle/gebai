/**
 * 「用户亲手发言」的让位窗口（C）。
 *
 * ## 要解决的问题
 *
 * 人格以用户本人名义在群里说话。用户**自己**也在那个群里说话时，会出现两种抢话：
 *
 * 1. 用户正在打字/刚说完，人格紧接着又发一条——看着像自问自答。
 * 2. 用户在纠正某件事，人格却还在按旧结论往下干。
 *
 * ## 为什么不直接静音
 *
 * 静音是个陷阱：用户随口一句「收到」，人格就永久不说话，而没有任何东西会把它解除。
 * 所以这里只做**限时让位**：用户在该群发言后 N 分钟内，人格不**主动**发言
 * （发起的对别人的回复也走这条）。窗口到期自动恢复，不需要任何人工解除。
 *
 * ## 为什么是「按群」而不是全局
 *
 * 让位是**针对那场对话**的礼貌：用户在 A 群说话，不该影响人格在 B 群回答别人的问题。
 * 按 chatId 记账，与人格的会话映射无关（这里是出站礼貌，不是上下文隔离）。
 *
 * ## 边界：被明确要求时不受限
 *
 * 用户说「你回他」之后再静音就荒谬了。所以调用方可以传 `explicit=true`
 * （来自用户本人的明确指示）来跳过让位判定。判定本身不认识语义，
 * 它只回答「这个群最近有没有用户亲手说过话」。
 */

/** 让位窗口长度（毫秒）。 */
export const DEFAULT_YIELD_WINDOW_MS = 3 * 60_000

export interface YieldWindowOptions {
  /** 窗口长度（缺省 3 分钟）。 */
  windowMs?: number
  /** 当前时刻（测试注入）。 */
  now?: () => number
}

/**
 * 按群记录「用户亲手发言」的时刻。
 *
 * 只保留最近一次：早先的发言被后来的覆盖是正确语义（用户又说话了，窗口重新开始）。
 */
export class HumanSpeechWindow {
  private readonly lastAt = new Map<string, number>()
  private readonly windowMs: number
  private readonly now: () => number

  constructor(opts: YieldWindowOptions = {}) {
    this.windowMs = opts.windowMs ?? DEFAULT_YIELD_WINDOW_MS
    this.now = opts.now ?? Date.now
  }

  /** 记一次用户亲手发言（chatId 为空则忽略——拿不到群就无法按群让位）。 */
  note(chatId: string | undefined, at = this.now()): void {
    if (!chatId) return
    this.lastAt.set(chatId, at)
    // 顺手清理陈旧条目（人格常驻，群数有限，但长期运行仍要防无界）
    if (this.lastAt.size > 512) {
      for (const [key, ts] of this.lastAt) {
        if (at - ts > this.windowMs * 4) this.lastAt.delete(key)
      }
    }
  }

  /** 该群是否正在让位窗口内。
   *
   * `windowMs` 可选：不同分身可以有不同让位窗口，而本对象是进程级的
   * （按群共享时间戳）。传了就按传入值算，不传用构造时的缺省值。
   */
  inWindow(chatId: string, at = this.now(), windowMs = this.windowMs): boolean {
    // 0 窗口 = 显式关闭（yieldSeconds: 0）。
    // 不单列这个分支的话，`at - ts > 0` 在同一毫秒内为 false，会返回 true——
    // 即“明明关了却还拦一次”，而配置的意图是彻底不拦。
    if (windowMs <= 0) return false
    const ts = this.lastAt.get(chatId)
    if (ts === undefined) return false
    if (at - ts >= windowMs) {
      this.lastAt.delete(chatId) // 过期即清：避免长期占着
      return false
    }
    return true
  }

  /** 距窗口结束还有多久（毫秒；不在窗口内为 0）。用于给模型的提示措辞。 */
  remainingMs(chatId: string, at = this.now(), windowMs = this.windowMs): number {
    if (windowMs <= 0) return 0
    const ts = this.lastAt.get(chatId)
    if (ts === undefined) return 0
    return Math.max(0, windowMs - (at - ts))
  }

  /** 当前受影响的群数（诊断用）。 */
  size(): number {
    return this.lastAt.size
  }

  clear(): void {
    this.lastAt.clear()
  }
}

/** 从环境读窗口长度：`GEBAI_PERSONA_YIELD_SECONDS`（0 = 关闭让位）。 */
export function yieldWindowFromEnv(env: Record<string, string | undefined>): number {
  const raw = (env.GEBAI_PERSONA_YIELD_SECONDS ?? "").trim()
  if (!raw) return DEFAULT_YIELD_WINDOW_MS
  const seconds = Number(raw)
  if (!Number.isFinite(seconds) || seconds < 0) return DEFAULT_YIELD_WINDOW_MS
  return seconds * 1000
}
