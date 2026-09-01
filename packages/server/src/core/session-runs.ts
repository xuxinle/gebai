import { randomUUID } from "node:crypto"
import type { SessionRunArchive } from "@gebai/sdk"

/**
 * agent_run 异步后台运行（DESIGN「新会话执行的异步运行」）：`agent_run async:true` 启动新会话执行进
 * 后台并立即返回 runId，模型可先处理其他任务，之后经 `bg_task`（status/wait/stop/list，与 sh 异步
 * 命令任务统一管理面）查询进度、等待结果或主动终止。
 *
 * 与 sh 异步任务（sh-tasks，子进程 + 磁盘落盘）不同：运行是**进程内异步任务**（引擎 runNewSession），
 * 存活与本进程绑定（服务重启即中断，不落盘不恢复）；执行过程事件（session:true + sessionRunId）照常
 * 实时推送前端，进度快照从存档引用实时推导。句柄存于引擎级共享表（跨工具调用可见），本类为按会话
 * 过滤的薄视图（buildContext 每次构建新实例，共享同一 store）。
 */

/** 运行状态：running 执行中 / done 正常完成 / failed 执行异常 / cancelled 被终止（bg_task stop 或父任务停止）。 */
export type SessionRunStatus = "running" | "done" | "failed" | "cancelled"

/** 运行记录（bg_task 返回给模型的快照形态；rounds/toolCalls/last 为从存档推导的进度快照）。 */
export interface SessionRunRecord {
  runId: string
  sessionId: string
  agents: string[]
  input: string
  startedAt: number
  status: SessionRunStatus
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
}

/** 运行句柄（引擎级共享存储形态；archive 为 runNewSession 存档的活引用——执行中持续增长）。 */
export interface SessionRunHandle {
  runId: string
  sessionId: string
  agents: string[]
  input: string
  startedAt: number
  status: SessionRunStatus
  endedAt?: number
  output?: string
  error?: string
  controller: AbortController
  /** bg_task stop 显式终止标记（与父任务停止传播区分）。 */
  cancelRequested?: boolean
  archive?: SessionRunArchive
  /** 运行结束 promise（完成/失败/终止均 settle；wait/cancel 用它精确唤醒）。 */
  done: Promise<void>
}

/** 异步运行启动器（引擎注入：绑定会话/用户/env/深度后调 runNewSession）。onArchive 在存档创建时回调
 *  （registry 据此持有活引用推导进度）。 */
export type SessionRunRunner = (
  agents: string[],
  input: string,
  signal: AbortSignal,
  opts?: {
    inheritGlobalTools?: boolean
    inheritGlobalPrompt?: boolean
    onArchive?: (archive: SessionRunArchive) => void
  },
) => Promise<{ output: string; archive: SessionRunArchive }>

export interface SessionRunService {
  /** 启动后台运行：校验预加载清单（validate 回调）+ 并发上限后立即返回（不等待完成）。 */
  start(agents: string[], input: string, opts?: { inheritGlobalTools?: boolean; inheritGlobalPrompt?: boolean }): Promise<SessionRunRecord>
  /** 读取运行快照（不存在或非本会话返回 undefined）。 */
  get(runId: string): SessionRunRecord | undefined
  /** 本会话全部运行（按启动顺序）。 */
  list(): SessionRunRecord[]
  /** 阻塞等待运行结束（或超时返回当前快照；不存在返回 undefined）。 */
  wait(runId: string, timeoutMs: number): Promise<SessionRunRecord | undefined>
  /** 主动终止运行（abort 传播进执行循环；已结束的原样返回快照）。 */
  cancel(runId: string): Promise<SessionRunRecord | undefined>
  /** 终态运行的最终结果与完整存档（wait/cancel 取回结果与回放存档用；运行中/不存在返回 undefined）。 */
  result(runId: string): { output: string; archive: SessionRunArchive } | undefined
}

/** 单会话并发后台运行上限（防失控堆积；超限拒绝新运行）。 */
export const SESSION_RUN_MAX_CONCURRENT = 8
/** 单会话保留的终态运行记录上限（超出淘汰最旧；运行中不淘汰）。 */
export const SESSION_RUN_KEEP = 20
/** cancel 后等待执行循环收尾的宽限毫秒（abort 异步传播，短暂等待让状态落定为终止）。 */
const SESSION_RUN_CANCEL_GRACE_MS = 5000

const sleep = (ms: number) => new Promise<void>((res) => setTimeout(res, ms))

/** 存档条目文本尾部截断（进度 last 字段）。 */
function tail(text: string, max = 200): string {
  const s = text.replace(/\s+/g, " ").trim()
  return s.length <= max ? s : `…${s.slice(-max)}`
}

/** agent_run 异步运行注册表：按会话过滤的薄视图（store 由引擎级共享，跨工具调用/跨实例可见）。 */
export class SessionRunRegistry implements SessionRunService {
  private store: Map<string, SessionRunHandle>
  private sessionId: string
  private runner: SessionRunRunner
  private parentSignal?: AbortSignal
  /** 预加载清单校验与规范化（引擎注入 normalizeRunAgents：去重/连带/上限/未知名检查，非法即抛；
   *  可异步——引擎在异步启动路径前置热加载重扫，新写/新修的子Agent 文件即时可见）。 */
  private validate: (agents: string[]) => string[] | Promise<string[]>

