/**
 * 通用后台任务注册表：给「不是命令、也不是子会话」的长任务提供与 `bg_task` **同一管理面**的运行记录
 * （`sh` 命令任务与 `s` 子会话运行是另两类，见 `core/tools/agent.ts` 的 bg_task）。
 *
 * 与另两类的分工：
 *   * `t`（sh-tasks）——真实子进程，输出落日志文件、跨服务重启仍可见；
 *   * `s`（subsessions）——LLM 循环，过程存存档、可快速结束（注入收敛指令）；
 *   * `j`（本模块）——**进程内的异步任务**（如两级研判的整批执行）：进度经 `progress` 实时上报、
 *     取消经 `AbortSignal` 协作中止、结果留在产物目录。进程内生命周期（服务重启即丢失运行记录），
 *     故**权威进度始终是被调方自己落盘的产物**（如 triage 的 job.json / results.jsonl）——本注册表
 *     只负责「同一会话内的可见性与控制」。
 *
 * 不做的两件事：不持久化（重启后 list 为空，符合其余两类中 `s` 的进程内语义）、不承载模型
 * （因此 bg_task 的 finish「快速结束」对本类无意义，应改用 stop）。
 */

/** 后台任务状态（与 `s` 子会话口径一致，便于 bg_task 同构呈现）。 */
export type BgJobStatus = "running" | "done" | "failed" | "cancelled"

/** 进度快照（由任务自行上报；`phase` 自由文本，`done/total` 供百分比估算）。 */
export interface BgJobProgress {
  phase: string
  done?: number
  total?: number
  /** 一行补充说明（如「1 条 L1 采纳、0 条待精审」）。 */
  detail?: string
}

export interface BgJobRecord {
  /** 任务 id（`j` 前缀，与 sh 的 t / 子会话的 s 区分）。 */
  id: string
  /** 任务种类（用于呈现与后续扩展，如 "triage"）。 */
  kind: string
  /** 展示名（一行）。 */
  name: string
  /** 所属会话（会话级视图据此过滤；跨会话不可见）。 */
  sessionId: string
  startedAt: number
  status: BgJobStatus
  endedAt?: number
  progress?: BgJobProgress
  /** 最终结果摘要（done 时；不塞全文，全文在产物目录）。 */
  summary?: string
  /** 失败原因（failed 时）。 */
  error?: string
  /** 业务侧标识（如 triage 的 job_id 与产物目录），便于调用方回查产物。 */
  ref?: Record<string, string>
}

/** 任务体（注册表只关心这三件事：干活、报进度、可取消）。 */
export interface BgJobTask {
  /** 上报进度（幂等、可多次调用；任务结束后忽略）。 */
  onProgress?: (p: BgJobProgress) => void
  signal: AbortSignal
}

export interface StartBgJobOptions {
  kind: string
  name: string
  /** 业务侧标识（如 { job_id, job_dir }）。 */
  ref?: Record<string, string>
  /** 任务体：返回最终结果摘要；抛错即 failed；signal.aborted 后应尽快返回（cancelled）。 */
  run: (task: BgJobTask) => Promise<string | undefined>
}

/** 内部句柄（注册表持有；对外只暴露记录快照）。 */
interface BgJobHandle {
  rec: BgJobRecord
  controller: AbortController
  done: Promise<void>
}

/** 进程级共享 store（跨会话可见性由会话级视图负责过滤；与子会话同一分层方式）。 */
export type BgJobStore = Map<string, BgJobHandle>

/** 每会话保留的终态记录上限（与子会话 SUBSESSION_KEEP 同口径，防长生命周期进程无界增长）。 */
export const BG_JOB_KEEP = 50

/**
 * 后台任务注册表：**会话视角的薄视图**（store 进程级共享，视图按 sessionId 过滤）——
 * 与 `SubSessionRegistry` 同一分层：引擎为每个任务上下文构造一份视图，工具经它看本会话的任务。
 */
export class BackgroundJobRegistry {
  private sessionId: string

