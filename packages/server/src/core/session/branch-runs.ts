import { randomUUID } from "node:crypto"
import type { MessageLike, SessionRunArchive } from "@gebai/sdk"

/**
 * 会话分支运行与合并（DESIGN「会话分支运行与合并」，branch_run 工具，git 式并发）：从主会话**当前上下文**
 * fork 出多个分支并行执行——各分支独立 LLM 循环 + 主上下文快照 + 会话工具面快照，可按**命名模型路由**
 * （GEBAI_LLM_ROUTES，多路接口）各走各的 Provider；分支最终报告完成后**自动合入主上下文**（引擎合并队列），
 * 主线下轮即见——像 git 一样不停分支合并，多路并行摆脱单轮串行的模型服务速度限制。
 *
 * 与 agent_run（新会话执行）的本质差异：新会话执行派生**隔离上下文**（子Agent 提示词，不继承主会话历史）；
 * 分支运行 fork **主会话完整上下文**（同一系统提示词/历史/工具面）——适合同一任务的并行多路探索与执行。
 *
 * 生命周期与 session-runs 同构：进程内异步任务（引擎 runBranch），句柄存引擎级共享表（跨工具调用可见），
 * 本类为按会话过滤的薄视图（buildContext 每次构建新实例，共享同一 store）；服务重启即中断，不落盘不恢复。
 */

/** 分支状态：running 执行中 / done 正常完成（报告已自动合入）/ failed 执行异常 / cancelled 被终止（不合入）。 */
export type BranchRunStatus = "running" | "done" | "failed" | "cancelled"

/** 分支规格（branch_run 的 branches 参数项，normalizeBranchSpecs 规范化后形态）。 */
export interface BranchSpec {
  name: string
  prompt: string
  /** 模型路由名或字面模型名（GEBAI_LLM_ROUTES 命中走独立 Provider，多路接口）；缺省沿用任务级模型。 */
  model?: string
  /** 合入粒度（调用级参数由工具统一盖章到各分支）：full=报告全文合入（缺省）；summary=超阈值的报告经
   *  模型压成「结论+要点」合入（全文保留在过程存档/bg_task，主线只进摘要——保主线上下文预算）。 */
  merge?: "full" | "summary"
}

/** 分支运行快照（bg_task 返回给模型的形态；rounds/toolCalls/last 从存档实时推导）。 */
export interface BranchRunRecord {
  branchId: string
  sessionId: string
  name: string
  prompt: string
  model?: string
  startedAt: number
  status: BranchRunStatus
  endedAt?: number
  /** 最终报告文本（done 时有效）。 */
  output?: string
  /** 失败/终止原因。 */
  error?: string
  /** 已执行的模型回复轮次。 */
  rounds: number
  /** 已执行的工具调用次数。 */
  toolCalls: number
  /** 最近一条存档条目尾部（≤200 字符，执行中进度参考）。 */
  last?: string
  /** 报告是否已合入主上下文（done 即合入）。 */
  merged: boolean
}

/** 分支运行句柄（引擎级共享存储形态；archive 为 runBranch 存档的活引用——执行中持续增长）。 */
export interface BranchRunHandle {
  branchId: string
  sessionId: string
  name: string
  prompt: string
  model?: string
  startedAt: number
  status: BranchRunStatus
  endedAt?: number
  output?: string
  error?: string
  /** 报告已合入主上下文标记（引擎合并回调置位，bg_task 状态展示用）。 */
  merged?: boolean
  /** 合入粒度（BranchSpec.merge 传入）：summary 时超阈值报告经模型摘要后合入（全文在存档）。 */
  mergeMode?: "full" | "summary"
  /** 主干通知收件箱（互相感知，DESIGN「会话分支运行与合并」）：其他分支合入/主线进展的通知积压于此，
   *  分支执行循环每轮轮首排空注入（注入点在上一轮 tool 结果之后，tool_calls 配对不破坏）。 */
  inbox?: string[]
  /** fork 点主干水位（存储消息数，branch_sync 增量的基准）：runBranch 首个 await 后
   *  由引擎记录——此刻存储尾部即 fork 快照的持久化等价（在途 tool 结果尚未落盘，不属主干可见内容）。 */
  forkAt?: number
  /** 上次主干同步水位（branch_sync 增量同步推进；未同步过 = forkAt）。 */
  syncedAt?: number
  /** 已回显给本分支的合并消息 id（防重复：队列中的合入先经同步回显、落盘后再入存储增量时跳过）。 */
  deliveredMergeIds?: Set<string>
  controller: AbortController
  /** bg_task stop 显式终止标记（与父任务停止传播区分）。 */
  cancelRequested?: boolean
  archive?: SessionRunArchive
  /** 运行结束 promise（完成/失败/终止均 settle；wait/cancel 用它精确唤醒）。 */
  done: Promise<void>
}

