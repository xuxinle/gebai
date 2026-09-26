import { randomUUID } from "node:crypto"
import type { MessageLike, SubSessionArchive } from "@gebai/sdk"

/**
 * 子会话运行（subsession_run，DESIGN「子会话运行」）：把「执行新会话」与「会话子会话运行」两套机制统一为一套
 * **父子会话（进程）模型**——一个注册表、一条引擎执行路径、一个工具入口。
 *
 * Linux 语义映射：
 * - `fork`：`inheritContext=true` 从父会话**当前上下文**派生（同一消息历史快照、同一系统提示词、同一工具面快照）；
 * - `exec`：`inheritContext=false` 派生**隔离新上下文**（子Agent 提示词 + 全局工具/全局提示词），`agents` 为预载程序；
 * - 管道/IPC：子会话经 `subsession_merge` 主动向父会话合入阶段性成果并互相感知（异步运行注入）；
 * - `waitpid`/`kill`：父会话经 `bg_task`（status/wait/stop/finish/list）查看进度、取回结果、终止；
 * - 快速结束（`finish`，先礼后兵）：中止前先注入收敛指令——子会话按提示词停止扩展性工作、基于已有信息给出结论
 *   并自然结束（报告照常产出/合入），宽限逾期才强制终止：硬杀会把「已跑到哪」的结论一并丢掉，只剩过程存档；
 * - 进程私有状态：子会话待办为运行内隔离清单（不落盘、不回流、不继承）；
 * - 进程树：`parentRunId` + `depth`（嵌套深度上限 SUBAGENT_DEPTH），子会话内可再派生子会话。
 *
 * 生命周期与其余进程内后台任务同哲学：句柄存引擎级共享表（跨工具调用可见），本类为按视角过滤的薄视图
 * （buildContext 每次构建新实例、共享同一 store）；服务重启即中断（不落盘恢复）。
 */

/** 子会话状态：running 执行中 / done 正常完成 / failed 执行异常 / cancelled 被终止。 */
export type SubSessionStatus = "running" | "done" | "failed" | "cancelled"

/** 合入粒度：full=报告全文合入父会话（缺省）；summary=超阈值报告经模型压成要点合入（全文留过程存档）。 */
export type SubSessionMergeMode = "full" | "summary"

/** 子会话环境变量的继承模式（`env_mode`）。 */
export type SubSessionEnvMode =
  /** 缺省：在父任务 env 基础上叠加 `env` 自定义项（后者优先）。 */
  | "inherit"
  /** 不继承父任务 env，只用 `env` 自定义项。 */
  | "clear"

/** 子会话规格（normalizeSubSessionSpecs 规范化后的形态）。 */
export interface SubSessionSpec {
  /** 展示名（缺省 s1..sN，批内唯一，≤32 字符不含空白）。 */
  name: string
  /** 任务指令（子会话初始消息）。 */
  input: string
  /** 预加载进子会话的子Agent 名单（空 = 不加载；已按依赖连带展开去重）。 */
  agents: string[]
  /** 模型路由名或字面模型名（GEBAI_LLM_ROUTES 命中走独立 Provider，多路接口并行）。 */
  model?: string
  /** 是否继承父会话上下文：true=fork（消息历史 + 系统提示词 + 工具面快照）；false=隔离新上下文。 */
  inheritContext: boolean
  /** 最终报告合入父会话的粒度（inheritContext=true 时生效）。 */
  merge: SubSessionMergeMode
  /** 异步后台运行：父会话不阻塞、子会话注入 subsession_merge 合并工具。 */
  async: boolean
  /** 隔离形态：是否继承全局工具（默认 true）。 */
  inheritGlobalTools: boolean
  /** 隔离形态：是否注入总Agent 全局系统提示词（默认 true）。 */
  inheritGlobalPrompt: boolean
  /** 运行时限（毫秒；缺省不设限）：到时进入**快速结束**（注入收敛指令给宽限让模型输出结论，逾期强制终止）。 */
  timeoutMs?: number
  /**
   * 环境变量继承模式（缺省 `inherit`）：`inherit` = 父任务 env + `env`；`clear` = 仅 `env`。
   * `clear` 清空的是**工具层可读的父任务 env**（含会话 env/任务级覆盖等 GEBAI_* 配置）；
   * 命令子进程的 OS 基线（PATH/HOME/TEMP 等）仍由执行层自进程环境合并，不受影响。
   */
  envMode: SubSessionEnvMode
  /** 子会话自定义环境变量（叠加或替换取决于 `envMode`；键已校验为标识符形式）。 */
  env: Record<string, string>
}

