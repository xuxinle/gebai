/**
 * 人格运行的**审批桥**：把引擎的审批请求 / 选择请求送到相期窗口，等人点。
 *
 * ## 为什么必须有这一层
 *
 * 人格跑在无人值守的通道里：群里来一条消息，它自己判断要不要回、要不要去查。
 * 这个形态下「需审批工具」（`sh` / 删文件 / `py` / 提交工单…）只有三个可能姿态，
 * 而**前两个都是错的**：
 *
 * | 姿态 | 后果 |
 * |---|---|
 * | 自动通过 | 提示词注入诱导模型跑一条 `rm -rf`，**没人拦**。人格以用户本人名义工作，责任落在用户身上 |
 * | 一律拒绝 | 人格一遇到要动手就哑掉（「这个需要你确认」然后什么也不做），等于半个残废 |
 * | **弹窗问人** | 人不在终端前也能在桌面上看到「要跑这条命令，准不准」，点一下继续 |
 *
 * 设计口径写的就是第三条（不能决策或需要实际用户帮助时用相期弹窗让人介入），
 * 而此前实现里相期只用于**出站审核**（发消息前批准），审批本身完全没接。
 *
 * ## 三档策略（`GEBAI_PERSONA_APPROVAL`）
 *
 * | 值 | 语义 | 适用 |
 * |---|---|---|
 * | `ask`（缺省） | 推相期卡，按答复放行/拒绝 | 生产默认 |
 * | `auto` | 直接通过（引擎 `autoApprove`） | 完全信任的隔离环境 |
 * | `deny` | 直接拒绝（fail-closed，不空等） | 只读人格 / 高敏环境 |
 *
 * ## 三条工程约束（都是真实事故形态）
 *
 * 1. **相期不在 ≠ 可以放行**：送不到就**拒绝**并说明原因（fail-closed）。
 *    反过来「送不到就放行」会让一张送不出去的卡变成一张空白支票。
 * 2. **同一个 toolCallId 只问一次**：引擎在审批被拒/超时后会重试同一调用（`retries` 递增），
 *    每次都推一张卡会把窗口刷成瀑布。已知结论的（已批准/已拒绝）按结论回答。
 * 3. **超时不清空结论**：卡还留在用户窗口上，他随时可能点；但引擎那一轮不能永远挂着——
 *    所以超时就按「拒绝」回给引擎，同时在理由里说清「卡还在，点了也认」。
 *
 * ## 与出站审核的关系
 *
 * 两者都用相期，但**不是一件事**：
 * - 出站审核（`persona-outbound-review.ts`）审的是「这条消息以你的名义发出去，行不行」
 * - 审批桥审的是「这个工具要不要执行」
 *
 * 前者在工具内部（act 的 execute 里），后者在引擎的审批点；不能合并——
 * 合并会让「批准了发消息」顺带批准了同一轮里的 `rm`。
 */

import type { AskCardTool } from "../tools/persona-outbound-review"

/** 相期 `card_ask` 的最小执行面（与 `persona-outbound-review` 同形，避免两处定义分叉）。 */
export type AskCardToolLike = AskCardTool

/** 审批桥的引擎面（只声明桥需要的部分，便于单测）。 */
export interface ApprovalBridgeEnginePort {
  /** 提交审批结论（与用户在前端点「批准/拒绝」同一条路径）。 */
  decideApproval(sessionId: string, toolCallId: string, approve: boolean): Promise<unknown>
  /** 提交选择结论（`null` = 取消/拒绝；单独选项传字符串）。 */
  decideChoice(sessionId: string, choiceId: string, selection: string | string[] | null): Promise<unknown>
  /** 判定该会话是否是人格会话（只有人格会话过桥：普通 web 会话有真正的前端，不需要弹桌面卡）。 */
  isPersonaSession(sessionId: string): Promise<boolean>
}

