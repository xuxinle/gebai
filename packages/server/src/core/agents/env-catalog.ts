/**
 * 环境变量配置目录：前端环境变量面板的白名单来源（不可自定义变量名）。
 * - 按「全局 / 各子Agent」分组展示；未配置的项前端显示为空、请求不携带
 * - 全局组为静态白名单；子Agent 组由各子Agent 导出的 `envVars` 声明汇总（见「子Agent文件格式」），
 *   不在此处硬编码——新增子Agent 环境变量只需在子Agent 定义中声明
 * - 只包含可运行时配置的项；启动级/安全敏感变量（GEBAI_MODE、GEBAI_ADMIN_PASSWORD_HASH、GEBAI_SAFE_MODE、
 *   GEBAI_SANDBOX、GEBAI_HOST/PORT、启动裁剪类 GEBAI_PROFILE / GEBAI_PROMPT_* / GEBAI_TOOL_* /
 *   GEBAI_SUB_AGENTS_* / GEBAI_PRELOAD_SUB_AGENTS 等）不在目录中，即不可配置
 */
import type { EnvCatalogVar, SubAgentDef } from "../base/types"

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
  { name: "GEBAI_LLM_MODELS", description:
      "模型配置清单（JSON 对象数组）：每项一份完整配置 {name?, model, url, api_key, api_kind?, max_context?, multimodal?, vision_model?, vision_api_base?, vision_api_key?}；首选生效、缺省用第一条；配置后旧 GEBAI_LLM_*/GEBAI_VISION_* 散项不再生效" },
  // ── 渠道实例（GEBAI_CHANNEL_IN_<类型> / GEBAI_CHANNEL_OUT_<类型>）──
  {
    name: "GEBAI_CHANNEL_IN_FEISHU",
    description:
      "飞书**入站**实例（JSON 数组，每项一个实例）：[{name, appId, appSecret, chats?, ignoreSelf?}]。只负责**收**（建长连接）；name 是实例名（人格用 listen.channels 引用，也是 target 前缀）；同一应用只能一条长连接，所以两个入站实例必须用不同应用",
  },
  {
    name: "GEBAI_CHANNEL_OUT_FEISHU",
    description:
      "飞书**出站**实例（JSON 数组，每项一个实例）：[{name, appId, appSecret, chats?, ignoreSelf?}]。只负责**发/读**（不建长连接）——所以可以与入站实例、会话机器人共用同一应用；chats 是出站白名单（fail-closed，留空 = 发不出去）",
  },
  {
    name: "GEBAI_CHANNEL_IN_LOCAL",
    description:
      "本机渠道**入站**实例（JSON 数组，可省略）：[{name, dropDir?, inboxDir?}]。配了才收文件（drop/inbox 目录）；不配则只有出站实例",
  },
  {
    name: "GEBAI_CHANNEL_OUT_LOCAL",
    description:
      "本机渠道**出站**实例（JSON 数组，可省略）：[{name?, outboxMax?, dropDir?, inboxDir?}]。不配 = 一个缺省实例（名字 local），提供记忆读写与产物落盘",
  },
  { name: "GEBAI_LLM_CONFIG", description: "指定使用清单中的哪一条（条目 name）；由会话选择自动写入，缺省为清单第一条" },
  { name: "GEBAI_LLM_MODEL", description: "主模型名称（如 deepseek-chat）；配置后本任务使用该模型，未配置用服务端启动配置（未配置 GEBAI_LLM_MODELS 时生效）" },
  { name: "GEBAI_LLM_API_BASE", description: "LLM 接口地址（OpenAI 兼容，如 https://api.deepseek.com）；覆盖服务端启动配置" },
  { name: "GEBAI_LLM_API_KEY", description: "LLM 接口密钥（敏感，仅本次任务临时注入，不落盘）" },
  { name: "GEBAI_LLM_API_KIND", description: "接口类型：openai（chat/completions）/ responses（OpenAI Responses）/ anthropic" },
  { name: "GEBAI_LLM_MAX_CONTEXT", description: "单次上下文最大 token 预算（数字，默认 128000）；覆盖服务端启动配置" },
  { name: "GEBAI_LLM_MAX_OUTPUT_TOKENS", description: "单次响应输出 token 上限（数字）：大文件生成截断防护；Anthropic 接口必填（缺省 8192）" },
  { name: "GEBAI_LLM_EXTRA_PARAMS", description: "额外模型接口参数（JSON 对象，如 {\"reasoning_effort\":\"high\"}），每次请求顶层合并" },
  { name: "GEBAI_LLM_MULTIMODAL", description: "主模型多模态能力声明：true/false（true 时图片附件 base64 内联进消息）；覆盖服务端启动配置" },
  { name: "GEBAI_VISION_MODEL", description: "视觉（外挂多模态）模型名称；配置后 vision 子代理（vision_analyze）使用独立视觉 Provider（本任务生效）" },
  { name: "GEBAI_VISION_API_BASE", description: "视觉模型接口地址（缺省继承主模型）" },
  { name: "GEBAI_VISION_API_KEY", description: "视觉模型接口密钥（敏感，仅本次任务临时注入，不落盘）" },
  { name: "GEBAI_VISION_API_KIND", description: "视觉模型接口类型：openai / responses / anthropic（缺省同主模型）" },
  { name: "GEBAI_VISION_MAX_CONTEXT", description: "视觉模型上下文 token 预算（数字，默认 128000）" },
  { name: "GEBAI_VISION_EXTRA_PARAMS", description: "视觉模型额外请求体参数（JSON 对象）" },
  { name: "GEBAI_CV_MODELS_DIR", description: "本地 CV（desktop_ocr/desktop_locate）模型目录：含 det.onnx/rec.onnx/dict.txt（PP-OCR 中英文）" },
  { name: "GEBAI_CV_MAX_SIDE", description: "本地 CV 输入图像降采样上限（像素长边，默认 1280；坐标仍映射回原始像素系）" },
  { name: "GEBAI_CV_DETECT_MODEL", description: "desktop_detect 自备 YOLO ONNX 模型路径（需自训练图标/控件模型）" },
  { name: "GEBAI_CV_DETECT_LABELS", description: "desktop_detect 标签文件路径（每行一个类别）" },
  { name: "GEBAI_RG_PATH", description: "内置 ripgrep（grep/glob 的 rg 引擎）可执行体路径：显式覆盖内置来源（npm 包 @vscode/ripgrep 或内嵌产物；指向不存在即视为不可用，不静默改用其他来源）" },
  { name: "GEBAI_GREP_ENGINE", description: "grep 搜索引擎：auto（默认，rg 优先、失败回退内置遍历）/ rg（强制，不可用则明确报错）/ builtin（强制内置遍历）" },
  { name: "GEBAI_APPROVAL_SKIP", description: "会话级审批跳过：true/false（用户本人会话生效；非管理员仍受沙箱约束）" },
  // ── 人格（GEBAI_PERSONA_<ID>）──
  {
    name: "GEBAI_PERSONA_ENABLE",
    description:
      "人格启用名单（逗号分隔人格 id）——**必须显式写**：未配置 = 没有任何人格启用。" +
      "为何不能默认启用：人格的裸 `message` 订阅与定时任务写在代码定义里，**不配任何 " +
      "GEBAI_PERSONA_<ID> 也能跑**（进名单就会收全部群消息、cron 到点自己跑）",
  },
  {
    name: "GEBAI_PERSONA_APPROVAL_ASK_TOOLS",
    description:
      "人格工具调用的审批名单**全局兑底**（逗号/空白分隔；支持尾部通配 code_* 与全量 *）。" +
      "逐分身的 `askTools`（GEBAI_PERSONA_<ID> 数组项）**优先于它**。" +
      "缺省空 = 工具调用全部不审批（直接执行）；名单里的工具会推相期弹窗问人。" +
      "不管对外输出（那个在 GEBAI_PERSONA_<ID> 的 reviewOutbound）",
  },
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
