import { describe, expect, test } from "bun:test"
import { mkdtempSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createHash } from "node:crypto"
import type { AgentEvent } from "@gebai/sdk"
import type { SessionData } from "../core/types"
import { FeishuBot, parseMessageContent, sanitizeId, sessionIdForChat, stripMentions, sniffImageMime, truncateForFeishu } from "./bot"

/** 测试辅助：与 bot.resolveUser 相同的映射用户名派生（openId 哈希前 24 位）。 */
const funame = (openId: string) => `feishu_${createHash("sha256").update(openId).digest("hex").slice(0, 24)}`
/** 测试辅助：oc_chat1 的派生会话 id（32-hex，满足存储层白名单）。 */
const sid = () => sessionIdForChat("oc_chat1")

/** 微任务冲刷（ChatOutbox 发送队列）。 */
const flush = () => new Promise((r) => setTimeout(r, 0))

/** 轮询等待条件成立（真实 fs 异步链后需要）。 */
async function waitUntil(fn: () => boolean, timeoutMs = 3000): Promise<void> {
  const start = Date.now()
  while (!fn()) {
    if (Date.now() - start > timeoutMs) throw new Error("waitUntil timeout")
    await new Promise((r) => setTimeout(r, 5))
  }
}

interface Fakes {
  bot: FeishuBot
  home: string
  sent: Array<{ receiveId: string; receiveIdType?: string; msgType: string; content: unknown }>
  runs: Array<{ sessionId: string; user: string; prompt: string; opts: unknown }>
  sub: ((ev: AgentEvent) => void) | null
  sessions: Map<string, SessionData>
  users: Map<string, { id: string; username: string; role: "user" | "admin" }>
  deleted: string[]
  envSets: Array<{ sessionId: string; vars: Record<string, string> }>
  approvals: Array<{ sessionId: string; toolCallId: string; approve: boolean }>
  choices: Array<{ sessionId: string; choiceId: string; selection: string | string[] | null }>
  drawResults: Array<{ sessionId: string; renderId: string; result: { ok: boolean; error?: string } }>
  uploads: Array<{ mime: string; bytes: Uint8Array }>
  /** 渲染器调用记录（code + format，验证三语言透传）。 */
  renderCalls: Array<{ code: string; format: string }>
  deletes: string[]
  /** 表情反应调用记录（add：messageId+emoji；delete：messageId+reactionId）。 */
  reactions: Array<{ messageId: string; emoji: string }>
  reactionDeletes: Array<{ messageId: string; reactionId: string }>
  cancels: string[]
  connCalls: string[]
  running: Set<string>
  api: {
    getTenantToken(): Promise<string>
    sendMessage(o: { receiveId: string; receiveIdType: string; msgType: string; content: unknown }): Promise<string>
    replyMessage(messageId: string, msgType: string, content: unknown): Promise<string>
    deleteMessage(id: string): Promise<boolean>
    addMessageReaction(messageId: string, emojiType: string): Promise<string | null>
    deleteMessageReaction(messageId: string, reactionId: string): Promise<boolean>
    downloadResource(id: string, key: string, type: string): Promise<Uint8Array>
    uploadImage(data: Uint8Array, mime: string, fileName?: string): Promise<string>
    getChatName(id: string): Promise<string | null>
  }
  emit(ev: AgentEvent): void
  /** 触发卡片按钮回调（模拟 conn 的 card 帧）。 */
  cardAction(payload: Record<string, unknown>): Record<string, unknown>
  /** 解除 fake adapter 的挂起任务（hangRun 测试结尾调用，避免进程悬挂）。 */
  releaseRuns: () => void
}

