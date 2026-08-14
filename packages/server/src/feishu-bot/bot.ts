/**
 * 飞书机器人对话桥接：消息事件 → 歌白会话 → Agent 回复推送。
 * - 会话映射：每个飞书单聊/群聊（chat_id）映射独立会话 `feishu_{chat_id}`
 * - 身份映射：单用户模式固定默认用户；多用户模式按 open_id 自动创建映射用户
 * - 桥接方式：经接口层（BotPromptAdapter）以「多轮交互 + 仅最终回复」运行——关键操作（审批/选择）
 *   回调询问用户，回复仅最终消息（无流式预览），不直接接触引擎层（不订阅事件总线）
 * - 命令：/help /new /sessions /cancel /approve /reject /approval-skip
 * 依赖全部注入（store/adapter/auth/api/conn），可独立单测。
 */
import { join } from "node:path"
import { mkdir, readFile, writeFile } from "node:fs/promises"
import type { AuthService } from "../auth"
import type { SessionStore } from "../core/store"
import { createFeishuApi, type FeishuApiLike } from "./api"
import { FeishuConn, type FeishuConnOptions } from "./conn"
import { createDiagramRenderer, type DiagramRenderer } from "../core/diagram-render"
import type { BotPromptAdapter } from "./adapter"

/** 桥接用到的依赖子集（Pick 结构类型，便于测试注入 fake）。 */
export type BotStore = Pick<SessionStore, "load" | "save" | "delete" | "listSessions" | "setEnv" | "getTmpDir">
export type BotAuth = Pick<AuthService, "defaultUser" | "listUsers" | "createUser">

/** 长连接客户端最小形状（测试注入 fake）。 */
export interface FeishuConnLike {
  start(): Promise<void>
  stop(): void
}

/** 发送队列：同一会话的消息严格串行（飞书发送限流友好）。 */
class ChatOutbox {
  private queue: Promise<unknown> = Promise.resolve()
  private previewMsgId: string | null = null
  private statusMsgId: string | null = null
  private deltaBuf = ""
  private flushTimer: ReturnType<typeof setTimeout> | null = null
  private lastFlush = 0
  private finalSent = false

  constructor(
    private chatId: string,
    private api: FeishuApiLike,
    private clock: () => number,
    private log: (msg: string) => void,
    private flushIntervalMs = 1500,
    private flushMinChars = 60,
  ) {}

  private enqueue(fn: () => Promise<unknown>): void {
    this.queue = this.queue.then(fn).catch((err) => this.log(`send failed: ${String((err as Error).message || err)}`))
  }

  /** 发送普通消息，或（replyTo 合法时）以引用气泡回复原消息。 */
  private post(msgType: string, content: unknown, replyTo?: string): Promise<unknown> {
    if (replyTo && /^[A-Za-z0-9_-]{8,64}$/.test(replyTo)) {
      return this.api.replyMessage(replyTo, msgType, content)
    }
    return this.api.sendMessage({ receiveId: this.chatId, receiveIdType: "chat_id", msgType, content })
  }

  /** 发送普通文本消息。 */
  sendText(text: string): void {
    this.enqueue(() =>
      this.api.sendMessage({ receiveId: this.chatId, receiveIdType: "chat_id", msgType: "text", content: { text } }),
    )
  }

  /** 状态消息（同一任务至多一条：重复调用不重发）。 */
  status(text: string): void {
    if (this.statusMsgId !== null) return
    this.enqueue(async () => {
      if (this.statusMsgId !== null) return
      const id = await this.api.sendMessage({ receiveId: this.chatId, receiveIdType: "chat_id", msgType: "text", content: { text } })
      this.statusMsgId = id
    })
  }

  /** 增量文本：累积并按（字符阈值/时间阈值）触发预览消息（发新撤旧）。 */
  feedDelta(text: string): void {
    this.deltaBuf += text
    const now = this.clock()
    const due = this.deltaBuf.length >= this.flushMinChars || now - this.lastFlush >= this.flushIntervalMs
    if (!due) {
      if (!this.flushTimer) {
        this.flushTimer = setTimeout(() => {
          this.flushTimer = null
          this.flushPreview()
        }, this.flushIntervalMs)
      }
      return
    }
    this.flushPreview()
  }

  private flushPreview(): void {
    if (this.flushTimer) {
      clearTimeout(this.flushTimer)
      this.flushTimer = null
    }
    const text = this.deltaBuf
    this.deltaBuf = ""
    if (!text) return
    this.lastFlush = this.clock()
    const prevId = this.previewMsgId
    this.enqueue(async () => {
      const id = await this.api.sendMessage({ receiveId: this.chatId, receiveIdType: "chat_id", msgType: "text", content: { text: `✍️ ${text}` } })
      this.previewMsgId = id
      if (prevId) void this.api.deleteMessage(prevId)
    })
  }

