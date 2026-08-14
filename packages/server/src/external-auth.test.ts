import { describe, expect, test } from "bun:test"
import { createHmac } from "node:crypto"
import { AuthService } from "./auth"
import { HmacExternalAuth, CallbackExternalAuth, createExternalAuthProvider, type FetchLike } from "./external-auth"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

const SECRET = "test-shared-secret"

function sign(username: string, exp: number): string {
  const sig = createHmac("sha256", SECRET).update(`${username}.${exp}`).digest("hex")
  return `${exp}.${sig}`
}

function cleanup(home: string) {
  rmSync(home, { recursive: true, force: true })
}

describe("HmacExternalAuth", () => {
  const p = new HmacExternalAuth(SECRET)

  test("valid signature within window passes", async () => {
    expect(await p.verify({ username: "alice", credential: sign("alice", Date.now() + 60_000) })).toBe("alice")
  })

  test("tampered signature / username mismatch rejected", async () => {
    expect(await p.verify({ username: "alice", credential: sign("bob", Date.now() + 60_000) })).toBeNull()
    const [exp, sig] = sign("alice", Date.now() + 60_000).split(".")
    expect(await p.verify({ username: "alice", credential: `${exp}.${sig.slice(0, -2)}xx` })).toBeNull()
  })

  test("expired / future-dated credential rejected (replay guard)", async () => {
    expect(await p.verify({ username: "alice", credential: sign("alice", Date.now() - 11 * 60_000) })).toBeNull()
    expect(await p.verify({ username: "alice", credential: sign("alice", Date.now() + 11 * 60_000) })).toBeNull()
  })

  test("malformed credential rejected", async () => {
    expect(await p.verify({ username: "alice", credential: "nope" })).toBeNull()
    expect(await p.verify({ username: "alice", credential: "abc." })).toBeNull()
  })
})

describe("CallbackExternalAuth", () => {
  test("http non-localhost URL rejected at construction (MITM guard)", () => {
    expect(() => new CallbackExternalAuth("http://cb.example/verify")).toThrow(/https/)
    // localhost / https 放行
    expect(() => new CallbackExternalAuth("http://127.0.0.1:9000/verify")).not.toThrow()
    expect(() => new CallbackExternalAuth("https://cb.example/verify")).not.toThrow()
  })

  test("2xx + ok:true passes; username must match request (override rejected)", async () => {
    const p = new CallbackExternalAuth("https://cb.example/verify", (async () => {
      return new Response(JSON.stringify({ ok: true, username: "mapped" }), { status: 200 })
    }) as FetchLike)
    // 回调返回与请求不一致的用户名：拒绝（防宽松回调被利用接管任意用户）
    expect(await p.verify({ username: "alice", credential: "tok" })).toBeNull()
    // 未返回 username（或规范化后一致）则按请求用户名通过
    const p2 = new CallbackExternalAuth("https://cb.example/verify", (async () => {
      return new Response(JSON.stringify({ ok: true }), { status: 200 })
    }) as FetchLike)
    expect(await p2.verify({ username: "alice", credential: "tok" })).toBe("alice")
    const p3 = new CallbackExternalAuth("https://cb.example/verify", (async () => {
      return new Response(JSON.stringify({ ok: true, username: " alice " }), { status: 200 })
    }) as FetchLike)
    expect(await p3.verify({ username: "alice", credential: "tok" })).toBe("alice")
  })

  test("non-2xx / ok:false / invalid body rejected", async () => {
    const p = new CallbackExternalAuth("https://cb.example/verify", (async () => {
      return new Response(JSON.stringify({ ok: false }), { status: 401 })
    }) as FetchLike)
    expect(await p.verify({ username: "alice", credential: "tok" })).toBeNull()

    const p2 = new CallbackExternalAuth("https://cb.example/verify", (async () => {
      return new Response(JSON.stringify({ nope: 1 }), { status: 200 })
    }) as FetchLike)
    expect(await p2.verify({ username: "alice", credential: "tok" })).toBeNull()
  })

  test("network failure / timeout rejected (null, no throw)", async () => {
    const p = new CallbackExternalAuth("https://cb.example/verify", (async () => {
      throw new Error("boom")
    }) as FetchLike)
    expect(await p.verify({ username: "alice", credential: "tok" })).toBeNull()
  })
})

describe("createExternalAuthProvider", () => {
  test("secret-only enables hmac; url-only enables callback", () => {
    expect(createExternalAuthProvider({ externalAuthSecret: SECRET } as never)?.kind).toBe("hmac")
    expect(createExternalAuthProvider({ externalAuthUrl: "https://cb.example/verify" } as never)?.kind).toBe("callback")
  })

  test("none configured returns null", () => {
    expect(createExternalAuthProvider({} as never)).toBeNull()
  })

  test("both configured throws (mutually exclusive)", () => {
    expect(() => createExternalAuthProvider({ externalAuthSecret: SECRET, externalAuthUrl: "https://cb.example/verify" } as never)).toThrow()
  })
})

describe("AuthService.exchangeExternal", () => {
  test("autocreate: external user is created as normal role and gets a token", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-ext-"))
    const auth = new AuthService(home, "server")
    const p = new HmacExternalAuth(SECRET)
    const token = await auth.exchangeExternal("alice", p, sign("alice", Date.now() + 60_000), true)
    expect(token).toBeTruthy()
    const user = await auth.authorize(token!)
    expect(user!.username).toBe("alice")
    expect(user!.role).toBe("user")
    // 自动创建的用户密码随机：不可密码登录
    expect(await auth.login("alice", "anything")).toBeNull()
    cleanup(home)
  })

  test("autocreate=false: unknown user rejected, existing user passes", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-ext2-"))
    const auth = new AuthService(home, "server")
    const p = new HmacExternalAuth(SECRET)
    await auth.createUser("precreated", "pw", "user")
    expect(await auth.exchangeExternal("nobody", p, sign("nobody", Date.now() + 60_000), false)).toBeNull()
    const token = await auth.exchangeExternal("precreated", p, sign("precreated", Date.now() + 60_000), false)
    expect(token).toBeTruthy()
    cleanup(home)
  })

  test("invalid signature / disabled user / unsafe username rejected", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-ext3-"))
    const auth = new AuthService(home, "server")
    const p = new HmacExternalAuth(SECRET)
    expect(await auth.exchangeExternal("alice", p, "bad.credential", true)).toBeNull()
    const u = await auth.createUser("bob", "pw")
    await auth.updateUser(u.id, { disabled: true })
    expect(await auth.exchangeExternal("bob", p, sign("bob", Date.now() + 60_000), true)).toBeNull()
    // 路径分隔符用户名拒绝（用户目录安全）
    expect(await auth.exchangeExternal("../../evil", p, sign("../../evil", Date.now() + 60_000), true)).toBeNull()
    cleanup(home)
  })

  test("repeated exchange failures lock the username briefly (callback abuse guard)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-ext-lock-"))
    const auth = new AuthService(home, "server")
    const p = new HmacExternalAuth(SECRET)
    // 连续失败 5 次后，即使签名正确也拒绝（锁定期内，与登录限流一致）
    for (let i = 0; i < 5; i++) {
      expect(await auth.exchangeExternal("mallory", p, "bad.credential", true)).toBeNull()
    }
    expect(await auth.exchangeExternal("mallory", p, sign("mallory", Date.now() + 60_000), true)).toBeNull()
    // 其他用户名不受影响
    expect(await auth.exchangeExternal("ok-user", p, sign("ok-user", Date.now() + 60_000), true)).toBeTruthy()
    cleanup(home)
  })
})
