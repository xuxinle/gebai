/** 自动审批补齐的纯逻辑（可单测，不碰 DOM/localStorage）。
 *
 * 为什么单独成模块：审批跳过的会话 env 只活在服务端内存里，进程重启/断线重连后需要对**全部在跑会话**
 * 补写一次（不只当前会话——定时任务、飞书桥接、其他标签页发起的会话没有「被打开」的时机）。
 * 「要补哪些会话」这个决定本身是纯函数，抽出来才能钉住边界。 */

/**
 * 需要补齐审批跳过的会话 id 列表。
 *
 * 语义：快照里的在跑会话 ∪ 当前会话（当前会话在快照里可能缺失——它可能是本页刚建的、快照尚未刷新）。
 * 去重保序（服务端 env 写入是幂等的，但重复请求没有价值）；空串/临时 id 一律剔除。
 */
export function approvalSkipTargets(running: readonly string[], currentId?: string): string[] {
  const out: string[] = []
  const seen = new Set<string>()
  for (const id of [...running, ...(currentId ? [currentId] : [])]) {
    const v = typeof id === "string" ? id.trim() : ""
    if (!v || seen.has(v)) continue
    seen.add(v)
    out.push(v)
  }
  return out
}