  /** 最终回复（卡片）：撤销预览/状态消息后发送（引用原消息）。 */
  final(text: string, replyTo?: string): void {
    this.clearTransient()
    const previewId = this.previewMsgId
    const statusId = this.statusMsgId
    this.previewMsgId = null
    this.statusMsgId = null
    this.enqueue(async () => {
      const card = buildReplyCard(text)
      await this.post("interactive", card, replyTo)
      this.finalSent = true
      if (previewId) void this.api.deleteMessage(previewId)
      if (statusId) void this.api.deleteMessage(statusId)
    })
  }

  /** 错误回复：清理瞬态消息后发送错误文本（引用原消息）。 */
  error(text: string, replyTo?: string): void {
    this.clearTransient()
    const previewId = this.previewMsgId
    const statusId = this.statusMsgId
    this.previewMsgId = null
    this.statusMsgId = null
    this.enqueue(async () => {
      await this.post("text", { text: `❌ ${text}` }, replyTo)
      if (previewId) void this.api.deleteMessage(previewId)
      if (statusId) void this.api.deleteMessage(statusId)
    })
  }

  /** 任务完成兜底：最终回复未发出时补一条完成提示并撤回状态消息（finalSent 检查在队列内，避免与 final 竞态）。 */
  taskDone(replyTo?: string): void {
    this.clearTransient()
    const statusId = this.statusMsgId
    this.statusMsgId = null
    this.enqueue(async () => {
      if (this.finalSent) return
      await this.post("text", { text: "✅ 任务完成" }, replyTo)
      if (statusId) void this.api.deleteMessage(statusId)
    })
  }

  private clearTransient(): void {
    if (this.flushTimer) {
      clearTimeout(this.flushTimer)
      this.flushTimer = null
    }
    this.deltaBuf = ""
  }
}

/** 最终回复卡片（lark_md 渲染 Markdown；无头部，仅内容）。 */
export function buildReplyCard(markdown: string): Record<string, unknown> {
  return {
    config: { wide_screen_mode: true },
    elements: [{ tag: "markdown", content: truncateForFeishu(markdown) }],
  }
}

/** 超长内容截断（卡片渲染上限保护）。 */
export function truncateForFeishu(text: string, limit = 12000): string {
  if (text.length <= limit) return text
  return `${text.slice(0, limit)}\n\n…（内容过长已截断，完整内容可在 Web UI 对应会话查看）`
}

/* ---------------- ask_user 选择卡片（后端实现：交互式卡片按钮，替代前端选择卡） ---------------- */

/** 选项值 → 按钮文案（复杂选项取 title，截断防超卡片上限）。 */
export function optionLabel(o: unknown, maxLen = 24): string {
  if (o && typeof o === "object") {
    const t = String((o as { title?: unknown }).title ?? "")
    if (t) return t.length > maxLen ? `${t.slice(0, maxLen)}…` : t
  }
  const s = String(o ?? "")
  return s.length > maxLen ? `${s.slice(0, maxLen)}…` : s
}

export interface ChoiceCardState {
  choiceId: string
  prompt: string
  options: unknown[]
  multi: boolean
  /** 多选模式已勾选项（按钮 value 集合）。 */
  selections: string[]
}

/** 构建 ask_user 选择交互卡片（按钮每行最多 5 个；多选模式含已选提示与「完成/放弃」，单选含「拒绝回答」）。 */
export function buildChoiceCard(state: ChoiceCardState): Record<string, unknown> {
  const valueOf = (act: string, v?: string) => (v !== undefined ? { choiceId: state.choiceId, act, v } : { choiceId: state.choiceId, act })
  const elements: Record<string, unknown>[] = [{ tag: "markdown", content: `**${state.prompt}**` }]
  if (state.multi && state.selections.length) {
    elements.push({ tag: "markdown", content: `已选：${state.selections.join("、")}（可继续选择，点「完成」提交）` })
  }
  const row = (actions: Record<string, unknown>[]) => ({ tag: "action", actions })
  const button = (label: string, value: Record<string, unknown>, type = "default") => ({
    tag: "button",
    text: { tag: "plain_text", content: label },
    type,
    value,
  })
  for (let i = 0; i < state.options.length; i += 5) {
    const chunk = state.options.slice(i, i + 5)
    elements.push(
      row(
        chunk.map((o) => {
          const v = optionLabel(o)
          const picked = state.multi && state.selections.includes(v)
          return button(v, valueOf("pick", v), picked ? "primary" : "default")
        }),
      ),
    )
  }
  elements.push(
    row(
      state.multi
        ? [
            button("✅ 完成选择", valueOf("done"), "primary"),
            button("❌ 放弃", valueOf("refuse"), "danger"),
          ]
        : [button("❌ 拒绝回答", valueOf("refuse"), "danger")],
    ),
  )
  return {
    config: { wide_screen_mode: true },
    header: { title: { tag: "plain_text", content: "🤖 歌白需要确认" }, template: "blue" },
    elements,
  }
}

