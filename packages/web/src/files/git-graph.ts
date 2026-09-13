/**
 * 提交图的泳道布局（纯逻辑，无 DOM）。
 *
 * 输入必须是**拓扑序**的提交列表（父提交一定排在子提交之后，服务端 `git log --date-order` 保证），
 * 输出每一行的节点车道、可见连线与颜色槽位。日志是**顺序追加**分页的，而布局逐行推导
 * （第 i 行只取决于前 i 行）——「加载更多」不会让已显示的行重新排布，这是本模块的硬约束。
 *
 * 车道模型：一条车道 = 「还欠着某个提交一条线」，记着期待的提交 hash 与自己的颜色槽位。
 * 提交出现时它所在的车道成为节点，其父接管该车道（首个还会出现的父）或另开车道（合并的其余父）；
 * 被消费的车道腾出槽位可被后续新线复用，尾部空槽裁掉——宽度不随历史长度增长。
 */

/** 颜色槽位数——渲染层的调色板长度必须与之相同。 */
export const GRAPH_COLORS = 10

/** 布局输入（按拓扑序排列）。 */
export interface GraphCommit {
  hash: string
  parents: string[]
}

/** 行内锚点：行顶 / 节点中心 / 行底。 */
export type GraphAnchor = "top" | "node" | "bottom"

/** 一条连线在本行内的可见段。 */
export interface GraphEdge {
  fromLane: number
  toLane: number
  from: GraphAnchor
  to: GraphAnchor
  /** 颜色槽位（0 .. GRAPH_COLORS-1） */
  color: number
}

export interface GraphRow {
  /** 节点所在车道 */
  lane: number
  /** 节点颜色槽位 */
  color: number
  /** 合并提交（多父） */
  merge: boolean
  /** 根提交（无父） */
  root: boolean
  edges: GraphEdge[]
  /** 本行用到的车道数 */
  cols: number
}

export interface GraphLayout {
  rows: GraphRow[]
  /** 全列表最大车道数——图列宽度按它统一，各行的节点才不会左右错位 */
  cols: number
}

/** 像素几何（必须与 CSS 中日志行的内容高度一致，连线才能跨行严丝合缝）。 */
export interface GraphGeometry {
  laneW: number
  rowH: number
}

