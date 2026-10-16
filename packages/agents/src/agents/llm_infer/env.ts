/**
 * llm_infer 环境变量读取（`GEBAI_LLM_INFER_*`，兼容旧名 `LOCAL_INFER_*`）。
 *
 * **为什么要双读**：这些变量是部署面配置（`.env`、docker compose、任务级 env 都是持久化的），
 * 改名后若只认新名，存量部署会**静默失效**——服务照常启动，只是档位/目标/目录全回到缺省，
 * 排查成本极高。因此新名优先、旧名照旧可用。
 *
 * **弃用怎么"不静默"**：agents 包没有日志设施，且这些变量多半在工具调用时才被读到——
 * 所以把「用了旧名」记进一份可查询的清单，由工具输出直接展示给模型/用户（比只写服务器日志
 * 更容易被看见）。不抛错、不刷屏：改名是渐进迁移，不该让旧部署报错。
 *
 * 统一入口的意义：让「双读 + 提示」只有一处实现。否则每处 `env.GEBAI_LLM_INFER_X` 各写一遍兼容逻辑，
 * 迟早漏掉一处——而漏掉的表现就是某个配置静默失效。
 */

/** 旧前缀与新前缀。 */
export const ENV_OLD_PREFIX = "LOCAL_INFER_"
export const ENV_NEW_PREFIX = "GEBAI_LLM_INFER_"

/** 本次进程内命中过旧名的短名（去重展示，避免同一行刷屏）。 */
const deprecatedUsed = new Set<string>()

/**
 * 读一个 llm_infer 环境变量：先查新名 `GEBAI_LLM_INFER_<名>`，其次旧名 `GEBAI_LLM_INFER_<名>`。
 *
 * @param env 环境（会话/任务 env 或进程 env）
 * @param suffix 短名，如 `"HOME"` / `"TARGETS"`（不含前缀）
 */
export function envVar(env: Record<string, string> | undefined, suffix: string): string | undefined {
  if (!env) return undefined
  const next = env[`${ENV_NEW_PREFIX}${suffix}`]
  if (typeof next === "string" && next.trim() !== "") return next
  const old = env[`${ENV_OLD_PREFIX}${suffix}`]
  if (typeof old === "string" && old.trim() !== "") {
    deprecatedUsed.add(suffix)
    return old
  }
  return undefined
}

/** 是否配置了该变量（新旧任一，不取值）。 */
export function hasEnvVar(env: Record<string, string> | undefined, suffix: string): boolean {
  return envVar(env, suffix) !== undefined
}

/** 数字型变量读取（非法/缺省返回 undefined）。 */
export function envNum(env: Record<string, string> | undefined, suffix: string): number | undefined {
  const v = envVar(env, suffix)
  if (v === undefined) return undefined
  const n = Number(v)
  return Number.isFinite(n) ? n : undefined
}

/** 工具输出用：旧名弃用提示行（无则空数组）。 */
export function envDeprecationLines(env: Record<string, string> | undefined): string[] {
  const hits = new Set(deprecatedUsed)
  // 诊断时顺带把「环境里现在用着旧名」的全找出来（不依赖是否已被读过）
  for (const k of Object.keys(env ?? {})) {
    if (k.startsWith(ENV_OLD_PREFIX)) hits.add(k.slice(ENV_OLD_PREFIX.length))
  }
  if (!hits.size) return []
  return [
    `环境变量已改名：${[...hits].sort().map((s) => `${ENV_OLD_PREFIX}${s} → ${ENV_NEW_PREFIX}${s}`).join("；")}` +
      "（旧名仍可读，但建议尽快改过来）",
  ]
}

/** 测试用：清空弃用记录（否则同进程内多次断言互相影响）。 */
export function resetEnvDeprecations(): void {
  deprecatedUsed.clear()
}