/** 会话/用户 id 消毒：飞书 chat_id/open_id 为 `oc_`/`ou_` 前缀的字母数字下划线串。 */
export function sanitizeId(raw: string): string | null {
  return /^[A-Za-z0-9_-]{1,64}$/.test(raw) ? raw : null
}

/** 解析消息 content（JSON 字符串）为文本；非文本类型返回 null。 */
export function parseMessageContent(messageType: string, content: string): string | null {
  try {
    const obj = JSON.parse(content) as { text?: unknown; title?: unknown; content?: unknown }
    if (messageType === "text") {
      return typeof obj.text === "string" ? obj.text : null
    }
    if (messageType === "post") {
      // 富文本：拼接标题与各段文本
      const parts: string[] = []
      if (typeof obj.title === "string" && obj.title) parts.push(obj.title)
      const lines = Array.isArray(obj.content)
        ? (obj.content as unknown[][]).map((line) =>
            Array.isArray(line)
              ? line
                  .map((seg) => {
                    const s = seg as { tag?: string; text?: unknown; href?: unknown }
                    if (s.tag === "a") return typeof s.text === "string" ? `${s.text}(${String(s.href ?? "")})` : ""
                    return typeof s.text === "string" ? s.text : ""
                  })
                  .join("")
              : "",
          )
        : []
      parts.push(...lines.filter((l) => l))
      return parts.join("\n") || null
    }
    return null
  } catch {
    return null
  }
}

/** 剥离群聊 @ 机器人占位符（`@_user_1` 等，配合 mentions 字段）。 */
export function stripMentions(text: string): string {
  return text.replace(/@_user_\d+/g, "").replace(/^\s+/, "")
}

/** 按魔数探测图片 mime。 */
export function sniffImageMime(data: Uint8Array): string {
  if (data.length >= 3 && data[0] === 0xff && data[1] === 0xd8 && data[2] === 0xff) return "image/jpeg"
  if (data.length >= 8 && data[0] === 0x89 && data[1] === 0x50 && data[2] === 0x4e && data[3] === 0x47) return "image/png"
  if (data.length >= 12 && data[0] === 0x52 && data[1] === 0x49 && data[2] === 0x46 && data[3] === 0x46) return "image/webp"
  if (data.length >= 6 && data[0] === 0x47 && data[1] === 0x49 && data[2] === 0x46) return "image/gif"
  return "application/octet-stream"
}

export interface FeishuBotOptions {
  appId: string
  appSecret: string
  authMode: "local" | "server"
  /** GEBAI_HOME：会话归属映射（feishu/chat-owners.json）持久化目录。 */
  home: string
  store: BotStore
  /** 引擎接口层（对接接口、不侵入引擎层）：多轮交互 + 仅最终回复桥接。 */
  adapter: BotPromptAdapter
  auth: BotAuth
  api?: FeishuApiLike
  conn?: FeishuConnLike
  connOptions?: Omit<FeishuConnOptions, "appId" | "appSecret" | "onEvent" | "onCardAction">
  /** 图表后端渲染器（draw 工具后端渲染成图片；三语言，测试注入 fake）。 */
  renderer?: DiagramRenderer
  clock?: () => number
  log?: (msg: string) => void
  /** 预览刷新窗口（测试注入）。 */
  flushIntervalMs?: number
  flushMinChars?: number
}

/** 待处理的选择卡片（chatId → 状态；卡片按钮回调解析用）。 */
interface PendingChoice {
  choiceId: string
  sessionId: string
  multi: boolean
  prompt: string
  options: unknown[]
  selections: string[]
  /** 卡片消息 id（任务结束时撤回清理）。 */
  cardMessageId: string
  /** 任务发起者 open_id（授权校验：仅发起者可作答，防群聊成员越权）。 */
  openId: string
}