/** 分支执行启动器（引擎注入：绑定会话/用户/env 后执行 runBranch——branchId 为注册表分配的运行标识
 *  （事件路由/存档 runId/bg_task 关联），fork 快照由注册表同步切片传入）。 */
export type BranchRunner = (
  spec: BranchSpec & { branchId: string },
  signal: AbortSignal,
  forkMessages: MessageLike[],
) => Promise<{ output: string; archive: SessionRunArchive }>

export interface BranchRunService {
  /** 启动一批分支（并发校验后立即返回，不等待完成）：fork 快照在调用点同步切片（fork 点=当前上下文，
   *  注册表构造时绑定本上下文 live messages——工具层无 live messages 访问权，fork 源随 ctx 注入）。 */
  start(specs: BranchSpec[]): Promise<BranchRunRecord[]>
  /** 读取分支快照（不存在或非本会话返回 undefined）。 */
  get(branchId: string): BranchRunRecord | undefined
  /** 本会话全部分支（按启动顺序）。 */
  list(): BranchRunRecord[]
  /** 阻塞等待分支结束（或超时返回当前快照）。 */
  wait(branchId: string, timeoutMs: number): Promise<BranchRunRecord | undefined>
  /** 主动终止分支（已结束的原样返回快照）。 */
  cancel(branchId: string): Promise<BranchRunRecord | undefined>
  /** 终态分支的最终报告与完整存档（bg_task wait/stop 取回与回放用）。 */
  result(branchId: string): { output: string; archive: SessionRunArchive } | undefined
}

/** 单会话并发分支上限（同步 fan-out 与异步分支合计，即单次调用上限；超限拒绝新分支）。 */
export const BRANCH_RUN_MAX_CONCURRENT = 8
/** 单会话保留的终态分支记录上限（超出淘汰最旧；运行中不淘汰）。 */
export const BRANCH_RUN_KEEP = 20
/** 分支报告合入主上下文的长度上限（超出保留头尾+省略说明——分支输出不落盘兜底，纯上下文保护）。 */
export const BRANCH_MERGE_MAX_CHARS = 16000
/** 摘要合入（merge=summary）的触发阈值：报告短于该值不值得一次模型调用，原文合入。 */
export const BRANCH_MERGE_SUMMARY_SKIP_CHARS = 1500
/** 主干通知（分支互相感知注入）长度上限：通知是注入其他分支上下文的信号，保持紧凑。 */
export const BRANCH_NOTICE_MAX_CHARS = 2000

/** 通知文本截断（保留头部，互相感知注入用）。 */
export function branchNoticeHead(text: string, max = BRANCH_NOTICE_MAX_CHARS): string {
  const flat = text.trim()
  return flat.length <= max ? flat : `${flat.slice(0, max)}\n…（已截断）`
}
/** cancel 后等待执行循环收尾的宽限毫秒（abort 异步传播）。 */
const BRANCH_RUN_CANCEL_GRACE_MS = 5000
/** 分支名规则（缺省 b1..bN 自动命名）：任意非空白字符、≤32 字符（展示与区分用，非标识符——中文名合法）。 */
const BRANCH_NAME_RE = /^\S{1,32}$/u

/**
 * branch_run 分支清单参数校验与规范化：非空数组（数量上限由注册表并发检查承担）、prompt 必填非空、
 * name 可选（缺省 b1..bN 按序自动命名）且批内唯一、model 可选字符串。非法即抛（错误直达模型）。
 */
