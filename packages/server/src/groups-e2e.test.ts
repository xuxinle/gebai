/**
 * 多人会话（群）端到端：两用户经 WS 建群/入群/发言，验证四件事——
 * ① 群会话不进任何用户目录（`{GEBAI_HOME}/groups/{groupId}/`），会话列表按成员资格各自可见；
 * ② 用户消息自动拼接「角色·名称」前缀并落盘（模型与 UI 看到的是同一条文本）；
 * ③ 系统提示词注入多人会话注记（成员表 + 发言约定 + 发起者）；
 * ④ 群会话事件多播：一次发言两个成员的连接都能收到同一条流。
 */
import { afterAll, beforeAll, describe, expect, it } from "bun:test"
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"
import type { ChatOptions, LLMChunk, LLMProvider } from "./core/llm/llm"

/** 回显最后一次调用的消息（用于断言系统提示词与用户消息前缀）。 */
class EchoFake implements LLMProvider {
  readonly id = "fake"
  seen: MessageLike[] = []
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 32000 }
  }
  async *chat(msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.seen = msgs
    yield { type: "text", text: "收到" }
    yield { type: "done" }
  }
}

const home = mkdtempSync(join(tmpdir(), "gebai-groups-e2e-"))
let h: ServerHandle
const fake = new EchoFake()