export class FeishuBot {
  private api: FeishuApiLike
  private conn: FeishuConnLike
  private clock: () => number
  private log: (msg: string) => void
  private renderer: DiagramRenderer
  private outboxes = new Map<string, ChatOutbox>()
  /** 运行中任务：sessionId → chatId（引擎事件 → 飞书回推的路由表）。 */
  private active = new Map<string, string>()
  private ensureLocks = new Map<string, Promise<string>>()
  private userCache = new Map<string, { userId: string; at: number }>()
  /** 待审批（命令 /approve /reject 用）：chatId → { toolCallId, tool, sessionId, openId }。 */
  private pendingApprovals = new Map<string, { toolCallId: string; tool: string; sessionId: string; openId: string }>()
  /** 待作答选择卡片（ask_user 交互卡片按钮用）：chatId → 状态。 */
  private pendingChoices = new Map<string, PendingChoice>()
  /** 运行中任务的发起飞书用户（审批授权校验用：仅任务发起者可批准/拒绝）。 */
  private runOwners = new Map<string, string>()
  /** chatId → 会话归属用户 id（内存 + feishu/chat-owners.json 持久化，重启恢复）。 */
  private chatOwners = new Map<string, string>()
  private ownersPath: string
  private ownersLoaded = false
  /** 归属映射写入串行队列（不同 chatId 首次交互并发时防整文件覆盖竞态）。 */
  private ownersWriteQueue: Promise<void> = Promise.resolve()

  constructor(private opts: FeishuBotOptions) {
    this.api = opts.api ?? createFeishuApi({ appId: opts.appId, appSecret: opts.appSecret })
    this.clock = opts.clock ?? Date.now
    this.log = opts.log ?? ((m) => console.log(`[feishu-bot] ${m}`))
    this.renderer = opts.renderer ?? createDiagramRenderer()
    this.ownersPath = join(opts.home, "feishu", "chat-owners.json")
    const connOpts: FeishuConnOptions = {
      appId: opts.appId,
      appSecret: opts.appSecret,
      onEvent: (ev) => void this.handleFeishuEvent(ev),
      onCardAction: (payload) => this.handleCardAction(payload),
      ...opts.connOptions,
    }
    this.conn = opts.conn ?? new FeishuConn(connOpts)
  }

  async start(): Promise<void> {
    // 不订阅引擎事件流：任务运行经接口层（BotPromptAdapter）回调驱动（仅最终回复 + 关键操作询问）
    await this.conn.start()
    this.log("started")
  }

  stop(): void {
    this.conn.stop()
  }

  /** 飞书事件入口（im.message.receive_v1）。 */
  async handleFeishuEvent(event: Record<string, unknown>): Promise<void> {
    if (event.schema !== "2.0") return
    const header = (event.header ?? {}) as { event_type?: string }
    if (header.event_type !== "im.message.receive_v1") return
    const ev = (event.event ?? {}) as {
      sender?: { sender_id?: { open_id?: string }; sender_type?: string }
      message?: { message_id?: string; chat_id?: string; message_type?: string; content?: string; mentions?: Array<{ key?: string }> }
    }
    const sender = ev.sender ?? {}
    const message = ev.message
    if (!message) return
    if (sender.sender_type === "app") return // 机器人自己的消息
    const chatId = sanitizeId(message.chat_id ?? "")
    const openId = sanitizeId(sender.sender_id?.open_id ?? "")
    if (!chatId || !openId) return
    const sessionId = await this.ensureSession(chatId, openId)
    const messageId = message.message_id ?? ""
    const messageType = message.message_type ?? ""
    const content = message.content ?? ""

    // 文本（含 @ 提及剥离）
    const text = parseMessageContent(messageType, content)
    if (text !== null) {
      const trimmed = stripMentions(text).trim()
      if (!trimmed) return
      if (trimmed.startsWith("/") && /^\/[a-z-]+/.test(trimmed)) {
        await this.handleCommand(trimmed, chatId, openId, sessionId)
        return
      }
      await this.runPrompt(chatId, sessionId, trimmed, messageId, openId)
      return
    }
    // 图片：下载为附件后进入会话
    if (messageType === "image") {
      let fileKey = ""
      try {
        fileKey = (JSON.parse(content) as { image_key?: string }).image_key ?? ""
      } catch {
        /* content 非法：忽略 */
      }
      if (!fileKey) return
      try {
        const data = await this.api.downloadResource(messageId, fileKey, "image")
        const mime = sniffImageMime(data)
        await this.runPrompt(chatId, sessionId, "（收到一张图片，请分析其内容）", messageId, openId, [
          { name: `${fileKey}.img`, mime, data },
        ])
      } catch (err) {
        this.outbox(chatId).sendText(`⚠️ 图片读取失败：${String((err as Error).message || err)}`)
      }
      return
    }
    this.outbox(chatId).sendText(`暂不支持 ${messageType} 类型消息，请发送文字。`)
  }