function makeBot(opts: Partial<{ authMode: "local" | "server"; flushIntervalMs: number; flushMinChars: number; home: string; hangRun: boolean; renderError: string | null }> = {}): Fakes {
  // 归属映射（feishu/chat-owners.json）写入真实临时目录；共享 home 可测「重启恢复」
  const home = opts.home ?? mkdtempSync(join(tmpdir(), "feishu-bot-test-"))
  const sessions = new Map<string, SessionData>()
  const users = new Map<string, { id: string; username: string; role: "user" | "admin" }>()
  const sent: Fakes["sent"] = []
  const runs: Fakes["runs"] = []
  const deleted: string[] = []
  const envSets: Fakes["envSets"] = []
  const approvals: Fakes["approvals"] = []
  const choices: Fakes["choices"] = []
  const drawResults: Fakes["drawResults"] = []
  const uploads: Fakes["uploads"] = []
  const renderCalls: Array<{ code: string; format: string }> = []
  const deletes: string[] = []
  const reactions: Fakes["reactions"] = []
  const reactionDeletes: Fakes["reactionDeletes"] = []
  const cancels: string[] = []
  const connCalls: string[] = []
  const running = new Set<string>()
  const hangResolvers: Array<() => void> = []
  const downloads: string[] = []

  const store = {
    load: async (id: string, user?: string) => {
      const s = sessions.get(id)
      if (!s) return null
      if (user && s.userId !== user) return null
      return s
    },
    save: async (s: SessionData) => {
      sessions.set(s.id, s)
    },
    delete: async (id: string) => {
      deleted.push(id)
      sessions.delete(id)
    },
    listSessions: async (userId: string) => [...sessions.values()].filter((s) => s.userId === userId),
    setEnv: async (sessionId: string, _user: string, vars: Record<string, string | null>) => {
      const flat = Object.fromEntries(Object.entries(vars).filter((kv): kv is [string, string] => kv[1] !== null))
      envSets.push({ sessionId, vars: flat })
      return flat
    },
    getTmpDir: (sessionId: string, _userId: string) => join(home, "sessions", "tmp", sessionId),
  }
  const engine = {
    isRunning: (id: string) => running.has(id),
    run: async (sessionId: string, user: string, prompt: string, runOpts: unknown, handlers: unknown) => {
      lastHandlers = handlers as import("./adapter").BotRunHandlers
      runs.push({ sessionId, user, prompt, opts: runOpts })
      if (opts.hangRun) {
        // 模拟长任务：保持运行中，run 不返回（真实引擎任务挂起时 isRunning 为 true、runOwners 保留）
        running.add(sessionId)
        await new Promise<void>((resolve) => hangResolvers.push(resolve))
      }
    },
    cancel: (id: string) => {
      cancels.push(id)
    },
    decideApproval: async (sessionId: string, toolCallId: string, approve: boolean) => {
      approvals.push({ sessionId, toolCallId, approve })
    },
    decideChoice: async (sessionId: string, choiceId: string, selection: string | string[] | null) => {
      choices.push({ sessionId, choiceId, selection })
    },
    decideDrawResult: async (sessionId: string, renderId: string, result: { ok: boolean; error?: string }) => {
      drawResults.push({ sessionId, renderId, result })
    },
  }
  const auth = {
    defaultUser: () => ({ id: "admin", username: "admin", role: "admin" as const, disabled: false, createdAt: 0, salt: "", hash: "" }),
    listUsers: async () => [...users.values()].map((u) => ({ ...u, disabled: false, createdAt: 0, salt: "", hash: "" })),
    createUser: async (username: string, _password: string, role: "user" | "admin") => {
      const u = { id: `uid_${username}`, username, role }
      users.set(username, u)
      return { ...u, disabled: false, createdAt: 0, salt: "", hash: "" }
    },
  }
  const api: Fakes["api"] = {
    getTenantToken: async () => "t-token",
    sendMessage: async (o) => {
      sent.push(o)
      return `om_sent_${sent.length}`
    },
    replyMessage: async (messageId, msgType, content) => {
      sent.push({ receiveId: messageId, receiveIdType: "reply", msgType, content })
      return `om_rep_${sent.length}`
    },
    deleteMessage: async (id) => {
      deletes.push(id)
      return true
    },
    addMessageReaction: async (messageId, emojiType) => {
      reactions.push({ messageId, emoji: emojiType })
      return `reaction_${reactions.length}`
    },
    deleteMessageReaction: async (messageId, reactionId) => {
      reactionDeletes.push({ messageId, reactionId })
      return true
    },
    downloadResource: async (id, key, type) => {
      downloads.push(`${id}:${key}:${type}`)
      // JPEG 魔数
      return new Uint8Array([0xff, 0xd8, 0xff, 0xe0, 1, 2, 3])
    },
    uploadImage: async (data, mime, fileName) => {
      uploads.push({ mime, bytes: data })
      return `img_v2_${fileName ?? "x"}`
    },
    getChatName: async () => "测试群",
  }
  let lastHandlers: import("./adapter").BotRunHandlers | null = null
  const bot = new FeishuBot({
    appId: "cli_a",
    appSecret: "sec",
    authMode: opts.authMode ?? "local",
    home,
    store,
    adapter: engine,
    auth,
    api,
    conn: {
      start: async () => {
        connCalls.push("start")
      },
      stop: () => {
        connCalls.push("stop")
      },
    },
    renderer: {
      renderPng: async (code, o) => {
        renderCalls.push({ code, format: o?.format ?? "plantuml" })
        if (opts.renderError) throw new Error(opts.renderError)
        return new TextEncoder().encode(`PNG:${code.length}`)
      },
    },
    clock: Date.now,
    flushIntervalMs: opts.flushIntervalMs ?? 1500,
    flushMinChars: opts.flushMinChars ?? 60,
  })
  return {
    bot,
    home,
    sent,
    runs,
    get sub() {
      return null
    },
    sessions,
    users,
    deleted,
    envSets,
    approvals,
    choices,
    drawResults,
    uploads,
    renderCalls,
    deletes,
    reactions,
    reactionDeletes,
    cancels,
    connCalls,
    running,
    api,
    emit: (ev) => {
      // 模拟接口层（EngineBotAdapter）的事件映射：转发到最近一次 run 的回调
      const h = lastHandlers
      if (!h) return
      const p = ev.payload as Record<string, unknown>
      switch (ev.type) {
        case "event.approval.request":
          h.onApproval?.(String(p.toolCallId ?? ""), String(p.tool ?? ""))
          break
        case "event.choice.request":
          h.onChoice?.(String(p.choiceId ?? ""), String(p.prompt ?? ""), Array.isArray(p.options) ? p.options : [], p.multi === true)
          break
        case "event.draw.render":
          h.onDraw?.(String(p.renderId ?? ""), String(p.code ?? ""), p.name != null ? String(p.name) : undefined, p.format != null ? String(p.format) : undefined)
          break
        case "event.message.done":
          if (p.session !== true) h.onDone?.(String(p.text ?? ""))
          break
        case "event.task.done":
          h.onEnd?.()
          break
        case "event.task.error":
          h.onError?.(String(p.error ?? "unknown error"))
          h.onEnd?.()
          break
      }
    },
    cardAction: (payload) => bot.handleCardAction(payload),
    releaseRuns: () => {
      while (hangResolvers.length) hangResolvers.shift()!()
    },
  }
}

function receiveEvent(over: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    schema: "2.0",
    header: { event_type: "im.message.receive_v1", event_id: "ev_1" },
    event: {
      sender: { sender_id: { open_id: "ou_123" }, sender_type: "user" },
      message: { message_id: "om_msg12", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "你好" }) },
      ...over,
    },
  }
}

