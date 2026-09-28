/**
 * 后台任务身份表（id → 身份描述）：工具参数里只有任务 id 时补上「在等什么」。
 *
 * 任务 id 本身无信息量（`t`/`s` + 8 位随机），身份只存在于服务端文本里——后台启动结果
 * （`sh` 的 `async:true`、`subsession_run` 的 `async:true`）与任务状态行（服务端
 * `shTaskLine`/`subSessionLine`，见 packages/server/src/core/tools/{exec,agent}.ts）。
 * 工具结果到达（appendToolResult）与历史加载（loadMessages）时把 id → 身份记入本表，
 * 参数区任务身份块（taskArgsBlock）查表补全：**等待中的卡片尚无输出，也一眼看出在等什么**；
 * 历史回放的卡片同样受益（登记在渲染之前完成，窗口化按需渲染不影响）。
 */

/** id → 身份描述（命令任务 `命令 <命令首行>`、子会话运行 `子会话「名」`）。 */
const labels = new Map<string, string>()

/** 表容量上限：超出按写入顺序淘汰最早的（一个会话内的任务数量有限，纯防无限增长）。 */
const MAX_LABELS = 500

function remember(id: string, label: string): void {
  const text = label.trim()
  if (!id || !text) return
  labels.delete(id) // 重新写入：刷新淘汰顺序，同时以最新身份覆盖
  labels.set(id, text)
  while (labels.size > MAX_LABELS) {
    const oldest = labels.keys().next().value
    if (oldest === undefined) break
    labels.delete(oldest)
  }
}

/** 任务 id 的身份描述（未登记为 undefined）。 */
export function taskLabel(id: string): string | undefined {
  return labels.get(id)
}

/** 清空身份表（测试用）。 */
export function clearTaskLabels(): void {
  labels.clear()
}

/* ---------- 服务端文本解析 ---------- */

/** 命令任务状态行：`taskId t1234abcd [running] 12s — <命令>`（含 `（exit N）`/终止标记等中间段）。 */
const SH_STATUS_LINE = /taskId[:：]?[ \t]+(t[0-9a-z]+)[ \t]+\[[^\]\n]*\][^\n]*?—[ \t]*([^\n]+)/g
/** 命令任务启动结果：`[后台任务已启动] taskId: t1234abcd\n命令: <命令>`。 */
const SH_START = /taskId[:：]?[ \t]+(t[0-9a-z]+)\n命令[:：][ \t]*([^\n]+)/g
/** 子会话状态行：`runId s1234abcd「名」 [running] …`。 */
const SUB_STATUS_LINE = /runId[:：]?[ \t]+(s[0-9a-z]+)「([^」\n]*)」/g
/** 子会话启动结果：`- 「名」 runId: s1234abcd（模型）· …`（名在 id 之前）。 */
const SUB_START = /「([^」\n]*)」[ \t]*runId[:：]?[ \t]+(s[0-9a-z]+)/g

/** 登记文本中出现的全部任务身份（任意工具输出/历史消息文本；无任务行时零开销返回）。 */
export function rememberTaskLabels(text: string): void {
  if (!text || (!text.includes("taskId") && !text.includes("runId"))) return
  for (const m of text.matchAll(SH_STATUS_LINE)) remember(m[1], `命令 ${m[2]}`)
  for (const m of text.matchAll(SH_START)) remember(m[1], `命令 ${m[2]}`)
  for (const m of text.matchAll(SUB_STATUS_LINE)) remember(m[1], `子会话「${m[2]}」`)
  for (const m of text.matchAll(SUB_START)) remember(m[2], `子会话「${m[1]}」`)
}