/** 子会话运行快照（bg_task/subsection_run 返回给模型的形态；进度从存档活引用实时推导）。 */
export interface SubSessionRecord {
  runId: string
  sessionId: string
  name: string
  input: string
  agents: string[]
  model?: string
  /** 环境变量继承模式（工具层可读，便于 bg_task status 如实展示）。 */
  envMode: SubSessionEnvMode
  /** 已应用的自定义环境变量**键名**（值不回传——可能含密钥）。 */
  envKeys: string[]
  inheritContext: boolean
  async: boolean
  /** 派生子会话的宿主：主任务的直接子会话为 undefined。 */
  parentRunId?: string
  depth: number
  startedAt: number
  status: SubSessionStatus
  /** 已进入快速结束收尾（收到收敛指令，宽限内等结论）。 */
  finishing: boolean
  endedAt?: number
  /** 最终输出文本（done 时有效）。 */
  output?: string
  /** 失败/终止原因（failed/cancelled 时有效）。 */
  error?: string
  /** 已执行的模型回复轮次。 */
  rounds: number
  /** 已执行的工具调用次数。 */
  toolCalls: number
  /** 最近一条存档条目尾部（≤200 字符，执行中进度参考）。 */
  last?: string
  /** 最终报告是否已合入父会话（继承上下文形态；隔离形态经返回值/bg_task 取回）。 */
  merged: boolean
}

/** 子会话运行句柄（引擎级共享存储形态；archive 为引擎存档的活引用——执行中持续增长）。 */
export interface SubSessionHandle {
  runId: string
  sessionId: string
  name: string
  input: string
  agents: string[]
  model?: string
  envMode: SubSessionEnvMode
  env: Record<string, string>
  inheritContext: boolean
  async: boolean
  merge: SubSessionMergeMode
  inheritGlobalTools: boolean
  inheritGlobalPrompt: boolean
  parentRunId?: string
  depth: number
  startedAt: number
  status: SubSessionStatus
  endedAt?: number
  output?: string
  error?: string
  /** 报告已合入父会话标记（引擎合入回调置位）。 */
  merged?: boolean
  /** 父会话通知收件箱（互相感知）：父会话其他子会话的合入/父会话进展通知积压于此，子会话执行循环每轮轮首排空注入。 */
  inbox?: string[]
  /** fork 点父会话水位（存储消息数，subsession_merge 拉取增量的基准）。 */
  forkAt?: number
  /** 上次同步水位（增量推进；未同步过 = forkAt）。 */
  syncedAt?: number
  /** 已回显给本子会话的合入消息 id（防重复投递）。 */
  deliveredMergeIds?: Set<string>
  controller: AbortController
  /** bg_task stop 显式终止标记（与父任务停止传播区分）。 */
  cancelRequested?: boolean
  /** 快速结束请求（收敛指令已注入的时间与原因；置位即收尾中，宽限计时随之启动）。 */
  finishRequested?: { reason: string; requestedAt: number }
  /** 快速结束宽限计时（到期强制终止；运行结束清除）。 */
  finishTimer?: ReturnType<typeof setTimeout>
  /** 运行时限计时（spec.timeoutMs；运行结束清除）。 */
  timeoutTimer?: ReturnType<typeof setTimeout>
  archive?: SubSessionArchive
  /** 存档活引用容器（运行期由引擎持续填充；进度（rounds/toolCalls/last）据此实时推导）。 */
  archiveHolder?: SubSessionArchiveHolder
  /** 运行结束 promise（完成/失败/终止均 settle；wait/cancel 用它精确唤醒）。 */
  done: Promise<void>
}