  constructor(opts: { sessionId: string; store: Map<string, SessionRunHandle>; runner: SessionRunRunner; validate: (agents: string[]) => string[] | Promise<string[]>; parentSignal?: AbortSignal }) {
    this.store = opts.store
    this.sessionId = opts.sessionId
    this.runner = opts.runner
    this.validate = opts.validate
    this.parentSignal = opts.parentSignal
  }

  private owned(h: SessionRunHandle | undefined): SessionRunHandle | undefined {
    return h && h.sessionId === this.sessionId ? h : undefined
  }

  /** 存档活引用 → 模型可见快照（进度实时推导）。 */
  private record(h: SessionRunHandle): SessionRunRecord {
    const msgs = h.archive?.messages ?? []
    const last = msgs.length ? msgs[msgs.length - 1] : undefined
    return {
      runId: h.runId,
      sessionId: h.sessionId,
      agents: h.agents,
      input: h.input,
      startedAt: h.startedAt,
      status: h.status,
      endedAt: h.endedAt,
      output: h.output,
      error: h.error,
      rounds: msgs.filter((m) => m.role === "assistant").length,
      toolCalls: msgs.filter((m) => m.role === "tool").length,
      ...(last ? { last: tail(last.content) } : {}),
    }
  }

  /** 单会话终态记录修剪（超出 SESSION_RUN_KEEP 淘汰最旧；运行中不淘汰）。 */
  private prune(): void {
    const finished = [...this.store.values()]
      .filter((h) => h.sessionId === this.sessionId && h.status !== "running")
      .sort((a, b) => (a.endedAt ?? 0) - (b.endedAt ?? 0))
    for (const h of finished.slice(0, Math.max(0, finished.length - SESSION_RUN_KEEP))) this.store.delete(h.runId)
  }

  async start(agents: string[], input: string, opts?: { inheritGlobalTools?: boolean; inheritGlobalPrompt?: boolean }): Promise<SessionRunRecord> {
    const normalized = await this.validate(agents)
    const running = [...this.store.values()].filter((h) => h.sessionId === this.sessionId && h.status === "running").length
    if (running >= SESSION_RUN_MAX_CONCURRENT) {
      throw new Error(`并发后台运行超限（≥${SESSION_RUN_MAX_CONCURRENT}）：请先用 bg_task（action=stop/list）终止或等待运行中的任务完成。`)
    }
    const runId = `r${randomUUID().replace(/-/g, "").slice(0, 8)}`
    const controller = new AbortController()
    let settleDone: () => void = () => {}
    const done = new Promise<void>((resolve) => (settleDone = resolve))
    const handle: SessionRunHandle = {
      runId,
      sessionId: this.sessionId,
      agents: normalized,
      input,
      startedAt: Date.now(),
      status: "running",
      controller,
      done,
    }
    // 父任务取消传播（用户停止/审批拒绝）：abort 即终止后台运行；运行结束后解绑（信号对象随任务存续，防监听器泄漏）
    const onParentAbort = () => controller.abort(this.parentSignal?.reason)
    if (this.parentSignal?.aborted) onParentAbort()
    else this.parentSignal?.addEventListener("abort", onParentAbort, { once: true })
    void this.runner(normalized, input, controller.signal, {
      inheritGlobalTools: opts?.inheritGlobalTools,
      inheritGlobalPrompt: opts?.inheritGlobalPrompt,
      onArchive: (a) => {
        handle.archive = a
      },
    }).then(
      (res) => {
        handle.status = "done"
        handle.output = res.output
        handle.archive = res.archive
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
    this.store.set(runId, handle)
    return this.record(handle)
  }

  get(runId: string): SessionRunRecord | undefined {
    const h = this.owned(this.store.get(runId))
    return h ? this.record(h) : undefined
  }

  list(): SessionRunRecord[] {
    return [...this.store.values()].filter((h) => h.sessionId === this.sessionId).map((h) => this.record(h))
  }

  async wait(runId: string, timeoutMs: number): Promise<SessionRunRecord | undefined> {
    const h = this.owned(this.store.get(runId))
    if (!h) return undefined
    const deadline = Date.now() + Math.max(0, timeoutMs)
    while (h.status === "running") {
      if (Date.now() >= deadline) break
      await Promise.race([h.done, sleep(300)])
    }
    return this.record(h)
  }

  async cancel(runId: string): Promise<SessionRunRecord | undefined> {
    const h = this.owned(this.store.get(runId))
    if (!h) return undefined
    if (h.status !== "running") return this.record(h)
    h.cancelRequested = true
    h.controller.abort(new Error("用户主动终止（bg_task stop）"))
    // abort 异步传播进执行循环：短暂等待收尾，让返回状态落定为 cancelled（超宽限期则如实报告仍在收尾）
    await Promise.race([h.done, sleep(SESSION_RUN_CANCEL_GRACE_MS)])
    return this.record(h)
  }

  result(runId: string): { output: string; archive: SessionRunArchive } | undefined {
    const h = this.owned(this.store.get(runId))
    if (!h || h.status === "running" || !h.archive) return undefined
    return { output: h.output ?? "", archive: h.archive }
  }
}
