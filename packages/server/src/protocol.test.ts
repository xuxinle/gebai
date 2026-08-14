import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import type { LLMProvider, LLMChunk, ChatOptions } from "./core/llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"

class ProtocolFake implements LLMProvider {
  readonly id = "fake"
  calls = 0
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 1000 }
  }
  async *chat(_msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    if (this.calls === 1) {
      yield { type: "tool_call", toolCall: { id: "tc-1", name: "current_time", arguments: {} } }
      yield { type: "done" }
      return
    }
    yield { type: "text", text: "已获取时间" }
    yield { type: "done" }
  }
}

const homeSingle = mkdtempSync(join(tmpdir(), "gebai-proto-"))
const homeMulti = mkdtempSync(join(tmpdir(), "gebai-proto-multi-"))
let single: ServerHandle
let multi: ServerHandle
let approval: ServerHandle

beforeAll(async () => {
  single = await startServer({ gebaiHome: homeSingle, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
  ;(single.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new ProtocolFake()
  multi = await startServer({ gebaiHome: homeMulti, auth: "server", sandbox: "on", binaryMode: false, preloadSubAgents: [], port: 0 })
  ;(multi.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new ProtocolFake()
  approval = await startServer({ gebaiHome: mkdtempSync(join(tmpdir(), "gebai-proto-approval-")), auth: "server", sandbox: "on", binaryMode: false, preloadSubAgents: [], port: 0, signupMode: "approval" })
  ;(approval.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new ProtocolFake()
  // 预置管理员（admin 引导走 GEBAI_ADMIN_PASSWORD_HASH，测试直接建用户）
  await multi.auth.createUser("admin", "admin123", "admin")
  await approval.auth.createUser("admin", "admin123", "admin")
})

afterAll(() => {
  single.gc?.stop()
  multi.gc?.stop()
  approval?.gc?.stop()
  single.server.stop(true)
  multi.server.stop(true)
  approval?.server.stop(true)
  rmSync(homeSingle, { recursive: true, force: true })
  rmSync(homeMulti, { recursive: true, force: true })
})

function base(h: ServerHandle) {
  return `http://127.0.0.1:${h.server.port}`
}

/** WS 请求-应答助手：返回 { ok, payload, error }。 */
function wsCall(h: ServerHandle, type: string, payload?: Record<string, unknown>) {
  return new Promise<{ ok: boolean; payload: Record<string, unknown> | undefined; error?: string }>((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${h.server.port}/ws`)
    ws.onopen = () => {
      ws.send(JSON.stringify({ type, payload, id: "t1" }))
    }
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean; payload?: Record<string, unknown>; error?: string }
      if (data.id === "t1") {
        ws.close()
        resolve({ ok: data.ok === true, payload: data.payload, error: data.error })
      }
    }
    ws.onerror = () => reject(new Error("ws error"))
  })
}

async function waitFor(cond: () => boolean, timeout = 3000): Promise<void> {
  const start = Date.now()
  while (!cond()) {
    if (Date.now() - start > timeout) throw new Error("timeout")
    await new Promise((r) => setTimeout(r, 20))
  }
}

describe("WS heartbeat", () => {
  test("ping returns pong roundtrip", async () => {
    const r = await wsCall(single, "ping")
    expect(r.ok).toBe(true)
    expect((r.payload as { pong?: boolean }).pong).toBe(true)
  })
})

describe("WS state model (MVC 一致性)", () => {
  test("state.snapshot returns model baseline (current/sessions/running/lastSeq)", async () => {
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    const r = await wsCall(single, "state.snapshot", {})
    expect(r.ok).toBe(true)
    const snap = r.payload as { currentSessionId: string | null; sessions: Array<{ id: string }>; running: string[]; lastSeq: number }
    expect(Array.isArray(snap.sessions)).toBe(true)
    expect(snap.sessions.some((s) => s.id === created.id)).toBe(true)
    expect(Array.isArray(snap.running)).toBe(true)
    expect(typeof snap.lastSeq).toBe("number")
  })

  test("session.switch persists per-user current session across connections (重连恢复)", async () => {
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    // 连接 A 切换当前会话
    const sw = await wsCall(single, "session.switch", { id: created.id })
    expect(sw.ok).toBe(true)
    // 新连接（模拟断线重连）：当前会话自动恢复，无需重新 switch
    const cur = await wsCall(single, "session.current", {})
    expect(cur.ok).toBe(true)
    expect((cur.payload?.session as { id: string }).id).toBe(created.id)
    // 快照中的 currentSessionId 同步恢复
    const snap = await wsCall(single, "state.snapshot", {})
    expect((snap.payload as { currentSessionId: string }).currentSessionId).toBe(created.id)
    // 删除当前会话：持久化状态清空
    const del = await wsCall(single, "session.delete", { id: created.id })
    expect(del.ok).toBe(true)
    const cur2 = await wsCall(single, "session.current", {})
    expect(cur2.ok).toBe(true)
    expect(cur2.payload?.session).toBeNull()
  })

  test("sync.request replays journal events missed while offline (跨连接)", async () => {
    ;(single.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new ProtocolFake()
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    // 连接 A 发起任务并等事件
    const ws = new WebSocket(`ws://127.0.0.1:${single.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws error"))
    })
    let done = false
    const got: string[] = []
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { type: string; seq?: number }
      if (data.type === "event.task.done") done = true
      else if (data.type.startsWith("event.")) got.push(data.type)
    }
    ws.send(JSON.stringify({ type: "session.prompt", payload: { id: created.id, prompt: "what time" }, id: "p1" }))
    await waitFor(() => done)
    ws.close()
    // 连接 B（模拟断线后重连）：sync.request 重放离线事件（seq 连续、payload 带 sessionId）
    const r = await wsCall(single, "sync.request", { lastSeq: 0 })
    expect(r.ok).toBe(true)
    const payload = r.payload as { events: Array<{ seq: number; type: string; sessionId: string; payload: { sessionId: string } }>; overrun: boolean; lastSeq: number }
    expect(payload.overrun).toBe(false)
    const seqs = payload.events.map((e) => e.seq)
    expect(seqs).toEqual([...seqs].sort((a, b) => a - b)) // seq 严格递增
    expect(payload.events.some((e) => e.type === "event.task.done")).toBe(true)
    for (const e of payload.events) expect(e.payload.sessionId).toBe(created.id)
    expect(payload.lastSeq).toBe(Math.max(...seqs))
    // 增量重放：从 lastSeq 之后无遗漏
    const r2 = await wsCall(single, "sync.request", { lastSeq: payload.lastSeq })
    expect((r2.payload as { events: unknown[] }).events).toEqual([])
  })

  test("state.snapshot pushed automatically on connection open (auth=none)", async () => {
    const ws = new WebSocket(`ws://127.0.0.1:${single.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws error"))
    })
    const snap = await new Promise<Record<string, unknown>>((resolve, reject) => {
      const t = setTimeout(() => reject(new Error("snapshot not pushed")), 2000)
      ws.onmessage = (m) => {
        const data = JSON.parse(String(m.data)) as { type: string; payload?: Record<string, unknown> }
        if (data.type === "state.snapshot") {
          clearTimeout(t)
          resolve(data.payload ?? {})
        }
      }
    })
    expect(Array.isArray(snap.sessions)).toBe(true)
    expect(typeof snap.lastSeq).toBe("number")
    ws.close()
  })
})

describe("REST protocol additions", () => {
  test("GET /api/docs returns OpenAPI document", async () => {
    const res = await fetch(`${base(single)}/api/docs`)
    expect(res.status).toBe(200)
    const spec = (await res.json()) as { openapi: string; info: { title: string }; paths: Record<string, unknown> }
    expect(spec.openapi).toBe("3.0.3")
    expect(spec.paths["/api/v1/sessions/{id}/prompt"]).toBeDefined()
  })

  test("GET /api/v1/env/catalog returns grouped env whitelist (global + agents, no special vars)", async () => {
    const res = await fetch(`${base(single)}/api/v1/env/catalog`)
    expect(res.status).toBe(200)
    const body = (await res.json()) as { groups: Array<{ group: string; vars: Array<{ name: string; description: string }> }> }
    const global = body.groups.find((g) => g.group === "global")
    expect(global).toBeDefined()
    expect(global!.vars.some((v) => v.name === "GEBAI_LLM_MODEL")).toBe(true) // 模型配置可配置
    expect(global!.vars.every((v) => v.description.length > 0)).toBe(true) // tip 说明非空
    expect(body.groups.some((g) => g.group === "code")).toBe(true) // 子Agent 分组
    const all = body.groups.flatMap((g) => g.vars.map((v) => v.name))
    expect(all).not.toContain("GEBAI_ADMIN_PASSWORD_HASH") // 特殊变量不可配置
  })

  test("GET /api/v1/env/catalog filters GEBAI_APPROVAL_SKIP by role (非管理员不可配置)", async () => {
    // 本地模式默认用户即 admin：可见
    const adminRes = await fetch(`${base(single)}/api/v1/env/catalog`)
    const adminBody = (await adminRes.json()) as { groups: Array<{ group: string; vars: Array<{ name: string }> }> }
    const adminGlobal = adminBody.groups.find((g) => g.group === "global")!
    expect(adminGlobal.vars.map((v) => v.name)).toContain("GEBAI_APPROVAL_SKIP")
    // 服务模式普通用户：不可见（该键非管理员配置后 prompt 注入会被拒，目录不应诱导配置）
    await multi.auth.createUser("envcat", "pw")
    const login = (await (
      await fetch(`${base(multi)}/api/v1/auth/login`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: "envcat", password: "pw" }),
      })
    ).json()) as { token: string }
    const userRes = await fetch(`${base(multi)}/api/v1/env/catalog`, { headers: { Authorization: `Bearer ${login.token}` } })
    expect(userRes.status).toBe(200)
    const userBody = (await userRes.json()) as { groups: Array<{ group: string; vars: Array<{ name: string }> }> }
    const userGlobal = userBody.groups.find((g) => g.group === "global")!
    expect(userGlobal.vars.map((v) => v.name)).not.toContain("GEBAI_APPROVAL_SKIP")
    expect(userBody.groups.some((g) => g.group === "feishu_docs")).toBe(true) // 子Agent 组不受影响
  })

  test("feedback POST uses sharded storage and GET returns it", async () => {
    const s = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    const post = await fetch(`${base(single)}/api/v1/feedback`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ messageId: "m1", sessionId: s.id, type: "thumbs_up" }),
    })
    expect(post.status).toBe(200)
    const list = (await (await fetch(`${base(single)}/api/v1/feedback`)).json()) as Array<{ messageId: string; userId: string }>
    expect(list.some((f) => f.messageId === "m1" && f.userId === "admin")).toBe(true)
    // 分片存储：反馈文件位于 users/admin/feedback/YYYY-MM-DD/{h0}/{h1}/
    const { readdirSync, existsSync } = await import("node:fs")
    const dayDir = join(homeSingle, "users", "admin", "feedback", new Date().toISOString().slice(0, 10))
    expect(existsSync(dayDir)).toBe(true)
    const shards = readdirSync(dayDir)
    expect(shards.length).toBeGreaterThan(0)
  })

  test("POST /sessions/:id/files/download returns a valid zip of selected files", async () => {
    const s = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    // 上传一个附件
    const form = new FormData()
    form.append("file", new Blob(["zip me please"], { type: "text/plain" }), "hello.txt")
    await fetch(`${base(single)}/api/v1/sessions/${s.id}/attachments`, { method: "POST", body: form })
    const res = await fetch(`${base(single)}/api/v1/sessions/${s.id}/files/download`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ paths: ["tmp/hello.txt"] }),
    })
    expect(res.status).toBe(200)
    expect(res.headers.get("Content-Type")).toContain("application/zip")
    const buf = new Uint8Array(await res.arrayBuffer())
    // ZIP 签名 PK\x03\x04
    expect(buf[0]).toBe(0x50)
    expect(buf[1]).toBe(0x4b)
    expect(buf[2]).toBe(0x03)
    expect(buf[3]).toBe(0x04)
  })

  test("webhooks CRUD via REST", async () => {
    const post = await fetch(`${base(single)}/api/v1/webhooks`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url: "https://example.com/hook", secret: "s" }),
    })
    expect(post.status).toBe(201)
    const cfg = (await post.json()) as { id: string; secret: string }
    expect(cfg.secret).toBe("***")
    const list = (await (await fetch(`${base(single)}/api/v1/webhooks`)).json()) as Array<{ id: string }>
    expect(list.some((w) => w.id === cfg.id)).toBe(true)
    const del = await fetch(`${base(single)}/api/v1/webhooks/${cfg.id}`, { method: "DELETE" })
    expect(del.status).toBe(200)
  })

  test("POST /sessions/:id/choice validates ownership and accepts decisions", async () => {
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    const res = await fetch(`${base(single)}/api/v1/sessions/${created.id}/choice`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ choiceId: "c1", option: "方案A" }),
    })
    expect(res.status).toBe(200)
    // 多选：options 数组（至少 1 项）
    const multi = await fetch(`${base(single)}/api/v1/sessions/${created.id}/choice`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ choiceId: "m1", options: ["A", "B"] }),
    })
    expect(multi.status).toBe(200)
    // 拒绝回答：refuse=true（无 option）
    const refuse = await fetch(`${base(single)}/api/v1/sessions/${created.id}/choice`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ choiceId: "c2", refuse: true }),
    })
    expect(refuse.status).toBe(200)
    // 既无 option 也无 refuse → 400；空 options 数组 → 400
    const bad = await fetch(`${base(single)}/api/v1/sessions/${created.id}/choice`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ choiceId: "c3" }),
    })
    expect(bad.status).toBe(400)
    const empty = await fetch(`${base(single)}/api/v1/sessions/${created.id}/choice`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ choiceId: "c4", options: [] }),
    })
    expect(empty.status).toBe(400)
    // 不存在的会话（合法 id 格式）→ 归属校验失败 404；非法 id（含穿越形态）→ 400 白名单拦截
    const notFound = await fetch(`${base(single)}/api/v1/sessions/00000000000000000000000000000000/choice`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ choiceId: "c1", option: "A" }),
    })
    expect(notFound.status).toBe(404)
    const traversal = await fetch(`${base(single)}/api/v1/sessions/nope/choice`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ choiceId: "c1", option: "A" }),
    })
    expect(traversal.status).toBe(400)
  })

  test("PUT /sessions/:id/env validates variable names and persists session env", async () => {
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    // 非法名（含 __proto__ 原型污染）→ 400
    for (const bad of [{ "bad name": "x" }, { "1abc": "x" }, { "a-b": "x" }, JSON.parse('{"__proto__": "x"}'), { A: 42 }]) {
      const res = await fetch(`${base(single)}/api/v1/sessions/${created.id}/env`, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(bad),
      })
      expect(res.status).toBe(400)
    }
    // 非对象 body → 400
    const notObj = await fetch(`${base(single)}/api/v1/sessions/${created.id}/env`, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: "null",
    })
    expect(notObj.status).toBe(400)
    // 合法设置 → 200，读取可见（来源 session）
    const ok = await fetch(`${base(single)}/api/v1/sessions/${created.id}/env`, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ TEST_FLAG: "1", TEST_SECRET: "abc123" }),
    })
    expect(ok.status).toBe(200)
    const envs = (await (await fetch(`${base(single)}/api/v1/sessions/${created.id}/env`)).json()) as Array<{ name: string; source: string; sensitive: boolean }>
    const flag = envs.find((e) => e.name === "TEST_FLAG")
    expect(flag?.source).toBe("session")
    const secret = envs.find((e) => e.name === "TEST_SECRET")
    expect(secret?.sensitive).toBe(true) // *_KEY 等敏感命名脱敏标记
  })

  test("POST /sessions/:id/prompt with env injects temporarily without persisting", async () => {
    // 浏览器本地环境变量随 prompt 请求临时注入（不写入会话 env 副本，服务端不持久化）
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    // 非法变量名 → 直接跳过不拒绝任务（宽容兼容：localStorage 残留旧版/目录外键不阻断正常使用）
    const bad = await fetch(`${base(single)}/api/v1/sessions/${created.id}/prompt`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ prompt: "hi", env: { "a-b": "x", GOOD_VAR: "1" } }),
    })
    expect(bad.status).toBe(200)
    await bad.text()
    const res = await fetch(`${base(single)}/api/v1/sessions/${created.id}/prompt`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ prompt: "hi", env: { TEMP_ONLY: "1" } }),
    })
    expect(res.status).toBe(200)
    await res.text() // 消费 SSE 流至任务完成
    // 未持久化：GET 会话 env 无 TEMP_ONLY
    const envs = (await (await fetch(`${base(single)}/api/v1/sessions/${created.id}/env`)).json()) as Array<{ name: string }>
    expect(envs.find((e) => e.name === "TEMP_ONLY")).toBeUndefined()
  })

  test("POST /sessions/:id/draw accepts frontend render results with ownership check", async () => {
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    const ok = await fetch(`${base(single)}/api/v1/sessions/${created.id}/draw`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ renderId: "r1", ok: true }),
    })
    expect(ok.status).toBe(200)
    const fail = await fetch(`${base(single)}/api/v1/sessions/${created.id}/draw`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ renderId: "r2", ok: false, error: "Syntax Error?" }),
    })
    expect(fail.status).toBe(200)
    // 不存在的会话（合法 id 格式）→ 归属校验失败 404；非法 id（含穿越形态）→ 400 白名单拦截
    const notFound = await fetch(`${base(single)}/api/v1/sessions/00000000000000000000000000000000/draw`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ renderId: "r1", ok: true }),
    })
    expect(notFound.status).toBe(404)
    const traversal = await fetch(`${base(single)}/api/v1/sessions/nope/draw`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ renderId: "r1", ok: true }),
    })
    expect(traversal.status).toBe(400)
  })

  test("POST /sessions/:id/compact actually compacts and returns counts", async () => {
    const s = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    // 通过引擎追加历史消息（FakeProvider 摘要降级/成功均可）
    for (let i = 0; i < 4; i++) {
      await single.store.appendMessage(s.id, { id: crypto.randomUUID(), role: "user", content: `q${i}`, createdAt: Date.now() })
      await single.store.appendMessage(s.id, { id: crypto.randomUUID(), role: "assistant", content: `a${i}`, createdAt: Date.now() })
    }
    const res = await fetch(`${base(single)}/api/v1/sessions/${s.id}/compact`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({}),
    })
    expect(res.status).toBe(200)
    const body = (await res.json()) as { compacted: number }
    expect(body.compacted).toBeGreaterThan(0)
  })
})

describe("WS protocol additions", () => {
  test("session.switch / session.current maintain per-connection current session", async () => {
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    // 同一连接内先 switch 再 current（连接级状态）
    const ws = new WebSocket(`ws://127.0.0.1:${single.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws error"))
    })
    const send = (type: string, payload?: Record<string, unknown>) =>
      new Promise<{ ok: boolean; payload?: Record<string, unknown>; error?: string }>((resolve) => {
        const id = crypto.randomUUID()
        ws.send(JSON.stringify({ type, payload, id }))
        const handler = (m: MessageEvent) => {
          const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean; payload?: Record<string, unknown>; error?: string }
          if (data.id === id) {
            ws.removeEventListener("message", handler)
            resolve({ ok: data.ok === true, payload: data.payload, error: data.error })
          }
        }
        ws.addEventListener("message", handler)
      })
    const sw = await send("session.switch", { id: created.id })
    expect(sw.ok).toBe(true)
    const cur = await send("session.current", {})
    expect(cur.ok).toBe(true)
    expect((cur.payload?.session as { id: string }).id).toBe(created.id)
    // 不存在的会话切换失败
    const bad = await send("session.switch", { id: "nope" })
    expect(bad.ok).toBe(false)
    // 切到别的会话后 current 跟随
    const created2 = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    await send("session.switch", { id: created2.id })
    const cur2 = await send("session.current", {})
    expect((cur2.payload?.session as { id: string }).id).toBe(created2.id)
    ws.close()
  })

  test("session.attachment.upload writes base64 payload to tmp/", async () => {
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    const b64 = Buffer.from("hello ws").toString("base64")
    const up = await wsCall(single, "session.attachment.upload", { id: created.id, name: "ws.txt", data: b64, mime: "text/plain" })
    expect(up.ok).toBe(true)
    expect((up.payload as { path: string }).path).toBe("tmp/ws.txt")
    const file = await Bun.file(join(single.store.getTmpDir(created.id, "admin"), "ws.txt")).text()
    expect(file).toBe("hello ws")
  })

  test("session.prompt via WS runs the task and pushes events", async () => {
    // 独立 provider 实例，避免其他测试消耗 calls 计数
    ;(single.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new ProtocolFake()
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    const ws = new WebSocket(`ws://127.0.0.1:${single.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws error"))
    })
    const events: string[] = []
    let promptReply: { ok: boolean; error?: string } | null = null
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { type: string; id?: string; ok?: boolean; error?: string }
      if (data.type === "session.prompt") promptReply = { ok: data.ok === true, error: data.error }
      else if (data.type.startsWith("event.")) events.push(data.type)
    }
    ws.send(JSON.stringify({ type: "session.prompt", payload: { id: created.id, prompt: "what time" }, id: "p1" }))
    await waitFor(() => promptReply !== null)
    expect(promptReply!.ok).toBe(true)
    await waitFor(() => events.includes("event.task.done"))
    expect(events).toContain("event.tool.call")
    expect(events).toContain("event.tool.result")
    ws.close()
  })

  test("session.compact via WS returns summary counts", async () => {
    const created = (await (await fetch(`${base(single)}/api/v1/sessions`, { method: "POST" })).json()) as { id: string }
    for (let i = 0; i < 4; i++) {
      await single.store.appendMessage(created.id, { id: crypto.randomUUID(), role: "user", content: `q${i}`, createdAt: Date.now() })
      await single.store.appendMessage(created.id, { id: crypto.randomUUID(), role: "assistant", content: `a${i}`, createdAt: Date.now() })
    }
    const r = await wsCall(single, "session.compact", { id: created.id })
    expect(r.ok).toBe(true)
    expect(Number(r.payload?.compacted)).toBeGreaterThan(0)
  })
})

describe("multi-user WS authorization", () => {
  function authedWs(): Promise<WebSocket> {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(`ws://127.0.0.1:${multi.server.port}/ws`)
      ws.onopen = () => {
        ws.send(JSON.stringify({ type: "auth.login", payload: { username: "admin", password: "admin123" }, id: "login" }))
      }
      ws.onmessage = (m) => {
        const data = JSON.parse(String(m.data)) as { type: string; id?: string; ok?: boolean }
        if (data.type === "auth.login" && data.ok) resolve(ws)
        else if (data.type === "auth.login") reject(new Error("login failed"))
      }
      ws.onerror = () => reject(new Error("ws error"))
    })
  }

  test("unauthenticated connection is rejected for all non-login messages", async () => {
    const r = await wsCall(multi, "session.list", {})
    expect(r.ok).toBe(false)
    expect(r.error).toContain("login required")
  })

  test("auth.login grants access; user.* admin APIs work after login", async () => {
    const ws = await authedWs()
    const result = await new Promise<{ ok: boolean; payload?: { users: Array<{ username: string }> }; error?: string }>((resolve, reject) => {
      ws.send(JSON.stringify({ type: "user.list", id: "u1" }))
      ws.onmessage = (m) => {
        const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean; payload?: { users: Array<{ username: string }> }; error?: string }
        if (data.id === "u1") resolve({ ok: data.ok === true, payload: data.payload, error: data.error })
      }
      ws.onerror = () => reject(new Error("ws error"))
    })
    expect(result.ok).toBe(true)
    expect(result.payload?.users.some((u) => u.username === "admin")).toBe(true)
    // session.list 可用
    const sl = await new Promise<{ ok: boolean }>((resolve) => {
      ws.send(JSON.stringify({ type: "session.list", id: "u2" }))
      ws.onmessage = (m) => {
        const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean }
        if (data.id === "u2") resolve({ ok: data.ok === true })
      }
    })
    expect(sl.ok).toBe(true)
    ws.close()
  })

  test("auth.logout resets connection to unauthenticated", async () => {
    const ws = await authedWs()
    const reply = await new Promise<{ ok: boolean }>((resolve) => {
      ws.send(JSON.stringify({ type: "auth.logout", id: "lo" }))
      ws.onmessage = (m) => {
        const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean }
        if (data.id === "lo") resolve({ ok: data.ok === true })
      }
    })
    expect(reply.ok).toBe(true)
    const after = await new Promise<{ ok: boolean }>((resolve) => {
      ws.send(JSON.stringify({ type: "session.list", id: "after" }))
      ws.onmessage = (m) => {
        const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean }
        if (data.id === "after") resolve({ ok: data.ok === true })
      }
    })
    expect(after.ok).toBe(false) // 登出后回到未登录态
    ws.close()
  })

  test("auth.login with existing token restores user context (重连自动重新认证)", async () => {
    // REST 登录拿令牌（模拟 SDK 已持有令牌的场景）
    const token = await new Promise<string>((resolve, reject) => {
      fetch(`${base(multi)}/api/v1/auth/login`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: "admin", password: "admin123" }),
      })
        .then((r) => r.json())
        .then((b) => resolve((b as { token: string }).token))
        .catch(reject)
    })
    expect(token).toBeTruthy()
    const ws = new WebSocket(`ws://127.0.0.1:${multi.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws error"))
    })
    // 令牌认证 → 成功并带快照；随后 session.list 可用（非 SERVICE_USER）
    const loginReply = await new Promise<{ ok: boolean; payload?: { snapshot?: { sessions: unknown[] } } }>((resolve) => {
      ws.onmessage = (m) => {
        const data = JSON.parse(String(m.data)) as { type: string; id?: string; ok?: boolean; payload?: { snapshot?: { sessions: unknown[] } } }
        if (data.type === "auth.login") resolve({ ok: data.ok === true, payload: data.payload })
      }
      ws.send(JSON.stringify({ type: "auth.login", payload: { token }, id: "t1" }))
    })
    expect(loginReply.ok).toBe(true)
    expect(Array.isArray(loginReply.payload?.snapshot?.sessions)).toBe(true)
    const sl = await new Promise<{ ok: boolean }>((resolve) => {
      ws.onmessage = (m) => {
        const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean }
        if (data.id === "u2") resolve({ ok: data.ok === true })
      }
      ws.send(JSON.stringify({ type: "session.list", id: "u2" }))
    })
    expect(sl.ok).toBe(true)
    ws.close()
  })

  test("auth.login 后紧接的请求不被并发竞态抢先（服务端按序处理消息）", async () => {
    // 回归：Bun 对 async websocket.message 处理器不保证串行——auth.login 处理器 await 磁盘
    // 读用户注册表时，紧随其后的请求会并发抢先执行（仍为未登录态）被拒
    // （unauthorized: login required）。认证与请求须按到达顺序处理。
    const token = await new Promise<string>((resolve, reject) => {
      fetch(`${base(multi)}/api/v1/auth/login`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: "admin", password: "admin123" }),
      })
        .then((r) => r.json())
        .then((b) => resolve((b as { token: string }).token))
        .catch(reject)
    })
    const ws = new WebSocket(`ws://127.0.0.1:${multi.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws error"))
    })
    const replies = new Map<string, { ok: boolean; error?: string }>()
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean; error?: string }
      if (data.id) replies.set(data.id, { ok: data.ok === true, error: data.error })
    }
    // 不等待认证应答，背靠背连发：认证后首个请求必须有序生效（无并发抢先）
    ws.send(JSON.stringify({ type: "auth.login", payload: { token }, id: "ra1" }))
    ws.send(JSON.stringify({ type: "session.list", id: "ra2" }))
    await waitFor(() => replies.has("ra1") && replies.has("ra2"))
    expect(replies.get("ra1")?.ok).toBe(true)
    expect(replies.get("ra2")?.ok).toBe(true)
    ws.close()
  })

  test("非管理员 prompt 注入 GEBAI_APPROVAL_SKIP 被过滤跳过，不拒绝任务（宽容兼容）", async () => {
    // 浏览器 env 注入通道对不支持变量宽容跳过：普通用户 localStorage 残留越权键
    // 不得阻断整个任务（显式管理通道 session.env.set 仍严格拒绝，见 core.test.ts）
    await multi.auth.createUser("envskip", "pw")
    const token = await new Promise<string>((resolve, reject) => {
      fetch(`${base(multi)}/api/v1/auth/login`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: "envskip", password: "pw" }),
      })
        .then((r) => r.json())
        .then((b) => resolve((b as { token: string }).token))
        .catch(reject)
    })
    const ws = new WebSocket(`ws://127.0.0.1:${multi.server.port}/ws`)
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve()
      ws.onerror = () => reject(new Error("ws error"))
    })
    const replies = new Map<string, { ok: boolean; error?: string }>()
    ws.onmessage = (m) => {
      const data = JSON.parse(String(m.data)) as { id?: string; ok?: boolean; error?: string }
      if (data.id) replies.set(data.id, { ok: data.ok === true, error: data.error })
    }
    ws.send(JSON.stringify({ type: "auth.login", payload: { token }, id: "rb1" }))
    await waitFor(() => replies.has("rb1"))
    expect(replies.get("rb1")?.ok).toBe(true)
    // 含越权键与非法名：任务照常接收（env 内这两个键被过滤，任务可能因 LLM 不可用失败，但绝不被注入校验拒绝）
    const created = (await (await fetch(`${base(multi)}/api/v1/sessions`, { method: "POST", headers: { Authorization: `Bearer ${token}` } })).json()) as { id: string }
    ws.send(
      JSON.stringify({
        type: "session.prompt",
        payload: { id: created.id, prompt: "hi", env: { GEBAI_APPROVAL_SKIP: "true", "a-b": "x", GOOD_VAR: "1" } },
        id: "rb2",
      }),
    )
    await waitFor(() => replies.has("rb2"))
    expect(replies.get("rb2")?.ok).toBe(true)
    expect(replies.get("rb2")?.error).toBeUndefined()
    ws.close()
  })
})