/** 存档活引用容器（引擎把内部 archive 提前挂入容器；注册表句柄持有同一引用 → 运行中进度实时可见）。 */
export interface SubSessionArchiveHolder {
  archive?: SubSessionArchive
}

/** 子会话执行启动器（引擎注入：绑定会话/用户/env 后执行 runSubSession——runId/depth 为注册表分配的运行标识与
 *  嵌套深度，forkMessages 为 fork 快照（隔离形态为空数组），archiveHolder 为存档活引用容器（引擎一创建
 *  存档即挂入，注册表据此实时推导运行中进度）。 */
export type SubSessionRunner = (
  spec: SubSessionSpec & { runId: string; depth: number },
  signal: AbortSignal,
  forkMessages: MessageLike[],
  archiveHolder?: SubSessionArchiveHolder,
) => Promise<{ output: string; archive: SubSessionArchive }>

/** 规格校验（引擎注入）：规范化预加载名单（去重/依赖连带/数量与深度上限/未知名检查），非法即抛。 */
export type SubSessionValidator = (spec: SubSessionSpec) => string[] | Promise<string[]>

export interface SubSessionService {
  /** 启动一批子会话：校验 + 并发上限检查后立即返回快照（不等待完成）；fork 快照在调用点同步切片。 */
  start(specs: SubSessionSpec[]): Promise<SubSessionRecord[]>
  /** 读取运行快照（不存在或非本视角可见返回 undefined）。 */
  get(runId: string): SubSessionRecord | undefined
  /** 本视角可见的全部运行（按启动顺序）。 */
  list(): SubSessionRecord[]
  /** 阻塞等待运行结束（或超时返回当前快照）。 */
  wait(runId: string, timeoutMs: number): Promise<SubSessionRecord | undefined>
  /** 主动终止运行（abort 传播进执行循环；已结束的原样返回快照）。 */
  cancel(runId: string): Promise<SubSessionRecord | undefined>
  /** 快速结束运行（注入收敛指令让子会话给出结论并自然结束；宽限逾期才强制终止；已结束的原样返回快照）。 */
  finish(runId: string, opts?: SubSessionFinishOptions): SubSessionRecord | undefined
  /** 终态运行的最终结果与完整存档（bg_task wait/stop 取回与回放用）。 */
  result(runId: string): { output: string; archive: SubSessionArchive } | undefined
}

/** 单会话并发子会话上限（全进程树合计；防失控堆积，超限拒绝新运行）。 */
export const SUBSESSION_MAX_CONCURRENT = 8
/** 单会话保留的终态运行记录上限（超出淘汰最旧；运行中不淘汰）。 */
export const SUBSESSION_KEEP = 20
/** 单次调用可派生的子会话数上限（多任务形态的批上限）。 */
export const SUBSESSION_MAX_PER_CALL = 8
/** 报告合入父会话的长度上限（超出保留头尾 + 省略说明——纯父上下文保护）。 */
export const SUBSESSION_MERGE_MAX_CHARS = 16000
/** 摘要合入（merge=summary）触发阈值：报告短于该值不值得一次模型调用，原文合入。 */
export const SUBSESSION_MERGE_SUMMARY_SKIP_CHARS = 1500
/** 父会话通知（互相感知注入）长度上限：通知是注入其他子会话上下文的信号，保持紧凑。 */
export const SUBSESSION_NOTICE_MAX_CHARS = 2000
/** cancel 后等待执行循环收尾的宽限毫秒（abort 异步传播，短暂等待让状态落定为终止）。 */
const CANCEL_GRACE_MS = 5000
/** 快速结束缺省宽限毫秒：注入收敛指令后留给模型输出结论的时间，逾期强制终止。 */
export const SUBSESSION_FINISH_GRACE_MS = 120_000
/** 快速结束宽限上下限（调用方传值夹取到该区间：太短来不及收敛，太长等于没超时）。 */
export const SUBSESSION_FINISH_GRACE_MIN_MS = 1_000
export const SUBSESSION_FINISH_GRACE_MAX_MS = 600_000

