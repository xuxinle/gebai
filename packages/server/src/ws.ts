import { join } from "node:path"
import type { FeedbackInfo, FeedbackInput, UserPatch } from "@gebai/sdk"
import type { AuthUser } from "./auth"
import { SERVICE_USER, type AppDeps } from "./app"
import { readFeedback, writeFeedback } from "./feedback"
import { toSessionInfo } from "./core/store"
import { basenameName, isValidSessionId } from "./core/paths"
import { validateEnvVars, maskEnv, filterEnvInjection } from "./core/env"
import { TokenBucket } from "./core/ratelimit"

/** 每用户 prompt 速率限制（容量 60 突发、30/秒补充；防单用户刷 LLM 配额，与 REST 同规则）。 */
const promptRateLimit = new TokenBucket(60, 30)

/** 需要合法会话 ID 的 WS 消息类型（入口统一校验，防 sessionId 路径穿越）。 */
const SESSION_ID_MSGS = new Set([
  "session.get", "session.delete", "session.rename", "session.switch", "session.compact",
  "session.prompt", "session.attachment.upload", "session.todo.get", "session.env.get", "session.env.set",
  "session.files.list", "session.files.get", "session.cancel", "session.restore",
  "approval.decide", "choice.decide", "env.decide", "draw.result", "capture.result",
])

/** WS 连接上下文：用户 + 连接级当前会话（session.switch/current 用）。 */
export interface WsConn {
  get: () => AuthUser
  set: (u: AuthUser) => void
  getCurrent: () => string | undefined
  setCurrent: (id: string | undefined) => void
  /** 连接用户变更回调注册（auth.login/logout 后事件订阅重绑用）；返回退订函数。 */
  onUserChange?: (cb: () => void) => () => void
}

export interface WsSink {
  send(data: string): void
}