describe("纯函数", () => {
  test("parseMessageContent: text/post/非法", () => {
    expect(parseMessageContent("text", '{"text":"hi"}')).toBe("hi")
    expect(parseMessageContent("post", '{"title":"标题","content":[[{"tag":"text","text":"段一"},{"tag":"a","text":"链接","href":"https://x"}],[{"tag":"text","text":"段二"}]]}')).toBe("标题\n段一链接(https://x)\n段二")
    expect(parseMessageContent("text", "not-json")).toBeNull()
    expect(parseMessageContent("image", '{"image_key":"k"}')).toBeNull()
  })

  test("stripMentions", () => {
    expect(stripMentions(" @_user_1 你好")).toBe("你好")
    expect(stripMentions("没有提及")).toBe("没有提及")
  })

  test("sanitizeId", () => {
    expect(sanitizeId("oc_abc123")).toBe("oc_abc123")
    expect(sanitizeId("../etc")).toBeNull()
    expect(sanitizeId("")).toBeNull()
    expect(sanitizeId("x".repeat(65))).toBeNull()
  })

  test("sniffImageMime", () => {
    expect(sniffImageMime(new Uint8Array([0xff, 0xd8, 0xff, 0xe0]))).toBe("image/jpeg")
    expect(sniffImageMime(new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]))).toBe("image/png")
    expect(sniffImageMime(new Uint8Array([0x52, 0x49, 0x46, 0x46, 0, 0, 0, 0, 0, 0, 0, 0]))).toBe("image/webp")
    expect(sniffImageMime(new Uint8Array([1, 2, 3]))).toBe("application/octet-stream")
  })

  test("truncateForFeishu", () => {
    expect(truncateForFeishu("short")).toBe("short")
    const long = "x".repeat(100)
    const t = truncateForFeishu(long, 10)
    expect(t.length).toBeLessThan(long.length)
    expect(t).toContain("截断")
  })
})

describe("消息处理", () => {
  test("文本消息：创建会话并触发 engine.run（messageId 透传）", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    expect(f.sessions.has(sid())).toBe(true)
    expect(f.sessions.get(sid())!.name).toBe("测试群")
    expect(f.runs).toHaveLength(1)
    expect(f.runs[0]).toMatchObject({ sessionId: sid(), user: "admin", prompt: "你好" })
    expect((f.runs[0].opts as { messageId: string }).messageId).toBe("om_msg12")
  })

  test("会话已存在时复用（不重复 save）", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent())
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_msg2", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "再问" }) } }))
    await flush()
    expect(f.runs).toHaveLength(2)
    expect(f.runs[1].prompt).toBe("再问")
  })

  test("并发消息只创建一个会话（ensureSession 锁）", async () => {
    const f = makeBot()
    await Promise.all([f.bot.ensureSession("oc_chat1", "ou_123"), f.bot.ensureSession("oc_chat1", "ou_123")])
    const saves = [...f.sessions.values()]
    expect(saves).toHaveLength(1)
  })

  test("@ 提及剥离后进入会话", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_m", chat_id: "oc_c", message_type: "text", content: JSON.stringify({ text: " @_user_1 帮我查一下" }) } }))
    await flush()
    expect(f.runs[0].prompt).toBe("帮我查一下")
  })

  test("机器人自己的消息忽略", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent({ sender: { sender_id: { open_id: "ou_bot" }, sender_type: "app" } }))
    await flush()
    expect(f.runs).toHaveLength(0)
  })

  test("事件类型不匹配忽略", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent({ schema: "2.0", header: { event_type: "im.chat.member.added" }, event: {} })
    await flush()
    expect(f.runs).toHaveLength(0)
  })

  test("非 schema 2.0 忽略", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent({ schema: "1.0", header: { event_type: "im.message.receive_v1" }, event: {} })
    expect(f.runs).toHaveLength(0)
  })

  test("非法 chat_id/open_id 忽略（注入防护）", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_m", chat_id: "../evil", message_type: "text", content: "{}" } }))
    expect(f.runs).toHaveLength(0)
  })

  test("任务运行中拒绝新消息", async () => {
    const f = makeBot()
    f.running.add(sid())
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    expect(f.runs).toHaveLength(0)
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("已有任务"))).toBe(true)
  })

  test("不支持的消息类型提示", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_m", chat_id: "oc_c", message_type: "audio", content: "{}" } }))
    await flush()
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("暂不支持"))).toBe(true)
  })

  test("图片消息：下载并作为附件", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_img", chat_id: "oc_c", message_type: "image", content: JSON.stringify({ image_key: "img_v2_k" }) } }))
    await flush()
    expect(f.runs).toHaveLength(1)
    const opts = f.runs[0].opts as { attachments?: Array<{ name: string; mime: string; data: Uint8Array }> }
    expect(opts.attachments).toHaveLength(1)
    expect(opts.attachments![0].mime).toBe("image/jpeg")
    expect(opts.attachments![0].name).toContain("img_v2_k")
  })

  test("图片下载失败提示", async () => {
    const f = makeBot()
    const orig = f.api.downloadResource
    f.api.downloadResource = async () => {
      throw new Error("download failed")
    }
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_img", chat_id: "oc_c", message_type: "image", content: JSON.stringify({ image_key: "k" }) } }))
    await flush()
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("图片读取失败"))).toBe(true)
    f.api.downloadResource = orig
  })
})