describe("multi-user REST authorization", () => {
  async function loginAs(username: string, password: string): Promise<string> {
    const res = await fetch(`${base(multi)}/api/v1/auth/login`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username, password }),
    })
    const body = (await res.json()) as { token?: string }
    return body.token ?? ""
  }
  async function loginAsAppr(username: string, password: string): Promise<string> {
    const res = await fetch(`http://127.0.0.1:${approval.server.port}/api/v1/auth/login`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username, password }),
    })
    const body = (await res.json()) as { token?: string }
    return body.token ?? ""
  }

  test("user management endpoints require admin (no privilege escalation)", async () => {
    await multi.auth.createUser("bob", "bob123")
    const bobToken = await loginAs("bob", "bob123")
    const bobAuth = { Authorization: `Bearer ${bobToken}` }
    // 普通用户：列表/创建/提权/删除一律 403
    expect((await fetch(`${base(multi)}/api/v1/users`, { headers: bobAuth })).status).toBe(403)
    expect(
      (
        await fetch(`${base(multi)}/api/v1/users`, {
          method: "POST",
          headers: { ...bobAuth, "Content-Type": "application/json" },
          body: JSON.stringify({ username: "eve", password: "x" }),
        })
      ).status,
    ).toBe(403)
    // 提权尝试：PATCH 自己 role=admin 被拒
    const me = (await (await fetch(`${base(multi)}/api/v1/auth/me`, { headers: bobAuth })).json()) as { id: string }
    expect(
      (
        await fetch(`${base(multi)}/api/v1/users/${me.id}`, {
          method: "PATCH",
          headers: { ...bobAuth, "Content-Type": "application/json" },
          body: JSON.stringify({ role: "admin" }),
        })
      ).status,
    ).toBe(403)
    expect(
      (await fetch(`${base(multi)}/api/v1/users/${me.id}`, { method: "DELETE", headers: bobAuth })).status,
    ).toBe(403)
    // 管理员正常可用
    const adminToken = await loginAs("admin", "admin123")
    const adminList = await fetch(`${base(multi)}/api/v1/users`, { headers: { Authorization: `Bearer ${adminToken}` } })
    expect(adminList.status).toBe(200)
    expect((await adminList.json()) as Array<{ username: string }>).toContainEqual(expect.objectContaining({ username: "bob" }))
  })

  test("multi-user REST authorization > static web UI not blocked by auth (page loads, API requires login)", async () => {
    // 服务模式未登录：静态页面/资源必须可加载（前端随后经 API 401 触发登录页），不得直接 401
    const page = await fetch(`${base(multi)}/`)
    expect(page.status).not.toBe(401)
    expect([200, 404]).toContain(page.status) // 200（web 已构建）或 404（无产物），均非鉴权拦截
    // API 未登录仍 401
    expect((await fetch(`${base(multi)}/api/v1/auth/me`)).status).toBe(401)
    expect((await fetch(`${base(multi)}/api/v1/sessions`)).status).toBe(401)
    // OpenAPI 文档公开
    expect((await fetch(`${base(multi)}/api/docs`)).status).toBe(200)
  })

  test("REST Basic auth: 单次请求凭据认证（等价隐式登录，不签发令牌）", async () => {
    const basic = (u: string, p: string) => `Basic ${Buffer.from(`${u}:${p}`).toString("base64")}`
    // admin 凭据直接访问管理接口（无需先 login 拿 token）
    const ok = await fetch(`${base(multi)}/api/v1/users`, { headers: { Authorization: basic("admin", "admin123") } })
    expect(ok.status).toBe(200)
    expect((await ok.json()) as Array<{ username: string }>).toContainEqual(expect.objectContaining({ username: "admin" }))
    // 普通用户凭据可访问本人资源（me），但管理接口 403
    const me = await fetch(`${base(multi)}/api/v1/auth/me`, { headers: { Authorization: basic("bob", "bob123") } })
    expect(me.status).toBe(200)
    expect(((await me.json()) as { username: string }).username).toBe("bob")
    expect((await fetch(`${base(multi)}/api/v1/users`, { headers: { Authorization: basic("bob", "bob123") } })).status).toBe(403)
    // 错误密码 / 非法格式 → 401（不泄露原因）
    expect((await fetch(`${base(multi)}/api/v1/users`, { headers: { Authorization: basic("admin", "wrong") } })).status).toBe(401)
    expect((await fetch(`${base(multi)}/api/v1/users`, { headers: { Authorization: "Basic !!!" } })).status).toBe(401)
    // scheme 大小写不敏感（RFC 7235）
    const lower = `basic ${Buffer.from("admin:admin123").toString("base64")}`
    expect((await fetch(`${base(multi)}/api/v1/users`, { headers: { Authorization: lower } })).status).toBe(200)
  })

  test("REST register: 服务模式开放注册（普通角色、注册即登录）；本地模式 404", async () => {
    const reg = async (u: string, p: string) =>
      fetch(`${base(multi)}/api/v1/auth/register`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: u, password: p }),
      })
    const ok = await reg("eve", "eve123")
    expect(ok.status).toBe(201)
    const body = (await ok.json()) as { token: string; user: { username: string; role: string } }
    expect(body.user.role).toBe("user") // 不可注册 admin
    expect(body.token).toBeTruthy()
    // 注册即登录：令牌可直接访问本人资源
    const me = await fetch(`${base(multi)}/api/v1/auth/me`, { headers: { Authorization: `Bearer ${body.token}` } })
    expect(((await me.json()) as { username: string }).username).toBe("eve")
    // 管理接口 403（普通角色）
    expect((await fetch(`${base(multi)}/api/v1/users`, { headers: { Authorization: `Bearer ${body.token}` } })).status).toBe(403)
    // 重名 400
    expect((await reg("eve", "x")).status).toBe(400)
    // 本地模式 404（注册仅服务模式）
    const localReg = await fetch(`${base(single)}/api/v1/auth/register`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: "x", password: "y" }),
    })
    expect(localReg.status).toBe(404)
  })

  test("REST register approval 模式：注册待审不可登录；admin 批准后可登录", async () => {
    const approvalBase = `http://127.0.0.1:${approval.server.port}`
    const reg = await fetch(`${approvalBase}/api/v1/auth/register`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: "applicant", password: "pw1" }),
    })
    expect(reg.status).toBe(201)
    const body = (await reg.json()) as { token?: string; pending: boolean; user: { username: string; role: string; disabled: boolean; pending?: boolean } }
    expect(body.pending).toBe(true)
    expect(body.token).toBeFalsy() // 不签发令牌（null）
    expect(body.user.disabled).toBe(true)
    expect(body.user.pending).toBe(true)
    // 待审不可登录
    const login = await fetch(`${approvalBase}/api/v1/auth/login`, {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ username: "applicant", password: "pw1" }),
    })
    expect(login.status).toBe(401)
    // admin 审批：列表可见待审 → 批准（启用+清除待审）→ 可登录
    const adminToken = await loginAsAppr("admin", "admin123")
    const list = (await (await fetch(`${approvalBase}/api/v1/users`, { headers: { Authorization: `Bearer ${adminToken}` } })).json()) as Array<{ id: string; username: string; pending?: boolean }>
    const appr = list.find((u) => u.username === "applicant")
    expect(appr?.pending).toBe(true)
    await fetch(`${approvalBase}/api/v1/users/${appr!.id}`, {
      method: "PATCH", headers: { "Content-Type": "application/json", Authorization: `Bearer ${adminToken}` },
      body: JSON.stringify({ disabled: false, pending: false }),
    })
    expect((await fetch(`${approvalBase}/api/v1/auth/login`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ username: "applicant", password: "pw1" }) })).status).toBe(200)
  })

  test("global tool enable/disable restricted to admin in multi-user mode", async () => {
    const bobToken = await loginAs("bob", "bob123")
    const denied = await fetch(`${base(multi)}/api/v1/tools`, {
      method: "PATCH",
      headers: { Authorization: `Bearer ${bobToken}`, "Content-Type": "application/json" },
      body: JSON.stringify({ name: "read", enabled: false }),
    })
    expect(denied.status).toBe(403)
    // 管理员可操作（恢复默认状态，避免影响其他用例）
    const adminToken = await loginAs("admin", "admin123")
    const ok = await fetch(`${base(multi)}/api/v1/tools`, {
      method: "PATCH",
      headers: { Authorization: `Bearer ${adminToken}`, "Content-Type": "application/json" },
      body: JSON.stringify({ name: "read", enabled: true }),
    })
    expect(ok.status).toBe(200)
  })

  test("GEBAI_APPROVAL_SKIP settable by admin only in multi-user mode", async () => {
    const bobToken = await loginAs("bob", "bob123")
    const bobAuth = { Authorization: `Bearer ${bobToken}`, "Content-Type": "application/json" }
    const session = (await (await fetch(`${base(multi)}/api/v1/sessions`, { method: "POST", headers: bobAuth })).json()) as { id: string }
    // 普通用户：设置审批跳过键 403（防绕过审批执行 sh/py/cron）
    const denied = await fetch(`${base(multi)}/api/v1/sessions/${session.id}/env`, {
      method: "PUT",
      headers: bobAuth,
      body: JSON.stringify({ GEBAI_APPROVAL_SKIP: "true" }),
    })
    expect(denied.status).toBe(403)
    // 普通用户：普通 env 不受影响
    const ok = await fetch(`${base(multi)}/api/v1/sessions/${session.id}/env`, {
      method: "PUT",
      headers: bobAuth,
      body: JSON.stringify({ CODE_PROJECT: "/x" }),
    })
    expect(ok.status).toBe(200)
    // 管理员可设置（自己会话）
    const adminToken = await loginAs("admin", "admin123")
    const adminSession = (await (await fetch(`${base(multi)}/api/v1/sessions`, { method: "POST", headers: { Authorization: `Bearer ${adminToken}` } })).json()) as { id: string }
    const adminOk = await fetch(`${base(multi)}/api/v1/sessions/${adminSession.id}/env`, {
      method: "PUT",
      headers: { Authorization: `Bearer ${adminToken}`, "Content-Type": "application/json" },
      body: JSON.stringify({ GEBAI_APPROVAL_SKIP: "true" }),
    })
    expect(adminOk.status).toBe(200)
  })
})