/** 快速结束选项。 */
export interface SubSessionFinishOptions {
  /** 结束原因（写进收敛指令，供子会话据此组织结论，如「执行超时」「父会话收尾」）。 */
  reason?: string
  /** 宽限毫秒（夹取到上下限；缺省 SUBSESSION_FINISH_GRACE_MS）。 */
  graceMs?: number
}

/** 宽限解析（缺省值 + 上下限夹取）。 */
export function subSessionFinishGraceMs(v?: number): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SUBSESSION_FINISH_GRACE_MS
  return Math.min(Math.max(Math.round(n), SUBSESSION_FINISH_GRACE_MIN_MS), SUBSESSION_FINISH_GRACE_MAX_MS)
}
/** 子会话名规则（缺省 s1..sN 自动命名）：任意非空白字符、≤32 字符（展示与区分用，中文名合法）。 */
const NAME_RE = /^\S{1,32}$/u
/** 自定义环境变量名规则（与引擎侧 validateEnvVars 同一口径）。 */
const ENV_NAME_RE = /^[A-Za-z_][A-Za-z0-9_]*$/

/**
 * 快速结束指令（收敛提示词，注入子会话收件箱）：要求停止扩展性工作、按已有信息给出结论——
 * 让中止以「模型自己收尾」的方式发生，产出可用结论而非只留过程存档。
 */
export function subSessionFinishPrompt(reason: string, graceSeconds: number): string {
  return (
    `【宿主指令：立即收尾（${reason}）】\n` +
    `本次运行被要求尽快结束：停止一切新的探索与扩展性工作，不再派生新的子会话，不再启动长耗时操作（构建/全量测试/大批量抓取等）。\n` +
    `请在 ${graceSeconds} 秒内直接输出最终回复（不要再调用工具；确有必须的收尾动作只做最小必要的一步），内容按你已经掌握的信息组织：\n` +
    `1. 已完成的结论、产物与关键位置（文件:行号 / 命令 / 链接）；\n` +
    `2. 尚未来得及做的部分；\n` +
    `3. 若要继续，下一步该做什么（供宿主接手）。\n` +
    `如实说明不确定性，不要把未完成的工作写成已完成；宽限期到仍在运行会被强制终止（现有过程保留在存档）。`
  )
}

/**
 * 触发快速结束（软终止）：注入收敛指令 + 启动宽限计时，逾期强制终止——
 * 子会话执行循环轮首排空收件箱即见该指令（下一轮模型调用按提示词给出结论并正常结束）。
 * 已在收尾或已结束返回 false（幂等，不重复注入）。
 */
export function requestSubSessionFinish(h: SubSessionHandle, opts: SubSessionFinishOptions = {}): boolean {
  if (h.status !== "running" || h.finishRequested) return false
  const reason = opts.reason?.trim() || "宿主要求尽快结束本次运行"
  const graceMs = subSessionFinishGraceMs(opts.graceMs)
  h.finishRequested = { reason, requestedAt: Date.now() }
  ;(h.inbox ??= []).push(subSessionFinishPrompt(reason, Math.round(graceMs / 1000)))
  h.finishTimer = setTimeout(() => {
    // 逾期未结束：与 bg_task stop 同口径强制终止（cancelled，执行过程保留在存档）
    if (h.status !== "running") return
    h.cancelRequested = true
    h.controller.abort(new Error(`快速结束宽限期（${Math.round(graceMs / 1000)}s）已到，强制终止`))
  }, graceMs)
  return true
}

/** 通知文本截断（保留头部，互相感知注入用）。 */
export function subSessionNoticeHead(text: string, max = SUBSESSION_NOTICE_MAX_CHARS): string {
  const flat = text.trim()
  return flat.length <= max ? flat : `${flat.slice(0, max)}\n…（已截断）`
}

/** 运行时限解析（秒 → 毫秒；非正数/非法忽略）。 */
function timeoutMsOf(v: unknown): number | undefined {
  const n = Number(v)
  return Number.isFinite(n) && n > 0 ? Math.round(n * 1000) : undefined
}

function strArray(v: unknown): string[] {
  const out: string[] = []
  for (const x of Array.isArray(v) ? v : []) {
    const s = String(x).trim()
    if (s && !out.includes(s)) out.push(s)
  }
  return out
}