export function normalizeBranchSpecs(raw: unknown): BranchSpec[] {
  if (!Array.isArray(raw) || raw.length === 0) throw new Error("参数 branches 必须为非空数组：[{ name?, prompt, model? }]")
  const specs: BranchSpec[] = []
  for (let i = 0; i < raw.length; i++) {
    const item = raw[i]
    if (!item || typeof item !== "object") throw new Error(`branches[${i}] 必须为对象：{ name?, prompt, model? }`)
    const r = item as Record<string, unknown>
    const prompt = typeof r.prompt === "string" ? r.prompt.trim() : ""
    if (!prompt) throw new Error(`branches[${i}].prompt 必填（分支任务指令）`)
    let name = typeof r.name === "string" ? r.name.trim() : ""
    if (!name) name = `b${i + 1}`
    if (!BRANCH_NAME_RE.test(name)) throw new Error(`branches[${i}].name 非法（${name}）：不能含空白，≤32 字符（中文名合法）`)
    if (specs.some((s) => s.name === name)) throw new Error(`分支名批内重复: ${name}（各分支需唯一命名以便区分报告）`)
    const model = typeof r.model === "string" ? r.model.trim() : ""
    specs.push(model ? { name, prompt, model } : { name, prompt })
  }
  return specs
}

const sleep = (ms: number) => new Promise<void>((res) => setTimeout(res, ms))

/** 存档条目文本尾部截断（进度 last 字段）。 */
function tail(text: string, max = 200): string {
  const s = text.replace(/\s+/g, " ").trim()
  return s.length <= max ? s : `…${s.slice(-max)}`
}

/** 分支运行注册表：按会话过滤的薄视图（store 由引擎级共享，跨工具调用/跨实例可见）。
 *  forkSource：本上下文 live messages（buildContext 注入——分支 fork 主线当前上下文，工具层无访问权）；
 *  onDone：分支正常完成时回调（引擎注入——报告合入主上下文，见引擎 trunkMerge）。 */
export class BranchRunRegistry implements BranchRunService {
  private store: Map<string, BranchRunHandle>
  private sessionId: string
  private runner: BranchRunner
  private forkSource: MessageLike[]
  private parentSignal?: AbortSignal
  private onDone?: (handle: BranchRunHandle) => void | Promise<void>

  constructor(opts: { sessionId: string; store: Map<string, BranchRunHandle>; runner: BranchRunner; forkSource: MessageLike[]; parentSignal?: AbortSignal; onDone?: (handle: BranchRunHandle) => void | Promise<void> }) {
    this.store = opts.store
    this.sessionId = opts.sessionId
    this.runner = opts.runner
    this.forkSource = opts.forkSource
    this.parentSignal = opts.parentSignal
    this.onDone = opts.onDone
  }

  private owned(h: BranchRunHandle | undefined): BranchRunHandle | undefined {
    return h && h.sessionId === this.sessionId ? h : undefined
  }