  constructor(
    private store: BgJobStore,
    sessionId: string,
  ) {
    this.sessionId = sessionId
  }

  /**
   * 启动一个后台任务：立即返回记录快照（不等待完成），任务体在后台跑。
   *
   * 任务体的返回值用作 `summary`；抛错记 `failed`；中止（signal）后正常返回记 `cancelled`
   * ——中止由任务体协作完成（注册表只发信号，不打断正在进行的 HTTP/模型调用）。
   */
  start(opts: StartBgJobOptions): BgJobRecord {
    const id = `j${crypto.randomUUID().replace(/-/g, "").slice(0, 8)}`
    const controller = new AbortController()
    const rec: BgJobRecord = {
      id,
      kind: opts.kind,
      name: opts.name,
      sessionId: this.sessionId,
      startedAt: Date.now(),
      status: "running",
      ...(opts.ref ? { ref: opts.ref } : {}),
    }
    let settle: () => void = () => {}
    const done = new Promise<void>((resolve) => (settle = resolve))
    const handle: BgJobHandle = { rec, controller, done }
    this.store.set(id, handle)

    const task: BgJobTask = {
      signal: controller.signal,
      onProgress: (p) => {
        if (rec.status !== "running") return
        rec.progress = p
      },
    }
    void (async () => {
      try {
        const summary = await opts.run(task)
        rec.status = controller.signal.aborted ? "cancelled" : "done"
        if (summary) rec.summary = summary
      } catch (e) {
        rec.status = controller.signal.aborted ? "cancelled" : "failed"
        rec.error = (e as Error)?.message ? String((e as Error).message) : String(e)
      } finally {
        rec.endedAt = Date.now()
        this.prune()
        settle()
      }
    })()
    return { ...rec }
  }

  /** 本会话的任务记录（按启动顺序；含已结束的，便于回看）。 */
  list(): BgJobRecord[] {
    return [...this.store.values()]
      .filter((h) => h.rec.sessionId === this.sessionId)
      .sort((a, b) => a.rec.startedAt - b.rec.startedAt)
      .map((h) => ({ ...h.rec }))
  }

  get(id: string): BgJobRecord | undefined {
    return this.owned(id)?.rec ? { ...this.owned(id)!.rec } : undefined
  }

  /** 等完成（超时返回当前快照）：与 bg_task 的 wait 语义一致（超时由调用方决定是否再等）。 */
  async wait(id: string, timeoutMs: number): Promise<BgJobRecord | undefined> {
    const h = this.owned(id)
    if (!h) return undefined
    if (h.rec.status === "running") {
      await Promise.race([h.done, new Promise<void>((res) => setTimeout(res, Math.max(0, timeoutMs)))])
    }
    return { ...h.rec }
  }

  /**
   * 协作取消：置中止信号并等待任务体收尾（最多 `graceMs`）。
   * 任务体应监听 signal 并尽快返回——未收尾也返回 running 快照（调用方可再查）。
   */
  async cancel(id: string, graceMs = 5_000): Promise<BgJobRecord | undefined> {
    const h = this.owned(id)
    if (!h) return undefined
    if (h.rec.status !== "running") return { ...h.rec }
    h.controller.abort(new Error("用户终止"))
    await Promise.race([h.done, new Promise<void>((res) => setTimeout(res, graceMs))])
    return { ...h.rec }
  }

  private owned(id: string): BgJobHandle | undefined {
    const h = this.store.get(id)
    return h && h.rec.sessionId === this.sessionId ? h : undefined
  }

  /** 本会话终态记录修剪（超出上限淘汰最旧；运行中不淘汰）。 */
  private prune(): void {
    const finished = [...this.store.values()]
      .filter((h) => h.rec.sessionId === this.sessionId && h.rec.status !== "running")
      .sort((a, b) => (a.rec.endedAt ?? 0) - (b.rec.endedAt ?? 0))
    for (const h of finished.slice(0, Math.max(0, finished.length - BG_JOB_KEEP))) this.store.delete(h.rec.id)
  }
}