/**
 * subsession_run 参数校验与规范化（单任务形态 `input`+`agents` 与多任务形态 `subsessions` 二选一）：
 * 非空、prompt 必填、name 缺省 s1..sN 且批内唯一、model 可选、批上限 SUBSESSION_MAX_PER_CALL。非法即抛。
 */
export function normalizeSubSessionSpecs(raw: {
  input?: unknown
  agents?: unknown
  model?: unknown
  subsessions?: unknown
  inherit_context?: unknown
  async?: unknown
  merge?: unknown
  inherit_global_tools?: unknown
  inherit_global_prompt?: unknown
  timeout?: unknown
  env_mode?: unknown
  env?: unknown
}): SubSessionSpec[] {
  const inheritContext = raw.inherit_context === true
  const async = raw.async === true
  const merge: SubSessionMergeMode = raw.merge === "summary" ? "summary" : "full"
  const inheritGlobalTools = raw.inherit_global_tools !== false
  const inheritGlobalPrompt = raw.inherit_global_prompt !== false
  const timeoutMs = timeoutMsOf(raw.timeout)
  const envMode = envModeOf(raw.env_mode)
  const env = envVarsOf(raw.env, "env")
  const shared = {
    inheritContext,
    merge,
    async,
    inheritGlobalTools,
    inheritGlobalPrompt,
    envMode,
    env,
    ...(timeoutMs ? { timeoutMs } : {}),
  }
  const batch = Array.isArray(raw.subsessions) ? raw.subsessions : undefined
  if (batch) {
    if (raw.input !== undefined || raw.agents !== undefined) throw new Error("参数二选一：单任务形态用 input/agents，多任务形态用 subsessions（不可同时给出）")
    if (!batch.length) throw new Error("参数 subsessions 必须为非空数组：[{ name?, input, agents?, model? }]")
    if (batch.length > SUBSESSION_MAX_PER_CALL) throw new Error(`子会话数量超限（${batch.length} > ${SUBSESSION_MAX_PER_CALL}）`)
    const specs: SubSessionSpec[] = []
    for (let i = 0; i < batch.length; i++) {
      const item = batch[i]
      if (!item || typeof item !== "object") throw new Error(`subsessions[${i}] 必须为对象：{ name?, input, agents?, model? }`)
      const r = item as Record<string, unknown>
      const input = typeof r.input === "string" ? r.input.trim() : ""
      if (!input) throw new Error(`subsessions[${i}].input 必填（子会话任务指令）`)
      const name = typeof r.name === "string" && r.name.trim() ? r.name.trim() : `s${i + 1}`
      if (!NAME_RE.test(name)) throw new Error(`subsessions[${i}].name 非法（${name}）：不能含空白，≤32 字符（中文名合法）`)
      if (specs.some((s) => s.name === name)) throw new Error(`子会话名批内重复: ${name}（各子会话需唯一命名以便区分报告）`)
      const model = typeof r.model === "string" ? r.model.trim() : ""
      // 每项可单独覆写环境配置（未给则沿用调用级共享值）
      const itemEnvMode = r.env_mode === undefined ? envMode : envModeOf(r.env_mode)
      const itemEnv = r.env === undefined ? env : envVarsOf(r.env, `subsessions[${i}].env`)
      specs.push({ name, input, agents: strArray(r.agents), ...(model ? { model } : {}), ...shared, envMode: itemEnvMode, env: itemEnv })
    }
    return specs
  }
  const input = typeof raw.input === "string" ? raw.input.trim() : ""
  if (!input) throw new Error("缺少参数 input（单任务形态的任务指令）或 subsessions（多任务形态的任务清单）")
  const model = typeof raw.model === "string" ? raw.model.trim() : ""
  return [{ name: "s1", input, agents: strArray(raw.agents), ...(model ? { model } : {}), ...shared }]
}

/** `env_mode` 归一：只能是 `inherit` / `clear`（缺省 inherit；给其他值即报错，不静默回退）。 */
export function envModeOf(v: unknown): SubSessionEnvMode {
  if (v === undefined || v === null || v === "") return "inherit"
  if (v === "inherit" || v === "clear") return v
  throw new Error(`env_mode 取值非法（${String(v)}）：只能是 "inherit"（父任务 env + 自定义项）或 "clear"（仅自定义项）`)
}

