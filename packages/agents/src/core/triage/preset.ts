/**
 * 研判的**任务配置**：把「每次调用都要重复传」的业务面参数收进一个文件——
 * 调用方从此只传 `items`（或其 `items_file`）。
 *
 * ```
 * {GEBAI_HOME}/triage/preset.json
 * {
 *   "target": "small",
 *   "threshold": 0.7,
 *   "min_evidence_chars": 8,
 *   "label_enum": ["无异常", "代码退出", "网络", "调度"],
 *   "accept_labels": ["无异常"],
 *   "schema": { "type": "object", "properties": { "label": { "type": "string" } }, "required": ["label"] }
 * }
 * ```
 *
 * **为什么是文件而不是环境变量**：这些是**任务配置**（schema、标签、提示词、阈值），不是启动参数。
 * 环境变量装它们的代价是实打实的：① JSON 塞进 shell 单引号后中文/引号转义极易出错，且报错发生在
 * 「调用时才炸」；② 改一次要重启服务；③ 会随 `job.json` 的选项快照散落进产物目录。
 * 文件则可以带注释地维护、改完即生效，也与本仓库既有约定一致（`profiles/*.json`、
 * `infer/config/profiles.json`、用户级 `todos.json`/`tasks.json`）。
 *
 * 仍然留在环境变量里的，是**部署/运维级**的东西：并发（`GEBAI_TRIAGE_L1_CONCURRENCY`）、
 * 全局在途上限（`GEBAI_TRIAGE_L1_GATE`）、REST 同步超时（`GEBAI_TRIAGE_SYNC_TIMEOUT_MS`）、
 * 限流（`GEBAI_TRIAGE_RATE_*`）、以及小模型端点本身（`GEBAI_LLM_INFER_TARGETS`）——
 * 它们随机器/部署走，不随研判任务走。
 *
 * 三个设计约束（为了让「预设」可预测，而不是再长出一套配置系统）：
 *   * **优先级恒为：显式调用参数 > 预设 > 内置缺省**。预设只是把「不传时的缺省」从硬编码换成可配置，
 *     不改变任何既有调用方的行为（文件不存在时，本模块完全不介入）。
 *   * **只管业务面**（schema/标签/提示词/阈值/目标选择/调优）。端点 URL 与密钥仍归
 *     `GEBAI_LLM_INFER_TARGETS` + `GEBAI_LLM_INFER_REMOTE_API_KEY`——否则密钥会出现在两个地方、
 *     掩码与审计口径也就分裂了。`preset.target` 只填**目标名**（如 `"small"`），不填 URL。
 *   * **不接收 `items`/`items_file`/`job_id`/`mode` 等调用侧字段**：预设是「配置」，不是「数据」。
 *     误填一律**报错**（而不是静默忽略）——静默忽略会让人以为配上了、实际没生效。
 */
import { existsSync, readFileSync } from "node:fs"
import { isAbsolute, join } from "node:path"
import { TRIAGE_PARAM_NAMES, TRIAGE_PARAM_SPECS, type TriageParamSpec } from "./params"

/** 预设可配置的字段（= 参数表里的业务面字段）。 */
export const TRIAGE_PRESET_FIELDS: string[] = [
  "target",
  "target_api_key",
  "schema",
  "label_enum",
  "threshold",
  "min_evidence_chars",
  "accept_labels",
  "escalate",
  "l1_model",
  "l1_system",
  "l1_prompt_template",
  "l1_concurrency",
  "l1_max_tokens",
  "l1_temperature",
  "l1_reminders",
  "l1_timeout_ms",
  "l1_retries",
  "l1_retry_delay_ms",
  "l1_gate",
  "l1_enable_thinking",
  "l2_agents",
  "l2_model",
  "l2_api_base",
  "l2_api_key",
  "l2_system",
  "l2_max_items",
  "l2_batch_size",
  "l2_timeout_ms",
]

/** 预设**不接受**的字段（调用侧数据 / 通道专属）——误填报错并说明为什么。 */
export const TRIAGE_PRESET_REJECTED: Record<string, string> = {
  items: "条目数据属于调用方输入，不放预设",
  items_file: "条目数据属于调用方输入，不放预设",
  job_id: "任务标识按每次调用生成，不放预设",
  job_dir: "产物目录属于调用侧，不放预设",
  result_limit: "返回条数属于调用侧视图，不放预设",
  mode: "执行模式是 REST 通道专属能力，不放预设",
  sync_timeout_ms: "同步超时是 REST 通道专属能力，不放预设",
}

/** 文件顶层的编排键（不参与字段校验）。 */
const PRESET_META_KEYS = ["$schema", "$comment", "name", "description"] as const

/** 预设文件相对 `{GEBAI_HOME}` 的缺省位置。 */
export const TRIAGE_PRESET_REL = join("triage", "preset.json")

