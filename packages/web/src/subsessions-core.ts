/**
 * 子会话观察的纯逻辑（无 DOM）：状态文案、单行入口摘要、时长格式与列表排序。
 *
 * 同一套口径服务两处展示——消息流里的单行入口（messages.ts 的 `subSessionEntry`）与
 * 观察面板的行/详情头（subsessions.ts），两处文案必须一致：同一个 run 在流里看到
 * 「3 轮 · 5 工具」，点开面板不该变成另一个数字。
 */

/** 运行状态：只有「进行中」与「已结束」两态——服务端 done 事件不带失败位（失败/取消/终止都是
 *  空输出收尾），前端把它渲染成「无返回」而非臆测成功或失败。 */
export type SubRunStatus = "running" | "done"

/** 展示所需的运行状态快照（记录本体与测试桩共用）。 */
export interface SubRunStateView {
  status: SubRunStatus
  rounds: number
  toolCalls: number
  merges: number
  startedAt: number
  endedAt?: number
  output?: string
}

/** 时长：<1s 用「<1s」，<60s 整秒，<1h 分秒，其余时分。 */
export function formatElapsed(ms: number): string {
  const total = Math.max(0, Math.round(ms / 1000))
  if (ms < 1000) return "<1s"
  if (total < 60) return `${total}s`
  const m = Math.floor(total / 60)
  const s = total % 60
  if (m < 60) return s ? `${m}m${s}s` : `${m}m`
  const h = Math.floor(m / 60)
  return `${h}h${m % 60}m`
}

/** 状态词：运行中 / 完成（空输出时由调用方另注「无返回」）。 */
export function statusText(status: SubRunStatus): string {
  return status === "running" ? "运行中" : "完成"
}

/** 输出摘要：去掉推理块、压平空白，超长保留头尾（避免只看到开头的前缀）。
 *  单行入口的「→ 摘要」与面板行共用——摘要只作方向指示，全文在详情里。 */
export function inlineSummary(text: string, max = 56): string {
  const flat = (text ?? "").replace(/<think>[\s\S]*?<\/think>/g, "").replace(/\s+/g, " ").trim()
  if (flat.length <= max) return flat
  return `${flat.slice(0, Math.floor(max * 0.7))}…${flat.slice(-Math.floor(max * 0.25))}`
}

/** 单行状态摘要（消息流入口 / 面板行）：
 *  - 运行中：`运行中 · 3 轮 · 5 工具`（有阶段合入时追加 `· 合入 2`）
 *  - 完成：`完成 · 42s · 3 轮 · 5 工具 → 摘要`（无输出时 `→ 无返回`）
 *  轮次/工具为 0 时省略对应段——刚启动就显示「0 轮」纯属噪声。
 *  耗时只在**真知道**时展示：历史存档（服务重启前/其他客户端的运行）只有输出没有起止时刻，
 *  拿当前时间当起止会显示「<1s」这种假数据，宁可不显示。 */
export function runStateLine(v: SubRunStateView): string {
  const parts: string[] = [statusText(v.status)]
  if (v.status === "done" && v.endedAt !== undefined) parts.push(formatElapsed(v.endedAt - v.startedAt))
  if (v.rounds > 0) parts.push(`${v.rounds} 轮`)
  if (v.toolCalls > 0) parts.push(`${v.toolCalls} 工具`)
  if (v.merges > 0) parts.push(`合入 ${v.merges}`)
  const head = parts.join(" · ")
  if (v.status === "running") return head
  const out = inlineSummary(v.output ?? "")
  return `${head} → ${out || "无返回"}`
}

/** 运行记录排序：运行中的在前（先开始的在上），已结束的按结束时间倒序（最近的在上）。
 *  面板一列里「正在跑的」必须永远在可视区顶部——否则并发多个 run 时要翻到底才看得到活的。 */
export function sortRuns<T extends { status: SubRunStatus; startedAt: number; endedAt?: number }>(list: T[]): T[] {
  return [...list].sort((a, b) => {
    if (a.status !== b.status) return a.status === "running" ? -1 : 1
    if (a.status === "running") return a.startedAt - b.startedAt
    return (b.endedAt ?? b.startedAt) - (a.endedAt ?? a.startedAt)
  })
}