/**
 * 自定义环境变量归一：取值仅允许 string；名称须为标识符形式且拒绝 `__proto__`
 * （与引擎侧 `validateEnvVars` 同一口径，非法即抛——不静默丢弃，否则模型以为设了实际没设）。
 */
export function envVarsOf(v: unknown, label = "env"): Record<string, string> {
  if (v === undefined || v === null) return {}
  if (typeof v !== "object" || Array.isArray(v)) throw new Error(`${label} 必须是对象：{"NAME": "value"}`)
  const out: Record<string, string> = {}
  for (const [k, val] of Object.entries(v as Record<string, unknown>)) {
    if (k === "__proto__" || !ENV_NAME_RE.test(k)) throw new Error(`${label} 变量名非法: ${k}`)
    if (typeof val !== "string") throw new Error(`${label}.${k} 必须是字符串（不设则该键不出现）`)
    out[k] = val
  }
  return out
}

const sleep = (ms: number) => new Promise<void>((res) => setTimeout(res, ms))

/** 存档条目文本尾部截断（进度 last 字段）。 */
function tail(text: string, max = 200): string {
  const s = text.replace(/\s+/g, " ").trim()
  return s.length <= max ? s : `…${s.slice(-max)}`
}

/**
 * 子会话运行注册表：按视角过滤的薄视图（store 由引擎级共享，跨工具调用/跨实例可见）。
 * forkSource：本上下文 live messages（继承上下文形态的 fork 源——工具层无 live messages 访问权）；
 * onDone：继承上下文形态正常完成时回调（引擎注入——报告合入父会话）。
 */
export class SubSessionRegistry implements SubSessionService {
  private store: Map<string, SubSessionHandle>
  private sessionId: string
  private runner: SubSessionRunner
  private forkSource: MessageLike[]
  private parentSignal?: AbortSignal
  private validate: SubSessionValidator
  private onDone?: (handle: SubSessionHandle) => void | Promise<void>
  /** 视角宿主：undefined=主任务（可见本会话全部子会话）；runId=该子会话（可见自己的直接子会话）。 */
  private ownerRunId?: string
  /** 本上下文嵌套深度（子会话深度 = depth + 1）。 */
  private depth: number

  constructor(opts: {
    sessionId: string
    store: Map<string, SubSessionHandle>
    runner: SubSessionRunner
    validate: SubSessionValidator
    forkSource?: MessageLike[]
    parentSignal?: AbortSignal
    onDone?: (handle: SubSessionHandle) => void | Promise<void>
    ownerRunId?: string
    depth: number
  }) {
    this.store = opts.store
    this.sessionId = opts.sessionId
    this.runner = opts.runner
    this.validate = opts.validate
    this.forkSource = opts.forkSource ?? []
    this.parentSignal = opts.parentSignal
    this.onDone = opts.onDone
    this.ownerRunId = opts.ownerRunId
    this.depth = opts.depth
  }

  /** 视角可见性：主任务视角见本会话全部子会话；子会话视角只见自己的直接子会话。 */
  private owned(h: SubSessionHandle | undefined): SubSessionHandle | undefined {
    if (!h || h.sessionId !== this.sessionId) return undefined
    if (this.ownerRunId === undefined) return h
    return h.parentRunId === this.ownerRunId ? h : undefined
  }

