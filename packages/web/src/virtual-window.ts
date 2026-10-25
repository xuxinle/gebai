/**
 * 消息列 DOM 窗口化——坐标与区间算术核（无 DOM；DOM 编排见 virtualize.ts）：
 *
 * 消息列按「块」组织（连续消息段，划分规则见 history-chunk.ts）：块挂载时是 DOM 中的真实
 * 节点（高度实测），未挂载时由一块 spacer 占位——高度取实测记忆，首次访问用估高，估高按
 * 「每条消息」自校准（样本取自已实测的块，随浏览持续收敛）。
 *
 * 布局不变式（对应 #messages 的 flex 列 + gap）：k 个块折叠成 1 个 spacer 子项时，相邻间距
 * 少 k-1 份，故 spacer 高度 = Σ块高 + (k-1)×gap（k>=1）；k=0 时高度为 0，由调用方摘除该
 * spacer（不占位、不产生间距）。
 *
 * 坐标一律取「全渲染坐标」pos(i) = Σ_{j<i} (height(j) + gap)：与折叠补偿后的真实布局高度
 * 一致（见上不变式），因此滚动位置换算与真实位置不会漂移，滚动条长度也随实测校准收敛。
 */

/** 未实测块的每消息估高默认值（px）：仅用于浏览到该区域前，随后被实测样本取代。 */
export const DEFAULT_PER_MSG = 150
/** 估高样本窗口（最近 N 个实测块参与均值，跟随会话内容形态变化）。 */
export const ESTIMATE_SAMPLES = 20

/** 槽位种子（建立一块）：key 用于滚动锚点与导航定位，weight 为块内消息条数（估高权重）。 */
export interface VzSlotSeed {
  key: string
  weight: number
}

export interface VzSlot {
  key: string
  weight: number
  /** 采用高度：已实测为真实值，未实测为当前估高（每次估高刷新时同步）。 */
  height: number
  measured: boolean
  /** 已渲染过（节点已创建，可重挂；未渲染块首次挂载需调渲染回调）。 */
  rendered: boolean
  mounted: boolean
}

export interface VzRange {
  start: number
  end: number
}

export interface VzModel {
  readonly slots: VzSlot[]
  count(): number
  setSlots(seeds: VzSlotSeed[]): void
  /** 块间间距（#messages 的 flex gap），参与 spacer 补偿与坐标换算。 */
  setGap(gap: number): void
  gap(): number
  /** 当前估高（每消息 px）。 */
  perMsg(): number
  heightAt(index: number): number
  /** 全渲染坐标：块 index 顶部（不含容器内边距）。 */
  pos(index: number): number
  totalHeight(): number
  /** 坐标 y 落在哪个块 + 块内偏移（块表覆盖不足时钳到末块末尾；越界判定用 beyondBlocks）。 */
  locate(y: number): { index: number; offset: number }
  /** 坐标 y 是否越过最后一个块（尾部活动区：其高度是真实 DOM、不入坐标表）。 */
  beyondBlocks(y: number): boolean
  /** 需处于挂载态的块区间：覆盖视口 + 上下余量；视口不可测时返回全部（安全阀）。 */
  rangeFor(scrollTop: number, viewportH: number, marginAbove: number, marginBelow: number): VzRange
  /** 折叠区间外块所需的 spacer 高度（已含 gap 补偿；0 表示该侧无需 spacer）。 */
  padHeight(range: VzRange): { top: number; bottom: number }
  markRendered(index: number): void
  markMounted(index: number): void
  markUnmounted(index: number): void
  /** 记录实测高度；返回高度表是否发生变化。 */
  measure(index: number, height: number): boolean
  /** 开/关批次：批次内的 measure 推迟 rebuild，结束时统一重建一次（避免 O(块数²)）。 */
  beginBatch(): void
  endBatch(): void
  /** 按当前估高铺开未实测块的高度（加载阶段首批渲染后调一次定稿；此后估高只作用于之后新增的块）。 */
  settleEstimates(): boolean
  /** 估高样本（测试 / 调试用）。 */
  estimateSamples(): readonly number[]
}