  /** 会话映射：`feishu_{chat_id}`，按需创建（chatId 级并发锁 + 归属持久化）。
   *  会话归属：首聊用户创建并持久化（feishu/chat-owners.json），群聊成员共享同一会话，
   *  引擎身份始终为会话归属用户（重启后从持久化恢复，不因他人发言重建/覆盖）。 */
  ensureSession(chatId: string, openId: string): Promise<string> {
    const locked = this.ensureLocks.get(chatId)
    if (locked) return locked
    const p = (async () => {
      const sessionId = `feishu_${chatId}`
      await this.loadOwners()
      const owner = this.chatOwners.get(chatId)
      if (owner) {
        // 归属已知：确认会话存在（被外部删除时以归属身份重建，不漂移）
        const existing = await this.opts.store.load(sessionId, owner)
        if (!existing) {
          const name = (await this.api.getChatName(chatId)) ?? "飞书会话"
          const now = this.clock()
          await this.opts.store.save({ id: sessionId, name, userId: owner, messages: [], todos: [], createdAt: now, updatedAt: now })
          this.log(`session recreated: ${sessionId} (user=${owner})`)
        }
        return sessionId
      }
      // 首次交互：当前用户创建并记录归属
      const userId = await this.resolveUser(openId)
      const name = (await this.api.getChatName(chatId)) ?? "飞书会话"
      const now = this.clock()
      await this.opts.store.save({ id: sessionId, name, userId, messages: [], todos: [], createdAt: now, updatedAt: now })
      this.chatOwners.set(chatId, userId)
      await this.saveOwners()
      this.log(`session created: ${sessionId} (user=${userId})`)
      return sessionId
    })()
    this.ensureLocks.set(chatId, p)
    void p.finally(() => this.ensureLocks.delete(chatId))
    return p
  }

  private async loadOwners(): Promise<void> {
    if (this.ownersLoaded) return
    this.ownersLoaded = true
    try {
      const raw = JSON.parse(await readFile(this.ownersPath, "utf8")) as Record<string, string>
      for (const [chatId, userId] of Object.entries(raw)) {
        // 双向校验：chatId 与 userId 均须为安全字符（防本地文件被篡改后注入引擎身份）
        if (/^[A-Za-z0-9_-]{1,64}$/.test(chatId) && /^[A-Za-z0-9_-]{1,64}$/.test(userId)) this.chatOwners.set(chatId, userId)
      }
    } catch {
      /* 首次运行/文件损坏：空表 */
    }
  }

  private saveOwners(): Promise<void> {
    this.ownersWriteQueue = this.ownersWriteQueue.then(async () => {
      try {
        await mkdir(join(this.opts.home, "feishu"), { recursive: true })
        await writeFile(this.ownersPath, JSON.stringify(Object.fromEntries(this.chatOwners), null, 2))
      } catch (err) {
        this.log(`chat owners persist failed: ${String((err as Error).message || err)}`)
      }
    })
    return this.ownersWriteQueue
  }

  /** 身份映射：单用户 → 默认用户；多用户 → open_id 映射用户（自动创建，10 分钟缓存）。 */
  async resolveUser(openId: string): Promise<string> {
    if (this.opts.authMode === "local") return this.opts.auth.defaultUser().id
    const username = `feishu_${openId}`
    const cached = this.userCache.get(username)
    if (cached && this.clock() - cached.at < 10 * 60_000) return cached.userId
    let user = (await this.opts.auth.listUsers()).find((u) => u.username === username)
    if (!user) {
      try {
        user = await this.opts.auth.createUser(username, crypto.randomUUID(), "user")
        this.log(`mapped feishu user created: ${username}`)
      } catch {
        // 并发创建冲突：重查
        user = (await this.opts.auth.listUsers()).find((u) => u.username === username)
      }
    }
    if (user) {
      this.userCache.set(username, { userId: user.id, at: this.clock() })
      return user.id
    }
    return this.opts.auth.defaultUser().id
  }