export interface ApprovalBridgeDeps {
  /** 相期 `card_ask`（compose 注入；缺省 = 无法问人 → ask 模式按拒绝处理）。 */
  askCard?: AskCardTool
  /** 相期是否已连接（拿不到视为可用——送卡本身还有 `delivered` 兜底）。 */
  isXiangqiConnected?: () => boolean
  /**
   * 审批姿态。可以传函数以支持热改（`GEBAI_PERSONA_APPROVAL` 改完下一个审批即生效）。
   */
  mode: "ask" | "auto" | "deny" | (() => "ask" | "auto" | "deny")
  /** 等待答复时限（毫秒，缺省 5 分钟；与引擎审批超时对齐）。 */
  waitMs?: number
  log?: { info(msg: string): void; warn(msg: string): void }
}

/** 默认等待时限：与引擎 `APPROVAL_TIMEOUT` 对齐（5 分钟）。 */
export const DEFAULT_APPROVAL_WAIT_MS = 5 * 60 * 1000

/** 卡片正文截断（人要能在小卡片上读完参数）。 */
const ARGS_MAX = 1200

/**
 * 工具名 → 给人看得懂的动词（卡片标题用）。
 *
 * 为什么要人话：卡片的读者是**用户本人**，他看到「sh」不知道要发生什么，
 * 而看到「执行一条命令」就知道该看参数。未列出的工具直接显示工具名（诚实 > 编一个动词）。
 */
const TOOL_VERB: Record<string, string> = {
  sh: "执行一条命令",
  py: "跑一段 Python",
  js: "跑一段脚本",
  write: "写一个文件",
  edit: "改一个文件",
  file: "操作文件",
  task_add: "创建一个无人值守任务",
  task_update: "修改任务",
  task_remove: "删除任务",
  task_run: "立即执行一个任务",
  driver_create: "给自己开一个新分身",
  driver_retire: "撤掉一个分身",
}

export function describeTool(tool: string): string {
  return TOOL_VERB[tool] ?? tool
}

/** 审批请求的参数摘要（`sh` 的 command 要完整给——那正是他要判断的东西）。 */
export function summarizeArgs(args: Record<string, unknown> | undefined): string {
  if (!args || !Object.keys(args).length) return "（无参数）"
  const lines: string[] = []
  for (const [k, v] of Object.entries(args)) {
    const text = typeof v === "string" ? v : JSON.stringify(v)
    if (text === undefined) continue
    lines.push(`${k}: ${text.length > ARGS_MAX ? `${text.slice(0, ARGS_MAX)}…（共 ${text.length} 字符）` : text}`)
  }
  return lines.join("\n")
}

/**
 * 已知结论的记忆（有界 LRU）。
 *
 * 为何按实例而不是模块级：多实例（测试并行 / 多租户）共享模块级 Map 会互相污染，
 * 表现为「A 部署的批准让 B 部署直接放行」。
 */
class VerdictMemory {
  private readonly m = new Map<string, "approved" | "rejected">()
  constructor(private readonly capacity = 256) {}
  get(key: string): "approved" | "rejected" | undefined {
    const v = this.m.get(key)
    if (v !== undefined) {
      // 命中刷新位置（热的条目不被冷事件冲掉）
      this.m.delete(key)
      this.m.set(key, v)
    }
    return v
  }
  set(key: string, v: "approved" | "rejected"): void {
    if (this.m.size >= this.capacity) {
      const oldest = this.m.keys().next().value
      if (oldest !== undefined) this.m.delete(oldest)
    }
    this.m.set(key, v)
  }
  size(): number {
    return this.m.size
  }
}

/** 一次审批请求。 */
export interface ApprovalRequest {
  sessionId: string
  toolCallId: string
  tool: string
  args: Record<string, unknown>
  /** 第几次重试（引擎给的；>0 说明前一次被拒/超时）。 */
  retries?: number
  /** 人格/分身（卡片抬头用）。 */
  personaName?: string
}

/** 一次选择请求（ask 选项分支）。 */
export interface ChoiceRequest {
  sessionId: string
  choiceId: string
  prompt: string
  options: unknown[]
  multi: boolean
  personaName?: string
}

