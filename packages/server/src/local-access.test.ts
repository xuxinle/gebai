/**
 * 本地模式访问口令（GEBAI_ADMIN_PASSWORD_HASH 在本地模式下的语义）：
 * 未设置 = 原本的免登录形态；设置 = 该实例需凭证才可达，但**运行形态语义不变**（仍是本地模式）。
 *
 * 咬住的是「一道门改变的是可达性、不是身份模型」这件事——四个面：
 * ① 未设口令：REST 免登录直接以 admin 工作（回归保护，防改造把默认行为改坏）
 * ② 设了口令：未认证一律 401、错误口令 401、正确口令签发令牌后可访问
 * ③ 设了口令：WS 未认证连接不得下发任何业务消息（REST 拦住了 WS 还能直连就是白做）
 * ④ 设了口令：身份仍是本地 admin（role=admin、auth 仍为 local——沙箱豁免等语义未被动）
 */
import { afterAll, describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import { createHash } from "node:crypto"
import { scryptSync } from "node:crypto"

/** 与 AuthService 同口径的口令哈希（salt 16 字节、hash 64 字节，均为 hex）。 */
function makeHash(password: string): string {
  const salt = createHash("sha256").update(`salt:${password}`).digest("hex").slice(0, 32)
  const hash = (scryptSync(password, salt, 64) as Buffer).toString("hex")
  return `${salt}:${hash}`
}

const homes: string[] = []
const handles: ServerHandle[] = []

async function boot(auth: "local" | "server", adminPasswordHash?: string): Promise<ServerHandle> {
  const home = mkdtempSync(join(tmpdir(), "gebai-local-access-"))
  homes.push(home)
  const h = await startServer({ gebaiHome: home, auth, sandbox: auth === "local" ? "off" : "on", binaryMode: false, preloadSubAgents: [], port: 0, adminPasswordHash })
  handles.push(h)
  return h
}

function base(h: ServerHandle): string {
  return `http://127.0.0.1:${h.server.port}`
}

afterAll(() => {
  for (const h of handles) {
    h.gc?.stop()
    h.server.stop(true)
  }
  for (const home of homes) rmSync(home, { recursive: true, force: true })
})

describe("本地模式未设访问口令：保持免登录（回归保护）", () => {
  test("无凭证可读会话列表与当前用户，身份为 admin", async () => {
    const h = await boot("local")
    const me = (await (await fetch(`${base(h)}/api/v1/auth/me`)).json()) as { id: string; role: string }
    expect(me.id).toBe("admin")
    expect(me.role).toBe("admin")
    const list = await fetch(`${base(h)}/api/v1/sessions`)
    expect(list.status).toBe(200)
    // 运行形态仍是本地（沙箱豁免、restart_server 注入等语义的判定依据）
    expect(h.config.auth).toBe("local")
  })
})

describe("本地模式设访问口令：需要登录才能访问", () => {
  test("未认证请求 401，且不泄露任何会话数据", async () => {
    const h = await boot("local", makeHash("s3cret-pass"))
    expect(h.config.auth).toBe("local") // 形态未变
    for (const p of ["/api/v1/sessions", "/api/v1/auth/me", "/api/v1/tasks", "/api/v1/users"]) {
      const res = await fetch(`${base(h)}${p}`)
      expect(res.status).toBe(401)
    }
    // 健康检查与登录端点必须放行（探活与首登无凭证）
    expect((await fetch(`${base(h)}/api/health`)).status).toBe(200)
    expect((await fetch(`${base(h)}/api/v1/auth/signup-config`)).status).toBe(200)
  })

  test("错误口令 401，正确口令签发令牌后可正常访问，身份仍是 admin", async () => {
    const h = await boot("local", makeHash("s3cret-pass"))
    const bad = await fetch(`${base(h)}/api/v1/auth/login`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ username: "admin", password: "wrong" }) })
    expect(bad.status).toBe(401)
    const ok = await fetch(`${base(h)}/api/v1/auth/login`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ username: "admin", password: "s3cret-pass" }) })
    expect(ok.status).toBe(200)
    const { token } = (await ok.json()) as { token: string }
    expect(token).toBeTruthy()
    const me = (await (await fetch(`${base(h)}/api/v1/auth/me`, { headers: { Authorization: `Bearer ${token}` } })).json()) as { id: string; role: string }
    expect(me.id).toBe("admin")
    expect(me.role).toBe("admin")
    expect((await fetch(`${base(h)}/api/v1/sessions`, { headers: { Authorization: `Bearer ${token}` } })).status).toBe(200)
  })

  test("signup-config 暴露 localAccess=true（前端据此改成单口令输入、无注册入口）", async () => {
    const h = await boot("local", makeHash("s3cret-pass"))
    const cfg = (await (await fetch(`${base(h)}/api/v1/auth/signup-config`)).json()) as { mode: string | null; builtinAllowed: boolean; localAccess?: boolean }
    expect(cfg.localAccess).toBe(true)
    expect(cfg.builtinAllowed).toBe(false)
    expect(cfg.mode).toBeNull()
  })

  test("WS：未认证连接的发消息一律被拒（不能绕 REST 直连）", async () => {
    const h = await boot("local", makeHash("s3cret-pass"))
    const res = await new Promise<{ ok: boolean; error?: string }>((resolve, reject) => {
      const ws = new WebSocket(`ws://127.0.0.1:${h.server.port}/ws`)
      ws.onopen = () => ws.send(JSON.stringify({ type: "session.list", payload: {}, id: "w1" }))
      ws.onmessage = (m) => {
        const d = JSON.parse(String(m.data)) as { id?: string; ok?: boolean; error?: string }
        // 跳过建连快照类无关消息，只认自己那条应答
        if (d.id !== "w1") return
        ws.close()
        resolve({ ok: d.ok === true, error: d.error })
      }
      ws.onerror = () => reject(new Error("ws error"))
    })
    expect(res.ok).toBe(false)
    expect(String(res.error ?? "")).toContain("unauthorized")
  })

  test("WS：带令牌 auth.login 后即可正常调用", async () => {
    const h = await boot("local", makeHash("s3cret-pass"))
    const { token } = (await (await fetch(`${base(h)}/api/v1/auth/login`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ username: "admin", password: "s3cret-pass" }) })).json()) as { token: string }
    const res = await new Promise<{ ok: boolean; user?: { id: string } }>((resolve, reject) => {
      const ws = new WebSocket(`ws://127.0.0.1:${h.server.port}/ws`)
      ws.onopen = () => ws.send(JSON.stringify({ type: "auth.login", payload: { token }, id: "a1" }))
      ws.onmessage = (m) => {
        const d = JSON.parse(String(m.data)) as { id?: string; ok?: boolean; payload?: { user?: { id: string } } }
        if (d.id !== "a1") return
        ws.close()
        resolve({ ok: d.ok === true, user: d.payload?.user })
      }
      ws.onerror = () => reject(new Error("ws error"))
    })
    expect(res.ok).toBe(true)
    expect(res.user?.id).toBe("admin")
  })

  test("口令只存进程内存：不写注册表、不建用户目录", async () => {
    const h = await boot("local", makeHash("s3cret-pass"))
    const { existsSync } = await import("node:fs")
    expect(existsSync(join(h.config.gebaiHome, "users", "registry.json"))).toBe(false)
  })
})

describe("服务模式行为不变", () => {
  test("未设 admin 口令哈希时 admin 不可登录（未认证一律 401）", async () => {
    const h = await boot("server")
    expect((await fetch(`${base(h)}/api/v1/sessions`)).status).toBe(401)
    const res = await fetch(`${base(h)}/api/v1/auth/login`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ username: "admin", password: "admin123" }) })
    expect(res.status).toBe(401)
  })

  test("设了口令哈希时 admin 可登录（原有语义：启动参数是权威配置）", async () => {
    const h = await boot("server", makeHash("admin-strong"))
    const res = await fetch(`${base(h)}/api/v1/auth/login`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ username: "admin", password: "admin-strong" }) })
    expect(res.status).toBe(200)
    const { token } = (await res.json()) as { token: string }
    expect((await fetch(`${base(h)}/api/v1/sessions`, { headers: { Authorization: `Bearer ${token}` } })).status).toBe(200)
  })
})
