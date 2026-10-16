/**
 * L1 全局公平闸门（FairGate）：把「同一推理端点上的总在途请求」收拢到一处统一分配，
 * 并按 owner（研判任务）轮转派发——**根治「大任务占满后小任务饿死」**。
 *
 * 为什么不是「任务级队列」：单个任务内部的并发（`l1_concurrency`）各自为政时，
 * 两个任务各开 8 并发 = 16 个请求同时压向网关；再加一层「任务串行」又会把小任务
 * 堵在大任务后面（2000 条跑完才轮到 20 条）。真正需要的是**在条目派发层做全局限额 +
 * 按任务轮转**：任何时刻全局在途 ≤ 上限，且每个任务每轮都能拿到份额——小任务几十秒跑完，
 * 大任务总体吞吐不受损（只是它的份额被别人分了一点）。
 *
 * 三个设计要点（都是实测推出来的，勿凭直觉推翻）：
 *   * **上限按端点共享、取各任务并发度的最大值，而不是相加**：两个 c8 任务共享 8 个在途额度
 *     （轮转各占一半），而不是把网关打到 16 并发——网关（sglang/llama.cpp）排队时并发越高
 *     单条越慢，总吞吐并不会随并发线性增长（实测 c1=75.3s → c8=51.3s，c4≈c1）。
 *   * **等待不是拒绝**：拿不到槽位就在队里等（不烧 token），槽位释放时按轮转派发；
 *     `signal` 中止即出队，不会泄漏槽位。
 *   * **不需要「等待超时降级放行」**：单个请求的时长已由 `l1_timeout_ms` 兜住（超时即释放槽位），
 *     再加一条「等太久就放行」的通路只会把上限悄悄捅破，等于没有上限。
 */

/** 取槽时被中止（调用方按「任务已取消」处理）。 */
export const FAIR_GATE_ABORTED = "fair gate acquire aborted"

/** 放槽函数（幂等；重复调用只生效一次）。 */
export type GateRelease = () => void

export interface FairGateStats {
  /** 当前全局在途上限。 */
  limit: number
  /** 当前在途请求数。 */
  inflight: number
  /** 当前排队等待的任务数（不烧 token，只在等槽位）。 */
  waiting: number
  /** 历史上同时在途的峰值（观察上限是否真的生效）。 */
  peak_inflight: number
  /** 历史上同时排队的峰值（观察争抢强度）。 */
  peak_waiting: number
  /** 排队中出现过的 owner 数（几个任务在抢）。 */
  waiting_owners: number
}

interface Waiter {
  owner: string
  resolve: (release: GateRelease) => void
  reject: (e: Error) => void
  /** 清掉中止监听（授予或出队时调用）。 */
  settle: () => void
}

/**
 * 公平闸门：`limit` 个在途额度，按 owner 轮转（round-robin）派发。
 *
 * 未排队时走快路径（在途未满直接取槽），因此**单任务独占时行为与无闸门完全一致**
 * （并发度就是 `limit`，不会被拖慢）；只有多任务同时在跑时才体现「份额」语义。
 */
export class FairGate {
  private count = 0
  private queue: Waiter[] = []
  /** 本轮已服务过的 owner（保证同轮内不重复服务同一个 owner）。 */
  private served = new Set<string>()
  private cursor = 0
  private peakInflight = 0
  private peakWaiting = 0

  constructor(private cap: number) {
    this.cap = Math.max(1, Math.floor(cap) || 1)
  }

  get limit(): number {
    return this.cap
  }

  /** 调整上限（任务挂/卸时由 `FairGateHub` 调用）；调大即唤醒等待者。 */
  setLimit(n: number): void {
    const next = Math.max(1, Math.floor(n) || 1)
    if (next === this.cap) return
    this.cap = next
    this.pump()
  }

  /**
   * 取一个槽位：有空位立即返回；否则排队等（中止即抛错）。
   * 返回的释放函数**必须**在 finally 里调用，否则槽位泄漏、等待者会一直等下去。
   */
  async acquire(o: { owner?: string; signal?: AbortSignal } = {}): Promise<GateRelease> {
    const owner = o.owner ?? ""
    const signal = o.signal
    if (signal?.aborted) throw new Error(FAIR_GATE_ABORTED)
    if (this.count < this.cap) return this.take()

    return await new Promise<GateRelease>((resolve, reject) => {
      const w: Waiter = { owner, resolve, reject, settle: () => {} }
      const onAbort = (): void => {
        w.settle()
        this.drop(w)
        reject(new Error(FAIR_GATE_ABORTED))
      }
      w.settle = () => {
        if (signal) signal.removeEventListener("abort", onAbort)
      }
      if (signal) signal.addEventListener("abort", onAbort, { once: true })
      this.queue.push(w)
      this.peakWaiting = Math.max(this.peakWaiting, this.queue.length)
    })
  }