beforeAll(async () => {
  h = await startServer({ gebaiHome: home, auth: "server", sandbox: "on", binaryMode: false, preloadSubAgents: [], port: 0 })
  ;(h.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = fake
  await h.auth.createUser("alice", "alice-pass-1", "user")
  await h.auth.createUser("bob", "bob-pass-1", "user")
})

afterAll(() => {
  h.gc?.stop()
  h.tasks?.stop()
  h.todos?.stop()
  h.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

function base() {
  return `http://127.0.0.1:${h.server.port}`
}

/** 读群会话正文（轮询到条件成立，超时抛错）：任务执行是异步的，prompt 应答只代表受理。 */
async function waitForChat(ready: (c: { messages: Array<{ role: string; content: string; speaker?: { name: string; role: string } }> }) => boolean, ms = 15000) {
  const file = join(home, "groups", current.groupId, "chat.json")
  const deadline = Date.now() + ms
  for (;;) {
    try {
      const c = JSON.parse(readFileSync(file, "utf8")) as { messages: Array<{ role: string; content: string; speaker?: { name: string; role: string } }> }
      if (ready(c)) return c
    } catch {
      /* 尚未写入 */
    }
    if (Date.now() > deadline) throw new Error("等待会话正文超时")
    await new Promise((r) => setTimeout(r, 50))
  }
}

/** 当前用例共享的群/会话坐标（跨 it 传递，Bun 的 it 顺序执行）。 */
const current = { groupId: "", sessionId: "" }

function json(body: unknown): RequestInit {
  return { headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }
}

/** 登录拿令牌（WS/REST 共用凭证来源链的 bearer 通道）。 */
async function login(username: string, password: string): Promise<string> {
  const r = await fetch(`${base()}/api/v1/auth/login`, { method: "POST", ...json({ username, password }) })
  const body = (await r.json()) as { token?: string }
  if (!body.token) throw new Error(`login failed: ${JSON.stringify(body)}`)
  return body.token
}

/** 单次 WS 请求-应答（先 auth.login 拿用户上下文，再发目标消息）：返回 { ok, payload, error }。 */
function wsCall(token: string, type: string, payload?: Record<string, unknown>) {
  return new Promise<{ ok: boolean; payload: Record<string, unknown> | undefined; error?: string }>((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${h.server.port}/ws`)
    const t = setTimeout(() => {
      ws.close()
      reject(new Error(`ws timeout: ${type}`))
    }, 10000)
    const finish = (r: { ok: boolean; payload: Record<string, unknown> | undefined; error?: string }) => {
      clearTimeout(t)
      ws.close()
      resolve(r)
    }
    ws.onopen = () => ws.send(JSON.stringify({ type: "auth.login", payload: { token }, id: "auth" }))
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { type?: string; id?: string; ok?: boolean; payload?: Record<string, unknown>; error?: string }
      if (data.id === "auth") {
        if (!data.ok) return finish({ ok: false, payload: undefined, error: data.error ?? "auth failed" })
        ws.send(JSON.stringify({ type, payload, id: "t1" }))
        return
      }
      if (data.id !== "t1") return
      finish({ ok: data.ok === true, payload: data.payload, error: data.error })
    }
    ws.onerror = () => {
      clearTimeout(t)
      reject(new Error(`ws error: ${type}`))
    }
  })
}

/** 打开一条已认证的长连接（收集事件），返回 { ws, events, waitFor, close }。 */
function wsStream(token: string) {
  const events: Array<{ type: string; sessionId: string; payload: Record<string, unknown> }> = []
  const ws = new WebSocket(`ws://127.0.0.1:${h.server.port}/ws`)
  let authed = false
  const ready = new Promise<void>((resolve) => {
    ws.onopen = () => ws.send(JSON.stringify({ type: "auth.login", payload: { token }, id: "auth" }))
    const timer = setInterval(() => {
      if (authed) {
        clearInterval(timer)
        resolve()
      }
    }, 20)
  })
  ws.onmessage = (m) => {
    const data = JSON.parse(String(m.data)) as { type?: string; id?: string; ok?: boolean; payload?: Record<string, unknown> }
    if (data.id === "auth" && data.ok) {
      authed = true
      return
    }
    if (!data.type || !data.payload) return
    // 在线推送的日志条目：{seq,type,sessionId,payload,timestamp}
    if (!data.type.startsWith("event.")) return
    const sid = String((data.payload as { sessionId?: string }).sessionId ?? "")
    events.push({ type: data.type, sessionId: sid, payload: data.payload })
  }
  return {
    ws,
    ready,
    events,
    /** 等到某类事件出现（含已到达的）：返回该事件，超时抛错。 */
    async waitFor(type: string, filter?: (e: { sessionId: string; payload: Record<string, unknown> }) => boolean): Promise<{ type: string; sessionId: string; payload: Record<string, unknown> }> {
      const deadline = Date.now() + 15000
      for (;;) {
        const hit = events.find((e) => e.type === type && (!filter || filter(e)))
        if (hit) return hit
        if (Date.now() > deadline) throw new Error(`等待事件超时: ${type}（已见 ${events.map((e) => e.type).join(",")}）`)
        await new Promise((r) => setTimeout(r, 30))
      }
    },
    close: () => ws.close(),
  }
}

describe("多人会话（群）端到端", () => {
  let aliceToken = ""
  let bobToken = ""
  let groupId = ""
  let joinCode = ""
  let sessionId = ""

  it("alice 建群（带进入口令）：会话落在 groups/{groupId}/，不在任何用户目录", async () => {
    aliceToken = await login("alice", "alice-pass-1")
    const r = await wsCall(aliceToken, "group.create", { name: "产品评审", passcode: "letmein", memberName: "张三", memberRole: "主持人" })
    expect(r.ok).toBe(true)
    const g = (r.payload as { group: { id: string; sessionId: string; joinCode: string; hasPasscode: boolean; me: { name: string; role: string } } }).group
    expect(g.hasPasscode).toBe(true)
    expect(g.me).toEqual({ name: "张三", role: "主持人", userId: expect.any(String), username: "alice" } as never)
    groupId = g.id
    joinCode = g.joinCode
    sessionId = g.sessionId
    current.groupId = groupId
    current.sessionId = sessionId
    // 磁盘：群目录有 chat.json + group.json + tmp/；用户目录下没有这个会话
    const gdir = join(home, "groups", groupId)
    expect(existsSync(join(gdir, "chat.json"))).toBe(true)
    expect(existsSync(join(gdir, "group.json"))).toBe(true)
    const raw = JSON.parse(readFileSync(join(gdir, "group.json"), "utf8")) as { passcode?: { hash: string }; members: Array<{ name: string; role: string }> }
    expect(raw.passcode?.hash).toBeTruthy()
    expect(JSON.stringify(raw)).not.toContain("letmein") // 口令明文不落盘
    expect(raw.members[0]).toMatchObject({ name: "张三", role: "主持人" })
    // 用户目录里不存在该会话 id
    const usersDir = join(home, "users")
    for (const u of readdirSync(usersDir)) {
      const sdir = join(usersDir, u, "sessions")
      if (!existsSync(sdir)) continue
      expect(JSON.stringify(readdirSync(sdir, { recursive: true })).includes(sessionId)).toBe(false)
    }
  })

  it("口令错误/不填都拒绝加入；正确口令 + 群内身份后加入成功并看到同一条会话", async () => {
    bobToken = await login("bob", "bob-pass-1")
    const noPass = await wsCall(bobToken, "group.join", { joinCode, memberName: "李四", memberRole: "工程师" })
    expect(noPass.ok).toBe(false)
    expect(noPass.error).toContain("口令")
    const wrong = await wsCall(bobToken, "group.join", { joinCode, passcode: "nope", memberName: "李四", memberRole: "工程师" })
    expect(wrong.ok).toBe(false)
    expect(wrong.error).toContain("口令不正确")
    const ok = await wsCall(bobToken, "group.join", { joinCode: joinCode.toLowerCase(), passcode: "letmein", memberName: "李四", memberRole: "工程师" })
    expect(ok.ok).toBe(true)
    // 会话列表：两人都看得到，且各自带自己的群内身份
    const aliceList = (await wsCall(aliceToken, "session.list")).payload as { sessions: Array<{ id: string; group?: { memberName: string; role: string; name: string } }> }
    const bobList = (await wsCall(bobToken, "session.list")).payload as { sessions: Array<{ id: string; group?: { memberName: string; role: string } }> }
    expect(aliceList.sessions.find((s) => s.id === sessionId)?.group).toMatchObject({ name: "产品评审", memberName: "张三", role: "主持人" })
    expect(bobList.sessions.find((s) => s.id === sessionId)?.group).toMatchObject({ memberName: "李四", role: "工程师" })
  })

  it("发言：用户消息自动拼接「角色·名称」前缀，系统提示词带多人会话注记", async () => {
    const bobStream = wsStream(bobToken)
    const aliceStream = wsStream(aliceToken)
    await Promise.all([bobStream.ready, aliceStream.ready])
    const send = await wsCall(bobToken, "session.prompt", { id: sessionId, prompt: "这个方案我建议先做灰度", stream: true })
    expect(send.ok).toBe(true)
    // 两次连接都收到这条任务流（群事件多播）
    const seenByAlice = await aliceStream.waitFor("event.message.done")
    expect(seenByAlice.sessionId).toBe(sessionId)
    const seenByBob = await bobStream.waitFor("event.message.done")
    expect(seenByBob.sessionId).toBe(sessionId)
    bobStream.close()
    aliceStream.close()
    // 落盘的用户消息带前缀（模型与 UI 看到同一文本）
    const chat = await waitForChat((c) => c.messages.some((m) => m.role === "user"))
    const userMsg = chat.messages.find((m) => m.role === "user")
    expect(userMsg?.content).toBe("【工程师·李四】这个方案我建议先做灰度")
    expect(userMsg?.speaker).toMatchObject({ name: "李四", role: "工程师" })
    // 模型侧：系统提示词含群注记（成员表 + 前缀约定），用户消息是带前缀的原文
    const sys = String(fake.seen.find((m) => m.role === "system")?.content ?? "")
    expect(sys).toContain("多人会话（群）")
    expect(sys).toContain("主持人·张三")
    expect(sys).toContain("工程师·李四")
    expect(sys).toContain("发起者是「工程师·李四」")
    const sentUser = fake.seen.filter((m) => m.role === "user").map((m) => String(m.content))
    expect(sentUser.some((c) => c.includes("【工程师·李四】这个方案我建议先做灰度"))).toBe(true)
  })

  it("会话列表更新群名后仍不影响前缀；改群内身份后新发言按新身份署名", async () => {
    const r = await wsCall(bobToken, "group.set_profile", { id: groupId, memberName: "李四·后端", memberRole: "工程师" })
    expect(r.ok).toBe(true)
    await wsCall(bobToken, "session.prompt", { id: sessionId, prompt: "收到，我排期" })
    // 任务由服务端异步执行（prompt 应答只是受理）：等新的 user 消息落盘再断言
    const chat = await waitForChat((c) => c.messages.filter((m) => m.role === "user").length >= 2)
    const us = chat.messages.filter((m) => m.role === "user").map((m) => m.content)
    expect(us).toContain("【工程师·李四·后端】收到，我排期")
    // 旧消息不被改写（历史保留当时身份）
    expect(us).toContain("【工程师·李四】这个方案我建议先做灰度")
  })

  it("非成员访问群会话：session.get / prompt 都被拒（与不存在同应答）", async () => {
    await h.auth.createUser("carol", "carol-pass-1", "user")
    const carolToken = await login("carol", "carol-pass-1")
    const get = await wsCall(carolToken, "session.get", { id: sessionId })
    expect(get.ok).toBe(true)
    expect((get.payload as { session: unknown }).session).toBeNull()
    const prompt = await wsCall(carolToken, "session.prompt", { id: sessionId, prompt: "我能插一句吗" })
    expect(prompt.ok).toBe(false)
    expect(prompt.error).toContain("session not found")
    // 群详情也不可见（不泄露存在性）
    const detail = await wsCall(carolToken, "group.get", { id: groupId })
    expect(detail.ok).toBe(false)
    expect((detail.payload as { code: string }).code).toBe("group_not_found")
  })

  it("群会话文件面：成员可读写产物，非成员一律 404（不能靠 userId 拼接路径做隐形隔离）", async () => {
    // 在群目录下写一份产物（等价于 agent 用 write 写进 tmp/）
    const { writeFileSync } = await import("node:fs")
    writeFileSync(join(home, "groups", groupId, "tmp", "report.md"), "# 周报")
    const alice = { Authorization: `Bearer ${aliceToken}` }
    const list = (await (await fetch(`${base()}/api/v1/sessions/${sessionId}/files`, { headers: alice })).json()) as Array<{ path: string }>
    expect(list.map((f) => f.path)).toContain("tmp/report.md")
    const content = await fetch(`${base()}/api/v1/sessions/${sessionId}/files/content?path=tmp%2Freport.md`, { headers: alice })
    expect(content.status).toBe(200)
    expect(await content.text()).toBe("# 周报")
    // 非成员：三个文件端点都 404（不泄露会话存在性）
    const carolToken = await login("carol", "carol-pass-1")
    const carol = { Authorization: `Bearer ${carolToken}` }
    expect((await fetch(`${base()}/api/v1/sessions/${sessionId}/files`, { headers: carol })).status).toBe(404)
    expect((await fetch(`${base()}/api/v1/sessions/${sessionId}/files/content?path=tmp%2Freport.md`, { headers: carol })).status).toBe(404)
    expect((await fetch(`${base()}/api/v1/sessions/${sessionId}/files/preview?path=tmp%2Freport.md`, { headers: carol })).status).toBe(404)
  })

  it("REST 同名能力：/api/v1/groups 列表、非成员 404、群主改名同步会话标题", async () => {
    const headers = { Authorization: `Bearer ${aliceToken}`, "Content-Type": "application/json" }
    const list = (await (await fetch(`${base()}/api/v1/groups`, { headers })).json()) as Array<{ id: string; name: string; memberCount: number }>
    expect(list).toHaveLength(1)
    expect(list[0]).toMatchObject({ id: groupId, name: "产品评审", memberCount: 2 })
    const carolToken = await login("carol", "carol-pass-1")
    expect((await fetch(`${base()}/api/v1/groups/${groupId}`, { headers: { Authorization: `Bearer ${carolToken}` } })).status).toBe(404)
    const renamed = await fetch(`${base()}/api/v1/groups/${groupId}`, { method: "PATCH", headers, body: JSON.stringify({ name: "产品评审（二期）" }) })
    expect(renamed.status).toBe(200)
    const session = (await (await fetch(`${base()}/api/v1/sessions/${sessionId}`, { headers })).json()) as { name: string }
    expect(session.name).toBe("产品评审（二期）")
  })

  it("解散群：群目录整体删除，会话随之不可访问", async () => {
    const r = await wsCall(aliceToken, "group.dissolve", { id: groupId })
    expect(r.ok).toBe(true)
    expect(existsSync(join(home, "groups", groupId))).toBe(false)
    const get = await wsCall(bobToken, "session.get", { id: sessionId })
    expect((get.payload as { session: unknown }).session).toBeNull()
  })
})
