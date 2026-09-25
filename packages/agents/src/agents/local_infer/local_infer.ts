import { existsSync } from "node:fs"
import type { SubAgentDef, Tool } from "@gebai/sdk"
import { requiresApproval as engineApproval, tools as engineTools } from "./engines"
import { inferHome } from "./paths"
import { requiresApproval as providerApproval, tools as providerTools } from "./providers"
import { requiresApproval as serverApproval, tools as serverTools } from "./server"
import { requiresApproval as sourceApproval, tools as sourceTools } from "./sourcetools"
import { requiresApproval as taskApproval, tools as taskTools } from "./tasks"

export const name = "local_infer"
export const description =
  "本地推理引擎（llama.cpp，跨平台跨设备）的全生命周期管理与统一调用入口：**引擎供给**（引擎矩阵/设备探测/下载安装，覆盖 Windows/Linux/macOS × CPU/CUDA/Vulkan/Metal/ROCm/SYCL；**内网/离线时发现 resources 下的源码就地自动编译**）、**进程管理**（状态/启停/重启/日志，不依赖 PowerShell）、**统一推理目标**（本机 / 局域网端点 / 云端 OpenAI 兼容端点，命名目标与鉴权）、**批量提交推理任务**（generate/batch/jobs，含后台执行、进度与断点续跑）、**结构化输出**（JSON Schema 约束解码 + 校验重试）。需要跑本地或自建大模型、批量跑结构化抽取/分类/生成任务、或在无 GPU / 非 Windows / 无外网环境部署推理时装载。输入：运维意图或推理任务；输出：引擎与任务状态、结构化结果、实测吞吐。"
