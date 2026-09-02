/** WS 任务与状态域消息：session.prompt（发起任务）、state.snapshot、sync.request（断线补偿）。 */
import type { WsHandler } from "./context"
import { filterEnvInjection } from "../core/session/env"
import { TokenBucket } from "../core/security/ratelimit"

/** 每用户 prompt 速率限制（容量 60 突发、30/秒补充；防单用户刷 LLM 配额，与 REST 同规则）。 */
const promptRateLimit = new TokenBucket(60, 30)

export const promptHandlers: Record<string, WsHandler> = {
  "session.prompt": async ({ d, p, user, reply }) => {
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
  },
  "state.snapshot": async ({ d, conn, reply }) => {
    // 状态快照（MVC 模型基线）：当前会话 + 会话列表 + 运行中会话 + 日志基线 seq；
    // 建连（单用户）/登录（多用户）时自动推送，也可主动请求
    const snap = (d.state ? await d.state.buildSnapshot(conn) : { currentSessionId: null, sessions: [], running: [], lastSeq: 0 }) as unknown as Record<string, unknown>
    return reply(true, snap)
  },
  "sync.request": async ({ d, p, user, reply }) => {
    // 断线补偿：按 seq 重放离线期间错过的日志事件；缓冲溢出（缺口）返回 overrun，
    // 客户端走全量重同步（session.get 等）收敛
    const lastSeq = Number(p.lastSeq ?? 0)
    const j = d.state?.journal(user.id)
    if (!j) return reply(true, { events: null, overrun: true, lastSeq: 0 })
    const r = j.replay(lastSeq)
    if (!r) return reply(true, { events: null, overrun: true, lastSeq: j.lastSeq() })
    return reply(true, { events: r.entries, overrun: false, lastSeq: r.lastSeq })
  },
}