/**
 * 审批桥。
 *
 * 用法（compose 装配时）：
 *
 * ```ts
 * const bridge = new ApprovalBridge({ engine, deps })
 * bus.subscribe((ev) => void bridge.onEvent(ev))
 * ```
 */
export class ApprovalBridge {
  private readonly engine: ApprovalBridgeEnginePort
  private readonly deps: ApprovalBridgeDeps
  private readonly verdicts = new VerdictMemory()
  /** 正在问的（同 id 并发只问一次：引擎重试与事件重投都可能二次触发）。 */
  private readonly inflight = new Map<string, Promise<void>>()

  constructor(opts: { engine: ApprovalBridgeEnginePort; deps: ApprovalBridgeDeps }) {
    this.engine = opts.engine
    this.deps = opts.deps
  }

  /** 审批姿态（运行期现读；改配置后下次请求即生效）。 */
  get mode(): "ask" | "auto" | "deny" {
    const m = this.deps.mode
    if (typeof m !== "function") return m
    try {
      return m()
    } catch {
      return "ask" // 解析抛错：回到最安全的一档（问人）
    }
  }

  /**
   * 事件入口：只处理 `event.approval.request` / `event.choice.request`。
   *
   * 为何不返回 Promise 让调用方 await：事件总线是同步广播，await 会阻塞其它订阅者
   * （包括前端推送）。桥自己保证不抛（内部 catch 全部转成「拒绝」），单飞靠 `inflight`。
   */
  onEvent(ev: { type: string; sessionId: string; payload?: unknown }): void {
    const p = (ev.payload ?? {}) as Record<string, unknown>
    if (ev.type === "event.approval.request") {
      const toolCallId = String(p.toolCallId ?? "")
      if (!toolCallId) return
      void this.handleApproval({
        sessionId: ev.sessionId,
        toolCallId,
        tool: String(p.tool ?? ""),
        args: (p.arguments && typeof p.arguments === "object" ? p.arguments : {}) as Record<string, unknown>,
        ...(p.retries !== undefined ? { retries: Number(p.retries) } : {}),
        ...(typeof p.personaName === "string" ? { personaName: p.personaName } : {}),
      })
      return
    }
    if (ev.type === "event.choice.request") {
      const choiceId = String(p.choiceId ?? "")
      if (!choiceId) return
      void this.handleChoice({
        sessionId: ev.sessionId,
        choiceId,
        prompt: String(p.prompt ?? ""),
        options: Array.isArray(p.options) ? p.options : [],
        multi: p.multi === true,
        ...(typeof p.personaName === "string" ? { personaName: p.personaName } : {}),
      })
    }
  }

  /** 处理一次审批请求（可 await，便于单测）。 */
  async handleApproval(req: ApprovalRequest): Promise<void> {
    const key = `a:${req.sessionId}:${req.toolCallId}`
    const running = this.inflight.get(key)
    if (running) return running
    const task = this.doApproval(req).finally(() => this.inflight.delete(key))
    this.inflight.set(key, task)
    return task
  }

