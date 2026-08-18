/**
 * 安全模式风险工具判定（DESIGN「安全模式」）：全局工具精确名 + 子Agent 工具短名
 * （{agent}_sh → sh 等，按最后一段匹配）。命令执行（sh/py/js）/写删文件/定时任务调度均为修改类操作，安全模式下只读可用。
 * 引擎主/子循环、flow 数据流编排工具与 js 脚本工具 RPC 分发层共用（后两者直接调 rt.tool.execute 不经引擎拦截，须同规则判定）。
 */
export const SAFE_MODE_RISKY_TOOLS = new Set([
  "sh", "py", "js", "write", "edit", "patch", "file", "delete",
  "cron_add", "cron_update", "cron_remove",
])

/** 安全模式风险工具判定：精确名命中，或子Agent 工具剥离 `{agent}_` 前缀后短名命中（如 code_sh → sh）。 */
export function isRiskyToolName(name: string): boolean {
  if (SAFE_MODE_RISKY_TOOLS.has(name)) return true
  const short = name.split("_").pop() ?? ""
  return short !== name && SAFE_MODE_RISKY_TOOLS.has(short)
}

/** 安全模式拦截返回给模型的限制信息（引擎与 flow 工具共用，措辞一致）。 */
export function safeModeRestrictionMsg(name: string): string {
  return `安全模式：工具 ${name} 已限制（安全模式下仅允许只读操作，命令执行与文件修改类工具不可用）。请改用只读方式（如 read/grep/fetch_url 等），或直接给出分析与建议。`
}
