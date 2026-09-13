/**
 * 提交图泳道布局的纯逻辑测试。
 *
 * 为什么值得专门测：日志是**顺序追加**分页的，布局却逐行推导——错一处（车道复用、缺失父、
 * 汇入判定）就会让连线错连到别的分支上，而这种错误在界面上只是「看起来乱」，不会报错。
 * 覆盖：线性历史、分叉与合并、汇聚、分支头、缺失父（已加载完 / 未加载完）、车道复用、
 * 前缀稳定（加载更多不重排已显示的行）、路径字符串几何。
 */
import { describe, expect, test } from "bun:test"
import { graphEdgePath, GRAPH_COLORS, layoutCommitGraph, type GraphCommit } from "./git-graph"

const c = (hash: string, ...parents: string[]): GraphCommit => ({ hash, parents })

describe("提交图泳道布局", () => {
  test("线性历史：单车道贯通，首行是分支头（上方无入线）、末行是根提交（下方无出线）", () => {
    const { rows, cols } = layoutCommitGraph([c("C3", "C2"), c("C2", "C1"), c("C1")])
    expect(cols).toBe(1)
    expect(rows.map((r) => r.lane)).toEqual([0, 0, 0])
    expect(rows.map((r) => r.color)).toEqual([0, 0, 0])
    // 首行：只有从节点向下到父的线
    expect(rows[0]!.edges).toEqual([{ fromLane: 0, toLane: 0, from: "node", to: "bottom", color: 0 }])
    // 中间行：上接父、下连父
    expect(rows[1]!.edges).toEqual([
      { fromLane: 0, toLane: 0, from: "top", to: "node", color: 0 },
      { fromLane: 0, toLane: 0, from: "node", to: "bottom", color: 0 },
    ])
    // 根提交：只有入线，线到此结束
    expect(rows[2]!.root).toBe(true)
    expect(rows[2]!.edges).toEqual([{ fromLane: 0, toLane: 0, from: "top", to: "node", color: 0 }])
  })

  test("合并提交：第二父另开车道且颜色不同，两条线在共同祖先处汇聚", () => {
    const { rows, cols } = layoutCommitGraph([c("M", "A", "B"), c("A", "R"), c("B", "R"), c("R")])
    expect(cols).toBe(2)
    expect(rows[0]!.merge).toBe(true)
    const mergeEdges = rows[0]!.edges
    expect(mergeEdges).toHaveLength(2)
    expect(mergeEdges.map((e) => e.toLane).sort()).toEqual([0, 1])
    // 并行分支不同色（同屏两条线不该同色）
    expect(mergeEdges[0]!.color).not.toBe(mergeEdges[1]!.color)
    expect(new Set(mergeEdges.map((e) => e.color)).size).toBe(2)

    // A 行：自己在 0 道，B 的线在 1 道直通
    expect(rows[1]!.lane).toBe(0)
    expect(rows[1]!.edges).toContainEqual({ fromLane: 1, toLane: 1, from: "top", to: "bottom", color: 1 })

    // B 行：自己在 1 道，A 的线在 0 道直通
    expect(rows[2]!.lane).toBe(1)
    expect(rows[2]!.edges).toContainEqual({ fromLane: 0, toLane: 0, from: "top", to: "bottom", color: 0 })

    // 共同祖先：两条线汇入同一节点，第二道随之结束
    expect(rows[3]!.lane).toBe(0)
    expect(rows[3]!.edges).toEqual([
      { fromLane: 0, toLane: 0, from: "top", to: "node", color: 0 },
      { fromLane: 1, toLane: 0, from: "top", to: "node", color: 1 },
    ])
  })

  test("分叉后合流：车道在合流处回收，宽度不随分支数累加", () => {
    const { rows, cols } = layoutCommitGraph([c("M", "A", "B"), c("A", "M2"), c("B", "M2"), c("M2", "R"), c("R")])
    expect(cols).toBe(2)
    expect(rows[3]!.lane).toBe(0)
    // 两条线汇入同一节点后只剩一条（第二道已回收）
    expect(rows[4]!.lane).toBe(0)
    expect(rows[4]!.cols).toBe(1)
  })

  test("分支头：前后互不相关的两条独立分支各自从空车道起步（--all 场景）", () => {
    const { rows, cols } = layoutCommitGraph([c("X1", "X2"), c("X2"), c("Y1", "Y2"), c("Y2")])
    expect(cols).toBe(1)
    expect(rows.map((r) => r.lane)).toEqual([0, 0, 0, 0])
    expect(rows[0]!.edges).toEqual([{ fromLane: 0, toLane: 0, from: "node", to: "bottom", color: 0 }])
    expect(rows[1]!.root).toBe(true)
    // Y1 不是任何车道的期待对象 → 新线起点（上方无入线）
    expect(rows[2]!.edges).toEqual([{ fromLane: 0, toLane: 0, from: "node", to: "bottom", color: 0 }])
  })

  test("缺失父 + 已加载完：不画挂不到实处的线（按路径过滤时父链会断裂）", () => {
    const { rows, cols } = layoutCommitGraph([c("A", "missing"), c("B", "missing")], { complete: true })
    expect(cols).toBe(1)
    for (const r of rows) expect(r.edges).toEqual([])
  })

  test("缺失父 + 未加载完：线先留着（父可能在下一次分页里出现）", () => {
    const { rows, cols } = layoutCommitGraph([c("A", "missing"), c("B", "missing")], { complete: false })
    expect(rows[0]!.edges).toEqual([{ fromLane: 0, toLane: 0, from: "node", to: "bottom", color: 0 }])
    // 待出现的父占着 0 道，B 只能另开车道
    expect(rows[1]!.lane).toBe(1)
    expect(cols).toBe(2)
  })

  test("前缀稳定：加载更多不会重排已显示的行（分页是顺序追加的）", () => {
    const all = [c("M", "A", "B"), c("A", "R"), c("B", "R"), c("R2", "R"), c("R")]
    const page1 = all.slice(0, 3)
    const partial = layoutCommitGraph(page1, { complete: false })
    const full = layoutCommitGraph(all, { complete: false })
    expect(partial.rows).toEqual(full.rows.slice(0, page1.length))
    // 图列宽度是全列表的最大值，后续页可能更宽（只增不减，不会让已有行错位）
    expect(partial.cols).toBeLessThanOrEqual(full.cols)
    // 后一页里 R2 是新的分支头（R 已被两条线期待），只能在右侧另开一道
    expect(full.rows[3]!.lane).toBe(2)
    expect(full.cols).toBe(3)
  })

  test("颜色槽位号始终落在调色板范围内", () => {
    const many: GraphCommit[] = [c("H", "b1", "b2", "b3", "b4")]
    for (let i = 1; i <= 4; i++) many.push(c(`b${i}`, `b${i}-1`), c(`b${i}-1`, "R"))
    many.push(c("R"))
    const { rows } = layoutCommitGraph(many)
    for (const r of rows) {
      expect(r.color).toBeGreaterThanOrEqual(0)
      expect(r.color).toBeLessThan(GRAPH_COLORS)
      for (const e of r.edges) expect(e.color).toBeLessThan(GRAPH_COLORS)
    }
  })
})

describe("提交图连线路径", () => {
  const geo = { laneW: 14, rowH: 44 }

  test("同车道是竖直线段", () => {
    expect(graphEdgePath({ fromLane: 0, toLane: 0, from: "top", to: "bottom", color: 0 }, geo)).toBe("M7 0L7 44")
    expect(graphEdgePath({ fromLane: 1, toLane: 1, from: "top", to: "node", color: 0 }, geo)).toBe("M21 0L21 22")
  })

  test("向右分出：节点起平走再圆角下拐", () => {
    expect(graphEdgePath({ fromLane: 0, toLane: 1, from: "node", to: "bottom", color: 0 }, geo)).toBe(
      "M7 22L14 22Q21 22 21 29L21 44",
    )
  })

  test("向左汇入：上方竖下来再圆角拐进节点", () => {
    expect(graphEdgePath({ fromLane: 1, toLane: 0, from: "top", to: "node", color: 0 }, geo)).toBe(
      "M21 0L21 15Q21 22 14 22L7 22",
    )
  })
})