describe("身份映射", () => {
  test("多用户模式：open_id 自动创建映射用户并缓存", async () => {
    const f = makeBot({ authMode: "server" })
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    expect(f.users.has(funame("ou_123"))).toBe(true)
    expect(f.runs[0].user).toBe(`uid_${funame("ou_123")}`)
    // 缓存命中：第二次不重复创建
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_m2", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "hi" }) } }))
    await flush()
    expect(f.users.size).toBe(1)
  })

  test("多用户模式：已存在用户直接映射", async () => {
    const f = makeBot({ authMode: "server" })
    f.users.set(funame("ou_123"), { id: "existing", username: funame("ou_123"), role: "user" })
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    expect(f.runs[0].user).toBe("existing")
  })

  test("单用户模式固定默认用户", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    expect(f.runs[0].user).toBe("admin")
  })

  test("归属持久化：重启后新实例恢复会话归属（不重建不漂移）", async () => {
    const home = mkdtempSync(join(tmpdir(), "feishu-bot-restart-"))
    const f1 = makeBot({ authMode: "server", home })
    await f1.bot.handleFeishuEvent(receiveEvent()) // ou_123 创建
    await flush()
    expect(f1.sessions.get(sid())!.userId).toBe(`uid_${funame("ou_123")}`)
    // 模拟重启：新 bot 实例（同一 home），内存状态全新，store 只有磁盘语义（fake Map 共享）
    const f2 = makeBot({ authMode: "server", home })
    await f2.bot.handleFeishuEvent(receiveEvent({ sender: { sender_id: { open_id: "ou_999" }, sender_type: "user" }, message: { message_id: "om_r", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "重启后发言" }) } }))
    await flush()
    // 会话仍属 ou_123，未以 ou_999 重建；引擎身份为归属用户
    expect(f2.sessions.get(sid())!.userId).toBe(`uid_${funame("ou_123")}`)
    expect(f2.runs[0].user).toBe(`uid_${funame("ou_123")}`)
  })
})