  private async doApproval(req: ApprovalRequest): Promise<void> {
    // ① 只代理人**人格会话**：普通 web 会话有真前端，用户会自己在页面上点。
    //    不判定的话，所有人格之外的会话也会往桌面弹卡（把网页里的审批重复推一遍）。
    const isPersona = await this.safeIsPersona(req.sessionId)
    if (!isPersona) return

    // ② 姿态：auto / deny 直接给结论（不推卡）
    if (this.deps.mode === "auto") {
      await this.decide(req.sessionId, req.toolCallId, true, "auto")
      return
    }
    if (this.deps.mode === "deny") {
      this.deps.log?.info(`[persona:approval] ${req.tool}（${req.toolCallId}）按 deny 策略拒绝`)
      await this.decide(req.sessionId, req.toolCallId, false, "deny")
      return
    }

    // ③ 同一调用已经问出过结论：按结论回答（引擎重试不该在窗口上再刷一张卡）
    const known = this.verdicts.get(`${req.sessionId}:${req.toolCallId}`)
    if (known !== undefined) {
      await this.decide(req.sessionId, req.toolCallId, known === "approved", `memory:${known}`)
      return
    }

    // ④ 问人
    const ask = this.deps.askCard
    if (!ask) {
      this.deps.log?.warn(
        `[persona:approval] ${req.tool} 需要审批，但相期卡工具未装配——按拒绝处理（fail-closed）。` +
          `设 GEBAI_PERSONA_APPROVAL=auto 可显式放开（不建议）`,
      )
      await this.decide(req.sessionId, req.toolCallId, false, "no-ask-card")
      return
    }
    if (this.deps.isXiangqiConnected && !this.deps.isXiangqiConnected()) {
      // 相期不在：卡送出去的概率接近 0，等下去只是把整轮拖到超时。
      // 但**不能放行**——送不到 ≠ 没人反对。
      this.deps.log?.warn(`[persona:approval] ${req.tool} 需要审批，但相期未连接——按拒绝处理（fail-closed）`)
      await this.decide(req.sessionId, req.toolCallId, false, "xiangqi-offline")
      return
    }

    const who = req.personaName ? `人格「${req.personaName}」` : "人格会话"
    const retryNote = req.retries && req.retries > 0 ? `\n\n（这是第 ${req.retries} 次询问同一操作：上一次被拒绝或超时。）` : ""
    let res: { data?: { delivered?: boolean; answered?: boolean; answer?: { choice?: unknown } } }
    try {
      res = await ask.execute(
        {
          kind: "choice",
          title: `${who}请求${describeTool(req.tool)}`,
          from: "人格审批",
          body:
            `**工具**：\`${req.tool}\`\n\n` +
            `**参数**：\n\`\`\`\n${summarizeArgs(req.args)}\n\`\`\`\n\n` +
            `批准后它将继续执行；驳回则这一轮会放弃这个操作。${retryNote}`,
          options: [
            { label: "批准", value: "approve" },
            { label: "驳回", value: "reject" },
          ],
          wait_ms: this.deps.waitMs ?? DEFAULT_APPROVAL_WAIT_MS,
        },
        {},
      )
    } catch (err) {
      // 弹卡本身炸了：不能当批准（那是把异常变成放行令）
      this.deps.log?.warn(`[persona:approval] 推审批卡失败（按拒绝处理）: ${String((err as Error)?.message || err)}`)
      await this.decide(req.sessionId, req.toolCallId, false, "card-error")
      return
    }

    const data = res.data ?? {}
    if (data.delivered === false) {
      this.deps.log?.warn(`[persona:approval] 审批卡未送达（相期未连接）——按拒绝处理（fail-closed）`)
      await this.decide(req.sessionId, req.toolCallId, false, "not-delivered")
      return
    }
    if (data.answered !== true) {
      // 超时：卡还在窗口上，但引擎这一轮不能永远挂着 → 按拒绝回。
      // 结论**不记入 memory**：人稍后点了不该被“上次超时”的旧账顶掉。
      this.deps.log?.info(`[persona:approval] ${req.tool} 等待超时（卡仍在相期窗口）——按拒绝处理`)
      await this.decide(req.sessionId, req.toolCallId, false, "timeout")
      return
    }
    const choice = String((data.answer as { choice?: unknown } | undefined)?.choice ?? "")
    const approved = choice === "approve"
    this.verdicts.set(`${req.sessionId}:${req.toolCallId}`, approved ? "approved" : "rejected")
    this.deps.log?.info(`[persona:approval] ${req.tool}（${req.toolCallId}）→ ${approved ? "批准" : "驳回"}`)
    await this.decide(req.sessionId, req.toolCallId, approved, `card:${choice}`)
  }