  private runPrompt(
    chatId: string,
    sessionId: string,
    text: string,
    messageId: string,
    openId: string,
    attachments?: Array<{ name: string; mime?: string; data: Uint8Array }>,
  ): Promise<void> {
    return (async () => {
      if (this.opts.adapter.isRunning(sessionId)) {
        this.outbox(chatId).sendText("⚠️ 该会话已有任务在运行，请稍候，或发送 /cancel 取消当前任务。")
        return
      }
      this.active.set(sessionId, chatId)
      this.runOwners.set(sessionId, openId) // 审批授权：绑定任务发起者
      // 收到消息：给用户消息添加「Typing」表情反应模拟「正在输入」（飞书无 typing 接口；emoji_type 用官方标准 ID，输出完成后撤回）
      let reactionId: string | null = null
      if (/^[A-Za-z0-9_-]{8,64}$/.test(messageId)) {
        try {
          reactionId = await this.api.addMessageReaction(messageId, "Typing")
        } catch {
          /* 提示为尽力而为：失败不影响主流程 */
        }
      }
      try {
        // 经接口层运行（多轮交互 + 仅最终回复）：关键操作（审批/选择）回调询问，回复仅最终消息
        await this.opts.adapter.run(sessionId, (await this.userOf(sessionId)), text, {
          messageId: /^[A-Za-z0-9_-]{8,64}$/.test(messageId) ? messageId : undefined,
          attachments,
        }, {
          onApproval: (toolCallId, tool) => {
            if (!toolCallId) return
            this.pendingApprovals.set(chatId, { toolCallId, tool, sessionId, openId: this.runOwners.get(sessionId) ?? "" })
            this.outbox(chatId).sendText(`⚠️ 需要审批：工具 \`${tool}\` 请求执行。\n回复 /approve 批准，/reject 拒绝（仅任务发起者可操作）。`)
          },
          onChoice: (choiceId, prompt, options, multi) => {
            void this.handleChoiceRequest(chatId, sessionId, { choiceId, prompt, options, multi })
          },
          onDraw: (renderId, code, name, format) => {
            void this.handleDrawRender(sessionId, chatId, { renderId, code, name, format })
          },
          onDone: (text) => {
            // 仅最终回复：直接发最终消息（引用原消息，无流式预览）
            if (text.trim()) this.outbox(chatId).final(text, messageId)
          },
          onError: (error) => {
            this.outbox(chatId).error(error, messageId)
          },
          onEnd: () => {
            // 任务结束（完成/出错）：收尾清理
            this.outbox(chatId).taskDone(messageId)
            this.cleanupChoices(chatId)
            this.active.delete(sessionId)
            this.runOwners.delete(sessionId)
          },
        })
      } catch (err) {
        this.outbox(chatId).error(String((err as Error).message || err))
      } finally {
        // 输出完成（成功/出错/兜底）：撤回「Typing」正在输入表情
        if (reactionId && /^[A-Za-z0-9_-]{8,64}$/.test(messageId)) {
          void this.api.deleteMessageReaction(messageId, reactionId)
        }
        this.runOwners.delete(sessionId)
      }
    })()
  }

  /** 引擎身份：会话归属用户（chatOwners 持久化映射优先，磁盘会话兜底）。 */
  private async userOf(sessionId: string): Promise<string> {
    const chatId = sessionId.startsWith("feishu_") ? sessionId.slice("feishu_".length) : ""
    if (chatId) {
      await this.loadOwners()
      const owner = this.chatOwners.get(chatId)
      if (owner) return owner
    }
    const s = await this.opts.store.load(sessionId)
    return s?.userId ?? this.opts.auth.defaultUser().id
  }

  private outbox(chatId: string): ChatOutbox {
    let o = this.outboxes.get(chatId)
    if (!o) {
      o = new ChatOutbox(chatId, this.api, this.clock, this.log, this.opts.flushIntervalMs, this.opts.flushMinChars)
      this.outboxes.set(chatId, o)
    }
    return o
  }

  /**
   * draw 工具后端渲染（替代前端渲染链路）：图表源码（按 format）→ PNG → 落盘会话 tmp/ + 飞书图片消息，
   * 结果经 decideDrawResult 回传引擎（渲染成功工具才返回成功；失败把错误回传模型供修正）。
   * 三语言均支持：plantuml（TeaVM 引擎）、mermaid（mermaid + happy-dom）、d2（@terrastruct/d2 WASM），
   * 统一走组合渲染器 core/diagram-render.ts（浅色主题白底图）。
   */
  private async handleDrawRender(sessionId: string, chatId: string, payload: Record<string, unknown>): Promise<void> {
    const renderId = String(payload.renderId ?? "")
    const code = String(payload.code ?? "")
    const format = payload.format === "mermaid" || payload.format === "d2" ? payload.format : "plantuml"
    const name = String(payload.name ?? "diagram").replace(/[^A-Za-z0-9_-]/g, "_") || "diagram"
    if (!renderId || !code) return
    try {
      const png = await this.renderer.renderPng(code, { format })
      // 产物落盘会话 tmp/（与 .puml 并列，Web UI 文件面板可见；失败不阻塞图片发送）
      try {
        const owner = await this.userOf(sessionId)
        const tmp = this.opts.store.getTmpDir(sessionId, owner)
        await mkdir(tmp, { recursive: true })
        await writeFile(join(tmp, `${name}.png`), png)
      } catch (err) {
        this.log(`draw png persist failed: ${String((err as Error).message || err)}`)
      }
      const imageKey = await this.api.uploadImage(png, "image/png", `${name}.png`)
      await this.api.sendMessage({ receiveId: chatId, receiveIdType: "chat_id", msgType: "image", content: { image_key: imageKey } })
      await this.opts.adapter.decideDrawResult(sessionId, renderId, { ok: true })
    } catch (err) {
      this.log(`draw render failed: ${String((err as Error).message || err)}`)
      await this.opts.adapter.decideDrawResult(sessionId, renderId, { ok: false, error: String((err as Error).message || err) })
    }
  }