  /** 存档活引用 → 模型可见快照（进度实时推导）。 */
  private record(h: SubSessionHandle): SubSessionRecord {
    // 运行期取活引用容器（引擎构建存档后即挂入，逐条 push 增长）；终态回退 handle.archive
    const msgs = (h.archiveHolder?.archive ?? h.archive)?.messages ?? []
    const last = msgs.length ? msgs[msgs.length - 1] : undefined
    return {
      runId: h.runId,
      sessionId: h.sessionId,
      name: h.name,
      input: h.input,
      agents: h.agents,
      ...(h.model ? { model: h.model } : {}),
      envMode: h.envMode,
      envKeys: Object.keys(h.env).sort(),
      inheritContext: h.inheritContext,
      async: h.async,
      ...(h.parentRunId ? { parentRunId: h.parentRunId } : {}),
      depth: h.depth,
      startedAt: h.startedAt,
      status: h.status,
      finishing: h.status === "running" && h.finishRequested !== undefined,
      endedAt: h.endedAt,
      output: h.output,
      error: h.error,
      rounds: msgs.filter((m) => m.role === "assistant").length,
      toolCalls: msgs.filter((m) => m.role === "tool").length,
      ...(last ? { last: tail(last.content) } : {}),
      merged: h.merged === true,
    }
  }

  /** 单会话终态记录修剪（超出 SUBSESSION_KEEP 淘汰最旧；运行中不淘汰）。 */
  private prune(): void {
    const finished = [...this.store.values()]
      .filter((h) => h.sessionId === this.sessionId && h.status !== "running")
      .sort((a, b) => (a.endedAt ?? 0) - (b.endedAt ?? 0))
    for (const h of finished.slice(0, Math.max(0, finished.length - SUBSESSION_KEEP))) this.store.delete(h.runId)
  }

  async start(specs: SubSessionSpec[]): Promise<SubSessionRecord[]> {
    if (!specs.length) throw new Error("缺少子会话规格")
    if (specs.length > SUBSESSION_MAX_PER_CALL) throw new Error(`子会话数量超限（${specs.length} > ${SUBSESSION_MAX_PER_CALL}）`)
    // fork 快照同步切片（fork 点 = 调用时的父上下文；父任务阻塞在本工具调用内，消息数组此刻稳定）
    const fork = specs.some((s) => s.inheritContext) ? [...this.forkSource] : []
    // 校验（引擎注入：预加载名单去重/连带/上限/未知名检查，可异步——异步启动路径前置热加载重扫）
    const normalized: Array<SubSessionSpec & { agents: string[] }> = []
    for (const spec of specs) {
      const agents = await this.validate(spec)
      normalized.push({ ...spec, agents })
    }
    const running = [...this.store.values()].filter((h) => h.sessionId === this.sessionId && h.status === "running").length
    if (running + normalized.length > SUBSESSION_MAX_CONCURRENT) {
      throw new Error(`并发子会话超限（${running} 运行中 + ${normalized.length} 新增 > ${SUBSESSION_MAX_CONCURRENT}）：请先用 bg_task（action=finish 快速结束 / stop 终止 / list 查看）收尾运行中的子会话，或等其完成。`)
    }
    const out: SubSessionRecord[] = []
    for (const spec of normalized) {
      const runId = `s${randomUUID().replace(/-/g, "").slice(0, 8)}`
      const controller = new AbortController()
      let settleDone: () => void = () => {}
      const done = new Promise<void>((resolve) => (settleDone = resolve))
      const archiveHolder: SubSessionArchiveHolder = {}
      const handle: SubSessionHandle = {
        runId,
        sessionId: this.sessionId,
        name: spec.name,
        input: spec.input,
        agents: spec.agents,
        ...(spec.model ? { model: spec.model } : {}),
        envMode: spec.envMode,
        env: spec.env,
        inheritContext: spec.inheritContext,
        async: spec.async,
        merge: spec.merge,
        inheritGlobalTools: spec.inheritGlobalTools,
        inheritGlobalPrompt: spec.inheritGlobalPrompt,
        ...(this.ownerRunId ? { parentRunId: this.ownerRunId } : {}),
        depth: this.depth + 1,
        startedAt: Date.now(),
        status: "running",
        archiveHolder,
        controller,
        done,
      }
      // 运行时限（spec.timeoutMs）：到时进快速结束（注入收敛指令 + 宽限让模型给出结论），逾期由宽限计时强制终止
      const runTimeoutMs = spec.timeoutMs
      if (runTimeoutMs) handle.timeoutTimer = setTimeout(() => requestSubSessionFinish(handle, { reason: `已到运行时限（${Math.round(runTimeoutMs / 1000)}s）` }), runTimeoutMs)
      // 父任务取消传播（用户停止/审批拒绝连带终止子会话；运行结束后解绑防监听器泄漏）
      const onParentAbort = () => controller.abort(this.parentSignal?.reason)
      if (this.parentSignal?.aborted) onParentAbort()
      else this.parentSignal?.addEventListener("abort", onParentAbort, { once: true })
      void this.runner({ ...spec, runId, depth: handle.depth }, controller.signal, spec.inheritContext ? fork : [], archiveHolder).then(
        async (res) => {
          handle.status = "done"
          handle.output = res.output
          handle.archive = res.archive
          // 合入回调 await 后才 settle done（摘要合入含模型调用）：同步 fan-out 的 wait 返回时
          // 合并消息必已入队/落盘（父会话下一轮即见）；合入异常不影响子会话终态（done 已定）。
          // 仅继承上下文形态回调（隔离形态的结果由工具返回值 / bg_task 交付，不存在合入路径）
          try {
            if (spec.inheritContext) await this.onDone?.(handle)
          } catch {
            /* 合入失败：全文兜底路径在引擎合并内，此处仅为防线 */
          }
        },
        (err) => {
          handle.status = handle.cancelRequested || controller.signal.aborted ? "cancelled" : "failed"
          handle.error = err instanceof Error ? err.message : String(err)
          // 取消/异常时 runner 随错误携带部分过程存档（已跑到哪就存到哪，历史回放不丢过程）
          const partial = (err as { archive?: SubSessionArchive } | undefined)?.archive
          if (partial) handle.archive = partial
        },
      ).finally(() => {
        handle.endedAt = Date.now()
        clearTimeout(handle.finishTimer)
        clearTimeout(handle.timeoutTimer)
        this.parentSignal?.removeEventListener("abort", onParentAbort)
        this.prune()
        settleDone()
      })
      this.store.set(runId, handle)
      out.push(this.record(handle))
    }
    return out
  }