  /** 处理一次选择请求（ask 选项分支）。 */
  async handleChoice(req: ChoiceRequest): Promise<void> {
    const key = `c:${req.sessionId}:${req.choiceId}`
    const running = this.inflight.get(key)
    if (running) return running
    const task = this.doChoice(req).finally(() => this.inflight.delete(key))
    this.inflight.set(key, task)
    return task
  }

  private async doChoice(req: ChoiceRequest): Promise<void> {
    const isPersona = await this.safeIsPersona(req.sessionId)
    if (!isPersona) return

    // auto：模型自己决定（回 null = 用户未在时限内作答，模型据提示自行决策）
    if (this.deps.mode === "auto") {
      await this.safeDecideChoice(req.sessionId, req.choiceId, null)
      return
    }
    // deny：明确回「用户拒绝」，模型据此停止追问
    if (this.deps.mode === "deny") {
      await this.safeDecideChoice(req.sessionId, req.choiceId, null)
      return
    }

    const ask = this.deps.askCard
    if (!ask) return // 没卡工具：不干预，等引擎自己的超时（拒绝语义）

    // 选项规范化：引擎的 options 可能是字符串或 {title,description}（与 ask 工具同口径）
    const options = req.options.map((o, i) => {
      if (typeof o === "string") return { label: o, value: o }
      const obj = o as { title?: unknown; description?: unknown }
      const label = obj?.title !== undefined ? String(obj.title) : `选项 ${i + 1}`
      return { label, value: label, ...(obj?.description !== undefined ? { hint: String(obj.description) } : {}) }
    })
    if (!options.length) return

    const who = req.personaName ? `人格「${req.personaName}」` : "人格"
    let res: { data?: { delivered?: boolean; answered?: boolean; answer?: { choice?: unknown } } }
    try {
      res = await ask.execute(
        {
          kind: "choice",
          title: `${who}在问你一件事`,
          from: "人格提问",
          body: `${req.prompt}\n\n${req.multi ? "（可多选）" : ""}`,
          options,
          wait_ms: this.deps.waitMs ?? DEFAULT_APPROVAL_WAIT_MS,
        },
        {},
      )
    } catch (err) {
      this.deps.log?.warn(`[persona:choice] 推选择卡失败（按“未作答”处理）: ${String((err as Error)?.message || err)}`)
      await this.safeDecideChoice(req.sessionId, req.choiceId, null)
      return
    }
    const data = res.data ?? {}
    if (data.delivered === false || data.answered !== true) {
      // 超时/未送达：按「用户未作答」回（与 ask 工具超时同语义），模型会自行决策
      await this.safeDecideChoice(req.sessionId, req.choiceId, null)
      return
    }
    const picked = String((data.answer as { choice?: unknown } | undefined)?.choice ?? "")
    if (!picked) {
      await this.safeDecideChoice(req.sessionId, req.choiceId, null)
      return
    }
    await this.safeDecideChoice(req.sessionId, req.choiceId, picked)
  }

  /** 诊断：已记住的结论条数。 */
  stats(): { remembered: number; inflight: number; mode: string } {
    return { remembered: this.verdicts.size(), inflight: this.inflight.size, mode: this.mode }
  }

  private async safeIsPersona(sessionId: string): Promise<boolean> {
    try {
      return await this.engine.isPersonaSession(sessionId)
    } catch {
      // 判定失败：宁可不干预（引擎自己会按超时/拒绝收口），也不误推卡
      return false
    }
  }

  private async decide(sessionId: string, toolCallId: string, approve: boolean, source: string): Promise<void> {
    try {
      await this.engine.decideApproval(sessionId, toolCallId, approve)
    } catch (err) {
      this.deps.log?.warn(`[persona:approval] 提交审批结论失败（${source}）: ${String((err as Error)?.message || err)}`)
    }
  }

  private async safeDecideChoice(sessionId: string, choiceId: string, selection: string | null): Promise<void> {
    try {
      await this.engine.decideChoice(sessionId, choiceId, selection)
    } catch (err) {
      this.deps.log?.warn(`[persona:choice] 提交选择结论失败: ${String((err as Error)?.message || err)}`)
    }
  }
}