  /** 存档活引用 → 模型可见快照（进度实时推导）。 */
  private record(h: BranchRunHandle): BranchRunRecord {
    const msgs = h.archive?.messages ?? []
    const last = msgs.length ? msgs[msgs.length - 1] : undefined
    return {
      branchId: h.branchId,
      sessionId: h.sessionId,
      name: h.name,
      prompt: h.prompt,
      ...(h.model ? { model: h.model } : {}),
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

  /** 单会话终态记录修剪（超出 BRANCH_RUN_KEEP 淘汰最旧；运行中不淘汰）。 */
  private prune(): void {
    const finished = [...this.store.values()]
      .filter((h) => h.sessionId === this.sessionId && h.status !== "running")
      .sort((a, b) => (a.endedAt ?? 0) - (b.endedAt ?? 0))
    for (const h of finished.slice(0, Math.max(0, finished.length - BRANCH_RUN_KEEP))) this.store.delete(h.branchId)
  }

  async start(specs: BranchSpec[]): Promise<BranchRunRecord[]> {
    const running = [...this.store.values()].filter((h) => h.sessionId === this.sessionId && h.status === "running").length
    if (running + specs.length > BRANCH_RUN_MAX_CONCURRENT) {
      throw new Error(`并发分支超限（${running} 运行中 + ${specs.length} 新增 > ${BRANCH_RUN_MAX_CONCURRENT}）：请先用 bg_task（action=stop/list）终止或等待运行中的分支完成。`)
    }
    // fork 快照同步切片（fork 点 = 调用时的主上下文；主任务阻塞在本工具调用内，消息数组此刻稳定）
    const fork = [...this.forkSource]
    const out: BranchRunRecord[] = []
    for (const spec of specs) {
      const branchId = `b${randomUUID().replace(/-/g, "").slice(0, 8)}`
      const controller = new AbortController()
      let settleDone: () => void = () => {}
      const done = new Promise<void>((resolve) => (settleDone = resolve))
      const handle: BranchRunHandle = {
        branchId,
        sessionId: this.sessionId,
        name: spec.name,
        prompt: spec.prompt,
        ...(spec.model ? { model: spec.model } : {}),
        ...(spec.merge ? { mergeMode: spec.merge } : {}),
        startedAt: Date.now(),
        status: "running",
        controller,
        done,
      }
      // 父任务取消传播（用户停止/审批拒绝连带终止分支；运行结束后解绑防监听器泄漏）
      const onParentAbort = () => controller.abort(this.parentSignal?.reason)
      if (this.parentSignal?.aborted) onParentAbort()
      else this.parentSignal?.addEventListener("abort", onParentAbort, { once: true })
      void this.runner({ ...spec, branchId }, controller.signal, fork).then(
        async (res) => {
          handle.status = "done"
          handle.output = res.output
          handle.archive = res.archive
          // 合入回调 await 后才 settle done（摘要合入含模型调用）：同步 fan-out 的 wait 返回时合并消息
          // 必已入队（结果前排空，主线下一轮即见）；合入异常不影响分支终态（done 已定，吞掉防误标 failed）
          try {
            await this.onDone?.(handle)
          } catch {
            /* 合入失败：全文兜底路径在 trunkMerge 内部，此处仅为防线 */
          }
        },
        (err) => {
          handle.status = handle.cancelRequested || controller.signal.aborted ? "cancelled" : "failed"
          handle.error = err instanceof Error ? err.message : String(err)
        },
      ).finally(() => {
        handle.endedAt = Date.now()
        this.parentSignal?.removeEventListener("abort", onParentAbort)
        this.prune()
        settleDone()
      })
      this.store.set(branchId, handle)
      out.push(this.record(handle))
    }
    return out
  }

  get(branchId: string): BranchRunRecord | undefined {
    const h = this.owned(this.store.get(branchId))
    return h ? this.record(h) : undefined
  }

  list(): BranchRunRecord[] {
    return [...this.store.values()].filter((h) => h.sessionId === this.sessionId).map((h) => this.record(h))
  }

  async wait(branchId: string, timeoutMs: number): Promise<BranchRunRecord | undefined> {
    const h = this.owned(this.store.get(branchId))
    if (!h) return undefined
    const deadline = Date.now() + Math.max(0, timeoutMs)
    while (h.status === "running") {
      if (Date.now() >= deadline) break
      await Promise.race([h.done, sleep(300)])
    }
    return this.record(h)
  }

  async cancel(branchId: string): Promise<BranchRunRecord | undefined> {
    const h = this.owned(this.store.get(branchId))
    if (!h) return undefined
    if (h.status !== "running") return this.record(h)
    h.cancelRequested = true
    h.controller.abort(new Error("用户主动终止（bg_task stop）"))
    // abort 异步传播进执行循环：短暂等待收尾，让返回状态落定为 cancelled（超宽限期则如实报告仍在收尾）
    await Promise.race([h.done, sleep(BRANCH_RUN_CANCEL_GRACE_MS)])
    return this.record(h)
  }

  result(branchId: string): { output: string; archive: SessionRunArchive } | undefined {
    const h = this.owned(this.store.get(branchId))
    if (!h || h.status === "running" || !h.archive) return undefined
    return { output: h.output ?? "", archive: h.archive }
  }
}