export async function handleWsMessage(
  d: AppDeps,
  ws: WsSink,
  msg: { type: string; payload?: Record<string, unknown>; id?: string },
  conn: WsConn,
) {
  const reply = (ok: boolean, payload?: Record<string, unknown>, error?: string) => {
    ws.send(JSON.stringify({ type: msg.type, id: msg.id, ok, payload, error }))
  }
  const p = (msg.payload || {}) as Record<string, unknown>
  const user = conn.get()

  // 多用户模式：未登录（SERVICE_USER 兜底）的连接仅允许 auth.login
  if (d.config.auth === "server" && user.id === SERVICE_USER.id && msg.type !== "auth.login") {
    return reply(false, undefined, "unauthorized: login required")
  }

  // 会话 ID 格式白名单：穿越串/畸形 id 统一拒绝（多用户隔离防线，与 REST/存储层同规则）
  if (SESSION_ID_MSGS.has(msg.type)) {
    const sid = String(p.id ?? "")
    if (!isValidSessionId(sid)) return reply(false, undefined, `invalid session id: ${sid}`)
  }

  try {
  switch (msg.type) {
    case "auth.login": {
      // 登录（多用户模式）：密码登录 / 已有令牌 / 服务 API Key 三种方式。
      // 令牌方式供 SDK 重连后自动重新认证（WS 连接无 Header，重连即恢复用户上下文）。
      let u: AuthUser | null = null
      let tok: string | undefined
      if (p.token != null) {
        tok = String(p.token)
        u = await d.auth.authorize(tok)
      } else {
        tok = (await d.auth.login(String(p.username), String(p.password))) ?? undefined
        if (tok) u = await d.auth.authorize(tok)
      }
      if (!u) return reply(false, undefined, "invalid credentials")
      conn.set(u)
      const replyPayload: Record<string, unknown> = { user: d.auth.strip(u) }
      if (tok) replyPayload.token = tok
      if (d.state) replyPayload.snapshot = await d.state.buildSnapshot(conn)
      reply(true, replyPayload)
      // 多用户模式登录成功推送状态快照（客户端模型基线）；单用户模式已在建连时推送
      if (d.state && d.config.auth === "server") void d.state.pushSnapshot(ws, conn).catch(() => {})
      return
    }
    case "auth.logout":
      // 撤销服务端令牌走 REST（需 token）；WS 侧清空连接用户态（回到未登录）
      conn.set(SERVICE_USER)
      conn.setCurrent(undefined)
      reply(true)
      if (d.state && d.config.auth === "server") void d.state.pushSnapshot(ws, conn).catch(() => {})
      return
    case "ping":
      // 心跳应答：客户端周期发送保活穿越代理（防止按闲置时间断连），回 pong
      return reply(true, { pong: true })
    case "session.list": {
      const sessions = await d.store.listSessions(user.id)
      return reply(true, { sessions: sessions.map(toSessionInfo), maxContextTokens: d.engine.contextWindow() })
    }
    case "session.create": {
      const s = await d.store.createSession(user.id, p.name ? String(p.name) : undefined)
      return reply(true, { session: toSessionInfo(s) })
    }
    case "session.get": {
      const s = await d.store.load(String(p.id), user.id)
      return reply(true, { session: s })
    }
    case "session.delete": {
      await d.store.delete(String(p.id), user.id)
      d.engine.forgetSession(String(p.id))
      // 删除的恰为当前会话：清空每用户持久化状态
      if (conn.getCurrent() === String(p.id)) conn.setCurrent(undefined)
      return reply(true)
    }
    case "session.rename":
      await d.store.rename(String(p.id), String(p.name), user.id)
      return reply(true)
    case "session.switch": {
      // 切换连接级当前会话（校验归属；当前会话持久化到每用户状态，重连/重启自动恢复）
      const s = await d.store.load(String(p.id), user.id)
      if (!s) return reply(false, undefined, "session not found")
      conn.setCurrent(s.id)
      return reply(true, { session: toSessionInfo(s) })
    }
    case "session.current": {
      const id = conn.getCurrent()
      if (!id) return reply(true, { session: null })
      const s = await d.store.load(id, user.id)
      return reply(true, { session: s ? toSessionInfo(s) : null })
    }
    case "state.snapshot": {
      // 状态快照（MVC 模型基线）：当前会话 + 会话列表 + 运行中会话 + 日志基线 seq；
      // 建连（单用户）/登录（多用户）时自动推送，也可主动请求
      const snap = (d.state ? await d.state.buildSnapshot(conn) : { currentSessionId: null, sessions: [], running: [], lastSeq: 0 }) as unknown as Record<string, unknown>
      return reply(true, snap)
    }
    case "sync.request": {
      // 断线补偿：按 seq 重放离线期间错过的日志事件；缓冲溢出（缺口）返回 overrun，
      // 客户端走全量重同步（session.get 等）收敛
      const lastSeq = Number(p.lastSeq ?? 0)
      const j = d.state?.journal(user.id)
      if (!j) return reply(true, { events: null, overrun: true, lastSeq: 0 })
      const r = j.replay(lastSeq)
      if (!r) return reply(true, { events: null, overrun: true, lastSeq: j.lastSeq() })
      return reply(true, { events: r.entries, overrun: false, lastSeq: r.lastSeq })
    }
    case "session.compact": {
      const scope = p.scope as "all" | { from: number; to: number } | undefined
      const result = await d.engine.compactSession(String(p.id), user.id, scope)
      return reply(true, result)
    }
    case "session.prompt": {
      // 发起任务（流式事件经 event.* 推送回流，reply 仅确认）
      const sessionId = String(p.id)
      const prompt = String(p.prompt ?? "")
      if (!prompt) return reply(false, { code: "prompt_required" }, "prompt required")
      // 每用户消息速率限制（防高频 prompt 消耗 LLM 配额/资源，与 REST 同规则）
      if (!promptRateLimit.allow(user.id)) return reply(false, { code: "rate_limited" }, "rate limited: too many requests")
      if (d.engine.isRunning(sessionId)) return reply(false, { code: "already_running" }, "task already running")
      // 应答语义如实：会话不存在/任务冲突等启动期失败在 reply 中返回错误码（客户端据此判定，
      // 不再依赖错误文案正则）；任务运行期错误仍经 event.task.error 推送
      const existing = await d.store.load(sessionId, user.id)
      if (!existing) return reply(false, { code: "session_not_found" }, "session not found")
      // 请求层交互模式与输出方式配置（默认 realtime + 流式输出，前端不传即现状）：
      // interactionMode 可选 none/multi_turn/realtime；stream=false 时仅最终响应（不推送文本增量/推理流）
      const interactionMode = p.interactionMode == null ? undefined : String(p.interactionMode)
      if (interactionMode && !["none", "multi_turn", "realtime"].includes(interactionMode)) return reply(false, undefined, `interactionMode 非法: ${interactionMode}（可选 none/multi_turn/realtime）`)
      const outputMode = p.stream === true ? "streaming" : p.stream === false ? "final_only" : undefined
      // 浏览器本地环境变量注入（与 REST prompt 同规则）：不支持/非法的变量直接跳过（宽容过滤，
      // 防 localStorage 残留旧版目录外键阻断整个任务），其余随任务临时生效、不持久化
      const envOverride: Record<string, string> | undefined = p.env
        ? filterEnvInjection(p.env as Record<string, string | null>)
        : undefined
      const attachments = (Array.isArray(p.attachments) ? p.attachments : []) as Array<{ name: string; mime?: string; path?: string; data?: number[] }>
      void d.engine
        .run(sessionId, user.id, prompt, {
          attachments: attachments.map((a) => ({ name: a.name, mime: a.mime, path: a.path, data: a.data ? new Uint8Array(a.data) : undefined })),
          messageId: p.messageId != null ? String(p.messageId) : undefined,
          envOverride,
          interactionMode: interactionMode as "none" | "multi_turn" | "realtime" | undefined,
          outputMode: outputMode as "final_only" | "streaming" | undefined,
          role: user.role,
        })
        .catch((err) => {
          // 引擎 run 内部已发布 event.task.error；此处兜底记录（正常不应到达）
          console.warn(`[ws] engine.run failed: ${String((err as Error).message || err)}`)
        })
      return reply(true)
    }
    case "session.attachment.upload": {
      // 附件上传（WS 通道，base64 整体传输；二进制分段见 DESIGN，整体传输已覆盖）
      const { writeFile, mkdir } = await import("node:fs/promises")
      const sessionId = String(p.id)
      const data = p.data as string | undefined
      if (!data) return reply(false, undefined, "data required (base64)")
      const name = basenameName(String(p.name ?? "upload")) || "upload"
      const tmp = d.store.getTmpDir(sessionId, user.id)
      const buf = Buffer.from(data, "base64")
      const safePath = join(tmp, name)
      await mkdir(tmp, { recursive: true })
      await writeFile(safePath, buf)
      return reply(true, { id: name, name, mime: p.mime ? String(p.mime) : "application/octet-stream", size: buf.length, path: `tmp/${name}` })
    }
    case "session.todo.get": {
      const todos = await d.store.getTodos(String(p.id), user.id)
      return reply(true, { todos })
    }
    case "session.env.get": {
      const envs = await d.env.describe(String(p.id), user.id)
      return reply(true, { env: envs })
    }
    case "session.env.set": {
      // 会话 env 写入（内存态，不落盘——用户环境变量只存浏览器本地，重启即空，前端加载会话自行同步）
      const err = validateEnvVars(p.vars)
      if (err) return reply(false, undefined, err)
      const env = await d.store.setEnv(String(p.id), user.id, (p.vars as Record<string, string | null>) || {})
      // 敏感键脱敏返回（与 REST 同规则，防明文密钥回读）
      return reply(true, { env: maskEnv(env) })
    }
    case "session.tool.get":
      return reply(true, {
        tools: d.registry.list(false).map((rt) => ({
          name: rt.name,
          description: rt.tool.description,
          enabled: rt.enabled,
          group: rt.agent || "global",
          approvalRequired: !!rt.tool.requiresApproval,
          card: rt.tool.card,
        })),
      })
    case "session.files.list": {
      const files = await d.store.listSessionFiles(String(p.id), user.id)
      return reply(true, { files })
    }
    case "session.files.get": {
      // 与 REST 文件接口一致：以会话 tmp/ 为根（DESIGN：文件操作严格限定在会话 tmp/ 内），
      // 兼容 `tmp/xxx` 前缀路径；沙箱启用时拒绝越界（../、绝对路径、符号链接）
      const safe = d.store.resolveSessionTmpFile(String(p.id), user.id, String(p.path ?? ""), d.sandbox.enforcedFor(user.id))
      const content = await Bun.file(safe).text()
      return reply(true, { content })
    }
    case "session.cancel": {
      const sessionId = String(p.id)
      const s = await d.store.load(sessionId, user.id)
      if (!s) return reply(false, undefined, "session not found")
      d.engine.cancel(sessionId)
      return reply(true)
    }
    case "approval.decide": {
      const sessionId = String(p.id)
      const s = await d.store.load(sessionId, user.id)
      if (!s) return reply(false, undefined, "session not found")
      await d.engine.decideApproval(sessionId, String(p.toolCallId), Boolean(p.approve))
      return reply(true)
    }
    case "choice.decide": {
      // 提交用户选择（ask_user 工具阻塞等待）；option 单选 / options 数组多选 / refuse=true（或均缺失）拒绝回答
      const sessionId = String(p.id)
      const s = await d.store.load(sessionId, user.id)
      if (!s) return reply(false, undefined, "session not found")
      const multi = Array.isArray(p.options)
      const refuse = p.refuse === true || (!multi && p.option == null)
      await d.engine.decideChoice(sessionId, String(p.choiceId), refuse ? null : multi ? (p.options as unknown[]).map(String) : String(p.option))
      return reply(true)
    }
    case "env.decide": {
      // 提交用户填写的环境变量值（ask_env 工具阻塞等待）；value 缺失视为拒绝
      const sessionId = String(p.id)
      const s = await d.store.load(sessionId, user.id)
      if (!s) return reply(false, undefined, "session not found")
      const value = p.value == null ? null : String(p.value)
      await d.engine.decideEnvResult(sessionId, String(p.envId ?? ""), value)
      return reply(true)
    }
    case "draw.result": {
      // 提交前端渲染结果（draw 工具阻塞等待）
      const sessionId = String(p.id)
      const s = await d.store.load(sessionId, user.id)
      if (!s) return reply(false, undefined, "session not found")
      await d.engine.decideDrawResult(sessionId, String(p.renderId), { ok: Boolean(p.ok), error: p.error != null ? String(p.error) : undefined })
      return reply(true)
    }
    case "capture.result": {
      // 提交前端页面捕获结果（page_capture 工具阻塞等待）
      const sessionId = String(p.id)
      const s = await d.store.load(sessionId, user.id)
      if (!s) return reply(false, undefined, "session not found")
      await d.engine.decideCaptureResult(sessionId, String(p.captureId), {
        html: String(p.html ?? ""),
        imageBase64: p.imageBase64 != null ? String(p.imageBase64) : undefined,
        error: p.error != null ? String(p.error) : undefined,
      })
      return reply(true)
    }
    case "feedback.submit": {
      const fb = p.feedback as FeedbackInput
      const id = await writeFeedback(d.config.gebaiHome, user.id, fb)
      return reply(true, { id })
    }
    case "feedback.list": {
      let list: FeedbackInfo[] = []
      if (user.role === "admin") {
        const users = await d.auth.listUsers()
        const targets = [...users, d.auth.defaultUser(), SERVICE_USER]
        const seen = new Set<string>()
        for (const u of targets) {
          if (seen.has(u.id)) continue
          seen.add(u.id)
          list = list.concat(await readFeedback(d.config.gebaiHome, u.id))
        }
      } else {
        list = await readFeedback(d.config.gebaiHome, user.id)
      }
      const filter = (p.filter as Record<string, string>) || {}
      if (filter.messageId) list = list.filter((f) => f.messageId === filter.messageId)
      if (filter.sessionId) list = list.filter((f) => f.sessionId === filter.sessionId)
      if (filter.type) list = list.filter((f) => f.type === filter.type)
      return reply(true, { feedback: list })
    }
    case "user.list": {
      if (user.role !== "admin") return reply(false, undefined, "admin only")
      const users = await d.auth.listUsers()
      return reply(true, { users: users.map((u) => d.auth.strip(u)) })
    }
    case "user.create": {
      if (user.role !== "admin") return reply(false, undefined, "admin only")
      const u = await d.auth.createUser(String(p.username), String(p.password), p.role as "user" | "admin" | undefined)
      return reply(true, { user: d.auth.strip(u) })
    }
    case "user.update": {
      if (user.role !== "admin") return reply(false, undefined, "admin only")
      const u = await d.auth.updateUser(String(p.id), (p.patch as UserPatch) || {})
      return reply(true, { user: d.auth.strip(u) })
    }
    case "user.delete": {
      if (user.role !== "admin") return reply(false, undefined, "admin only")
      await d.auth.deleteUser(String(p.id))
      return reply(true)
    }
    case "sub_agent.list":
      return reply(true, { subAgents: d.subAgents.list() })
    case "sub_agent.load": {
      const name = String(p.name)
      if (p.sessionId) {
        // 会话级装载：注册工具 + 提示词消息写入该会话记录并落盘（恢复历史会话时按记录还原）
        try {
          await d.engine.loadAgentToSession(String(p.sessionId), user.id, name)
          return reply(true)
        } catch (err) {
          return reply(false, undefined, String((err as Error).message || err))
        }
      }
      await d.subAgents.load(name)
      return reply(true)
    }
    case "sub_agent.unload": {
      // 卸载子Agent：带 sessionId 时同步清理该会话内已持久化的提示词消息与装载记录（与 load 对称，
      // 卸载后提示词不再占用上下文；ensureSessionAgents 也不会再按记录重新装载）
      const name = String(p.name)
      if (p.sessionId) {
        const sid = String(p.sessionId)
        if (!isValidSessionId(sid)) return reply(false, undefined, `invalid session id: ${sid}`)
        try {
          await d.engine.unloadAgentFromSession(sid, user.id, name)
          return reply(true)
        } catch (err) {
          return reply(false, undefined, String((err as Error).message || err))
        }
      }
      d.subAgents.unload(name)
      return reply(true)
    }
    case "session.restore": {
      // 从 GC 归档（trash/）恢复会话（归属用户或 admin；详见 REST /sessions/:id/restore）
      const { rename } = await import("node:fs/promises")
      const { existsSync } = await import("node:fs")
      const { findInTrash } = await import("./core/gc")
      const { sessionPath } = await import("./core/paths")
      const id = String(p.id)
      const hit = await findInTrash(d.config.gebaiHome, id)
      if (!hit || (user.role !== "admin" && hit.owner !== user.id)) return reply(false, { code: "session_not_found" }, "session not found")
      const target = sessionPath(d.config.gebaiHome, hit.owner, id)
      if (existsSync(target)) return reply(false, { code: "session_exists" }, "session already exists")
      try {
        await rename(hit.trashDir, target)
      } catch (err) {
        return reply(false, undefined, `restore failed: ${String((err as Error).message || err)}`)
      }
      d.store.evict(id)
      return reply(true)
    }
    default:
      return reply(false, undefined, `unknown message type: ${msg.type}`)
  }
  } catch (err) {
    // 分支内异常（如路径沙箱拒绝）统一转为错误应答，不中断连接
    return reply(false, undefined, String((err as Error).message || err))
  }
}
