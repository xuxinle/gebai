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
 * - `waitpid`/`kill`：父会话经 `bg_task`（status/wait/stop/list）查看进度、取回结果、终止；
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
}

/** 子会话运行快照（bg_task/subsection_run 返回给模型的形态；进度从存档活引用实时推导）。 */
export interface SubSessionRecord {
  runId: string
  sessionId: string
  name: string
  input: string
  agents: string[]
  model?: string
  inheritContext: boolean
  async: boolean
  /** 派生子会话的宿主：主任务的直接子会话为 undefined。 */
  parentRunId?: string
  depth: number
  startedAt: number
  status: SubSessionStatus
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
  archive?: SubSessionArchive
  /** 运行结束 promise（完成/失败/终止均 settle；wait/cancel 用它精确唤醒）。 */
  done: Promise<void>
}

/** 子会话执行启动器（引擎注入：绑定会话/用户/env 后执行 runSubSession——runId/depth 为注册表分配的运行标识与
 *  嵌套深度，forkMessages 为 fork 快照（隔离形态为空数组）。 */
export type SubSessionRunner = (
  spec: SubSessionSpec & { runId: string; depth: number },
  signal: AbortSignal,
  forkMessages: MessageLike[],
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
/** 子会话名规则（缺省 s1..sN 自动命名）：任意非空白字符、≤32 字符（展示与区分用，中文名合法）。 */
const NAME_RE = /^\S{1,32}$/u

/** 通知文本截断（保留头部，互相感知注入用）。 */
export function subSessionNoticeHead(text: string, max = SUBSESSION_NOTICE_MAX_CHARS): string {
  const flat = text.trim()
  return flat.length <= max ? flat : `${flat.slice(0, max)}\n…（已截断）`
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
}): SubSessionSpec[] {
  const inheritContext = raw.inherit_context === true
  const async = raw.async === true
  const merge: SubSessionMergeMode = raw.merge === "summary" ? "summary" : "full"
  const inheritGlobalTools = raw.inherit_global_tools !== false
  const inheritGlobalPrompt = raw.inherit_global_prompt !== false
  const shared = { inheritContext, merge, async, inheritGlobalTools, inheritGlobalPrompt }
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
      specs.push({ name, input, agents: strArray(r.agents), ...(model ? { model } : {}), ...shared })
    }
    return specs
  }
  const input = typeof raw.input === "string" ? raw.input.trim() : ""
  if (!input) throw new Error("缺少参数 input（单任务形态的任务指令）或 subsessions（多任务形态的任务清单）")
  const model = typeof raw.model === "string" ? raw.model.trim() : ""
  return [{ name: "s1", input, agents: strArray(raw.agents), ...(model ? { model } : {}), ...shared }]
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
    const msgs = h.archive?.messages ?? []
    const last = msgs.length ? msgs[msgs.length - 1] : undefined
    return {
      runId: h.runId,
      sessionId: h.sessionId,
      name: h.name,
      input: h.input,
      agents: h.agents,
      ...(h.model ? { model: h.model } : {}),
      inheritContext: h.inheritContext,
      async: h.async,
      ...(h.parentRunId ? { parentRunId: h.parentRunId } : {}),
      depth: h.depth,
      startedAt: h.startedAt,
      status: h.status,
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
      throw new Error(`并发子会话超限（${running} 运行中 + ${normalized.length} 新增 > ${SUBSESSION_MAX_CONCURRENT}）：请先用 bg_task（action=stop/list）终止或等待运行中的子会话完成。`)
    }
    const out: SubSessionRecord[] = []
    for (const spec of normalized) {
      const runId = `s${randomUUID().replace(/-/g, "").slice(0, 8)}`
      const controller = new AbortController()
      let settleDone: () => void = () => {}
      const done = new Promise<void>((resolve) => (settleDone = resolve))
      const handle: SubSessionHandle = {
        runId,
        sessionId: this.sessionId,
        name: spec.name,
        input: spec.input,
        agents: spec.agents,
        ...(spec.model ? { model: spec.model } : {}),
        inheritContext: spec.inheritContext,
        async: spec.async,
        merge: spec.merge,
        inheritGlobalTools: spec.inheritGlobalTools,
        inheritGlobalPrompt: spec.inheritGlobalPrompt,
        ...(this.ownerRunId ? { parentRunId: this.ownerRunId } : {}),
        depth: this.depth + 1,
        startedAt: Date.now(),
        status: "running",
        controller,
        done,
      }
      // 父任务取消传播（用户停止/审批拒绝连带终止子会话；运行结束后解绑防监听器泄漏）
      const onParentAbort = () => controller.abort(this.parentSignal?.reason)
      if (this.parentSignal?.aborted) onParentAbort()
      else this.parentSignal?.addEventListener("abort", onParentAbort, { once: true })
      void this.runner({ ...spec, runId, depth: handle.depth }, controller.signal, spec.inheritContext ? fork : []).then(
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
    h.controller.abort(new Error("用户主动终止（bg_task stop）"))
    // abort 异步传播进执行循环：短暂等待收尾，让返回状态落定为 cancelled（超宽限期则如实报告仍在收尾）
    await Promise.race([h.done, sleep(CANCEL_GRACE_MS)])
    return this.record(h)
  }

  result(runId: string): { output: string; archive: SubSessionArchive } | undefined {
    const h = this.owned(this.store.get(runId))
    if (!h || h.status === "running" || !h.archive) return undefined
    return { output: h.output ?? "", archive: h.archive }
  }
}