/** 预设解析结果：`fields` 为已校验的扁平字段（可直接当调用参数的最底层缺省）。 */
export interface TriagePresetParse {
  fields: Record<string, unknown>
  /** 非致命问题（未知键被忽略等）——调用方应如实展示，别让配置错得无声无息。 */
  warnings: string[]
  /** 致命问题（JSON 非法 / 不是对象 / 填了不该填的字段）。 */
  error?: string
  /** 文件路径（从文件载入时）。 */
  path?: string
}

/**
 * 解析预设对象（容错但**不静默**）：语法与键名问题一律回报。
 * 错误时返回 `error`（调用方据此让本次调用失败，而不是带着半套配置往下跑）。
 */
export function parseTriagePreset(raw: unknown, path?: string): TriagePresetParse {
  const empty: TriagePresetParse = { fields: {}, warnings: [], ...(path ? { path } : {}) }
  if (raw === undefined || raw === null) return empty
  if (typeof raw === "string") {
    const text = raw.trim()
    if (!text) return empty
    try {
      return parseTriagePreset(JSON.parse(text), path)
    } catch (e) {
      return { ...empty, error: `预设不是合法 JSON${path ? `（${path}）` : ""}：${(e as Error).message}` }
    }
  }
  if (typeof raw !== "object" || Array.isArray(raw)) {
    const kind = Array.isArray(raw) ? "数组" : typeof raw
    return { ...empty, error: `预设必须是 JSON 对象，当前是${kind}${path ? `（${path}）` : ""}` }
  }

  const warnings: string[] = []
  const fields: Record<string, unknown> = {}
  const specByName = new Map<string, TriageParamSpec>(TRIAGE_PARAM_SPECS.map((s) => [s.name, s]))

  for (const [key, value] of Object.entries(raw as Record<string, unknown>)) {
    if ((PRESET_META_KEYS as readonly string[]).includes(key)) continue
    if (TRIAGE_PRESET_REJECTED[key]) {
      return { ...empty, error: `预设里有不该出现的字段 ${key}：${TRIAGE_PRESET_REJECTED[key]}。` }
    }
    if (!specByName.has(key)) {
      warnings.push(`未知字段 ${key} 已忽略（可用字段：${TRIAGE_PRESET_FIELDS.join(" / ")}）`)
      continue
    }
    if (value === undefined || value === null) continue
    fields[key] = value
  }
  return { fields, warnings, ...(path ? { path } : {}) }
}

/**
 * 解析预设文件路径：绝对路径直接用；其余一律相对 `{GEBAI_HOME}`（名字缺 `.json` 后缀则补上）。
 *
 * 为何与 `profiles` 的相对路径语义（相对进程 cwd）不同：预设文件就住在 `{GEBAI_HOME}` 下，
 * 相对 cwd 解析会让「同一个配置、服务从不同目录启动就找不到」，而预设的读者是调用方与部署方，
 * 不是站在 cwd 里的人。
 */
export function resolvePresetPath(ref: string, home: string): string {
  const v = ref.trim()
  if (isAbsolute(v)) return v
  return join(home, /\.json$/i.test(v) ? v : `${v}.json`)
}

export interface LoadPresetOptions {
  /** 数据根（`{GEBAI_HOME}`）。 */
  home: string
  /**
   * 文件路径或名字（相对 `{GEBAI_HOME}`；缺省 `triage/preset.json`）。
   * 供未来扩展多套预设时按名选用；当前通道不暴露该开关（见 DESIGN「研判任务配置」）。
   */
  ref?: string
  /** 读文本（走宿主守卫的通道可注入；缺省 node:fs）。 */
  readText?: (absPath: string) => string
}

/**
 * 从文件载入预设。**每次调用重读**（不缓存）：改完即生效，不必重启服务；
 * 文件不存在返回空预设（不报错——没配就是没配，与旧行为一致）。
 */
export function loadTriagePreset(o: LoadPresetOptions): TriagePresetParse {
  const path = o.ref ? resolvePresetPath(o.ref, o.home) : join(o.home, TRIAGE_PRESET_REL)
  const read = o.readText ?? ((p: string) => readFileSync(p, "utf-8"))
  if (!existsSync(path)) return { fields: {}, warnings: [], path }
  let text: string
  try {
    text = read(path)
  } catch (e) {
    return { fields: {}, warnings: [], path, error: `预设文件读取失败（${path}）：${(e as Error).message}` }
  }
  const parsed = parseTriagePreset(text, path)
  return { ...parsed, path }
}

/** 参数名清单与预设字段的对齐检查（供测试与文档生成）。 */
export function presetFieldCoverage(): { covered: string[]; missing: string[] } {
  const covered = TRIAGE_PARAM_NAMES.filter((n) => TRIAGE_PRESET_FIELDS.includes(n) || TRIAGE_PRESET_REJECTED[n])
  return { covered, missing: TRIAGE_PARAM_NAMES.filter((n) => !covered.includes(n)) }
}
