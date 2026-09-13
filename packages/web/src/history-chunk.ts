/**
 * 历史分片渲染的切分规则（纯函数，无 DOM）：长会话首屏只渲染最近一段，更早历史由
 * 调用方分片补齐。切分必须尊重「子会话运行过程容器」的分组边界——同一 runId 的过程消息
 * 渲染进同一个折叠容器，起点切在组中间会把一次执行拆成两个容器（回放形态错乱）。
 */

/** 执行过程消息（子会话运行标记 subSession / 旧版 subAgent 存档）。 */
export interface RunMessageLike {
  subSession?: boolean
  subAgent?: boolean
  subSessionId?: string
  subAgentRunId?: string
}

/** 是否为执行过程消息（进折叠容器）。 */
export function isRunMessage(m: RunMessageLike | undefined): boolean {
  return !!m && (m.subSession === true || m.subAgent === true)
}

/** 执行过程消息的运行标识（新版 subSessionId / 旧版 subAgentRunId）。 */
export function runIdOfMessage(m: RunMessageLike): string | undefined {
  return m.subSessionId ?? m.subAgentRunId
}

/**
 * 首批渲染起点：尾部取 tailCount 条，并向前扩展到分组边界（同 runId 的过程消息整组入首批）。
 * 消息总数不超过 tailCount 时返回 0（全部渲染）。
 */
export function historySplitIndex(msgs: RunMessageLike[], tailCount: number): number {
  const n = msgs.length
  if (n <= tailCount) return 0
  let start = n - tailCount
  while (start > 0 && isRunMessage(msgs[start]) && isRunMessage(msgs[start - 1]) && runIdOfMessage(msgs[start]) === runIdOfMessage(msgs[start - 1])) start--
  return start
}
