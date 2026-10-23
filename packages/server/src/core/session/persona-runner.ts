/**
 * `PersonaRunner` 的引擎实现：把「一个 driver 的一轮」交给 `engine.run` 跑完。
 *
 * ## 为什么不改 engine
 *
 * engine 已有 `channelNote` 入参（通道无关的注入注记，进系统提示词的该段）。
 * 人格身份与「这个分身在关注什么」正是同一性质的东西——**注入注记**，
 * 不是引擎逻辑。因此 runner 只需：
 *
 * ```
 * renderBatch(events)            → prompt
 * personaNote(identity, driver)  → opts.channelNote
 * engine.run(sessionId, user, prompt, { channelNote, attachments })
 * ```
 *
 * **net 效果：engine 一行不改**。这比「给 engine 加 persona 入参」安全得多——
 * 后者会让所有会话的提示词构建路径都多一个分支（开关关闭时也得走那个分支判断）。
 *
 * ## 多轮与插话
 *
 * persona 的调度器会在轮末检查信箱：若非空则再调一次 `run`（引擎视角是两次独立 run，
 * 不违反同会话串行约束）。本 runner 只负责「跑好这一轮」，多轮由调度器驱动。
 *
 * runner 在跑之前先 `mailbox.drain()`，把运行期间到达的事件与传入的 events 合批：
 * 这样「说话说到一半又被补充了信息」不会丢掉后半段（否则那些事件要等下一轮）。
 */

import type { PersonaEvent } from "@gebai/persona"
import { renderBatch, summarizeBatch } from "@gebai/persona"
import type { PersonaRunInput, PersonaRunner } from "@gebai/persona"

/** 引擎面（只声明 runner 需要的部分，便于单测）。 */
export interface PersonaEnginePort {
  isRunning(sessionId: string): boolean
  run(
    sessionId: string,
    user: string,
    prompt: string,
    opts?: {
      attachments?: Array<{ name: string; mime?: string; path?: string; data?: Uint8Array }>
      channelNote?: string
      /** 无人值守：人格运行在无交互通道，需审批工具自动通过（部署方整体担责，与既有 bot 同姿态）。 */
      autoApprove?: boolean
    },
  ): Promise<void>
}

export interface EnginePersonaRunnerOptions {
  engine: PersonaEnginePort
  log?: { info(msg: string): void; warn(msg: string): void }
  /**
   * 人格运行是否自动通过审批。
   *
   * 默认 `true`：人格的典型场景是「群里来消息 → 它自己判断要不要回」，没有人在
   * 审批卡前面等着。这与既有飞书 bot 的 `GEBAI_FEISHU_BOT_AUTO_APPROVE` 同一姿态
   * （部署方整体担责）。需要人工把关时设为 `false`，此时需审批工具会**直接拒绝**
   * （不空等超时）——人格会如实说"我需要确认"而不是卡死。
   */
  autoApprove?: boolean
}

/**
 * 构造注入系统提示词的「人格注记」。
 *
 * 这段文本决定模型「知道自己是谁、此刻在盯什么、回话该走什么通道」。
 * 三件必须写进去的事：
 *
 * 1. **身份**（identity 原样，不做改写——它是部署方写的权威文本）
 * 2. **关注面**（driver.focus）：否则同一人格的不同分身行为完全一样，分工白设
 * 3. **出站纪律**：告诉它「回话要走 `act`」，而不是把回复写在正文里等人转达
 *    （无人转达——这是最容易被忽略、后果最直接的一条）
 */
export function personaNote(input: PersonaRunInput, now = Date.now()): string {
  const { identity, driver } = input
  const lines: string[] = []
  lines.push("## 你此刻的身份")
  lines.push(identity.identity.trim())
  lines.push("")
  lines.push(`- 角色：${identity.role}（${identity.name}）`)
  lines.push(`- 本次关注面：**${driver.focus}**（分身 \`${driver.id}\`）`)
  lines.push(`- 会话归属：${input.user}`)
  lines.push("")
  lines.push("## 工作方式")
  lines.push("你在**无人值守**地处理外部事件，没有人会替你转达消息、也没有人实时看着你的输出。因此：")
  lines.push("")
  lines.push(`1. **要回复谁就自己发**：用 \`act(to:"<渠道>:<地址>", verb:"send", args:{text:"…", as_user:true})\` 把话发出去。`)
  lines.push("   事件里的 `refs.chatId` / `refs.messageId` 就是收件地址与可引用回复的原消息。")
  lines.push("   **只写在正文里的回复没有任何人能看到。**")
  lines.push("   `as_user: true` 让你以用户本人（而不是机器人）的名义发言——群里看到的是「你」在说话。")
  lines.push("   若用户授权失效（报错里会直说），改 `as_user: false` 以机器人名义发，或在界面完成授权后重试。")
  lines.push("2. **不需要回复就什么都不做**：判断后直接结束是合法的，不要为了「有产出」而发消息。")
  lines.push("3. **不确定时宁可先说清楚**：需要人确认的时候，发消息问，不要自己猜测执行。")
  lines.push("4. 你只能看到**本轮**的这几个事件；更早的上下文在会话历史里。")
  lines.push("")
  lines.push(`（注记生成于 ${new Date(now).toISOString()}）`)
  return lines.join("\n")
}

export class EnginePersonaRunner implements PersonaRunner {
  private readonly engine: PersonaEnginePort
  private readonly autoApprove: boolean
  private readonly log?: EnginePersonaRunnerOptions["log"]

  constructor(opts: EnginePersonaRunnerOptions) {
    this.engine = opts.engine
    this.autoApprove = opts.autoApprove !== false
    this.log = opts.log
  }

  isRunning(sessionId: string): boolean {
    return this.engine.isRunning(sessionId)
  }

  async run(input: PersonaRunInput): Promise<void> {
    // ① 合批：把运行期间插进来的事件并进本轮（不并的话它们要等下一轮才处理）
    const drained = input.mailbox.drain()
    const events: PersonaEvent[] = [...input.events, ...drained]
    if (events.length === 0) return // 防御：调度器不该传空轮

    // ② 渲染成模型输入（多事件按到达顺序，带「谁说的/回到哪」上下文）
    const prompt = renderBatch(events, { includeReplyHint: true })

    // ③ 聚合附件（图片等：人格看到的和用户发的是同一份）
    const attachments = events.flatMap((e) => e.attachments ?? [])

    this.log?.info(
      `[persona:runner] 跑 ${input.personaId}/${input.driver.id} 第 ${input.round} 轮，` +
        `${events.length} 个事件（${summarizeBatch(events)}）`,
    )

    // ④ 交给引擎。channelNote 承载人格身份与出站纪律——engine 无需任何 persona 分支
    await this.engine.run(input.sessionId, input.user, prompt, {
      channelNote: personaNote(input),
      ...(attachments.length ? { attachments } : {}),
      ...(this.autoApprove ? { autoApprove: true } : {}),
    })
  }
}
