/** 会话域路由：/sessions CRUD/恢复/改名 + 会话 env/todos + 截断/附件/取消/审批/选择/画图回传/压缩
 *  + prompt（SSE 通道）。文件类路由见 session-files.ts（同属 /sessions/:id 前缀，共享会话 id 白名单中间件）。 */
import type { RouteCtx } from "./context"
import type { AttachmentInput, EnvVarSource, TodoItem } from "@gebai/sdk"
import { existsSync } from "node:fs"
import { rename } from "node:fs/promises"
import { join } from "node:path"
import { basenameName, isValidSessionId, sessionPath } from "../core/base/paths"
import { findInTrash } from "../core/session/gc"
import { validateEnvVars, maskEnv, filterEnvInjection } from "../core/session/env"
import { getEnvCatalog } from "../core/agents/env-catalog"
import { toSessionInfo } from "../core/session/store"
import { TokenBucket } from "../core/security/ratelimit"

export function registerSessionRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  // 会话 ID 格式白名单（多用户隔离防线）：`:id` 段必须为 32 位小写 hex，
  // 畸形/穿越形态一律 400。Hono 路由匹配前已整体 decodeURI，`%2F` 不可能进入单段，
  // 此处兜底 `..`/非 hex 等异常形态（与 WS/存储层同规则）。
  const validateSessionId = async (c: { req: { param: (k: string) => string | undefined }; json: (b: unknown, s: number) => Response }, next: () => Promise<void>) => {
    const id = c.req.param("id") ?? ""
    if (!isValidSessionId(id)) return c.json({ error: `invalid session id: ${id}` }, 400)
    await next()
  }
  app.use("/api/v1/sessions/:id", validateSessionId)
  app.use("/api/v1/sessions/:id/*", validateSessionId)

  app.get("/api/v1/sessions", async (c) => {
    const user = await userOf(c)
    const sessions = await d.store.listSessions(user.id)
    return c.json(sessions.map(toSessionInfo))
  })
  app.post("/api/v1/sessions", async (c) => {
    const user = await userOf(c)
    const body = (await c.req.json().catch(() => ({}))) as { name?: string }
    const session = await d.store.createSession(user.id, body.name)
    return c.json({ id: session.id, name: session.name, userId: session.userId, createdAt: session.createdAt, updatedAt: session.updatedAt }, 201)
  })
  app.get("/api/v1/sessions/:id", async (c) => {
    const user = await userOf(c)
    const session = await d.store.load(c.req.param("id"), user.id)
    if (!session) return c.json({ error: "not found" }, 404)
    return c.json(session)
  })
  app.delete("/api/v1/sessions/:id", async (c) => {
    const user = await userOf(c)
    await d.store.delete(c.req.param("id"), user.id)
    d.engine.forgetSession(c.req.param("id"))
    return c.json({ ok: true })
  })
  // 从 GC 归档（trash/，保留期 7 天）恢复会话：归属用户或 admin 可操作；
  // 恢复 = 目录整体移回分片存储位置（会话数据/tmp 附件/env 一并恢复）
  app.post("/api/v1/sessions/:id/restore", async (c) => {
    const user = await userOf(c)
    const id = c.req.param("id")
    const hit = await findInTrash(d.config.gebaiHome, id)
    // 归属不符与未找到同应答（不泄露他人会话存在性）
    if (!hit || (user.role !== "admin" && hit.owner !== user.id)) return c.json({ error: "not found" }, 404)
    const target = sessionPath(d.config.gebaiHome, hit.owner, id)
    if (existsSync(target)) return c.json({ error: "session already exists" }, 409)
    try {
      await rename(hit.trashDir, target)
    } catch (err) {
      return c.json({ error: `restore failed: ${String((err as Error).message || err)}` }, 500)
    }
    d.store.evict(id)
    return c.json({ ok: true })
  })
  app.patch("/api/v1/sessions/:id", async (c) => {
    const user = await userOf(c)
    const body = await c.req.json<{ name?: string; pinned?: boolean }>()
    if (body.name) await d.store.rename(c.req.param("id"), body.name, user.id)
    if (typeof body.pinned === "boolean") await d.store.setPinned(c.req.param("id"), body.pinned, user.id)
    return c.json({ ok: true })
  })

  // Env
  // 环境变量配置目录：前端面板白名单（全局静态组 + 各子Agent 导出 envVars 汇总组；不含启动级/安全敏感变量）。
  // 无敏感信息，local/server 模式均可用（服务模式需登录）。
  app.get("/api/v1/env/catalog", async (c) => {
    await userOf(c)
    return c.json({ groups: getEnvCatalog(d.subAgents.allDefs()) })
  })
  app.get("/api/v1/sessions/:id/env", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const envs: EnvVarSource[] = await d.env.describe(sessionId, user.id)
    return c.json(envs)
  })
  // 会话 env 写入（内存态，不落盘——用户环境变量只存浏览器本地，此处仅供运行中即时生效类开关使用；
  // 进程重启即空，前端每次加载会话自行重新同步）
  app.put("/api/v1/sessions/:id/env", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    let vars: unknown
    try {
      vars = await c.req.json()
    } catch {
      return c.json({ error: "请求体必须是 JSON 对象" }, 400)
    }
    // 名称合法性校验：仅允许标识符形式且拒绝 __proto__（原型污染，防注入/防污染会话 env）
    const err = validateEnvVars(vars)
    if (err) return c.json({ error: err }, 400)
    const env = await d.store.setEnv(sessionId, user.id, vars as Record<string, string | null>)
    // 敏感键脱敏返回（与 GET describe 同规则，防明文密钥回读）
    return c.json(maskEnv(env))
  })

  // Todos
  app.get("/api/v1/sessions/:id/todos", async (c) => {
    const user = await userOf(c)
    const todos: TodoItem[] = await d.store.getTodos(c.req.param("id"), user.id)
    return c.json(todos)
  })

  // Truncate / Attachments
  app.post("/api/v1/sessions/:id/truncate", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    // 运行中任务持有自己的上下文快照并继续追加消息，中途截断会产生交错历史：拒绝，先停止或等任务完成
    if (d.engine.isRunning(sessionId)) return c.json({ error: "task already running" }, 409)
    const body = await c.req.json<{ before: string }>()
    if (!body.before) return c.json({ error: "before required" }, 400)
    await d.store.truncateMessages(sessionId, user.id, body.before)
    return c.json({ ok: true })
  })
  app.post("/api/v1/sessions/:id/attachments", async (c) => {
    const user = await userOf(c)
    const body = await c.req.formData()
    const file = body.get("file")
    if (!file) return c.json({ error: "no file" }, 400)
    const tmp = d.store.getTmpDir(c.req.param("id"), user.id)
    // 文件名消毒：仅 basename，拒绝路径穿越
    const rawName = typeof file === "object" && "name" in file ? (file as File).name : "upload"
    const name = basenameName(rawName) || "upload"
    const buf = await (file as Blob).arrayBuffer()
    await Bun.write(join(tmp, name), new Uint8Array(buf))
    return c.json({ id: name, name, mime: (file as Blob).type, size: buf.byteLength, path: `tmp/${name}` })
  })

  // Cancel / Approval / Choice / Draw / Compact
  app.post("/api/v1/sessions/:id/cancel", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    d.engine.cancel(sessionId)
    return c.json({ ok: true })
  })
  app.post("/api/v1/sessions/:id/approval", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    const { toolCallId, approve } = await c.req.json<{ toolCallId: string; approve: boolean }>()
    await d.engine.decideApproval(sessionId, toolCallId, approve)
    return c.json({ ok: true })
  })
  // 选择决策（ask 选项询问分支等待的用户选择）；option 单选 / options 数组多选 / refuse=true 拒绝回答
  app.post("/api/v1/sessions/:id/choice", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    const { choiceId, option, options, refuse } = await c.req.json<{ choiceId: string; option?: string; options?: string[]; refuse?: boolean }>()
    const multi = Array.isArray(options)
    if (multi && !options.length) return c.json({ error: "options must not be empty" }, 400)
    if (option == null && !multi && refuse !== true) return c.json({ error: "option, options or refuse required" }, 400)
    await d.engine.decideChoice(sessionId, choiceId, refuse === true ? null : multi ? options!.map(String) : String(option))
    return c.json({ ok: true })
  })
  // 画图渲染结果回传（show 图表分支等待的前端渲染结果）
  app.post("/api/v1/sessions/:id/draw", async (c) => {
    const user = await userOf(c)
    const sessionId = c.req.param("id")
    const s = await d.store.load(sessionId, user.id)
    if (!s) return c.json({ error: "not found" }, 404)
    const { renderId, ok, error } = await c.req.json<{ renderId: string; ok: boolean; error?: string }>()
    await d.engine.decideDrawResult(sessionId, renderId, { ok, error })
    return c.json({ ok: true })
  })
  app.post("/api/v1/sessions/:id/compact", async (c) => {
    const user = await userOf(c)
    const body = (await c.req.json().catch(() => ({}))) as { scope?: "all" | { from: number; to: number } }
    const result = await d.engine.compactSession(c.req.param("id"), user.id, body.scope)
    return c.json(result)
  })

  // Prompt（同步 JSON）与 Chat（单 HTTP 一站式）
  /** 每用户 prompt 速率限制（容量 60 突发、30/秒补充；防单用户刷 LLM 配额，与 WS 同规则；
   *  prompt 与 chat 同桶共用——防双端点交替绕限）。 */
  const promptRateLimit = new TokenBucket(60, 30)

  /** 同步等待任务完成并取最终 assistant 消息（prompt 与 chat 共用）：订阅任务错误（LLM 失败等经
   *  event.task.error 发布）；引擎级错误（会话不存在/任务冲突）同样归一为 error 字段。
   *  交互模式与输出方式均为请求层配置：依赖前端/多轮交互的工具（声明 interaction 高于所选模式）由引擎
   *  自动禁用；autoApprove 控制审批姿态（见 engine.run）；stream=true 时推送流式事件（event.message.delta
   *  等，接入方经 WS 事件订阅消费）；需要完整交互能力请使用 WS 通道（/ws）。 */
  const runPromptTask = async (
    sessionId: string,
    user: { id: string; role: string },
    prompt: string,
    opts: { attachments?: AttachmentInput[]; envOverride?: Record<string, string>; messageId?: string; autoApprove?: boolean; outputMode?: "final_only" | "streaming"; interactionMode?: "none" | "multi_turn" | "realtime" },
  ): Promise<{ message: { id: string; content: string; createdAt: number } | null; error?: string }> => {
    let taskError: string | null = null
    const unsub = d.events.subscribe((ev) => {
      if (ev.sessionId !== sessionId || ev.type !== "event.task.error") return
      taskError = String(ev.payload.error ?? "unknown error")
    })
    try {
      await d.engine.run(sessionId, user.id, prompt, {
        attachments: opts.attachments,
        envOverride: opts.envOverride,
        messageId: opts.messageId,
        autoApprove: opts.autoApprove,
        interactionMode: opts.interactionMode ?? "none",
        outputMode: opts.outputMode,
        role: user.role,
      })
    } catch (e) {
      // 会话不存在/任务冲突等引擎级错误：与任务错误同一返回形态（error 字段）
      return { message: null, error: String((e as Error).message || e) }
    } finally {
      unsub()
    }
    const session = await d.store.load(sessionId, user.id)
    const last = session ? [...session.messages].reverse().find((m) => m.role === "assistant") : undefined
    return {
      message: last
        ? { id: last.id, content: typeof last.content === "string" ? last.content : "", createdAt: last.createdAt }
        : null,
      ...(taskError ? { error: taskError } : {}),
    }
  }

  /** prompt 请求体公共解析：附件/浏览器本地 env 注入（宽容过滤，与 WS prompt 同规则）/messageId/autoApprove。 */
  const parsePromptBody = (body: { attachments?: Array<{ name: string; mime?: string; path?: string; data?: number[] }>; env?: Record<string, string | null>; messageId?: string; autoApprove?: unknown }) => {
    const attachments: AttachmentInput[] | undefined = body.attachments?.map((a) => ({
      name: a.name,
      mime: a.mime,
      path: a.path,
      data: a.data ? new Uint8Array(a.data) : undefined,
    }))
    const envOverride: Record<string, string> | undefined = body.env ? filterEnvInjection(body.env) : undefined
    return {
      attachments,
      envOverride,
      messageId: body.messageId != null ? String(body.messageId) : undefined,
      autoApprove: typeof body.autoApprove === "boolean" ? body.autoApprove : undefined,
    }
  }

  app.post("/api/v1/sessions/:id/prompt", async (c) => {
    const user = await userOf(c)
    // 每用户消息速率限制（防高频 prompt 消耗 LLM 配额/资源）
    if (!promptRateLimit.allow(user.id)) return c.json({ error: "rate limited: too many requests" }, 429)
    const sessionId = c.req.param("id")
    const body = await c.req.json<{ prompt: string; attachments?: Array<{ name: string; mime?: string; path?: string; data?: number[] }>; env?: Record<string, string | null>; messageId?: string; interactionMode?: string; stream?: boolean; autoApprove?: unknown }>()
    if (typeof body.prompt !== "string" || !body.prompt) return c.json({ error: "prompt required" }, 400)
    if (d.engine.isRunning(sessionId)) return c.json({ error: "task already running" }, 409)
    // 请求层交互模式与输出方式配置（服务端全部支持，接入方按需选择）：
    // interactionMode 默认 none（无交互）；stream=false（默认）仅最终响应，true 流式输出（经 WS 事件订阅消费）
    const interactionMode = body.interactionMode ?? "none"
    if (!["none", "multi_turn", "realtime"].includes(interactionMode)) return c.json({ error: `interactionMode 非法: ${interactionMode}（可选 none/multi_turn/realtime）` }, 400)
    const parsed = parsePromptBody(body)
    const result = await runPromptTask(sessionId, user, body.prompt, { ...parsed, outputMode: body.stream === true ? "streaming" : "final_only", interactionMode: interactionMode as "none" | "multi_turn" | "realtime" })
    return c.json(result)
  })

  // 单 HTTP 一站式调用（DESIGN「REST HTTP（同步通道）」）：一次请求完成「建会话（缺省自动）→ 执行任务 →
  // 返回最终回复」；带 sessionId 则在既有会话续聊（多轮上下文延续）。interactionMode 固定 none（单次调用
  // 无交互往返），审批姿态经 autoApprove 显式控制（true 自动通过 / false 需审批工具直接拒绝 / 缺省通道默认）。
  app.post("/api/v1/chat", async (c) => {
    const user = await userOf(c)
    if (!promptRateLimit.allow(user.id)) return c.json({ error: "rate limited: too many requests" }, 429)
    const body = (await c.req.json<{ prompt?: string; sessionId?: string; name?: string; attachments?: Array<{ name: string; mime?: string; path?: string; data?: number[] }>; env?: Record<string, string | null>; messageId?: string; autoApprove?: unknown; stream?: boolean }>().catch(() => ({}))) as { prompt?: string; sessionId?: string; name?: string; attachments?: Array<{ name: string; mime?: string; path?: string; data?: number[] }>; env?: Record<string, string | null>; messageId?: string; autoApprove?: unknown; stream?: boolean }
    if (typeof body.prompt !== "string" || !body.prompt) return c.json({ error: "prompt required" }, 400)
    let sessionId: string
    if (body.sessionId != null) {
      // 既有会话续聊：id 白名单（与 :id 路径段同规则）+ 归属校验（不存在/非本人同一 404，不泄露存在性）
      const id = String(body.sessionId)
      if (!isValidSessionId(id)) return c.json({ error: `invalid session id: ${id}` }, 400)
      const existing = await d.store.load(id, user.id)
      if (!existing) return c.json({ error: "session not found" }, 404)
      sessionId = id
    } else {
      const session = await d.store.createSession(user.id, body.name)
      sessionId = session.id
    }
    if (d.engine.isRunning(sessionId)) return c.json({ error: "task already running" }, 409)
    const parsed = parsePromptBody(body)
    const result = await runPromptTask(sessionId, user, body.prompt, { ...parsed, outputMode: body.stream === true ? "streaming" : "final_only" })
    // sessionId 一并返回：自动建会话的调用方凭此续聊（后续请求带 sessionId）
    return c.json({ sessionId, ...result })
  })
}
