/**
 * llm_infer 环境变量读取（`GEBAI_LLM_INFER_*`）。
 *
 * 统一入口的意义：变量名前缀只在这一处拼——若各处各写 `env.GEBAI_LLM_INFER_X`，
 * 前缀改动时会漏掉某处，而漏掉的表现是某个配置静默失效。
 */

/** 变量前缀。 */
export const ENV_PREFIX = "GEBAI_LLM_INFER_"

/**
 * 读一个 llm_infer 环境变量（`GEBAI_LLM_INFER_<名>`）。
 *
 * @param env 环境（会话/任务 env 或进程 env）
 * @param suffix 短名，如 `"HOME"` / `"TARGETS"`（不含前缀）
 */
export function envVar(env: Record<string, string> | undefined, suffix: string): string | undefined {
  if (!env) return undefined
  const v = env[`${ENV_PREFIX}${suffix}`]
  return typeof v === "string" && v.trim() !== "" ? v : undefined
}

/** 数字型变量读取（非法/缺省返回 undefined）。 */
export function envNum(env: Record<string, string> | undefined, suffix: string): number | undefined {
  const v = envVar(env, suffix)
  if (v === undefined) return undefined
  const n = Number(v)
  return Number.isFinite(n) ? n : undefined
}
