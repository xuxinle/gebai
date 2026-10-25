/**
 * 角色层的大脑/手模型：**主驱动会话（driver）+ 工作会话（worker）**。
 *
 * ## 架构
 *
 * ```
 * 事件（广播来的、定时来的、worker 回报的）
 *          │
 *          ▼
 *   ┌──────────────┐   决策：自己答 / 派活 / 合并成一件事
 *   │  driver 会话 │   （唯一入口，串行，可被新事件插入）
 *   └──────┬───────┘
 *          │  派活（工具调用）
 *          ├──────────▶ worker A ──┐
 *          ├──────────▶ worker B ──┤ 并行执行，各自独立上下文
 *          └──────────▶ worker C ──┘
 *                                   │
 *          回报（作为新事件注入 driver）◀┘
 * ```
 *
 * ## 为什么是 driver 决策而不是事件各自找会话
 *
 * 之前的设计是「事件按 threadKey 找自己的会话」，等价于把人格拆成多个互不知情的小助手。
 * 那不符合"一个人工作"：真正的人是**一个脑子**在决定"这件事我自己答、那件事派人查、
 * 这三条其实是同一件事合并处理"。
 *
 * driver 模型把这种判断力放回模型手里：
 * - **合流**：同一段时间涌来的多条消息，一起看、一起判断，而不是各回各的
 * - **派活**：耗时的活派给 worker 并行做，自己能继续响应新消息
 * - **收口**：worker 回报后由 driver 决定怎么组织最终答复
 *
 * ## 会话数不再是"事件的函数"
 *
 * 会话语义变成：**1 个 driver（常驻）+ N 个 worker（按需，跑完即走）**。
 * 事件多不多与会话数无关——这正是"会话是工作单元"的彻底形态。
 */

import type { PersonaAttachment, PersonaEvent } from "./event"
import type { PersonaIdentity } from "./ports"

/**
 * 工作会话派发器（宿主实现）。
 *
 * 为什么是 Port 而不是内部实现：派 worker 需要引擎的 `subsession_run` 能力，
 * 而 persona 不依赖引擎。宿主把"起一个子会话"包成这个接口注入进来即可。
 */
export interface WorkerSpawner {
  /**
   * 派一个工作会话。返回的 Promise 在 worker **完成后** resolve（带回报）。
   *
   * 实现方注意：worker 必须独立上下文（不继承 driver 的对话），
   * 否则 driver 的上下文会被复制 N 份、token 成本爆炸。
   */
  spawn(input: WorkerSpawnInput): Promise<WorkerOutcome>
  /** 取消一个在跑的 worker（driver 判定"这个活不用干了"）。 */
  cancel(runId: string): Promise<void>
}

export interface WorkerSpawnInput {
  /** 所属人格（日志/计费/权限归属）。 */
  personaId: string
  /** 派出它的 driver（日志/诊断；回报路由以 `driverSessionId` 为准）。 */
  driverId?: string
  /**
   * 可选的运行标识（缺省由 spawner 生成）。
   *
   * 为何要让调用方给：派活**异步**时，工具必须在 `spawn()` 返回 promise 之前就把
   * runId 告诉模型（否则 `worker_list`/`worker_cancel` 在结论回来前找不到这个活）。
   * 由调用方预分配，两边的账本从第一刻就是同一套键。
   */
  runId?: string
  user: string
  identity: PersonaIdentity
  /** **driver 给的活**（自然语言指令；driver 自己决定怎么拆、怎么描述）。 */
  task: string
  /** 可选的补充上下文（driver 摘出来的关键事实，避免 worker 重复查）。 */
  context?: string
  /** 验收标准（driver 可要求"必须给出可复现的结论"之类）。 */
  verify?: string
  /** 派活的 driver 会话 id（用于回传路由与审计）。 */
  driverSessionId: string
  /** 建议的名字（诊断/展示用）。 */
  label?: string
  /** 取消信号（driver 取消 / 人格停止）。 */
  signal: AbortSignal
  attachments?: PersonaAttachment[]
}

/** worker 执行结果（无论成败都回到 driver）。 */
export interface WorkerOutcome {
  runId: string
  label?: string
  status: "done" | "failed" | "cancelled"
  /** 成功时：worker 的结论（driver 据此组织答复）。 */
  report?: string
  /** 失败/取消时的原因（driver 需要知道"这活没干成"才能决定补救）。 */
  error?: string
  /** 耗时（ms，诊断用）。 */
  elapsedMs?: number
}

/**
 * worker 事件的上下文构造：把 worker 的回报包装成一条可由 driver 处理的事件。
 *
 * 为什么回报走**事件**而不是回调直接塞进 driver：
 * 1. driver 是串行的——回报必须排队/插入，不能打断它正在做的事
 * 2. 回报与用户消息在 driver 眼里应当是**同一类东西**（都是"新信息到了"），
 *    统一成事件才能让 driver 用同一套逻辑决定优先级
 * 3. 审计上一致：所有进入 driver 的东西都有事件 id 与来源
 */
export function workerReportEvent(input: {
  personaId: string
  /** 派活的那个 driver（回报要回到它，不是回到角色里的随便谁）。 */
  driverId: string
  driverSessionId: string
  outcome: WorkerOutcome
  /** 原始派活指令（回报里要能对上"这是哪件活的回音"）。 */
  task: string
}): PersonaEvent {
  const { outcome } = input
  const ok = outcome.status === "done"
  const label = outcome.label ? `「${outcome.label}」` : ""
  const head = ok ? `工作会话${label}已完成` : `工作会话${label}${
    outcome.status === "cancelled" ? "已取消" : "失败"
  }`
  const parts: string[] = []
  parts.push(`【任务】${truncate(input.task, 800)}`)
  if (ok) {
    parts.push(`【结论】\n${outcome.report?.trim() || "（worker 未给出内容）"}`)
  } else {
    parts.push(`【原因】${outcome.error?.trim() || "未提供错误信息"}`)
  }
  if (outcome.elapsedMs !== undefined) parts.push(`【耗时】${formatMs(outcome.elapsedMs)}`)

  return {
    id: `worker:${outcome.runId}`,
    // 复用 message 类目：对 driver 而言"有信息到了"与收到消息是同一类处理
    kind: "message",
    // 定向回派活的那个 driver（不广播给同角色的其它 driver）
    personaId: input.personaId,
    driverId: input.driverId,
    channel: "worker",
    threadKey: input.driverId,
    context: {
      source: head,
      summary: ok ? `工作会话${label}回报了结论` : `工作会话${label}未完成`,
      refs: { extra: { runId: outcome.runId, status: outcome.status, driverId: input.driverId, driverSessionId: input.driverSessionId } },
    },
    body: parts.join("\n\n"),
    receivedAt: Date.now(),
  }
}

function truncate(text: string, max: number): string {
  const t = text.trim()
  return t.length <= max ? t : `${t.slice(0, max)}…`
}

function formatMs(ms: number): string {
  if (ms < 1000) return `${ms}ms`
  const s = Math.round(ms / 1000)
  if (s < 60) return `${s}秒`
  const m = Math.floor(s / 60)
  return `${m}分${s % 60}秒`
}
