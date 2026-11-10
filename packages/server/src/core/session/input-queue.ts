/** 会话输入队列（服务端权威）：任务运行期间提交的用户输入在此排队，当前任务收尾
 *  （完成/取消/出错）后按序自动发起下一次任务。
 *
 *  **为何在服务端**：队列是「这个会话还有几条待说」的事实，不是某个浏览器标签页的临时状态——
 *  页面刷新、换设备、飞书/定时任务与网页同时向同一会话发话时，前端本地队列要么丢失、要么与服务端
 *  真实运行态打架（本地 drain 与「另一个标签页已在跑」互相看不见）。放在这里，队列随会话走：
 *  刷新不丢、多端一致、后台会话照常推进，前端只剩展示与转发用户意图。
 *
 *  队列为**进程内存态**：与「正在运行的任务」同生命周期（重启即在跑的任务已不存在，排队项也没有
 *  执行者），不落盘、不进模型上下文、不计入会话统计；会话删除经 `forget` 释放。 */
import type { AttachmentRef, QueuedPrompt } from "@gebai/sdk"

/** 服务端内部形态 → 对外契约形态（丢掉 userId 等内部字段）。 */
function toPublic(x: QueuedInput): QueuedPrompt {
  return { id: x.id, sessionId: x.sessionId, text: x.text, attachments: x.attachments, messageId: x.messageId, createdAt: x.createdAt }
}

/** 单条排队输入（服务端形态；对外经 `QueuedPrompt` 序列化）。 */
export interface QueuedInput {
  id: string
  sessionId: string
  /** 归属用户：多用户隔离（只有 owner 的排队项对 owner 可见/可操作）。 */
  userId: string
  text: string
  /** 已上传会话 tmp/ 的附件引用（入队时上传，执行时直用）。 */
  attachments: AttachmentRef[]
  /** 预生成的用户消息 id：执行时按同一 id 落盘，撤回/反馈定位因此可用。 */
  messageId: string
  createdAt: number
  /** 提交时的任务级环境变量（浏览器本地注入，仅本次任务生效；不对外序列化、不落盘）。 */
  env?: Record<string, string>
  /** 发起者角色（公共资源权限判定用，与直接发送一致）。 */
  role?: string
}

/** 单会话排队上限（防刷屏堆积把内存与后续任务排到天荒地老）。 */
export const MAX_QUEUED_PER_SESSION = 50

/** 执行回调：引擎收尾后由队列调用（发起下一次任务）。返回值无意义（失败由回调自行记录）。 */
export type QueueRunner = (item: QueuedInput, meta: { front: boolean }) => Promise<void>

/** 变更通知：队列内容变动（入队/出队/编辑/撤回/置顶/清理）时调用（用于事件推送与界面刷新）。 */
export type QueueNotifier = (sessionId: string) => void

export class SessionInputQueue {
  /** sessionId → 待执行条目（数组顺序即执行顺序）。 */
  private queues = new Map<string, QueuedInput[]>()
  /** 所有者索引：sessionId → userId（删除/隔离判定）。 */
  private owners = new Map<string, string>()
  /** 正在派发中的会话（防重入：任务收尾通知与用户「立即执行」并发时不得双发）。 */
  private dispatching = new Set<string>()
  /** 会话是否在运行（由引擎侧回调注入，避免队列反向依赖引擎实例）。 */
  private isRunning: (sessionId: string) => boolean = () => false
  /** 派发执行器（装配期注入）。 */
  private runner: QueueRunner | null = null
  private notifier: QueueNotifier | null = null

  /** 装配：运行态判定 + 派发执行器 + 变更通知（缺任一项时队列只记不跑）。 */
  attach(deps: { isRunning: (sessionId: string) => boolean; runner: QueueRunner; notify?: QueueNotifier }): void {
    this.isRunning = deps.isRunning
    this.runner = deps.runner
    this.notifier = deps.notify ?? null
  }

  /** 某会话的排队条目（副本，调用方可安全持有/序列化）。userId 给定时校验归属。 */
  list(sessionId: string, userId?: string): QueuedInput[] {
    const owner = this.owners.get(sessionId)
    if (userId && owner && owner !== userId) return []
    return (this.queues.get(sessionId) ?? []).map((x) => ({ ...x }))
  }

  /** 全部排队会话的副本（快照用；userId 给定时只出该用户的会话）。 */
  snapshot(userId?: string): Record<string, QueuedInput[]> {
    const out: Record<string, QueuedInput[]> = {}
    for (const [sid, items] of this.queues) {
      if (!items.length) continue
      if (userId && this.owners.get(sid) !== userId) continue
      out[sid] = items.map((x) => ({ ...x }))
    }
    return out
  }

  /** 对外序列化（快照用）：去掉服务端内部字段（userId）。 */
  snapshotPublic(userId?: string): Record<string, QueuedPrompt[]> {
    const out: Record<string, QueuedPrompt[]> = {}
    for (const [sid, items] of Object.entries(this.snapshot(userId))) out[sid] = items.map(toPublic)
    return out
  }

  /** 单会话对外序列化（应答/事件用）。 */
  listPublic(sessionId: string, userId?: string): QueuedPrompt[] {
    return this.list(sessionId, userId).map(toPublic)
  }