export function createVzModel(): VzModel {
  const slots: VzSlot[] = []
  const samples: number[] = []
  let gap = 0
  let prefix: number[] = [0]
  let est = DEFAULT_PER_MSG
  /** 批次深度（>0 时 measure 推迟 rebuild）：窗口一次挂载多块，逐个重建是 O(块数²)。 */
  let batch = 0
  /** 批次内有高度变化（待 endBatch 时统一重建）。 */
  let dirty = false

  function rebuild(): void {
    prefix = new Array<number>(slots.length + 1)
    prefix[0] = 0
    for (let i = 0; i < slots.length; i++) prefix[i + 1] = prefix[i] + slots[i].height + gap
  }

  /** 估高变化后同步未实测块的高度（实测块的 height 已是真实值，不动）。 */
  function applyEstimate(): boolean {    let changed = false
    for (const s of slots) {
      if (s.measured) continue
      const h = s.weight * est
      if (h !== s.height) {
        s.height = h
        changed = true
      }
    }
    if (changed) rebuild()
    return changed
  }

  /**
   * 估高（每消息像素）：取最近样本**均值**。
   *
   * 估高只是未实测块的占位相对量，影响观感的是它在**流式追加时变得平不平滑**。实测两个
   * 长会话（20 样本滑窗、逐条推进）对比：中位数在相邻样本间抖动更大（5.4% / 5.8%
   * vs 均值 4.6% / 3.6%；最大跳变 76% vs 59%）——少数超大值被中位数推开、下一次滑出
   * 窗口时估值突变。均值反而平滑，故用均值。
   */
  function recomputeEstimate(): void {
    const n = Math.min(samples.length, ESTIMATE_SAMPLES)
    if (!n) {
      est = DEFAULT_PER_MSG
      return
    }
    let sum = 0
    for (let i = samples.length - n; i < samples.length; i++) sum += samples[i]
    est = sum / n
    if (!(est > 0)) est = DEFAULT_PER_MSG
  }

  function heightAt(index: number): number {
    const s = slots[index]
    return s ? s.height : 0
  }

  /** 坐标 y 落在哪个块 + 块内偏移（二分；y 超出末尾时归入末块）。 */
  function locate(y: number): { index: number; offset: number } {
    const n = slots.length
    if (!n) return { index: 0, offset: 0 }
    if (!(y > 0)) return { index: 0, offset: 0 }
    let lo = 0
    let hi = n - 1
    let best = 0
    while (lo <= hi) {
      const mid = (lo + hi) >> 1
      if (prefix[mid] <= y) {
        best = mid
        lo = mid + 1
      } else {
        hi = mid - 1
      }
    }
    return { index: best, offset: Math.max(0, Math.min(y - prefix[best], heightAt(best))) }
  }

  /** 坐标 y 是否已越过最后一个块（尾部活动区：新消息/在途流/工具卡等追加在块表之外，
   *  高度是真实 DOM、不入坐标表——块表坐标无从表达，调用方需改按底锚定）。 */
  function beyondBlocks(y: number): boolean {
    const n = slots.length
    if (!n) return true
    return y > prefix[n - 1] + heightAt(n - 1)
  }

  return {
    slots,
    count: () => slots.length,
    setSlots(seeds) {
      slots.length = 0
      for (const seed of seeds) {
        slots.push({ key: seed.key, weight: Math.max(1, seed.weight), height: 0, measured: false, rendered: false, mounted: false })
      }
      applyEstimate()
      rebuild()
    },
    setGap(g) {
      const next = Number.isFinite(g) && g > 0 ? g : 0
      if (next === gap) return
      gap = next
      rebuild()
    },
    gap: () => gap,
    perMsg: () => est,
    heightAt,
    pos: (index) => {
      const i = Math.max(0, Math.min(index, slots.length))
      return prefix[i] ?? 0
    },
    totalHeight() {
      if (!slots.length) return 0
      return prefix[slots.length] - gap
    },
    locate,
    beyondBlocks,
    rangeFor(scrollTop, viewportH, marginAbove, marginBelow) {
      const n = slots.length
      if (!n) return { start: 0, end: 0 }
      // 视口不可测（测试替身 / 容器隐藏）：全部挂载，退化为不窗口化
      if (!(viewportH > 0)) return { start: 0, end: n }
      const top = Math.max(0, scrollTop - viewportH * marginAbove)
      const bottom = Math.max(top, scrollTop + viewportH * (1 + marginBelow))
      return { start: locate(top).index, end: locate(bottom).index + 1 }
    },
    padHeight(range) {
      const n = slots.length
      const start = Math.max(0, Math.min(range.start, n))
      const end = Math.max(start, Math.min(range.end, n))
      // 折叠 k 块为 1 个 spacer：Σ块高 + (k-1)×gap = prefix[k 块末] - gap
      const top = start > 0 ? prefix[start] - gap : 0
      const bottom = end < n ? prefix[n] - prefix[end] - gap : 0
      return { top: Math.max(0, top), bottom: Math.max(0, bottom) }
    },
    markRendered(index) {
      const s = slots[index]
      if (s) s.rendered = true
    },
    markMounted(index) {
      const s = slots[index]
      if (s) s.mounted = true
    },
    markUnmounted(index) {
      const s = slots[index]
      if (s) s.mounted = false
    },
    measure(index, height) {
      const s = slots[index]
      if (!s || !(height > 0)) return false
      let changed = false
      if (!s.measured || Math.abs(s.height - height) > 0.5) {
        s.height = height
        s.measured = true
        changed = true
      }
      samples.push(height / s.weight)
      if (samples.length > ESTIMATE_SAMPLES * 4) samples.splice(0, samples.length - ESTIMATE_SAMPLES * 4)
      recomputeEstimate()
      // 只更新本块：估高变化不回头重算其余未实测块——滚动过程中反复重算总高是位置漂移的主因
      // （布局在加载期由 settleEstimates 一次性定稿）。批次内（batch>0）推迟重建，
      // 由 endBatch 统一重建一次——逐个重建是 O(块数²)（长会话上百块时这就是切会话的抖动源）
      if (changed) {
        dirty = true
        if (!batch) rebuild()
      }
      return changed
    },
    beginBatch() {
      batch++
    },
    endBatch() {
      if (batch > 0) batch--
      if (batch === 0 && dirty) {
        dirty = false
        rebuild()
      }
    },
    settleEstimates() {
      return applyEstimate()
    },
    estimateSamples: () => samples,
  }
}