export const systemPrompt =
  "你负责 GEBAI 的推理引擎（子项目 infer/）。**核心设计：推理不绑定操作系统、不绑定 GPU**——引擎矩阵覆盖 Windows/Linux/macOS × CPU/CUDA/Vulkan/Metal/ROCm/SYCL，调用面统一为 OpenAI 兼容端点，目标可以是本机、局域网另一台机器或云端端点。\n" +
  "**工具**：\n" +
  "· 引擎供给：local_infer_engines（列出引擎矩阵与设备探测——CPU 核数/CUDA/Vulkan/Metal 可用性与探测依据）、local_infer_engine_fetch（按 id 下载安装 llama.cpp 引擎，需审批）、local_infer_model_fetch（下载模型，含预置小模型，需审批）。缺引擎/缺模型时先走这三个，不要假定用户已装。\n" +
  "· **内网/离线编译**：local_infer_sources（发现 {GEBAI_HOME}/resources/src/ 等目录里的源码，含归档预览与「是否自带 vendor 依赖」的离线可行性判定；action=toolchain 看工具链与各设备后端就绪）、local_infer_source_build（把源码就地编译成引擎并安装到 vendor/<id>/，需审批；长编译用 background=true 走后台，action=status/log/cancel 管理）。**拿不到预编译包（无外网）时走这条**：源码 + 本机工具链即可产出引擎，产物与下载安装的引擎同一落点，后续 start/status 一视同仁。\n" +
  "· 目标与调用：local_infer_targets（列出可用推理目标：本机/命名远端，可探活看 n_ctx 与 slots）、local_infer_generate（单条，含结构化输出）、local_infer_batch（批量提交，可后台执行 + 断点续跑）、local_infer_jobs（批次进度/存活/结果/真中止）。generate/batch 的 target 参数选目标（缺省 local=本机）；远端端点用 target=\"http://host:port\" 直连，或在 LOCAL_INFER_TARGETS 里配命名目标（含 api_key）。\n" +
  "· 进程管理：local_infer_status（实例/PID 存活/端口/探测/显存/引擎）、local_infer_start、local_infer_stop、local_infer_restart（换档位换模型必须重启，不支持热切换）、local_infer_logs（日志尾部与错误特征）、local_infer_models。\n" +
  "· 其他：local_infer_bench（llama-bench 压测）、local_infer_inspect（GGUF 结构与张量布局）。\n" +
  "**启动路径（跨平台）**：缺省走 TS 层的 launcher（detached 拉起 + 日志重定向 + /health 就绪轮询），Windows 上若既无已安装引擎又未显式要求 launcher，会自动回退 infer/scripts/run-server.ps1（GPU 档位的老路径）；LOCAL_INFER_LAUNCH=script 可强制脚本模式。启动失败先看 local_infer_logs，不要靠猜。\n" +
  "**关键事实（实测得出，勿凭直觉推翻）**：\n" +
  "1) **后端选择按设备实测**：在 16GB 消费级 NVIDIA 卡上跑 35B MoE 时 CUDA 解码比 Vulkan 快 2.6 倍（143 vs 54 t/s，本模型每 token 触发 320 次小 GEMM）——有 CUDA 就用 CUDA；CPU 是无 GPU 环境的通用退路（小模型可用，35B 会慢到不实用，见 8)。\n" +
  "2) **GPU 上**：专家权重必须全部驻显存（-ncmoe 0），任几层落 CPU 即损失三成吞吐；正确策略是「选能整体装进显存的量化」。\n" +
  "3) 档位由 infer/config/profiles.json 定义，数值为实测结论：fast（IQ3_XXS 全 GPU，解码 143 t/s）为默认推荐；quality（IQ4_XS、8 层专家在 CPU，82 t/s）仅在确需最高保真度时用；**cpu-small** 是无 GPU/跨平台的通用档（小模型 + CPU）。\n" +
  "4) 投机解码在本场景无收益（ngram 系低于无投机基线，外部 draft 模型与合成接受率都更慢），MTP 头也不在 GGUF 内——不要开启投机参数。\n" +
  "5) **并发是否有效取决于瓶颈**：GPU 且 prompt 短时并发是唯一被证明有效的吞吐手段（np=16 聚合 627.8 t/s = 4.30×）；但长 prompt（如 GEBAI 会话的 15K 系统提示+工具定义）预填充占主导、并发无增益；**纯 CPU 上并发无增益**（实测 0.5B 单流 15.7 t/s，4 路各约 2.5 t/s、聚合持平——CPU 计算资源被平分）。所以：先看 local_infer_targets/status 的 slot 数，再按「短 prompt + 有 GPU」决定 concurrency，否则保持 1 串行。\n" +
  "6) 推理型模型（如 Qwen-AgentWorld-35B-A3B）的输出正文在 content、思维链在 reasoning_content——判断是否正常必须看 reasoning_content，只看 content 会把正常的思维链输出误判为空响应；generate/batch 默认 enable_thinking=false（实测省 91% token 且工具调用结果一致），需要复杂推理时显式开。普通小模型（如 Qwen2.5-0.5B）没有思维链字段，属正常。\n" +
  "7) 结构化输出：优先 schema 参数（JSON Schema → 服务端转 GBNF 约束解码，工具侧再抽取校验并对无效输出回灌重试一次），结果在 data.json；校验失败会返回 json_errors 与原文，此时**如实报告失败**而不是把残缺结果当成功。grammar 是 GBNF 直传兜底（与 -bs 后端采样不兼容，引擎会自动回退 CPU 采样）；服务端不认识约束时工具会自动降级为 json_object 并给预警。\n" +
  "**工作要点**：\n" +
  "1) 先 local_infer_status / local_infer_targets 确认目标与端口占用（模型加载会吃满显存或 CPU）；\n" +
  "2) 批量任务：小批或需立即拿结果用默认同步；**条目多时 background=true**（立即返回 job_id、随服务进程后台逐条落盘），再用 local_infer_jobs 取进度/结果。会话中断不会杀后台批次，真中止用 jobs(action=\"cancel\")；服务重启后在跑批次会被如实判为「已中断」，用 batch(resume=true) 续跑（不自动续跑，避免无人值守地占满算力）；\n" +
  "3) 换档位/换模型用 local_infer_restart（profile/model/engine 参数）；\n" +
  "4) 性能结论一律以本机实测为准（local_infer_bench 或 generate 返回的 decode_tps），不引用文档历史数字当现状；\n" +
  "5) 模型与引擎都不会自动下载——缺什么先 local_infer_model_fetch / local_infer_engine_fetch，再启动；\n" +
  "6) 详细设计、实测数据与已知负结果（无效优化）见子项目文档 infer/README.md。\n" +
  "启动/停止/重启/压测/批量都会长时间占用算力（GPU 或 CPU 全线），执行前确认没有其他任务在跑，并向用户说明规模与预计耗时。"