  /** 该会话排队条数。 */
  size(sessionId: string): number {
    return this.queues.get(sessionId)?.length ?? 0
  }

  /**
   * 入队。`front=true` 插队首（中断插入：调用方负责先取消当前任务）。
   * 返回入队后的队列位置（1 起）与条目；超上限或条目非法返回错误。
   */
  enqueue(input: QueuedInput, opts: { front?: boolean } = {}): { ok: true; item: QueuedInput; position: number } | { ok: false; error: string } {
    if (!input.text.trim()) return { ok: false, error: "排队输入内容为空" }
    const q = this.queues.get(input.sessionId) ?? []
    if (q.length >= MAX_QUEUED_PER_SESSION) return { ok: false, error: `排队已满（上限 ${MAX_QUEUED_PER_SESSION} 条），请先撤回或等待执行` }
    this.owners.set(input.sessionId, input.userId)
    const item: QueuedInput = { ...input, text: input.text }
    if (opts.front) q.unshift(item)
    else q.push(item)
    this.queues.set(input.sessionId, q)
    this.notify(input.sessionId)
    const position = q.findIndex((x) => x.id === item.id) + 1
    // 入队即尝试推进：会话空闲（入队时当前任务恰好刚收尾）时立刻执行队首，
    // 不必等下一次任务收尾事件——否则这条输入要等到「下一条任务」才有机会跑
    this.drain(input.sessionId)
    return { ok: true, item, position }
  }

  /** 编辑未执行的排队输入（内容替换）。 */
  edit(sessionId: string, id: string, text: string, userId?: string): boolean {
    if (userId && this.owners.get(sessionId) !== userId) return false
    const item = (this.queues.get(sessionId) ?? []).find((x) => x.id === id)
    if (!item) return false
    const next = text.trim()
    if (!next) return false
    item.text = next
    this.notify(sessionId)
    return true
  }

  /** 撤回（移除）未执行的排队输入。 */
  remove(sessionId: string, id: string, userId?: string): boolean {
    if (userId && this.owners.get(sessionId) !== userId) return false
    const q = this.queues.get(sessionId) ?? []
    const idx = q.findIndex((x) => x.id === id)
    if (idx < 0) return false
    q.splice(idx, 1)
    if (!q.length) this.queues.delete(sessionId)
    this.notify(sessionId)
    return true
  }

  /**
   * 置顶（排队条 ⚡ 立即执行）：把条目移到队首并取消当前任务，本任务收尾后立即执行它。
   * 返回是否找到该条目。
   */
  promote(sessionId: string, id: string, userId?: string): boolean {
    if (userId && this.owners.get(sessionId) !== userId) return false
    const q = this.queues.get(sessionId) ?? []
    const idx = q.findIndex((x) => x.id === id)
    if (idx < 0) return false
    if (idx > 0) {
      const [item] = q.splice(idx, 1)
      q.unshift(item)
      this.queues.set(sessionId, q)
      this.notify(sessionId)
    }
    // 运行中：取消当前任务，其收尾（onSettled）后由 drain 取队首；空闲则直接推进
    this.drain(sessionId)
    return true
  }

  /**
   * 消化队列（幂等）：会话不在运行且队列非空时取队首派发。
   *
   * 触发点：① 引擎任务收尾（完成/取消/出错）回调；② 入队与置顶后（覆盖「运行恰在入队前结束」
   * 的竞态窗口）。运行中或已在派发中时为空转——派发是异步的（引擎 run 要时间），
   * 期间重复进来必须挡住，否则同一条会被派两次。
   */
  drain(sessionId: string): void {
    if (this.dispatching.has(sessionId)) return
    if (this.isRunning(sessionId)) return
    const q = this.queues.get(sessionId)
    if (!q?.length) return
    if (!this.runner) return // 未装配（单测/未接线）：只记不跑
    const item = q.shift()!
    if (!q.length) this.queues.delete(sessionId)
    this.dispatching.add(sessionId)
    this.notify(sessionId)
    void this.runner(item, { front: false })
      .catch(() => {
        /* 派发失败：条目已出队（再放回会与后续入队顺序打架）；失败原因由 runner 侧记录到会话 */
      })
      .finally(() => {
        this.dispatching.delete(sessionId)
        // 派发结束（无论成败）：若会话又空闲且队列还有条目，继续下一条
        this.drain(sessionId)
      })
  }

  /** 队列内容清空（会话删除/归档）。 */
  clear(sessionId: string): void {
    if (!this.queues.delete(sessionId)) return
    this.notify(sessionId)
  }

  /** 会话删除：释放队列与归属（幂等）。只有实际清掉了排队条目才通知——
   *  会话压根没排队时不得因此产生一条事件（订阅方会把它当作队列变动重渲染/写日志）。 */
  forget(sessionId: string): void {
    const had = this.queues.delete(sessionId)
    this.owners.delete(sessionId)
    this.dispatching.delete(sessionId)
    if (had) this.notify(sessionId)
  }

  /** 用户删除：清空该用户全部会话的排队项（账号清理路径）。 */
  forgetUser(userId: string): void {
    for (const [sid, owner] of [...this.owners]) {
      if (owner === userId) this.forget(sid)
    }
  }

  private notify(sessionId: string): void {
    try {
      this.notifier?.(sessionId)
    } catch {
      /* 通知失败不影响队列本身 */
    }
  }
}