describe("引擎事件推送", () => {
  const base = { sessionId: sid(), timestamp: 0 }

  test("仅最终回复：无预览流，done 直接发最终卡片", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    // 接口层不转发文本增量（引擎 final_only 不推送 delta），无预览消息
    f.emit({ type: "event.message.delta", ...base, payload: { text: "你好" } })
    await flush()
    const previews = f.sent.filter((s) => String(JSON.stringify(s.content)).includes("✍️"))
    expect(previews).toHaveLength(0)
    // 最终回复（onDone）直接发卡片
    f.emit({ type: "event.message.done", ...base, payload: { text: "最终完整回复" } })
    await flush()
    const cards = f.sent.filter((s) => s.msgType === "interactive")
    expect(cards).toHaveLength(1)
    expect(JSON.stringify(cards[0].content)).toContain("最终完整回复")
  })

  test("新会话执行过程的 done（session 标记）不触发 final 卡片", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    // 新会话执行过程的 done（session 标记）不是任务最终回复：接口层不转发 onDone（任务未结束）
    f.emit({ type: "event.message.done", ...base, payload: { text: "子代理完成", session: true } })
    await flush()
    const cards = f.sent.filter((s) => s.msgType === "interactive")
    expect(cards).toHaveLength(0)
    // 主循环最终 done（无标记）正常发最终卡片
    f.emit({ type: "event.message.done", ...base, payload: { text: "主回复" } })
    await flush()
    const cards2 = f.sent.filter((s) => s.msgType === "interactive")
    expect(cards2).toHaveLength(1)
    expect(JSON.stringify(cards2[0].content)).toContain("主回复")
  })

  test("工具调用不推送状态消息（仅最终回复，过程事件不转发）", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.tool.call", ...base, payload: { name: "read", toolCallId: "tc1" } })
    f.emit({ type: "event.tool.call", ...base, payload: { name: "write", toolCallId: "tc2" } })
    await flush()
    const statuses = f.sent.filter((s) => String(JSON.stringify(s.content)).includes("正在执行工具"))
    expect(statuses).toHaveLength(0)
  })

  test("收到消息添加「Typing」正在输入表情，输出完成撤回", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    // 收到消息：立即给用户消息添加 Typing 表情反应
    expect(f.reactions[0]).toEqual({ messageId: "om_msg12", emoji: "Typing" })
    // 最终回复（onDone）发出后撤回 Typing 表情；回复引用原消息
    f.emit({ type: "event.message.done", ...base, payload: { text: "最终完整回复" } })
    await flush()
    const cards = f.sent.filter((s) => s.msgType === "interactive")
    expect(cards).toHaveLength(1)
    expect(cards[0].receiveIdType).toBe("reply")
    expect(cards[0].receiveId).toBe("om_msg12")
    expect(f.reactionDeletes.some((d) => d.messageId === "om_msg12" && d.reactionId === "reaction_1")).toBe(true)
  })

  test("任务出错：错误文本发出并撤回「Typing」表情", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    expect(f.reactions[0]).toEqual({ messageId: "om_msg12", emoji: "Typing" })
    f.emit({ type: "event.task.error", ...base, payload: { error: "LLM 超时" } })
    await flush()
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("LLM 超时"))).toBe(true)
    const err = f.sent.find((s) => String(JSON.stringify(s.content)).includes("LLM 超时"))!
    expect(err.receiveIdType).toBe("reply")
    expect(err.receiveId).toBe("om_msg12")
    expect(f.reactionDeletes.some((d) => d.messageId === "om_msg12" && d.reactionId === "reaction_1")).toBe(true)
  })

  test("任务完成兜底：未发最终回复时补完成提示并撤回「Typing」表情", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    expect(f.reactions[0]).toEqual({ messageId: "om_msg12", emoji: "Typing" })
    // 仅 task.done（无 message.done）：finalSent=false → taskDone 补发完成提示
    f.emit({ type: "event.task.done", ...base, payload: {} })
    await flush()
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("✅ 任务完成"))).toBe(true)
    const done = f.sent.find((s) => String(JSON.stringify(s.content)).includes("✅ 任务完成"))!
    expect(done.receiveIdType).toBe("reply")
    expect(done.receiveId).toBe("om_msg12")
    expect(f.reactionDeletes.some((d) => d.messageId === "om_msg12" && d.reactionId === "reaction_1")).toBe(true)
  })

  test("审批：请求发提示，/approve 批准", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.approval.request", ...base, payload: { toolCallId: "tc9", tool: "write" } })
    await flush()
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("需要审批"))).toBe(true)
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_ap", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/approve" }) } }))
    await flush()
    expect(f.approvals).toEqual([{ sessionId: sid(), toolCallId: "tc9", approve: true }])
    // 无待审批时拒绝
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_ap2", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/approve" }) } }))
    await flush()
    expect(f.approvals).toHaveLength(1)
  })

  test("审批授权：仅发起者可批准（他人 /approve 被拒）", async () => {
    const f = makeBot({ hangRun: true }) // 任务保持运行中，runOwners 保留
    await f.bot.start()
    void f.bot.handleFeishuEvent(receiveEvent()) // ou_123 触发任务（fake 挂起，不 await）
    await waitUntil(() => f.runs.length === 1) // 等 runPrompt 完成注册（active/runOwners）
    f.emit({ type: "event.approval.request", ...base, payload: { toolCallId: "tc7", tool: "sh" } })
    await flush()
    // 另一成员 ou_999 尝试批准 → 拒绝
    await f.bot.handleFeishuEvent(receiveEvent({ sender: { sender_id: { open_id: "ou_999" }, sender_type: "user" }, message: { message_id: "om_ap3", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/approve" }) } }))
    await flush()
    expect(f.approvals).toHaveLength(0)
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("只有发起"))).toBe(true)
    // 发起者本人批准 → 成功
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_ap4", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/approve" }) } }))
    await flush()
    expect(f.approvals).toEqual([{ sessionId: sid(), toolCallId: "tc7", approve: true }])
    f.releaseRuns()
  })

  test("审批授权：任务运行中他人发言不改变审批归属", async () => {
    const f = makeBot({ hangRun: true })
    await f.bot.start()
    void f.bot.handleFeishuEvent(receiveEvent()) // ou_123 触发任务（fake engine 挂起，任务运行中）
    await waitUntil(() => f.runs.length === 1)
    // 任务运行中（模拟），另一成员发普通消息
    await f.bot.handleFeishuEvent(receiveEvent({ sender: { sender_id: { open_id: "ou_999" }, sender_type: "user" }, message: { message_id: "om_mid", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "我也说一句" }) } }))
    await flush()
    f.emit({ type: "event.approval.request", ...base, payload: { toolCallId: "tc8", tool: "write" } })
    await flush()
    // ou_999 不能批准（归属仍为 ou_123）
    await f.bot.handleFeishuEvent(receiveEvent({ sender: { sender_id: { open_id: "ou_999" }, sender_type: "user" }, message: { message_id: "om_ap5", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/approve" }) } }))
    await flush()
    expect(f.approvals).toHaveLength(0)
    // ou_123 可以批准
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_ap6", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/approve" }) } }))
    await flush()
    expect(f.approvals).toEqual([{ sessionId: sid(), toolCallId: "tc8", approve: true }])
    f.releaseRuns()
  })

  test("任务完成兜底与清理", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.message.done", ...base, payload: { text: "回复" } })
    f.emit({ type: "event.task.done", ...base, payload: {} })
    await flush()
    // 最终已发，task.done 不再补发
    expect(f.sent.filter((s) => s.msgType === "interactive")).toHaveLength(1)
  })

  test("画图（draw）：后端直接渲染成图片——上传 + 发图片消息 + 回传成功 + PNG 落盘", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.draw.render", ...base, payload: { renderId: "r1", code: "@startuml\nAlice->Bob\n@enduml", name: "flow" } })
    await waitUntil(() => f.drawResults.length === 1)
    // 图片上传 + 图片消息发送
    expect(f.uploads).toHaveLength(1)
    expect(f.uploads[0].mime).toBe("image/png")
    const imgMsg = f.sent.find((s) => s.msgType === "image")
    expect(imgMsg).toBeDefined()
    expect(JSON.stringify(imgMsg!.content)).toContain("image_key")
    // 渲染成功回传引擎（工具返回成功）
    expect(f.drawResults[0]).toEqual({ sessionId: sid(), renderId: "r1", result: { ok: true } })
    // PNG 产物落盘会话 tmp/（与 .puml 并列，Web UI 文件面板可见）
    const { existsSync } = await import("node:fs")
    expect(existsSync(join(f.home, "sessions", "tmp", sid(), "flow.png"))).toBe(true)
  })

  test("画图（draw）：渲染失败把错误回传引擎（模型据此修正）", async () => {
    const f = makeBot({ renderError: "PlantUML 渲染错误：Syntax Error" })
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.draw.render", ...base, payload: { renderId: "r2", code: "bad code", name: "x" } })
    await waitUntil(() => f.drawResults.length === 1)
    expect(f.drawResults[0].result).toEqual({ ok: false, error: "PlantUML 渲染错误：Syntax Error" })
    // 失败不发送图片消息、不上传
    expect(f.sent.some((s) => s.msgType === "image")).toBe(false)
    expect(f.uploads).toHaveLength(0)
  })

  test("画图（draw）：mermaid/d2 格式走后端三语言渲染——format 透传 + 上传发送图片", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.draw.render", ...base, payload: { renderId: "r3", code: "flowchart LR\nA --> B", name: "flow", format: "mermaid" } })
    await waitUntil(() => f.drawResults.length === 1)
    expect(f.drawResults[0]).toEqual({ sessionId: sid(), renderId: "r3", result: { ok: true } })
    expect(f.renderCalls[0]).toEqual({ code: "flowchart LR\nA --> B", format: "mermaid" })
    expect(f.uploads).toHaveLength(1)
    expect(f.sent.some((s) => s.msgType === "image")).toBe(true)
    // d2 同样渲染成功
    f.emit({ type: "event.draw.render", ...base, payload: { renderId: "r4", code: "gw -> svc", name: "arch", format: "d2" } })
    await waitUntil(() => f.drawResults.length === 2)
    expect(f.drawResults[1].result.ok).toBe(true)
    expect(f.renderCalls[1]).toEqual({ code: "gw -> svc", format: "d2" })
    expect(f.uploads).toHaveLength(2)
  })

  test("画图（draw）：mermaid 渲染失败把错误回传引擎（模型据此修正）", async () => {
    const f = makeBot({ renderError: "Mermaid 渲染错误：Parse error on line 2" })
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.draw.render", ...base, payload: { renderId: "r5", code: "flowchart", name: "bad", format: "mermaid" } })
    await waitUntil(() => f.drawResults.length === 1)
    expect(f.drawResults[0].result).toEqual({ ok: false, error: "Mermaid 渲染错误：Parse error on line 2" })
    expect(f.sent.some((s) => s.msgType === "image")).toBe(false)
    expect(f.uploads).toHaveLength(0)
  })

  test("ask_user 选择卡片：choice.request 发送交互式按钮卡片并记录待作答状态", async () => {
    const f = makeBot({ hangRun: true })
    await f.bot.start()
    void f.bot.handleFeishuEvent(receiveEvent())
    await waitUntil(() => f.runs.length === 1)
    f.emit({ type: "event.choice.request", ...base, payload: { choiceId: "c1", prompt: "选择实现方案", options: ["方案A", "方案B"], multi: false } })
    await waitUntil(() => f.sent.some((s) => s.msgType === "interactive" && JSON.stringify(s.content).includes("选择实现方案")))
    const card = f.sent.find((s) => s.msgType === "interactive" && JSON.stringify(s.content).includes("选择实现方案"))!
    const json = JSON.stringify(card.content)
    // 按钮带 choiceId + 选项值
    expect(json).toContain('"choiceId":"c1"')
    expect(json).toContain("方案A")
    expect(json).toContain("方案B")
    expect(json).toContain("拒绝回答")
    f.releaseRuns()
  })

  test("选择卡片按钮：单选点击立即决策；拒绝回答决策 null", async () => {
    const f = makeBot({ hangRun: true })
    await f.bot.start()
    void f.bot.handleFeishuEvent(receiveEvent())
    await waitUntil(() => f.runs.length === 1)
    f.emit({ type: "event.choice.request", ...base, payload: { choiceId: "c1", prompt: "选一个", options: ["A", "B"], multi: false } })
    await waitUntil(() => f.sent.some((s) => String(JSON.stringify(s.content)).includes("选一个")))
    // 发起者点「B」→ 单选立即决策
    const resp = f.cardAction({
      event: {
        operator: { operator_id: { open_id: "ou_123" } },
        action: { value: { choiceId: "c1", act: "pick", v: "B" }, tag: "button" },
        context: { open_message_id: "om_c1", open_chat_id: "oc_chat1" },
      },
    })
    expect(f.choices).toEqual([{ sessionId: sid(), choiceId: "c1", selection: "B" }])
    // 决策后卡片更新为终态（「已选择」文案，按钮不可再点）
    expect(JSON.stringify(resp)).toContain("已选择：B")
    // 再发一次选择卡并拒绝
    f.emit({ type: "event.choice.request", ...base, payload: { choiceId: "c2", prompt: "再选", options: ["A"], multi: false } })
    await waitUntil(() => f.sent.filter((s) => String(JSON.stringify(s.content)).includes("再选")).length >= 1)
    f.cardAction({
      event: {
        operator: { operator_id: { open_id: "ou_123" } },
        action: { value: { choiceId: "c2", act: "refuse" }, tag: "button" },
        context: { open_message_id: "om_c2", open_chat_id: "oc_chat1" },
      },
    })
    expect(f.choices[1]).toEqual({ sessionId: sid(), choiceId: "c2", selection: null })
    f.releaseRuns()
  })

  test("选择卡片按钮：多选切换勾选并更新卡片（响应 card 更新），完成提交集合", async () => {
    const f = makeBot({ hangRun: true })
    await f.bot.start()
    void f.bot.handleFeishuEvent(receiveEvent())
    await waitUntil(() => f.runs.length === 1)
    f.emit({ type: "event.choice.request", ...base, payload: { choiceId: "c3", prompt: "多选框架", options: ["React", "Vue", "Svelte"], multi: true } })
    await waitUntil(() => f.sent.some((s) => String(JSON.stringify(s.content)).includes("多选框架")))
    const action = (v: string) => ({
      event: {
        operator: { operator_id: { open_id: "ou_123" } },
        action: { value: { choiceId: "c3", act: "pick", v }, tag: "button" },
        context: { open_message_id: "om_c3", open_chat_id: "oc_chat1" },
      },
    })
    // 勾选 React → 响应携带更新后的卡片（含「完成选择」按钮），未决策
    const resp1 = f.cardAction(action("React"))
    expect(f.choices).toHaveLength(0)
    expect(JSON.stringify(resp1.card)).toContain("已选：React")
    expect(JSON.stringify(resp1.card)).toContain("完成选择")
    // 勾选 Vue、再取消 React
    f.cardAction(action("Vue"))
    f.cardAction(action("React"))
    // 点「完成」→ 提交当前勾选集合
    f.cardAction({
      event: {
        operator: { operator_id: { open_id: "ou_123" } },
        action: { value: { choiceId: "c3", act: "done" }, tag: "button" },
        context: { open_message_id: "om_c3", open_chat_id: "oc_chat1" },
      },
    })
    expect(f.choices).toEqual([{ sessionId: sid(), choiceId: "c3", selection: ["Vue"] }])
    f.releaseRuns()
  })

  test("选择卡片授权：非任务发起者作答被拒（toast 提示，不决策）", async () => {
    const f = makeBot({ hangRun: true })
    await f.bot.start()
    void f.bot.handleFeishuEvent(receiveEvent()) // 发起者 ou_123
    await waitUntil(() => f.runs.length === 1)
    f.emit({ type: "event.choice.request", ...base, payload: { choiceId: "c4", prompt: "选", options: ["A"], multi: false } })
    await waitUntil(() => f.sent.some((s) => String(JSON.stringify(s.content)).includes('"choiceId":"c4"')))
    const resp = f.cardAction({
      event: {
        operator: { operator_id: { open_id: "ou_999" } },
        action: { value: { choiceId: "c4", act: "pick", v: "A" }, tag: "button" },
        context: { open_message_id: "om_c4", open_chat_id: "oc_chat1" },
      },
    })
    expect(f.choices).toHaveLength(0)
    expect(JSON.stringify(resp)).toContain("只有发起")
    f.releaseRuns()
  })

  test("任务结束撤回待作答选择卡片并清理状态", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.choice.request", ...base, payload: { choiceId: "c5", prompt: "选", options: ["A"], multi: false } })
    await waitUntil(() => f.sent.some((s) => String(JSON.stringify(s.content)).includes('"choiceId":"c5"')))
    const cardMsgId = f.sent.find((s) => String(JSON.stringify(s.content)).includes('"choiceId":"c5"'))!.content
    void cardMsgId
    // 任务完成：卡片消息被撤回（deleteMessage 调用），状态清理后点击回调不决策
    f.emit({ type: "event.task.done", ...base, payload: {} })
    await flush()
    expect(f.deletes.length).toBeGreaterThanOrEqual(1)
    const resp = f.cardAction({
      event: {
        operator: { operator_id: { open_id: "ou_123" } },
        action: { value: { choiceId: "c5", act: "pick", v: "A" }, tag: "button" },
        context: { open_message_id: "om_c5", open_chat_id: "oc_chat1" },
      },
    })
    expect(f.choices).toHaveLength(0)
    expect(resp).toEqual({})
  })

  test("run 经接口层（bot 只传消息参数，交互/输出模式由接口层固定）", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    expect(f.runs).toHaveLength(1)
    // bot 只传消息参数（messageId 透传）；interactionMode/outputMode 由接口层（EngineBotAdapter）固定为多轮交互 + 仅最终回复
    const opts = f.runs[0].opts as Record<string, unknown>
    expect(opts.messageId).toBe("om_msg12")
    expect(opts.interactionMode).toBeUndefined()
    expect(opts.outputMode).toBeUndefined()
  })

  test("message.done 与 task.done 同批到达不重复发送完成提示", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.message.done", ...base, payload: { text: "最终" } })
    f.emit({ type: "event.task.done", ...base, payload: {} })
    await flush()
    const finals = f.sent.filter((s) => s.msgType === "interactive")
    expect(finals).toHaveLength(1)
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("✅ 任务完成"))).toBe(false)
  })

  test("任务错误推送错误消息", async () => {
    const f = makeBot()
    await f.bot.start()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    f.emit({ type: "event.task.error", ...base, payload: { error: "模型调用失败" } })
    await flush()
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("模型调用失败"))).toBe(true)
  })

  test("无活跃会话的事件不推送", async () => {
    const f = makeBot()
    f.emit({ type: "event.task.error", sessionId: "other", timestamp: 0, payload: { error: "x" } })
    await flush()
    expect(f.sent).toHaveLength(0)
  })
})