  /** ask_user 选择卡片：发送交互式按钮卡片，记录待作答状态（卡片按钮回调经 handleCardAction 处理）。 */
  private async handleChoiceRequest(chatId: string, sessionId: string, payload: Record<string, unknown>): Promise<void> {
    const choiceId = String(payload.choiceId ?? "")
    const prompt = String(payload.prompt ?? "")
    const options = Array.isArray(payload.options) ? payload.options : []
    if (!choiceId) return
    try {
      const cardMessageId = await this.api.sendMessage({
        receiveId: chatId,
        receiveIdType: "chat_id",
        msgType: "interactive",
        content: buildChoiceCard({ choiceId, prompt, options, multi: payload.multi === true, selections: [] }),
      })
      this.pendingChoices.set(chatId, {
        choiceId,
        sessionId,
        multi: payload.multi === true,
        prompt,
        options,
        selections: [],
        cardMessageId,
        openId: this.runOwners.get(sessionId) ?? "",
      })
    } catch (err) {
      this.log(`choice card send failed: ${String((err as Error).message || err)}`)
      // 卡片发送失败（如超长 prompt）：以文本形式兜底询问，用户可回复任意文本（不阻塞任务，超时由引擎判定）
      this.outbox(chatId).sendText(`❓ ${truncateForFeishu(prompt, 400)}\n（请在 Web UI 对应会话中作答，或稍等片刻）`)
    }
  }

  /**
   * 卡片按钮回调（conn 的 card 帧 → card.action.trigger）：
   * 解析 choiceId/选项，单选立即决策、多选切换勾选并回传更新后的卡片（ACK 响应卡片更新），拒绝/完成提交决策；
   * 已决策的卡片更新为终态（「已选择」/「已放弃」），按钮不可再点。
   * 授权校验：仅任务发起者可作答（与 /approve 一致，防群聊成员越权）。
   */
  handleCardAction(payload: Record<string, unknown>): Record<string, unknown> {
    const ev = (payload.event ?? payload) as Record<string, unknown>
    const action = (ev.action ?? {}) as { value?: Record<string, unknown>; tag?: string }
    const value = action.value ?? {}
    const choiceId = String(value.choiceId ?? "")
    const act = String(value.act ?? "")
    const v = value.v !== undefined ? String(value.v) : ""
    const context = (ev.context ?? {}) as { open_chat_id?: unknown }
    const operator = (ev.operator ?? {}) as { operator_id?: { open_id?: unknown } }
    const chatId = sanitizeId(String(context.open_chat_id ?? ""))
    const openId = sanitizeId(String(operator.operator_id?.open_id ?? ""))
    if (!chatId || !choiceId) return {}
    const pending = this.pendingChoices.get(chatId)
    if (!pending || pending.choiceId !== choiceId) return {}
    if (pending.openId && pending.openId !== openId) {
      this.log(`choice card denied: not the initiator (chat=${chatId})`)
      return { toast: { type: "error", content: "只有发起该询问的用户可以作答" } }
    }
    // 决策终态卡片（替换按钮，不可再交互）
    const finalCard = (text: string) => ({
      card: {
        config: { wide_screen_mode: true },
        header: { title: { tag: "plain_text", content: "🤖 歌白需要确认" }, template: "blue" },
        elements: [{ tag: "markdown", content: `**${pending.prompt}**\n\n${text}` }],
      },
    })
    if (act === "refuse") {
      this.pendingChoices.delete(chatId)
      void this.opts.adapter.decideChoice(pending.sessionId, choiceId, null)
      return finalCard("❌ 已放弃回答")
    }
    if (act === "done") {
      const selections = [...pending.selections]
      this.pendingChoices.delete(chatId)
      void this.opts.adapter.decideChoice(pending.sessionId, choiceId, selections)
      return finalCard(`✅ 已选择：${selections.join("、")}`)
    }
    // pick：单选直接提交；多选切换勾选，返回更新后的卡片（ACK 响应更新按钮态与已选提示）
    if (!pending.multi) {
      this.pendingChoices.delete(chatId)
      void this.opts.adapter.decideChoice(pending.sessionId, choiceId, v)
      return finalCard(`✅ 已选择：${v}`)
    }
    const idx = pending.selections.indexOf(v)
    if (idx >= 0) pending.selections.splice(idx, 1)
    else pending.selections.push(v)
    return {
      toast: { type: "success", content: pending.selections.length ? `已选：${pending.selections.join("、")}` : "已取消选择" },
      card: buildChoiceCard({ choiceId, prompt: pending.prompt, options: pending.options, multi: true, selections: pending.selections }),
    }
  }