  stats(): FairGateStats {
    return {
      limit: this.cap,
      inflight: this.count,
      waiting: this.queue.length,
      peak_inflight: this.peakInflight,
      peak_waiting: this.peakWaiting,
      waiting_owners: new Set(this.queue.map((w) => w.owner)).size,
    }
  }

  /** 取槽（调用方已确认有空位或刚被授予）。 */
  private take(): GateRelease {
    this.count++
    this.peakInflight = Math.max(this.peakInflight, this.count)
    let released = false
    return () => {
      if (released) return
      released = true
      this.count = Math.max(0, this.count - 1)
      this.pump()
    }
  }

  private drop(w: Waiter): void {
    const i = this.queue.indexOf(w)
    if (i >= 0) this.queue.splice(i, 1)
    if (!this.queue.length) this.served.clear()
  }

  private pump(): void {
    while (this.queue.length && this.count < this.cap) {
      const w = this.pick()
      if (!w) break
      this.drop(w)
      w.settle()
      w.resolve(this.take())
    }
    if (!this.queue.length) this.served.clear()
  }

  /**
   * 挑下一个被授予的等待者：**按 owner 轮转**——优先服务本轮还没被服务过的 owner，
   * 一圈之内每个 owner 至少拿到一次（这就是「小任务不被大任务饿死」的全部机制）。
   */
  private pick(): Waiter | undefined {
    if (!this.queue.length) return undefined
    for (let pass = 0; pass < 2; pass++) {
      for (let i = 0; i < this.queue.length; i++) {
        const idx = (this.cursor + i) % this.queue.length
        const w = this.queue[idx]
        if (this.served.has(w.owner)) continue
        this.served.add(w.owner)
        this.cursor = (idx + 1) % this.queue.length
        return w
      }
      // 一圈内所有 owner 都服务过了 → 开始新一轮
      this.served.clear()
    }
    return this.queue[0]
  }
}

/**
 * 端点级闸门仓库：同一 `key`（L1 端点）上的多个研判任务**共享一个闸门**。
 *
 * 上限规则：未设 `cap` 时取「当前挂在该端点上的各任务并发度的**最大值**」——
 * 单任务 c8 就是 8（与无闸门一致），第二个 c8 任务挂进来仍是 8，两个任务轮转各占一半；
 * 一个任务跑完卸载后，剩下的任务恢复独享。要放开到多任务并行打满网关就设 `cap`
 * （管线读 `GEBAI_TRIAGE_L1_GATE`），此时 cap 为准。
 */
export class FairGateHub {
  /** key → (owner → 该 owner 请求的并发度)。 */
  private members = new Map<string, Map<string, number>>()
  private gates = new Map<string, FairGate>()
  private caps = new Map<string, number>()

  /**
   * 挂号并取回该端点的闸门（同一 key 多次调用返回**同一个**实例）。
   * `cap` 给出时作为该端点的硬上限（env 配置），否则由成员并发度的最大值推导。
   */
  attach(key: string, owner: string, concurrency: number, cap?: number): FairGate {
    const m = this.members.get(key) ?? new Map<string, number>()
    m.set(owner, Math.max(1, Math.floor(concurrency) || 1))
    this.members.set(key, m)
    if (cap !== undefined) this.caps.set(key, Math.max(1, Math.floor(cap) || 1))

    let gate = this.gates.get(key)
    if (!gate) {
      gate = new FairGate(this.limitFor(key))
      this.gates.set(key, gate)
    } else {
      gate.setLimit(this.limitFor(key))
    }
    return gate
  }

  /** 卸载任务（闸门本身保留：可能有别的任务还在用；上限按剩余成员重算）。 */
  detach(key: string, owner: string): void {
    const m = this.members.get(key)
    if (!m) return
    m.delete(owner)
    if (!m.size) {
      this.members.delete(key)
      return
    }
    this.gates.get(key)?.setLimit(this.limitFor(key))
  }

  /** 当前上限（供报告与测试断言）。 */
  limitOf(key: string): number {
    return this.limitFor(key)
  }

  private limitFor(key: string): number {
    const cap = this.caps.get(key)
    if (cap !== undefined) return cap
    let max = 1
    for (const v of this.members.get(key)?.values() ?? []) max = Math.max(max, v)
    return max
  }
}

/** 进程级共享仓库：同一进程里所有研判任务（工具通道 + REST 通道）共用端点闸门。 */
export const l1GateHub = new FairGateHub()