export function layoutCommitGraph(commits: GraphCommit[], opts: { complete?: boolean } = {}): GraphLayout {
  // complete = 日志已加载到末尾（没有更多页）；据此决定「后面不会出现的父」要不要继续留线
  const complete = opts.complete ?? true
  const at = new Map<string, number>()
  commits.forEach((c, i) => {
    if (!at.has(c.hash)) at.set(c.hash, i)
  })

  const lanes: Array<{ hash: string; color: number } | null> = []
  const rows: GraphRow[] = []
  let cols = 1

  /** 这个父提交后面还会出现吗？不会再出现的父不画线——按路径过滤时父链常断裂，否则线会挂满整列表。 */
  const pending = (hash: string, i: number): boolean => {
    const seen = at.get(hash)
    if (seen !== undefined && seen > i) return true
    return !complete // 还没加载完：可能在下一次分页里出现，先留着
  }

  /** 找一个空槽（必要时尾部追加）。被占用的车道不可复用，否则穿过本行的线会被截断。 */
  const alloc = (): number => {
    for (let i = 0; i < lanes.length; i++) if (!lanes[i]) return i
    lanes.push(null)
    return lanes.length - 1
  }

  /** 颜色槽位：优先用当前没被占用的，避免同屏两条线同色。 */
  const pickColor = (): number => {
    const used = new Set<number>()
    for (const l of lanes) if (l) used.add(l.color)
    for (let c = 0; c < GRAPH_COLORS; c++) if (!used.has(c)) return c
    return lanes.length % GRAPH_COLORS
  }

  commits.forEach((c, i) => {
    const before = lanes.slice()
    // 指向本提交的车道：多个分支汇到同一提交时不止一条
    const entrants: number[] = []
    before.forEach((l, k) => {
      if (l && l.hash === c.hash) entrants.push(k)
    })
    let lane = entrants.length ? entrants[0]! : -1
    if (lane < 0) {
      // 分支头：一条新线的起点（上方无入线）
      lane = alloc()
      lanes[lane] = { hash: c.hash, color: pickColor() }
    }
    const color = lanes[lane]!.color
    // 除节点车道外，其余汇入的车道到此结束
    for (const k of entrants) if (k !== lane) lanes[k] = null

    const parents = c.parents.filter((p) => p && p !== c.hash)
    // 首个「后面还会出现」的父接管节点车道；一个都没有时线就此终止
    let owner = -1
    for (let k = 0; k < parents.length; k++) {
      if (pending(parents[k]!, i)) {
        owner = k
        break
      }
    }
    lanes[lane] = owner >= 0 ? { hash: parents[owner]!, color } : null
    for (let k = 0; k < parents.length; k++) {
      if (k === owner || !pending(parents[k]!, i)) continue
      // 已有车道通向这个父：两条线稍后在父提交处自然汇合，无需再开一条
      if (lanes.some((l) => l?.hash === parents[k])) continue
      const slot = alloc()
      lanes[slot] = { hash: parents[k]!, color: pickColor() }
    }
    while (lanes.length && !lanes[lanes.length - 1]) lanes.pop()

    // 本行可见的连线：汇入节点 / 穿过本行 / 从节点分向父
    const edges: GraphEdge[] = []
    const width = Math.max(before.length, lanes.length)
    for (let k = 0; k < width; k++) {
      const inLane = before[k] ?? null
      const outLane = lanes[k] ?? null
      if (inLane) {
        edges.push(
          inLane.hash === c.hash
            ? { fromLane: k, toLane: lane, from: "top", to: "node", color: inLane.color }
            : { fromLane: k, toLane: k, from: "top", to: "bottom", color: inLane.color },
        )
      }
      // 同一车道换了对象 = 新线（槽位被消费后又复用）
      if (outLane && before[k] !== outLane) {
        edges.push({ fromLane: lane, toLane: k, from: "node", to: "bottom", color: outLane.color })
      }
    }
    const rowCols = Math.max(width, lane + 1, 1)
    cols = Math.max(cols, rowCols)
    rows.push({ lane, color, merge: parents.length > 1, root: parents.length === 0, edges, cols: rowCols })
  })

  return { rows, cols }
}

/** 一条连线的 SVG path（坐标为像素，原点在本行左上角）。 */
export function graphEdgePath(e: GraphEdge, geo: GraphGeometry): string {
  const n = (v: number): number => Math.round(v * 10) / 10
  const cx = (lane: number): number => lane * geo.laneW + geo.laneW / 2
  const mid = geo.rowH / 2
  const y = (a: GraphAnchor): number => (a === "top" ? 0 : a === "node" ? mid : geo.rowH)
  const x0 = cx(e.fromLane)
  const x1 = cx(e.toLane)
  const y0 = y(e.from)
  const y1 = y(e.to)
  if (x0 === x1) return `M${n(x0)} ${n(y0)}L${n(x0)} ${n(y1)}`
  const r = Math.min(geo.laneW / 2, mid / 2, Math.abs(x1 - x0))
  const dir = x1 > x0 ? 1 : -1
  if (e.from === "top") {
    // 从上方竖下来，再圆角拐进节点
    return `M${n(x0)} ${n(y0)}L${n(x0)} ${n(mid - r)}Q${n(x0)} ${n(mid)} ${n(x0 + dir * r)} ${n(mid)}L${n(x1)} ${n(mid)}`
  }
  // 从节点平着走到目标车道，再圆角拐下去
  return `M${n(x0)} ${n(mid)}L${n(x1 - dir * r)} ${n(mid)}Q${n(x1)} ${n(mid)} ${n(x1)} ${n(mid + r)}L${n(x1)} ${n(y1)}`
}