describe("命令", () => {
  test("/help 输出命令列表", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_h", chat_id: "oc_c", message_type: "text", content: JSON.stringify({ text: "/help" }) } }))
    await flush()
    expect(f.runs).toHaveLength(0)
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("/approval-skip"))).toBe(true)
  })

  test("/new 重建会话", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_n", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/new" }) } }))
    await flush()
    expect(f.deleted).toContain(sid())
    expect(f.sessions.get(sid())!.messages).toEqual([])
  })

  test("/sessions 列出会话", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_s", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/sessions" }) } }))
    await flush()
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("测试群"))).toBe(true)
  })

  test("/new 多用户群聊：非创建者被拒；创建者以会话所有者身份重建（不产生归属漂移）", async () => {
    const f = makeBot({ authMode: "server" })
    await f.bot.handleFeishuEvent(receiveEvent()) // ou_123 建会话（即创建者）
    await flush()
    const owner = f.sessions.get(sid())!.userId
    expect(owner).toBe(`uid_${funame("ou_123")}`)
    // 另一成员 ou_999 触发 /new：被拒（群聊防越权——清空共享会话上下文仅创建者可做）
    await f.bot.handleFeishuEvent(receiveEvent({ sender: { sender_id: { open_id: "ou_999" }, sender_type: "user" }, message: { message_id: "om_n2", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/new" }) } }))
    await flush()
    expect(f.deleted).toEqual([])
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("只有创建该会话的用户"))).toBe(true)
    // 创建者 ou_123 触发 /new：成功重建，归属不漂移
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_n3", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/new" }) } }))
    await flush()
    expect(f.sessions.get(sid())!.userId).toBe(owner)
    expect(f.deleted).toContain(sid())
  })

  test("/cancel 群聊：非任务发起者被拒，发起者可取消", async () => {
    const f = makeBot({ hangRun: true })
    void f.bot.handleFeishuEvent(receiveEvent()) // ou_123 发起任务（fake engine 挂起，不 await）
    await waitUntil(() => f.runs.length === 1)
    // 另一成员 ou_999 /cancel：被拒
    await f.bot.handleFeishuEvent(receiveEvent({ sender: { sender_id: { open_id: "ou_999" }, sender_type: "user" }, message: { message_id: "om_c2", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/cancel" }) } }))
    await flush()
    expect(f.cancels).toEqual([])
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("只有任务发起者"))).toBe(true)
    // 发起者 ou_123 /cancel：生效
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_c3", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/cancel" }) } }))
    await flush()
    expect(f.cancels).toContain(sid())
    f.releaseRuns()
  })

  test("/cancel 取消任务", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_c", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/cancel" }) } }))
    await flush()
    expect(f.cancels).toContain(sid())
  })

  test("/approval-skip 单用户可设、多用户拒绝", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent())
    await flush()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_a", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/approval-skip" }) } }))
    await flush()
    expect(f.envSets).toEqual([{ sessionId: sid(), vars: { GEBAI_APPROVAL_SKIP: "true" } }])

    const m = makeBot({ authMode: "server" })
    await m.bot.handleFeishuEvent(receiveEvent())
    await flush()
    await m.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_a2", chat_id: "oc_chat1", message_type: "text", content: JSON.stringify({ text: "/approval-skip" }) } }))
    await flush()
    expect(m.envSets).toHaveLength(0)
    expect(m.sent.some((s) => String(JSON.stringify(s.content)).includes("非管理员"))).toBe(true)
  })

  test("未知命令提示", async () => {
    const f = makeBot()
    await f.bot.handleFeishuEvent(receiveEvent({ message: { message_id: "om_u", chat_id: "oc_c", message_type: "text", content: JSON.stringify({ text: "/bogus" }) } }))
    await flush()
    expect(f.sent.some((s) => String(JSON.stringify(s.content)).includes("未知命令"))).toBe(true)
  })
})

