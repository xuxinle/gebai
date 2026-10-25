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
   * 每轮开始前调一次（实现方自己负责节流）。
   *
   * 用途：**事件源热加载**——`EventSourceManager.refreshIfChanged()` 要有调用者，
   * 否则“放文件即注册”只是一句文档：源文件改了/新增了，进程内永远不会重扫。
   *
   * 为何放在 runner 而不是定时器：人格的每轮就是“这个世界刚刚发生了什么”的边界，
   * 在它前面重扫一次最自然；定时器则会在完全空闲的进程里白跑目录遍历。
   * 实现方必须节流（目录遍历虽然便宜，但每一轮都扫在多盘部署下不便宜）。
   */
  refreshBeforeRun?: () => Promise<void>
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
 * 五件必须写进去的事（前三条是基础身份，后两条是自组织能力）：
 *
 * 1. **身份**（identity 原样，不做改写——它是部署方写的权威文本）
 * 2. **关注面**（driver.focus）：否则同一人格的不同分身行为完全一样，分工白设
 * 3. **出站纪律**：告诉它「回话要走 `act`」，而不是把回复写在正文里等人转达
 *    （无人转达——这是最容易被忽略、后果最直接的一条）
 * 4. **自组织**：它可以自己开/撤分工。不写进去时模型根本不知道自己有这个能力
 *    （工具在表里，但它面对新情况只会“这次先顶着”——那份分工永远建不起来）
 * 5. **同人格其它分身**：不告诉它有哪些兄弟时，同一个问题会被几个分身重复答一遍
 *    （每个都在自己的会话里把它当第一次看见）
 */
export function personaNote(input: PersonaRunInput, now = Date.now()): string {
  const { identity, driver } = input
  const siblings = (input.siblings ?? []).filter((s) => s.id !== driver.id)
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
  lines.push("## 你可以调整自己的分工")
  lines.push("遇到「这类事以后长期归我管」或「这次专项得单独盯」时，**不必等人改配置**：")
  lines.push("")
  lines.push("- `driver_list`：先看自己现在有哪些分身（避免建重复的）")
  lines.push("- `driver_create`：开一个新分身（写清 `focus` 与 `subscriptions`，并给 `reason`）")
  lines.push("- `driver_retire`：撤掉不再需要的（**静态定义的撤不掉**，那是部署方写死的）")
  lines.push("")
  lines.push("新建的分身立即生效（下一条符合它订阅的事件就投给它），并有自己的常驻会话。")
  lines.push("反过来：**一时的事不要建分身**——它会长驻，长期占一份订阅面与上下文。")
  if (siblings.length) {
    lines.push("")
    lines.push(`## 同一个「${identity.name}」的其它分身`)
    lines.push("它们各盯一面、各有上下文，看不到你看到的事件。别替它们把话说尽：")
    for (const s of siblings) lines.push(`- \`${s.id}\`：${s.focus}`)
  }
  // 记忆纪律：只在人格声明了工具包时才写。
  //
  // 为何必须跟工具面同源：只装工具不写这段，模型面对一排 cefu_* 工具想不起来用；
  // 只写这段不装工具，它会去调不存在的工具然后失败——两种错配都是“配了但没效果”。
  const memoryAgents = (input.agents ?? []).filter((a) => a === "cefu")
  if (memoryAgents.length) {
    lines.push("")
    lines.push("## 你的记忆（册府）")
    lines.push("你有一个持久化的知识库。**你每轮都从零开始，但册府不会。** 所以：")
    lines.push("")
    lines.push("### 答之前先查")
    lines.push("回答任何「怎么办 / 找谁 / 在哪查 / 上次是怎么解决的」之前，先 `cefu_search`。")
    lines.push("用你自然想问的那句话就行（中文短句即可，不必拆词）。")
    lines.push("要具体步骤时用 `cefu_get(id)` 取全文——摘要只够判断“是不是这条”。")
    lines.push("")
    lines.push("### 什么时候写")
    lines.push("只有三种时机，其余先别写：")
    lines.push("")
    lines.push("1. **用户纠正了你**（「不对，应该…」）——这是最贵也最准的知识，立刻写。")
    lines.push("   `source_ref` 写 `用户纠正@<会话id>`，``confidence`` 给 ``verified``。")
    lines.push("2. **你实测出稳定结论**（跑了一次命令看到真阈值/IP/账号，且下次还会用到）。")
    lines.push("3. **同一个问题被问第二遍**——说明它反复出现，值得成册。")
    lines.push("")
    lines.push("### 写的时候三条硬约束")
    lines.push("- **`title`**：一行能认出来是什么。没有它，条目在列表里认不出。")
    lines.push("- **`category`**：成册的依据（如 `调度/配额`、`人/职责`）。没分类的条目就是找不到的碎片。")
    lines.push("- **`source_ref`**：出处（`文件:行号` / 会话 id / 人名）。**没有出处的知识事后无法判断该不该信**——")
    lines.push("  这条是硬性的，缺了会被拒绝。")
    lines.push("")
    lines.push("另两件最值钱的事：")
    lines.push("- **`aliases` 是召回率最便宜的投资**：把“别人会怎么问”都写上（同义词/口语/报错原文/缩写）。")
    lines.push("  “写入延迟高”和“p99 上涨”和“TSDB 慢”是同一个问题的三种问法。")
    lines.push("- **`steps` 必须有序**：第一条是“最该先查/先做”的动作，而不是背景介绍。")
    lines.push("")
    lines.push("### 别把它当成万能笔记")
    lines.push("**能在代码/配置里 diff 的，就写代码；不能的（运行时事实、他人知识、环境特有配置）才进册府。**")
    lines.push("比如“构建用 -Werror”是代码约定，“生产 TSDB 写放大阈值 5x、超了找张三”才是册府该记的。")
    lines.push("")
    lines.push("### 引用时带出处")
    lines.push("回话引用知识时带上 id 与出处（如 `[e_tsdb] TSDB 写入延迟升高（出处 runbook.md:12）`）——")
    lines.push("这让人能追溯，也让你下次能复查。**检索无命中就明说“册府里没有”，不要用记忆中的常识冒充。**")
  }
  lines.push("")
  lines.push(`（注记生成于 ${new Date(now).toISOString()}）`)
  return lines.join("\n")
}

export class EnginePersonaRunner implements PersonaRunner {
  private readonly engine: PersonaEnginePort
  private readonly autoApprove: boolean
  private readonly log?: EnginePersonaRunnerOptions["log"]
  private readonly refreshBeforeRun?: () => Promise<void>

  constructor(opts: EnginePersonaRunnerOptions) {
    this.engine = opts.engine
    this.autoApprove = opts.autoApprove !== false
    this.log = opts.log
    this.refreshBeforeRun = opts.refreshBeforeRun
  }

  isRunning(sessionId: string): boolean {
    return this.engine.isRunning(sessionId)
  }

  async run(input: PersonaRunInput): Promise<void> {
    // ⓪ 热加载检查（失败不影响本轮：源目录不可读不该让已经到达的事件丢掉）
    if (this.refreshBeforeRun) {
      try {
        await this.refreshBeforeRun()
      } catch (err) {
        this.log?.warn(`[persona:runner] 事件源热加载检查失败（本轮照常跑）: ${String((err as Error)?.message || err)}`)
      }
    }
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
