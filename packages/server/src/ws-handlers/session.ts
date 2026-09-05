/** WS 会话域消息：session.* 增删改查/切换/恢复/附加/取消/压缩 + env/todo/tool/files/attachment 子资源。 */
import type { WsHandler } from "./context"
import { join } from "node:path"
import { toSessionInfo } from "../core/session/store"
import { basenameName } from "../core/base/paths"
import { validateEnvVars, maskEnv } from "../core/session/env"
import { findInTrash } from "../core/session/gc"
import { sessionPath } from "../core/base/paths"
import { rename } from "node:fs/promises"
import { existsSync } from "node:fs"

export const sessionHandlers: Record<string, WsHandler> = {
  "session.list": async ({ d, user, reply }) => {
    const sessions = await d.store.listSessions(user.id)
    return reply(true, { sessions: sessions.map(toSessionInfo), maxContextTokens: d.engine.contextWindow() })
  },
  "session.create": async ({ d, user, p, reply }) => {
    const s = await d.store.createSession(user.id, p.name ? String(p.name) : undefined)
    return reply(true, { session: toSessionInfo(s) })
  },
  "session.get": async ({ d, user, p, reply }) => {
    const s = await d.store.load(String(p.id), user.id)
    return reply(true, { session: s })
  },
  "session.delete": async ({ d, user, p, conn, reply }) => {
    await d.store.delete(String(p.id), user.id)
    d.engine.forgetSession(String(p.id))
    // 删除的恰为当前会话：清空每用户持久化状态
    if (conn.getCurrent() === String(p.id)) conn.setCurrent(undefined)
    return reply(true)
  },
  "session.rename": async ({ d, user, p, reply }) => {
    await d.store.rename(String(p.id), String(p.name), user.id)
    return reply(true)
  },
  "session.pin": async ({ d, user, p, reply }) => {
    await d.store.setPinned(String(p.id), p.pinned === true, user.id)
    return reply(true)
  },
  "session.switch": async ({ d, user, p, conn, reply }) => {
    // 切换连接级当前会话（校验归属；当前会话持久化到每用户状态，重连/重启自动恢复）
    const s = await d.store.load(String(p.id), user.id)
    if (!s) return reply(false, undefined, "session not found")
    conn.setCurrent(s.id)
    return reply(true, { session: toSessionInfo(s) })
  },
  "session.current": async ({ d, user, conn, reply }) => {
    const id = conn.getCurrent()
    if (!id) return reply(true, { session: null })
    const s = await d.store.load(id, user.id)
    return reply(true, { session: s ? toSessionInfo(s) : null })
  },
  "session.compact": async ({ d, user, p, reply }) => {
    const scope = p.scope as "all" | { from: number; to: number } | undefined
    const result = await d.engine.compactSession(String(p.id), user.id, scope)
    return reply(true, result)
  },
  "session.cancel": async ({ d, user, p, reply }) => {
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, undefined, "session not found")
    d.engine.cancel(sessionId)
    return reply(true)
  },
  "session.attach": async ({ d, user, p, reply }) => {
    // 运行中会话附加快照（DESIGN「运行中会话恢复」）：页面刷新/切换后前端恢复在途流与待决交互卡。
    // 归属校验（仅所有者）；lastSeq 为该用户事件日志基线——快照反映到该 seq 为止，前端附加流据此
    // 过滤已含入快照的事件并按 seq 重放缺口（与断线恢复同一套 seq 机制）
    const sessionId = String(p.id)
    const s = await d.store.load(sessionId, user.id)
    if (!s) return reply(false, undefined, "session not found")
    const snap = d.engine.attachSnapshot(sessionId)
    return reply(true, { ...(snap ?? { running: false, pending: [] }), lastSeq: d.state ? d.state.journal(user.id).lastSeq() : 0 })
  },
  "session.restore": async ({ d, user, p, reply }) => {
    // 从 GC 归档（trash/）恢复会话（归属用户或 admin；详见 REST /sessions/:id/restore）
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
  },
  "session.todo.get": async ({ d, user, p, reply }) => {
    const todos = await d.store.getTodos(String(p.id), user.id)
    return reply(true, { todos })
  },
  "session.env.get": async ({ d, user, p, reply }) => {
    const envs = await d.env.describe(String(p.id), user.id)
    return reply(true, { env: envs })
  },
  "session.env.set": async ({ d, user, p, reply }) => {
    // 会话 env 写入（内存态，不落盘——用户环境变量只存浏览器本地，重启即空，前端加载会话自行同步）
    const err = validateEnvVars(p.vars)
    if (err) return reply(false, undefined, err)
    const env = await d.store.setEnv(String(p.id), user.id, (p.vars as Record<string, string | null>) || {})
    // 敏感键脱敏返回（与 REST 同规则，防明文密钥回读）
    return reply(true, { env: maskEnv(env) })
  },
  "session.tool.get": ({ d, reply }) =>
    reply(true, {
      tools: d.registry.list(false).map((rt) => ({
        name: rt.name,
        description: rt.tool.description,
        enabled: rt.enabled,
        group: rt.agent || "global",
        approvalRequired: !!rt.tool.requiresApproval,
        card: rt.tool.card,
      })),
    }),
  "session.files.list": async ({ d, user, p, reply }) => {
    const files = await d.store.listSessionFiles(String(p.id), user.id)
    return reply(true, { files })
  },
  "session.files.get": async ({ d, user, p, reply }) => {
    // 与 REST 文件接口一致：以会话 tmp/ 为根（DESIGN：文件操作严格限定在会话 tmp/ 内），
    // 兼容 `tmp/xxx` 前缀路径；沙箱启用时拒绝越界（../、绝对路径、符号链接）
    const safe = d.store.resolveSessionTmpFile(String(p.id), user.id, String(p.path ?? ""), d.sandbox.enforcedFor(user.id))
    const content = await Bun.file(safe).text()
    return reply(true, { content })
  },
  "session.attachment.upload": async ({ d, user, p, reply }) => {
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
  },
}