  get(runId: string): SubSessionRecord | undefined {
    const h = this.owned(this.store.get(runId))
    return h ? this.record(h) : undefined
  }

  list(): SubSessionRecord[] {
    return [...this.store.values()].filter((h) => this.owned(h)).map((h) => this.record(h))
  }

  async wait(runId: string, timeoutMs: number): Promise<SubSessionRecord | undefined> {
    const h = this.owned(this.store.get(runId))
    if (!h) return undefined
    const deadline = Date.now() + Math.max(0, timeoutMs)
    while (h.status === "running") {
      if (Date.now() >= deadline) break
      await Promise.race([h.done, sleep(300)])
    }
    return this.record(h)
  }

  async cancel(runId: string): Promise<SubSessionRecord | undefined> {
    const h = this.owned(this.store.get(runId))
    if (!h) return undefined
    if (h.status !== "running") return this.record(h)
    h.cancelRequested = true
    clearTimeout(h.finishTimer)
    h.controller.abort(new Error("用户主动终止（bg_task stop）"))
    // abort 异步传播进执行循环：短暂等待收尾，让返回状态落定为 cancelled（超宽限期则如实报告仍在收尾）
    await Promise.race([h.done, sleep(CANCEL_GRACE_MS)])
    return this.record(h)
  }

  /**
   * 快速结束（软终止，DESIGN「子会话运行」快速结束）：向子会话注入收敛指令并启动宽限计时——模型在宽限内按
   * 提示词输出结论并自然结束（终态 done，报告照常交付/合入）；宽限逾期由注册表强制终止（cancelled，过程留存档）。
   * 同步返回当前快照（不等待收敛完成）；幂等（已收尾/已结束不重复触发）。
   */
  finish(runId: string, opts: SubSessionFinishOptions = {}): SubSessionRecord | undefined {
    const h = this.owned(this.store.get(runId))
    if (!h) return undefined
    requestSubSessionFinish(h, opts)
    return this.record(h)
  }

  result(runId: string): { output: string; archive: SubSessionArchive } | undefined {
    const h = this.owned(this.store.get(runId))
    if (!h || h.status === "running" || !h.archive) return undefined
    return { output: h.output ?? "", archive: h.archive }
  }
}
