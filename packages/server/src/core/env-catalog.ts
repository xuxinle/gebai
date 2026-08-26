/**
 * 环境变量配置目录：前端环境变量面板的白名单来源（不可自定义变量名）。
 * - 按「全局 / 各子Agent」分组展示；未配置的项前端显示为空、请求不携带
 * - 全局组为静态白名单；子Agent 组由各子Agent 导出的 `envVars` 声明汇总（见「子Agent文件格式」），
 *   不在此处硬编码——新增子Agent 环境变量只需在子Agent 定义中声明
 * - 只包含可运行时配置的项；启动级/安全敏感变量（GEBAI_MODE、GEBAI_ADMIN_PASSWORD_HASH、
 *   GEBAI_SAFE_MODE、GEBAI_SANDBOX、GEBAI_HOST/PORT 等）不在目录中，即不可配置
 */
import type { EnvCatalogVar, SubAgentDef } from "./types"

export type { EnvCatalogVar }

export interface EnvCatalogGroup {
  /** 分组标识：global=全局，其余为子Agent 名（与 `{AGENT_NAME_UPPER}_*` 前缀对应）。 */
  group: string
  /** 分组展示名。 */
  label: string
  vars: EnvCatalogVar[]
}

/** 全局可配置变量（含模型相关配置）。模型相关项为任务级生效：配置后覆盖服务端启动配置（按任务重建 Provider），
 *  未配置的项沿用服务端启动配置。 */
const GLOBAL_VARS: EnvCatalogVar[] = [
  { name: "GEBAI_LLM_MODEL", description: "主模型名称（如 deepseek-chat）；配置后本任务使用该模型，未配置用服务端启动配置" },
  { name: "GEBAI_LLM_API_BASE", description: "LLM 接口地址（OpenAI 兼容，如 https://api.deepseek.com）；覆盖服务端启动配置" },
  { name: "GEBAI_LLM_API_KEY", description: "LLM 接口密钥（敏感，仅本次任务临时注入，不落盘）" },
  { name: "GEBAI_LLM_API_KIND", description: "接口类型：openai（chat/completions）/ responses（OpenAI Responses）/ anthropic" },
  { name: "GEBAI_LLM_MAX_CONTEXT", description: "单次上下文最大 token 预算（数字，默认 128000）；覆盖服务端启动配置" },
  { name: "GEBAI_LLM_MAX_OUTPUT_TOKENS", description: "单次响应输出 token 上限（数字）：大文件生成截断防护；Anthropic 接口必填（缺省 8192）" },
  { name: "GEBAI_LLM_EXTRA_PARAMS", description: "额外模型接口参数（JSON 对象，如 {\"reasoning_effort\":\"high\"}），每次请求顶层合并" },
  { name: "GEBAI_LLM_MULTIMODAL", description: "主模型多模态能力声明：true/false（true 时图片附件 base64 内联进消息）；覆盖服务端启动配置" },
  { name: "GEBAI_VISION_MODEL", description: "视觉（外挂多模态）模型名称；配置后 vision 工具使用独立视觉 Provider（本任务生效）" },
  { name: "GEBAI_VISION_API_BASE", description: "视觉模型接口地址（缺省继承主模型）" },
  { name: "GEBAI_VISION_API_KEY", description: "视觉模型接口密钥（敏感，仅本次任务临时注入，不落盘）" },
  { name: "GEBAI_VISION_API_KIND", description: "视觉模型接口类型：openai / responses / anthropic（缺省同主模型）" },
  { name: "GEBAI_VISION_MAX_CONTEXT", description: "视觉模型上下文 token 预算（数字，默认 128000）" },
  { name: "GEBAI_VISION_EXTRA_PARAMS", description: "视觉模型额外请求体参数（JSON 对象）" },
  { name: "GEBAI_APPROVAL_SKIP", description: "会话级审批跳过：true/false（用户本人会话生效；非管理员仍受沙箱约束）" },
  { name: "GEBAI_SEARCH_PROVIDER", description: "web_search 工具的搜索服务：brave / serper / tavily（与 GEBAI_SEARCH_API_KEY 搭配）" },
  { name: "GEBAI_SEARCH_API_KEY", description: "搜索服务 API Key（敏感，仅本次任务临时注入，不落盘）" },
  { name: "GEBAI_MINIMAL_MODE", description: "会话级极简模式：true/false（仅启用 sh 与 edit 工具，其余工具从 schema 移除）" },
  { name: "HTTP_PROXY", description: "HTTP 代理地址（脚本/网络工具使用）" },
  { name: "HTTPS_PROXY", description: "HTTPS 代理地址（脚本/网络工具使用）" },
  { name: "NO_PROXY", description: "不走代理的地址列表（逗号分隔）" },
  { name: "LANG", description: "语言环境（如 zh_CN.UTF-8），影响脚本子进程输出编码" },
  { name: "TZ", description: "时区（如 Asia/Shanghai），影响定时任务与时间显示" },
]

/** 环境变量目录（前端展示白名单）：全局静态组 + 各子Agent 导出 envVars 汇总组。 */
export function getEnvCatalog(subAgentDefs: SubAgentDef[] = []): EnvCatalogGroup[] {
  const groups: EnvCatalogGroup[] = [{ group: "global", label: "全局", vars: GLOBAL_VARS }]
  for (const def of subAgentDefs) {
    if (!def.envVars?.length) continue
    groups.push({ group: def.name, label: def.name, vars: def.envVars })
  }
  return groups
}
