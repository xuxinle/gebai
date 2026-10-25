import { describe, expect, test } from "bun:test"
import { createVzModel, DEFAULT_PER_MSG, ESTIMATE_SAMPLES } from "./virtual-window"

/** 建 n 个块，每块 weight 条消息。 */
function model(weights: number[], gap = 0) {
  const m = createVzModel()
  m.setGap(gap)
  m.setSlots(weights.map((w, i) => ({ key: `b${i}`, weight: w })))
  return m
}

describe("窗口化坐标核（virtual-window）", () => {
  test("未实测块用估高（每消息估高 × 条数）", () => {
    const m = model([10, 20])
    expect(m.perMsg()).toBe(DEFAULT_PER_MSG)
    expect(m.heightAt(0)).toBe(10 * DEFAULT_PER_MSG)
    expect(m.heightAt(1)).toBe(20 * DEFAULT_PER_MSG)
    expect(m.count()).toBe(2)
  })

  test("全渲染坐标含块间距；totalHeight 为 Σ块高 + (n-1)×gap", () => {
    const m = model([1, 1, 1], 12)
    m.measure(0, 100)
    m.measure(1, 200)
    m.measure(2, 300)
    expect(m.pos(0)).toBe(0)
    expect(m.pos(1)).toBe(112)
    expect(m.pos(2)).toBe(324)
    expect(m.totalHeight()).toBe(100 + 12 + 200 + 12 + 300)
  })

  test("locate：坐标落在块内取偏移，超出末尾归入末块并钳制偏移", () => {
    const m = model([1, 1, 1], 0)
    m.measure(0, 100)
    m.measure(1, 200)
    m.measure(2, 300)
    expect(m.locate(0)).toEqual({ index: 0, offset: 0 })
    expect(m.locate(40)).toEqual({ index: 0, offset: 40 })
    expect(m.locate(100)).toEqual({ index: 1, offset: 0 })
    expect(m.locate(350)).toEqual({ index: 2, offset: 50 })
    expect(m.locate(9000)).toEqual({ index: 2, offset: 300 })
    expect(m.locate(-5)).toEqual({ index: 0, offset: 0 })
  })

  test("rangeFor：覆盖视口 + 上下余量，边界块整块纳入", () => {
    const m = model([1, 1, 1, 1, 1, 1], 0)
    for (let i = 0; i < 6; i++) m.measure(i, 100)
    // scrollTop=250、视口 100：上方余量 50 → 200（块 2 首），下方余量 150 → 500（块 5 首）
    expect(m.rangeFor(250, 100, 0.5, 1.5)).toEqual({ start: 2, end: 6 })
    // 贴底：视口在末尾，末块纳入
    expect(m.rangeFor(500, 100, 0.5, 1.5)).toEqual({ start: 4, end: 6 })
    // 顶部：不允许负区间（上方余量落到视口外被按 0 夹取）
    expect(m.rangeFor(0, 100, 0.5, 1.5)).toEqual({ start: 0, end: 3 })
  })

  test("rangeFor：视口不可测（clientHeight 0）时全部纳入（安全阀）", () => {
    const m = model([1, 1, 1], 0)
    expect(m.rangeFor(0, 0, 0.5, 1.5)).toEqual({ start: 0, end: 3 })
    expect(m.rangeFor(0, -1, 0.5, 1.5)).toEqual({ start: 0, end: 3 })
  })

  test("padHeight：折叠 k 块为 1 个 spacer 时补回少掉的 k-1 份间距", () => {
    const m = model([1, 1, 1, 1], 10)
    for (let i = 0; i < 4; i++) m.measure(i, 100)
    // 全挂载（区间覆盖全部）：两侧无 spacer
    expect(m.padHeight({ start: 0, end: 4 })).toEqual({ top: 0, bottom: 0 })
    // 区间 [1,3)：上方折 1 块（高度恰为其块高）、下方折 1 块
    expect(m.padHeight({ start: 1, end: 3 })).toEqual({ top: 100, bottom: 100 })
    // 区间 [2,3)：上方折 2 块 = 200 + 1×gap
    expect(m.padHeight({ start: 2, end: 3 })).toEqual({ top: 210, bottom: 100 })
    // 区间 [3,3)：上方折 3 块、下方折 1 块
    expect(m.padHeight({ start: 3, end: 3 })).toEqual({ top: 320, bottom: 100 })
  })

  test("spacer 补偿后总布局高度与全渲染一致（滚动条长度不漂移）", () => {
    const m = model([1, 1, 1, 1, 1], 12)
    for (let i = 0; i < 5; i++) m.measure(i, 80 + i * 10)
    const full = m.totalHeight()
    const range = { start: 2, end: 4 }
    const pad = m.padHeight(range)
    const mounted = m.heightAt(2) + m.heightAt(3)
    // 折叠后布局高度 = 挂载块 + 两侧 spacer + 项间距（4 个子项 → 3 份间距）
    const folded = pad.top + mounted + pad.bottom + m.gap() * 3
    expect(folded).toBe(full)
  })

  test("measure 自校准：实测只改本块，估高由 settleEstimates 定稿（滚动期不重算已定型布局）", () => {
    const m = model([10, 10, 10])
    expect(m.heightAt(0)).toBe(10 * DEFAULT_PER_MSG)
    m.measure(0, 500) // 每消息 50px
    expect(m.perMsg()).toBe(50)
    expect(m.heightAt(0)).toBe(500)
    expect(m.heightAt(1)).toBe(10 * DEFAULT_PER_MSG) // 未定稿：其余块高度不被单次测量改动
    expect(m.settleEstimates()).toBe(true)
    expect(m.heightAt(1)).toBe(500)
    expect(m.heightAt(2)).toBe(500)
    m.measure(1, 800) // 每消息 80px → 窗口均值 65
    expect(m.perMsg()).toBe((50 + 80) / 2)
    expect(m.heightAt(0)).toBe(500)
    expect(m.heightAt(1)).toBe(800)
    expect(m.heightAt(2)).toBe(500) // 已定稿的估高不随后续样本漂移
    expect(m.settleEstimates()).toBe(true)
    expect(m.heightAt(2)).toBe(650)
  })

  test("measure：高度未变返回 false，非正高度忽略", () => {
    const m = model([1, 1])
    expect(m.measure(0, 100)).toBe(true)
    expect(m.measure(0, 100)).toBe(false)
    expect(m.measure(0, 100.2)).toBe(false)
    expect(m.measure(0, 101)).toBe(true)
    expect(m.measure(0, 0)).toBe(false)
    expect(m.measure(0, -3)).toBe(false)
    expect(m.measure(9, 100)).toBe(false)
  })

  test("估高样本只用最近 ESTIMATE_SAMPLES 个（跟随内容形态变化）", () => {
    const m = createVzModel()
    m.setSlots(Array.from({ length: ESTIMATE_SAMPLES + 5 }, (_, i) => ({ key: `b${i}`, weight: 1 })))
    for (let i = 0; i < ESTIMATE_SAMPLES; i++) m.measure(i, 100)
    for (let i = ESTIMATE_SAMPLES; i < ESTIMATE_SAMPLES + 5; i++) m.measure(i, 400)
    // 窗口只留最近 20 个样本：15 个 100 + 5 个 400 → 均值 175（旧样本已滑出）
    expect(m.perMsg()).toBe(175)
  })

  test("估高用均值不用中位数（流式追加时更平滑）", () => {
    // 实测两个长会话、20 样本滑窗逐条推进：中位数相邻抖动 5.4%/5.8%、最大跳变 76%，
    // 均值 4.6%/3.6%、最大 59%——少数超大值被中位数推开后，下一次滑出窗口时估值突变。
    const m = createVzModel()
    m.setSlots(Array.from({ length: ESTIMATE_SAMPLES }, (_, i) => ({ key: `b${i}`, weight: 1 })))
    // 19 个 100 + 1 个 1000：中位数会是 100，均值是 145
    for (let i = 0; i < ESTIMATE_SAMPLES - 1; i++) m.measure(i, 100)
    m.measure(ESTIMATE_SAMPLES - 1, 1000)
    expect(m.perMsg()).toBe(145)
  })

  test("状态标记：渲染 / 挂载 / 卸载", () => {
    const m = model([1, 2])
    expect(m.slots[0]).toMatchObject({ key: "b0", weight: 1, measured: false, rendered: false, mounted: false })
    m.markRendered(0)
    m.markMounted(0)
    expect(m.slots[0].rendered).toBe(true)
    expect(m.slots[0].mounted).toBe(true)
    m.markUnmounted(0)
    expect(m.slots[0].mounted).toBe(false)
    expect(m.slots[0].rendered).toBe(true)
    // 越界操作静默忽略
    m.markMounted(5)
    expect(m.slots).toHaveLength(2)
  })

  test("setSlots 重置槽位表；空表下各查询安全", () => {
    const m = model([1, 1, 1])
    m.measure(0, 300)
    m.setSlots([{ key: "x", weight: 2 }])
    expect(m.count()).toBe(1)
    expect(m.slots[0]).toMatchObject({ key: "x", weight: 2, measured: false })
    expect(m.pos(9)).toBe(600) // 越界取总高（不会越界读）
    m.setSlots([])
    expect(m.count()).toBe(0)
    expect(m.totalHeight()).toBe(0)
    expect(m.locate(100)).toEqual({ index: 0, offset: 0 })
    expect(m.rangeFor(0, 0, 0.5, 1.5)).toEqual({ start: 0, end: 0 })
    expect(m.padHeight({ start: 0, end: 0 })).toEqual({ top: 0, bottom: 0 })
  })

  test("beyondBlocks：坐标越过末块末尾（尾部活动区）为真，块内与恰好末尾为假", () => {
    const m = model([1, 1])
    m.measure(0, 100)
    m.measure(1, 200)
    expect(m.beyondBlocks(0)).toBe(false)
    expect(m.beyondBlocks(50)).toBe(false)
    expect(m.beyondBlocks(100)).toBe(false)
    expect(m.beyondBlocks(299)).toBe(false)
    expect(m.beyondBlocks(300)).toBe(false) // 恰好末块末尾：仍属块表坐标
    expect(m.beyondBlocks(300.5)).toBe(true)
    expect(m.beyondBlocks(5000)).toBe(true)
  })

  test("beyondBlocks：空表视为全部越出（此时只有尾部活动区）", () => {
    const m = createVzModel()
    expect(m.beyondBlocks(0)).toBe(true)
    expect(m.beyondBlocks(500)).toBe(true)
  })

  test("locate 越界时钳到末块末尾（调用方必须先用 beyondBlocks 分流，否则会把视口拉回末块末尾）", () => {
    const m = model([1, 1])
    m.measure(0, 100)
    m.measure(1, 200)
    expect(m.locate(9000)).toEqual({ index: 1, offset: 200 })
  })

  test("weight 至少为 1（空块不产生零高度槽位）", () => {
    const m = createVzModel()
    m.setSlots([{ key: "b0", weight: 0 }])
    expect(m.slots[0].weight).toBe(1)
  })

  test("批次内多次实测只重建一次（避免 O(块数²)，长会话切回不再抖）", () => {
    // 造一个长会话：200 块（典型 800+ 消息会话的块量级）
    const m = model(Array.from({ length: 200 }, () => 6))
    const before = m.totalHeight()
    // 一次窗口变更会挂载多块：旧实现每块都重建一次（200 块 × 每次 O(200)）
    m.beginBatch()
    for (let i = 100; i < 108; i++) m.measure(i, 120 + i)
    // 批次未结束：高度表尚未重建，总高还是一个整体（不是中间态）
    m.endBatch()
    const after = m.totalHeight()
    expect(after).not.toBe(before) // 实测生效（8 块换成真实高度）
    // 批次结束后位置一致：块 100 的起点 = 前 100 块高度之和（实测块已计入）
    const pos = m.pos(100)
    expect(pos).toBeGreaterThan(0)
    expect(Number.isFinite(pos)).toBe(true)
  })

  test("批次外单块实测即时生效（不依赖 begin/end）", () => {
    const m = model([6, 6, 6])
    const before = m.heightAt(0)
    m.measure(0, 999)
    expect(m.heightAt(0)).toBe(999)
    expect(m.heightAt(0)).not.toBe(before)
  })
})
