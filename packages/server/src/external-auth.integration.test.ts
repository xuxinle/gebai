import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { createHmac } from "node:crypto"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"

const SECRET = "integration-secret"
const home = mkdtempSync(join(tmpdir(), "gebai-ext-http-"))
let handle: ServerHandle

beforeAll(async () => {
  handle = await startServer({
    gebaiHome: home,
    auth: "server",
    sandbox: "off",
    binaryMode: false,
    preloadSubAgents: [],
    port: 0,
    externalAuthSecret: SECRET,
    externalAuthAutocreate: true,
    externalAuthStorageKey: "myapp.auth",
  })
})

afterAll(() => {
  handle.gc?.stop()
  handle.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

function base() {
  return `http://127.0.0.1:${handle.server.port}`
}

function sign(username: string): string {
  const exp = Date.now() + 60_000
  const sig = createHmac("sha256", SECRET).update(`${username}.${exp}`).digest("hex")
  return `${exp}.${sig}`
}

describe("external auth endpoints", () => {
  test("external-config exposes enabled/storageKey/autocreate without secret", async () => {
    const res = await fetch(`${base()}/api/v1/auth/external-config`)
    expect(res.status).toBe(200)
    const body = (await res.json()) as Record<string, unknown>
    expect(body.enabled).toBe(true)
    expect(body.storageKey).toBe("myapp.auth")
    expect(body.autocreate).toBe(true)
    expect(JSON.stringify(body)).not.toContain(SECRET)
  })

  test("exchange with valid signature creates user and returns token", async () => {
    const res = await fetch(`${base()}/api/v1/auth/exchange`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: "ext-alice", credential: sign("ext-alice") }),
    })
    expect(res.status).toBe(200)
    const body = (await res.json()) as { token: string; user: { username: string; role: string } }
    expect(body.token).toBeTruthy()
    expect(body.user.username).toBe("ext-alice")
    expect(body.user.role).toBe("user")
    // 兑换的令牌可直接访问用户信息
    const me = await fetch(`${base()}/api/v1/auth/me`, { headers: { Authorization: `Bearer ${body.token}` } })
    expect(me.status).toBe(200)
    expect(((await me.json()) as { username: string }).username).toBe("ext-alice")
  })

  test("exchange rejects invalid signature / missing fields / when disabled", async () => {
    const bad = await fetch(`${base()}/api/v1/auth/exchange`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: "ext-alice", credential: "1.deadbeef" }),
    })
    expect(bad.status).toBe(401)
    const missing = await fetch(`${base()}/api/v1/auth/exchange`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: "ext-alice" }),
    })
    expect(missing.status).toBe(400)
  })

  test("autocreate=false rejects unknown external users", async () => {
    const h2 = await startServer({
      gebaiHome: home,
      auth: "server",
      sandbox: "off",
      binaryMode: false,
      preloadSubAgents: [],
      port: 0,
      externalAuthSecret: SECRET,
      externalAuthAutocreate: false,
    })
    try {
      const res = await fetch(`http://127.0.0.1:${h2.server.port}/api/v1/auth/exchange`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: "nobody", credential: sign("nobody") }),
      })
      expect(res.status).toBe(401)
    } finally {
      h2.gc?.stop()
      h2.server.stop(true)
    }
  })
})

describe("external auth disabled", () => {
  test("auth=none returns enabled:false and exchange 404 even with secret configured", async () => {
    const h2 = await startServer({
      gebaiHome: home,
      auth: "local",
      sandbox: "off",
      binaryMode: false,
      preloadSubAgents: [],
      port: 0,
      externalAuthSecret: SECRET,
    })
    try {
      const base2 = `http://127.0.0.1:${h2.server.port}`
      const cfg = await (await fetch(`${base2}/api/v1/auth/external-config`)).json() as { enabled: boolean }
      expect(cfg.enabled).toBe(false)
      const res = await fetch(`${base2}/api/v1/auth/exchange`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: "x", credential: "y" }),
      })
      expect(res.status).toBe(404)
    } finally {
      h2.gc?.stop()
      h2.server.stop(true)
    }
  })

  test("multi without provider: exchange 404, config disabled", async () => {
    const h2 = await startServer({
      gebaiHome: home,
      auth: "server",
      sandbox: "off",
      binaryMode: false,
      preloadSubAgents: [],
      port: 0,
    })
    try {
      const base2 = `http://127.0.0.1:${h2.server.port}`
      const cfg = await (await fetch(`${base2}/api/v1/auth/external-config`)).json() as { enabled: boolean }
      expect(cfg.enabled).toBe(false)
      const res = await fetch(`${base2}/api/v1/auth/exchange`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username: "x", credential: "y" }),
      })
      expect(res.status).toBe(404)
    } finally {
      h2.gc?.stop()
      h2.server.stop(true)
    }
  })
})