  /** 任务结束清理：撤回待作答选择卡片（尽力而为）并清除状态。 */
  private cleanupChoices(chatId: string): void {
    const pending = this.pendingChoices.get(chatId)
    if (!pending) return
    this.pendingChoices.delete(chatId)
    if (pending.cardMessageId) void this.api.deleteMessage(pending.cardMessageId)
  }

  /** 斜杠命令。 */
  private async handleCommand(raw: string, chatId: string, openId: string, sessionId: string): Promise<void> {
    const cmd = raw.toLowerCase().split(/\s+/)[0]
    const outbox = this.outbox(chatId)
    const userId = await this.resolveUser(openId)
    switch (cmd) {
      case "/help":
        outbox.sendText(
          [
            "🤖 歌白飞书机器人命令：",
            "/help — 本帮助",
            "/new — 新建会话（清空当前对话上下文）",
            "/sessions — 列出我的会话",
            "/cancel — 取消当前任务",
            "/approve — 批准待审批工具调用",
            "/reject — 拒绝待审批工具调用",
            "/approval-skip — 跳过审批（会话级）",
          ].join("\n"),
        )
        break
      case "/new": {
        // 运行中任务先取消（避免重建后事件路由丢失）；以会话归属用户身份重建（不产生归属漂移）
        if (this.opts.adapter.isRunning(sessionId)) this.opts.adapter.cancel(sessionId)
        await this.loadOwners()
        const owner = this.chatOwners.get(chatId) ?? userId
        if (!this.chatOwners.has(chatId)) {
          // 归属未知（如映射文件被清）时记录，避免下一位成员以自己的身份重建
          this.chatOwners.set(chatId, owner)
          await this.saveOwners()
        }
        this.pendingApprovals.delete(chatId)
        this.cleanupChoices(chatId)
        await this.opts.store.delete(sessionId, owner)
        this.active.delete(sessionId)
        this.runOwners.delete(sessionId)
        const now = this.clock()
        const name = (await this.api.getChatName(chatId)) ?? "飞书会话"
        await this.opts.store.save({ id: sessionId, name, userId: owner, messages: [], todos: [], createdAt: now, updatedAt: now })
        outbox.sendText("✅ 已新建会话，上下文已清空。")
        break
      }
      case "/sessions": {
        const sessions = await this.opts.store.listSessions(userId)
        if (!sessions.length) {
          outbox.sendText("暂无会话。")
          break
        }
        const lines = sessions.slice(0, 10).map((s, i) => {
          const time = new Date(s.updatedAt).toLocaleString("zh-CN", { hour12: false })
          return `${i + 1}. ${s.name}（${time}）${s.id === sessionId ? " ← 当前" : ""}`
        })
        outbox.sendText(`📂 最近会话：\n${lines.join("\n")}`)
        break
      }
      case "/cancel":
        this.opts.adapter.cancel(sessionId)
        outbox.sendText("🛑 已发送取消指令。")
        break
      case "/approve":
      case "/reject": {
        const pending = this.pendingApprovals.get(chatId)
        if (!pending) {
          outbox.sendText("当前没有待审批的工具调用。")
          break
        }
        // 授权校验：仅审批发起者可批准/拒绝（防群聊成员越权）
        if (pending.openId && pending.openId !== openId) {
          outbox.sendText("⚠️ 只有发起该审批的用户可以处理。")
          break
        }
        this.pendingApprovals.delete(chatId)
        const approve = cmd === "/approve"
        await this.opts.adapter.decideApproval(pending.sessionId, pending.toolCallId, approve)
        outbox.sendText(approve ? `✅ 已批准 \`${pending.tool}\`，任务继续。` : `❌ 已拒绝 \`${pending.tool}\`，任务已取消。`)
        break
      }
      case "/approval-skip": {
        if (this.opts.authMode === "server") {
          outbox.sendText("⚠️ 多用户模式下非管理员不能设置审批跳过。")
          break
        }
        // 以会话真实所有者身份设置（群聊中任何成员触发均作用于同一会话）
        const cur = await this.opts.store.load(sessionId)
        const owner = cur?.userId ?? userId
        await this.opts.store.setEnv(sessionId, owner, { GEBAI_APPROVAL_SKIP: "true" })
        outbox.sendText("✅ 已设置会话级审批跳过。")
        break
      }
      default:
        outbox.sendText(`未知命令 ${cmd}，发送 /help 查看可用命令。`)
    }
  }
}
