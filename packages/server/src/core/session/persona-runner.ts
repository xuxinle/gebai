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
  /** 取消一个正在跑的任务（用户停止按钮同款路径：abort + 解开全部挂起等待）。 */
  cancel(sessionId: string): void
  run(
    sessionId: string,
    user: string,
    prompt: string,
    opts?: {
      attachments?: Array<{ name: string; mime?: string; path?: string; data?: Uint8Array }>
      channelNote?: string
      /** auto 姿态：需审批工具自动通过（仅当 `approvalMode` 为 readonly/auto）。 */
      autoApprove?: boolean
      /**
       * 交互模式（**必须显式传**）。
       *
       * 引擎缺省是 `realtime`（假定有实时前端）。人格会话没有前端——不传的话：
       * `ask` 每次推一张没人看的选择卡并阻塞 5 分钟，`show` 的 html 分支 / `page_capture`
       * 被当成可用随后失败。`multi_turn` 才是“有交互但经卡片/后端适配”的真实形态。
       */
      interactionMode?: "none" | "multi_turn" | "realtime"
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
   * 审批姿态（`GEBAI_PERSONA_APPROVAL` 的解析结果）。
   *
   * 可以传**函数**以支持热改（推荐：读环境变量的解析函数）——人格进程常常跑几周，
   * 而“临时放开/收紧”是真实诉求；写死成构造快照就必须重启，而重启会中断在跑的人格任务。
   *
   * - `readonly`（缺省）：**工具执行不弹卡，输出才审**（出站审核独立开关）
   * - `ask`：需审批工具走引擎审批点 → 审批桥推相期卡问人
   * - `auto`：同 readonly（旧名）
   * - `deny`：需审批工具直接拒绝
   *
   * 四档与引擎入参的对应关系见 `run()` 里的表格（尤其 deny 必须显式传
   * `autoApprove: false`，否则会被引擎的“无交互自动通过”变成全放行）。
   */
  approvalMode?: import("@gebai/persona").PersonaApprovalMode | (() => import("@gebai/persona").PersonaApprovalMode)
  /**
   * @deprecated 旧字段（等价 `approvalMode` 的 deny/readonly 两态）。保留只为兼容既有装配代码。
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
export function personaNote(input: PersonaRunInput, now = Date.now(), approvalMode: "readonly" | "ask" | "auto" | "deny" = "readonly"): string {
  const { identity, driver } = input
  const siblings = (input.siblings ?? []).filter((s) => s.id !== driver.id)
  const lines: string[] = []
  lines.push("## 你此刻的身份")
  lines.push(identity.identity.trim())
  lines.push("")
  lines.push(`- 角色：${identity.role}（${identity.name}）`)
  lines.push(`- 本次关注面：**${driver.focus}**（分身 \`${driver.id}\`）`)
  lines.push(`- 会话归属：${input.user}`)
  // 扮演者（实际用户身份）：**不写这段，模型会把自己当机器人**，
  // 于是说“我帮你转达”“请以您的名义发”——而它本来就该以这个人的名义说话。
  // 扮演者（实际用户身份）：**不写这段，模型会把自己当机器人**，
  // 于是说“我帮你转达”“请以您的名义发”——而它本来就该以这个人的名义说话。
  //
  // 字段是**自由键值**（部署方定：姓名/工号/部门/邮箱/花名…），这里不挑不筛：
  // 有哪些就全列给模型（都是该部署认定“此人是谁”的事实，挑掉哪一条都是我们替部署做了决定）。
  const actorFields = Object.entries(identity.actor?.fields ?? {}).filter(([, v]) => v.trim())
  if (actorFields.length) {
    lines.push(`- 你扮演的是：${actorFields.map(([k, v]) => `**${k}** ${v}`).join("｜")}`)
    lines.push("  （你以这个人的身份在群里说话，别人看到的就是他；不要自称机器人或助手）")
  }
  if (input.sessionKey) {
    lines.push(`- 当前会话：\`${input.sessionKey}\`（同一会话内的历史是连续上下文；不同群/话题的上下文互相隔离）`)
  }
  lines.push("")
  lines.push("## 工作方式")
  lines.push("你在**无人值守**地处理外部事件，没有人会替你转达消息、也没有人实时看着你的输出。因此：")
  lines.push("")
  lines.push(`1. **要回复谁就自己发**：用 \`act(to:"<渠道>:<地址>", verb:"send", args:{text:"…", as_user:true})\` 把话发出去。`)
  lines.push("   事件里的 `refs.chatId` / `refs.messageId` 就是收件地址与可引用回复的原消息。**收件地址必须逐字复制本轮事件 refs.chatId 的值，不要凭记忆或猜测写群 id**（写错会被白名单拒绝，白白浪费一次发送）。")
  lines.push("   **只写在正文里的回复没有任何人能看到。**")
  lines.push("   `as_user: true` 让你以用户本人（而不是机器人）的名义发言——群里看到的是「你」在说话。这是唯一允许的发送名义。")
  lines.push("   若用户授权失效（报错里会直说），**不要改用机器人名义发，也不要反复重试**——授权完成后会自动恢复；期间需要对外说明时，等用户重新完成授权后再发。")
  lines.push("   **话题群的消息必须回到原话题**：事件 refs 带 `threadId` 时，发送务必传 `args:{root: refs.messageId}`（reply 触发消息，飞书自动落回原话题）。漏了 root 会**另开一个新话题**，打不到正在讨论的人。")
  lines.push("   引用回复某条消息用 verb `reply`（`reply_to` = 那条消息 id）；话题内的消息 reply 自动落在原话题里。")
  lines.push("2. **不需要回复就什么都不做**：判断后直接结束是合法的，不要为了「有产出」而发消息。")
  lines.push("3. **不确定时宁可先说清楚**：需要人确认的时候，发消息问，不要自己猜测执行。")
  lines.push("4. 你只能看到**本轮**的这几个事件；更早的上下文在会话历史里。")
  lines.push("")
  // 输出目标：驱动层配置的"去哪儿发"。
  //
  // 为何这一段不能省：回来源消息时 refs 里有 chatId，但**主动**行为（定时巡检报群、
  // 告警推运维群）没有入站 refs——模型只能猜一个 id，猜错就被白名单拒。
  const outputs = driver.outputs ?? []
  if (outputs.length) {
    lines.push("## 你可以发到哪里（已配置）")
    lines.push("")
    for (const o of outputs) {
      const to = `${o.channel}:${o.address}`
      const label = o.label ? `（${o.label}）` : ""
      const thread = o.thread ? `，发话题时传 args:{root:"${o.thread}"}` : ""
      lines.push(`- ${o.default ? "**默认**：" : ""}\`${to}\`${label}${thread}`)
    }
    lines.push("")
    lines.push("主动发消息（不是回某条事件）时用上面的地址；回某条事件时优先回它自带的 chatId。")
    lines.push("**名单之外的目标一律会被拒**——不要尝试别的群。")
    lines.push("")
  }
  // 审批姿态：模型必须知道“哪些事会被人卡住”，否则它会反复重试被拒的操作，
  // 或者把“等审批”误当成“已失败”。
  //
  // 关键在于把**工具执行**与**对外输出**分开说：人格在 readonly 下可以随便动手查/算，
  // 但“发出去的话”仍要人批准——这两件事混淆时，模型要么不敢动手，要么以为发消息也不用审。
  lines.push("## 动手与发言：审的东西不一样")
  lines.push("")
  if (approvalMode === "deny") {
    lines.push("这个部署把你设成了**只读**：需要授权的操作（改删文件、提交单据等）会直接失败。")
    lines.push("遇到失败不要反复重试——改用只读手段查清事实，并在回复里把“需要人来做的那一步”说清楚。")
  } else if (approvalMode === "ask") {
    lines.push("这个部署要求：**需要授权的操作会弹卡让人点头**（卡片在他桌面上）。因此：")
    lines.push("")
    lines.push("- 被拒或超时后**不要重复试同一个操作**——先换方案，或在消息里说清需要人做什么。")
    lines.push("- 能自己查清的不必等人：只读操作（读文件/查接口/跑查询）不弹卡。")
  } else {
    lines.push("- **动手不弹卡**：读文件、跑命令、查接口这些你自己做，不必等人批准（错了能改）。")
    lines.push("- **发言要人点头**：以用户本人名义发出去的消息会先弹卡给人看（发出去就是他说的，错发代价在他）。")
    lines.push("  所以一次把话说清就行；**被驳回了不要改个字再发一遍**——那等于绕过审核。")
  }
  lines.push("- 需要主管决策、需要外部信息、需要人去某个地方动手时，不要硬猜也不要装成功：")
  lines.push("  在你要发出去的回复里**直接把问题问出来**（“这个要你确认一下：……”），不要自言自语。")
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
  private readonly approvalModeOf: () => import("@gebai/persona").PersonaApprovalMode
  private readonly log?: EnginePersonaRunnerOptions["log"]
  private readonly refreshBeforeRun?: () => Promise<void>

  constructor(opts: EnginePersonaRunnerOptions) {
    this.engine = opts.engine
    // 兼容旧字段：显式给 autoApprove=false 的场景等价于 deny；旧缺省 true 等价 readonly。
    // 缺省（都不给）= readonly（工具执行不弹卡，输出仍审）。
    const fixed = opts.approvalMode ?? (opts.autoApprove === false ? "deny" : "readonly")
    this.approvalModeOf = typeof fixed === "function" ? fixed : () => fixed
    this.log = opts.log
    this.refreshBeforeRun = opts.refreshBeforeRun
  }

  /** 当前审批姿态（每次现取，支持热改）。 */
  get approvalMode(): import("@gebai/persona").PersonaApprovalMode {
    try {
      return this.approvalModeOf()
    } catch {
      // 解析函数抛错：回到缺省 readonly（不把人格卡死，也不意外提权）
      return "readonly"
    }
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

    // ② 渲染成模型输入（多事件按到达顺序，带「谁说的/回到哪」上下文 +
    //    本轮会话键 + 已注入的输出目标）。输出目标是**驱动层配置**——
    //    不告诉模型的话，它在“主动发消息”场景只能猜群 id（猜错就被白名单拒）。
    const outputs = [...(input.driver.outputs ?? [])]
    const prompt = renderBatch(events, {
      includeReplyHint: true,
      ...(input.sessionKey ? { sessionKey: input.sessionKey } : {}),
      ...(outputs.length ? { outputs } : {}),
    })

    // ③ 聚合附件（图片等：人格看到的和用户发的是同一份）
    const attachments = events.flatMap((e) => e.attachments ?? [])

    this.log?.info(
      `[persona:runner] 跑 ${input.personaId}/${input.driver.id} 第 ${input.round} 轮，` +
        `${events.length} 个事件（${summarizeBatch(events)}）`,
    )

    // ④ 交给引擎。channelNote 承载人格身份与出站纪律——engine 无需任何 persona 分支
    //
    // 取消桥（signal → engine.cancel）：engine.run 没有 signal 入参（任务自管 controller），
    // 调度器的 cancelRunning/stop 置位的 abort 若无人消费，正在跑的引擎轮会完整执行完
    // （可能长达数分钟、含对外动作）——「停了但还在动」。这里监听 signal，触发时走
    // engine.cancel（与用户停止按钮同一条路径：abort + 解开审批/选择等挂起等待），
    // 引擎轮得以尽快退出；监听器用完即摘，不残留。
    const onAbort = () => {
      try {
        this.engine.cancel(input.sessionId)
        this.log?.info(`[persona:runner] ${input.personaId}/${input.driver.id} 的引擎轮已被取消（signal abort → engine.cancel）`)
      } catch (err) {
        this.log?.warn(`[persona:runner] 取消引擎轮失败（${input.sessionId}）: ${String((err as Error)?.message || err)}`)
      }
    }
    input.signal.addEventListener("abort", onAbort, { once: true })
    try {
      // 审批姿态 → 引擎入参。**三个都要显式传**，少一个就会走错分支：
      //
      // | 姿态 | autoApprove | interactionMode | 实际效果 |
      // |---|---|---|---|
      // | readonly（缺省） | true | multi_turn | 工具直接执行；模型主动 ask 时仍能弹卡问人 |
      // | ask | 不传 | multi_turn | 需审批工具 → 事件 → 审批桥推相期卡 |
      // | auto | true | multi_turn | 同 readonly（兼容旧值） |
      // | deny | **false** | none | 需审批工具直接拒绝（不空等超时） |
      //
      // ## deny 必须显式传 `autoApprove: false`（否则它等于 auto）
      //
      // 引擎里“无交互模式 + 本地形态”会**自动通过**审批（engine.ts 的 isApprovalSkipped：
      // `interactionMode === "none" && authMode !== "server"` → true）。所以只传
      // `interactionMode: "none"` 的 deny，实际行为是“全部放行”——与语义完全相反。
      // 传 `autoApprove: false` 后引擎的 approvalPolicy 变 `deny`，无交互硬门槛才真的拦住。
      //
      // ## interactionMode 绝不能用引擎缺省的 `realtime`
      //
      // 后者假定有实时前端：`ask` 会推一张没人看的选择卡并阻塞 5 分钟，
      // `show` 的 html 分支 / `page_capture` 被当成可用随后失败。
      const mode = this.approvalMode
      const interactionMode: "none" | "multi_turn" = mode === "deny" ? "none" : "multi_turn"
      const approveOpt = mode === "readonly" || mode === "auto" ? { autoApprove: true } : mode === "deny" ? { autoApprove: false } : {}
      await this.engine.run(input.sessionId, input.user, prompt, {
        channelNote: personaNote(input, undefined, mode),
        interactionMode,
        ...approveOpt,
        ...(attachments.length ? { attachments } : {}),
      })
    } finally {
      input.signal.removeEventListener("abort", onAbort)
    }
  }
}
