/**
 * 领域专用模式档案（`GEBAI_PROFILE`）：把「启动裁剪」的各类清单收敛进一个可复用文件——
 * 部署方按领域（编码 / 文档 / 运维 / 客服……）产出档案，启动时一个变量切换能力面：
 * 全局提示词段落裁剪与领域补充、全局工具启停、子Agent 启停与预载。
 *
 * 档案提供**默认值**，显式环境变量按字段覆盖（空 = 未配置即用档案值，见 `loadConfig`）；
 * 启动配置错误（档案缺失 / JSON 非法 / 字段类型不符 / 未知字段）在启动期直接抛错，
 * 不静默降级——配错档案却按默认能力面运行，比启动失败难排查得多。
 */
import { existsSync, readFileSync } from "node:fs"
import { isAbsolute, join, resolve } from "node:path"

/** 档案内的提示词声明（对应 `GEBAI_PROMPT_ENABLE`/`GEBAI_PROMPT_DISABLE`/`GEBAI_PROMPT_EXTRA`/`GEBAI_PROMPT_EXTRA_FILE`）。 */
export interface DomainProfilePrompt {
  /** 段落白名单（非空时仅注入名单内段落）。 */
  enable?: string[]
  /** 段落黑名单（先白后黑）。 */
  disable?: string[]
  /** 领域补充提示词（追加在全局提示词末尾）。 */
  extra?: string
  /** 领域补充提示词文件（相对路径相对档案文件所在目录）。 */
  extra_file?: string
}

/** 档案内的工具声明（对应 `GEBAI_TOOL_ENABLE`/`GEBAI_TOOL_DISABLE`）。 */
export interface DomainProfileTools {
  enable?: string[]
  disable?: string[]
}

/** 档案内的子Agent 声明（对应 `GEBAI_SUB_AGENTS_ENABLE`/`GEBAI_SUB_AGENTS_DISABLE`/`GEBAI_PRELOAD_SUB_AGENTS`）。 */
export interface DomainProfileSubAgents {
  enable?: string[]
  disable?: string[]
  preload?: string[]
}

export interface DomainProfile {
  /** 档案名（与文件名无关，便于日志与档案清单辨认）。 */
  name: string
  description?: string
  prompt?: DomainProfilePrompt
  tools?: DomainProfileTools
  sub_agents?: DomainProfileSubAgents
}

/** 载入结果：档案本体 + 文件路径（`prompt.extra_file` 相对基准）。 */
export interface LoadedDomainProfile {
  profile: DomainProfile
  path: string
}

const TOP_KEYS = ["name", "description", "prompt", "tools", "sub_agents"] as const
const PROMPT_KEYS = ["enable", "disable", "extra", "extra_file"] as const
const TOOLS_KEYS = ["enable", "disable"] as const
const SUB_AGENT_KEYS = ["enable", "disable", "preload"] as const

/**
 * 档案路径解析：含路径分隔符或以 `.json` 结尾 → 按文件路径（绝对路径直接用，相对路径相对进程 cwd）；
 * 否则视为档案名 → `{GEBAI_HOME}/profiles/{名}.json`。
 */
export function resolveProfilePath(ref: string, gebaiHome: string): string {
  const v = ref.trim()
  if (isAbsolute(v) || /[\\/]/.test(v) || /\.json$/i.test(v)) return resolve(v)
  return join(gebaiHome, "profiles", `${v}.json`)
}

function fail(msg: string): never {
  throw new Error(`GEBAI_PROFILE ${msg}`)
}

function strList(v: unknown, where: string): string[] | undefined {
  if (v === undefined) return undefined
  if (!Array.isArray(v) || v.some((x) => typeof x !== "string")) fail(`${where} 必须是字符串数组`)
  return (v as string[]).map((s) => s.trim()).filter(Boolean)
}

function str(v: unknown, where: string): string | undefined {
  if (v === undefined) return undefined
  if (typeof v !== "string") fail(`${where} 必须是字符串`)
  return v.trim() || undefined
}

function obj(v: unknown, where: string): Record<string, unknown> | undefined {
  if (v === undefined) return undefined
  if (!v || typeof v !== "object" || Array.isArray(v)) fail(`${where} 必须是对象`)
  return v as Record<string, unknown>
}

/** 未知字段校验（字段名拼错会静默失效，启动期直接拒绝并列出可用字段）。 */
function checkKeys(o: Record<string, unknown> | undefined, allowed: readonly string[], where: string): void {
  if (!o) return
  for (const k of Object.keys(o)) {
    if (!allowed.includes(k)) fail(`${where} 含未知字段 "${k}"（可用: ${allowed.join(" / ")}）`)
  }
}

/** 载入领域档案（档案缺失 / JSON 非法 / 字段类型不符 / 未知字段直接抛错）。 */
export function loadDomainProfile(ref: string, gebaiHome: string): LoadedDomainProfile {
  const path = resolveProfilePath(ref, gebaiHome)
  if (!existsSync(path)) fail(`档案不存在: ${path}`)
  let raw: unknown
  try {
    raw = JSON.parse(readFileSync(path, "utf8"))
  } catch (err) {
    fail(`档案 JSON 解析失败（${path}）: ${err instanceof Error ? err.message : String(err)}`)
  }
  const root = obj(raw, "档案")!
  checkKeys(root, TOP_KEYS, "档案")
  const name = str(root.name, "档案 name") ?? fail("档案缺少 name（字符串）")
  const prompt = obj(root.prompt, "档案 prompt")
  const tools = obj(root.tools, "档案 tools")
  const subs = obj(root.sub_agents, "档案 sub_agents")
  checkKeys(prompt, PROMPT_KEYS, "档案 prompt")
  checkKeys(tools, TOOLS_KEYS, "档案 tools")
  checkKeys(subs, SUB_AGENT_KEYS, "档案 sub_agents")
  return {
    path,
    profile: {
      name,
      description: str(root.description, "档案 description"),
      ...(prompt
        ? {
            prompt: {
              enable: strList(prompt.enable, "档案 prompt.enable"),
              disable: strList(prompt.disable, "档案 prompt.disable"),
              extra: str(prompt.extra, "档案 prompt.extra"),
              extra_file: str(prompt.extra_file, "档案 prompt.extra_file"),
            },
          }
        : {}),
      ...(tools ? { tools: { enable: strList(tools.enable, "档案 tools.enable"), disable: strList(tools.disable, "档案 tools.disable") } } : {}),
      ...(subs
        ? {
            sub_agents: {
              enable: strList(subs.enable, "档案 sub_agents.enable"),
              disable: strList(subs.disable, "档案 sub_agents.disable"),
              preload: strList(subs.preload, "档案 sub_agents.preload"),
            },
          }
        : {}),
    },
  }
}

/** 领域补充提示词文件内容（相对路径相对 baseDir；缺失直接抛错——漏读会静默丢失领域约束）。 */
export function readExtraFile(path: string, baseDir: string): string {
  const p = isAbsolute(path) ? path : resolve(baseDir, path)
  if (!existsSync(p)) fail(`提示词文件不存在: ${p}`)
  return readFileSync(p, "utf8")
}

/** 领域补充提示词拼接：文件内容在前、内联文本在后；两者皆空返回 undefined。 */
export function combinePromptExtra(fileText: string | undefined, inlineText: string | undefined): string | undefined {
  const parts = [fileText?.trim(), inlineText?.trim()].filter((s): s is string => !!s)
  return parts.length ? parts.join("\n") : undefined
}
