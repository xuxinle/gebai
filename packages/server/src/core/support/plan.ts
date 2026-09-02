/** 计划文档助手（自 core/tools.ts 抽取；ask 计划审批与前端展示双端同规则）。 */

/** 计划文档存储目录（会话 tmp/plans/，与其它文件工具同一路径基准，随会话文件面板可见）。 */
export const PLAN_DIR = "plans"

/** 计划文件名清洗：仅保留字母/数字/下划线/连字符，其余替换为 `-`，空标题回退 `plan`（直接作文件名，防路径穿越）。 */
export function planFileName(title: string): string {
  const slug = String(title ?? "")
    .replace(/[^\p{L}\p{N}_-]+/gu, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 60)
  return `${slug || "plan"}.md`
}

/** 组装计划 Markdown：content 提供时原样使用，否则由 title + steps 拼装（勾选清单；前端展示与落盘同构，双端同规则）。 */
export function buildPlanMarkdown(title: string, steps: string[], content?: string): string {
  const body = content && String(content).trim() ? String(content).trim() : ["# " + title, "", "## 执行计划", ""].concat(steps.map((s) => `- [ ] ${s}`)).join("\n")
  return body
}