export const tools: Record<string, Tool> = { ...serverTools, ...taskTools, ...engineTools, ...providerTools, ...sourceTools }
export const requiresApproval: Record<string, boolean> = {
  ...serverApproval,
  ...taskApproval,
  ...engineApproval,
  ...providerApproval,
  ...sourceApproval,
}
export const preload = false

/** 可配置环境变量（`LOCAL_INFER_*` 前缀，汇总进前端环境变量面板白名单）。 */
export const envVars = [
  { name: "LOCAL_INFER_HOME", description: "本地推理子项目根目录（缺省 dev 模式自动推导为仓库根下的 infer/）" },
  { name: "LOCAL_INFER_PROFILE", description: "默认运行档位名（缺省取 config/profiles.json 的 default_profile；有 GPU 用 fast，无 GPU 用 cpu-small）" },
  { name: "LOCAL_INFER_PORT", description: "本地推理服务端口（缺省 8080，OpenAI 兼容端点；start 的 port 参数可覆盖）" },
  { name: "LOCAL_INFER_MODELS_DIR", description: "模型权重目录（缺省 {GEBAI_HOME}/resources/models/infer）" },
  { name: "LOCAL_INFER_TIMEOUT_MS", description: "单条推理请求超时（缺省 600000 ms；本地模型慢，勿设过小）" },
  { name: "LOCAL_INFER_BATCH_MAX_ITEMS", description: "单次 batch 的条目处理上限（未配置=不限制）：批量同步执行会长时间占住会话，建议按机时预算设一个上限自动分片" },
  { name: "LOCAL_INFER_TARGETS", description: '命名推理目标（JSON 数组，如 [{"name":"lan-4090","base_url":"http://192.168.1.9:8080","api_key":"sk-...","model":"qwen3-aw"}]）：generate/batch 的 target 参数即可选用，用于把任务派到局域网机器或云端 OpenAI 兼容端点' },
  { name: "LOCAL_INFER_REMOTE_API_KEY", description: "远端推理端点的缺省 API Key（缺省鉴权头，target 里未单独配 key 时使用）" },
  { name: "LOCAL_INFER_LAUNCH", description: "启动方式：launcher（缺省，TS 跨平台直启）/ script（Windows 上走 infer/scripts/run-server.ps1）" },
  { name: "LOCAL_INFER_SOURCE_DIRS", description: "额外的源码发现根（内网编译用；路径分隔符分隔，可指向随包带入的源码目录），缺省已包含 {GEBAI_HOME}/resources/src 与 resources/engines、以及 infer/engine" },
]

// ── 兼容再导出 ────────────────────────────────────────────────────────────
// 数据访问层（路径/档位/模型/状态文件/日志/批次）的实现已迁至 ./paths，
// 引擎层在 ./engines、进程管理在 ./launcher、推理目标在 ./providers。
// 这里原样再导出，保持子系统既有引用与既有测试不变。

export {
  baseUrl,
  clearServerState,
  defaultPort,
  downloadCacheDir,
  engineDir,
  inferHome,
  isAbsolutePath,
  listBatchJobs,
  listModels,
  listServerLogs,
  loadProfiles,
  modelsDir,
  profileModel,
  profilesPath,
  readBatchJob,
  readLaunchRecords,
  readServerStates,
  reportsDir,
  runsDir,
  serverStatePath,
  stateDir,
  tailLines,
  vendorDir,
  writeServerState,
  type BatchJobEntry,
  type BatchJobState,
  type InferProfile,
  type InferProfiles,
  type LaunchRecord,
  type LogFile,
  type ServerState,
} from "./paths"

/** 项目根绑定：dev 模式指向仓库内的 infer/ 子项目（二进制形态无该目录，返回 undefined）。 */
const projectRoot = (env: Record<string, string>): string | undefined => {
  const home = inferHome(env)
  return existsSync(home) ? home : undefined
}

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars, projectRoot }
