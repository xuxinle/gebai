/** 会话消息视图（pane）注册表：按会话分容器、运行中永不释放、已停止按 LRU 缓存。
 *  这里只测纯注册表逻辑（DOM 由测试基线的 Proxy 元素替身承担）。 */
import { afterEach, describe, expect, test } from "bun:test"
import { activatePane, activePane, deactivatePane, ensurePane, IDLE_PANE_CACHE, paneCount, paneOf, reapIdlePanes, resetPanes, setPaneRunning } from "./msg-panes"

afterEach(() => {
  resetPanes()
})

describe("容器创建与激活", () => {
  test("ensurePane 幂等：同一会话复用同一容器", () => {
    const a = ensurePane("s1")
    const b = ensurePane("s1")
    expect(a).toBe(b)
    expect(paneCount()).toBe(1)
  })

  test("paneOf 不创建（不存在返回 undefined，调用方据此走一次性加载）", () => {
    expect(paneOf("nope")).toBeUndefined()
    expect(paneCount()).toBe(0)
  })

  test("激活只显示一个：其余容器隐藏（DOM 保留）", () => {
    const a = activatePane("s1")
    const b = activatePane("s2")
    expect(a.container.hidden).toBe(true)
    expect(b.container.hidden).toBe(false)
    expect(activePane()).toBe(b)
    // 两个容器都还在（切回不用重建）
    expect(paneCount()).toBe(2)
  })

  test("容器带自己的块表字段（缓存页切回靠它重建历史块）", () => {
    const pane = ensurePane("s1")
    expect(pane.chunks).toBeNull()
    pane.chunks = { msgs: [], bounds: [0], sessionId: "s1", index: new Map() }
    expect(paneOf("s1")!.chunks).toBeTruthy()
  })

  test("取消激活（草稿页）：全部隐藏但容器与内容保留", () => {
    activatePane("s1")
    deactivatePane()
    expect(activePane()).toBeNull()
    expect(paneCount()).toBe(1)
    expect(paneOf("s1")!.container.hidden).toBe(true)
  })
})

describe("LRU 缓存：已停止会话来回切换不重拉", () => {
  test("已停止会话在缓存上限内不被释放（切回仍命中）", () => {
    // 建 IDLE_PANE_CACHE 个已停止会话容器
    for (let i = 0; i < IDLE_PANE_CACHE; i++) ensurePane(`s${i}`)
    activatePane("s0") // s0 为活动，不可释放
    expect(reapIdlePanes()).toBe(0)
    expect(paneCount()).toBe(IDLE_PANE_CACHE)
  })

  test("超出上限按最久未用释放；活动会话与运行中的永不释放", () => {
    for (let i = 0; i < IDLE_PANE_CACHE + 3; i++) ensurePane(`s${i}`)
    // s0 设为运行中（永不释放），s1 设为活动
    setPaneRunning("s0", true)
    activatePane("s1")
    const freed = reapIdlePanes()
    // 总容器 IDLE+3 个，其中运行中 1 + 活动 1 不可释放，可释放 IDLE+1 个；上限 IDLE 个空闲 → 释放 1 个
    expect(freed).toBe(1)
    expect(paneOf("s0")).toBeTruthy() // 运行中保留
    expect(paneOf("s1")).toBeTruthy() // 活动保留
  })

  test("最久未用的先被释放（LRU 序）", () => {
    resetPanes()
    // 造 3 个会话，按 s0 → s1 → s2 顺序使用（lastUsed 递增）
    for (const id of ["s0", "s1", "s2"]) {
      ensurePane(id)
      // 拉开时间戳（同毫秒内创建时 LRU 序不可辨）
      Bun.sleepSync(2)
    }
    activatePane("s2")
    // 只允许保留 1 个空闲 → 释放 1 个（最久未用的 s0）
    reapIdlePanes(1)
    expect(paneOf("s0")).toBeUndefined()
    expect(paneOf("s1")).toBeTruthy()
    expect(paneOf("s2")).toBeTruthy()
  })

  test("运行中的容器即使数量超限也不释放", () => {
    for (let i = 0; i < IDLE_PANE_CACHE + 5; i++) {
      ensurePane(`r${i}`)
      setPaneRunning(`r${i}`, true)
    }
    expect(reapIdlePanes()).toBe(0)
    expect(paneCount()).toBe(IDLE_PANE_CACHE + 5)
  })

  test("任务结束后降为可回收（不再锁住容器）", () => {
    const pane = ensurePane("s1")
    setPaneRunning("s1", true)
    expect(pane.running).toBe(true)
    activatePane("s2")
    expect(reapIdlePanes(0)).toBe(0) // 运行中不释放
    setPaneRunning("s1", false)
    expect(reapIdlePanes(0)).toBe(1) // 停止后可释放
    expect(paneOf("s1")).toBeUndefined()
    void pane
  })
})

describe("激活时间戳（LRU 依据）", () => {
  test("激活会刷新 lastUsed", () => {
    const pane = ensurePane("s1")
    const before = pane.lastUsed
    Bun.sleepSync(3)
    activatePane("s2")
    activatePane("s1")
    expect(pane.lastUsed).toBeGreaterThan(before)
  })
})