describe("启动/停止", () => {
  test("start 启动长连接；stop 停止连接（不订阅引擎事件流，事件经接口层回调）", async () => {
    const f = makeBot()
    await f.bot.start()
    expect(f.connCalls).toContain("start")
    f.bot.stop()
    expect(f.connCalls).toContain("stop")
  })
})

describe("真实存储集成", () => {
  test("派生会话 id 满足存储层白名单：真实 SessionStore 下创建/加载/运行全链路可用", async () => {
    const { SessionStore } = await import("../core/store")
    const home = mkdtempSync(join(tmpdir(), "feishu-real-store-"))
    const store = new SessionStore({ home })
    const sessions: SessionData[] = []
    const runs: Array<{ sessionId: string; user: string }> = []
    const auth = {
      defaultUser: () => ({ id: "admin", username: "admin", role: "admin" as const, disabled: false, createdAt: 0, salt: "", hash: "" }),
      listUsers: async () => [] as never[],
      createUser: async () => {
        throw new Error("not expected")
      },
    }
    const adapter = {
      isRunning: () => false,
      run: async (sessionId: string, user: string) => {
        runs.push({ sessionId, user })
      },
    }
    const bot = new FeishuBot({
      appId: "cli_a",
      appSecret: "sec",
      authMode: "local",
      home,
      store: store as never,
      adapter: adapter as never,
      auth: auth as never,
      api: {
        getTenantToken: async () => "t",
        sendMessage: async () => "om_1",
        replyMessage: async () => "om_2",
        deleteMessage: async () => true,
        addMessageReaction: async () => null,
        deleteMessageReaction: async () => true,
        downloadResource: async () => new Uint8Array(),
        uploadImage: async () => "img",
        getChatName: async () => "测试群",
      },
      conn: { start: async () => {}, stop: () => {} },
    })
    await bot.handleFeishuEvent(receiveEvent())
    await flush()
    // 派生 id 为 32 位小写 hex（feishu_{chatId} 形态会被存储层白名单拒绝——本测试即回归防线）
    const id = sid()
    expect(id).toMatch(/^[0-9a-f]{32}$/)
    expect(runs).toEqual([{ sessionId: id, user: "admin" }])
    const loaded = await store.load(id, "admin")
    expect(loaded).not.toBeNull()
    expect(loaded!.name).toBe("测试群")
    expect(sessions.length).toBe(0)
  })

  test("映射用户创建失败时中止（绝不以默认 admin 兜底运行任务）", async () => {
    const home = mkdtempSync(join(tmpdir(), "feishu-nofallback-"))
    const runs: Array<{ sessionId: string; user: string }> = []
    const auth = {
      defaultUser: () => ({ id: "admin", username: "admin", role: "admin" as const, disabled: false, createdAt: 0, salt: "", hash: "" }),
      listUsers: async () => [] as never[],
      createUser: async () => {
        throw new Error("registry broken")
      },
    }
    const sent: Array<{ content: unknown }> = []
    const bot = new FeishuBot({
      appId: "cli_a",
      appSecret: "sec",
      authMode: "server",
      home,
      store: {
        load: async () => null,
        save: async (s: SessionData) => void (s as SessionData),
        delete: async () => {},
        listSessions: async () => [],
        setEnv: async () => ({}),
        getTmpDir: () => home,
      } as never,
      adapter: {
        isRunning: () => false,
        run: async (sessionId: string, user: string) => {
          runs.push({ sessionId, user })
        },
      } as never,
      auth: auth as never,
      api: {
        getTenantToken: async () => "t",
        sendMessage: async (_o: { content: unknown }) => {
          sent.push({ content: _o.content })
          return "om_1"
        },
        replyMessage: async () => "om_2",
        deleteMessage: async () => true,
        addMessageReaction: async () => null,
        deleteMessageReaction: async () => true,
        downloadResource: async () => new Uint8Array(),
        uploadImage: async () => "img",
        getChatName: async () => "测试群",
      },
      conn: { start: async () => {}, stop: () => {} },
    })
    await bot.handleFeishuEvent(receiveEvent())
    await flush()
    expect(runs).toEqual([]) // 不得以 admin 兜底运行
    expect(sent.some((s) => String(JSON.stringify(s.content)).includes("会话初始化失败"))).toBe(true)
  })
})
