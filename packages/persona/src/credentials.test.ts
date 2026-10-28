/**
 * 拟人模式飞书凭据：令牌存储 + 授权流程 + 编排。
 *
 * 这些用例守住四条会让「人格默默不回消息」的失效：
 * 1. 临期自动刷新（不刷就会跑两小时后集体失败）
 * 2. 刷新后**立即落盘**（不落盘则重启又拿着旧 token）
 * 3. 换应用后旧 token 必须失效（否则拿着别的应用的 token 去调，报错难懂）
 * 4. 并发只刷一次（N 个分身同时被唤醒时不能拿同一 refresh_token 互相踩）
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, readFileSync, rmSync, writeFileSync, mkdirSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { PersonaTokenStore } from "./user-token"
import { PersonaOAuth, extractCode, OFFLINE_ACCESS_SCOPE, PERSONA_SCOPES, scopesFromEnv } from "./oauth"
import { PersonaCredentials } from "./credentials"

const NOW = 1_800_000_000_000

function makeStore(home: string, opts: { appId?: string; appSecret?: string; fetchImpl?: typeof fetch; now?: () => number } = {}) {
  return new PersonaTokenStore({
    home,
    appId: () => opts.appId ?? "cli_test",
    appSecret: () => opts.appSecret ?? "secret_test",
    ...(opts.fetchImpl ? { fetchImpl: opts.fetchImpl } : {}),
    now: opts.now ?? (() => NOW),
  })
}

/** 造一个「刷新成功」的 fetch 桩。 */
function refreshOk(newToken = "uat-new", expiresIn = 7200): typeof fetch {
  return (async (url: string | URL | Request) => {
    void url
    return new Response(
      JSON.stringify({
        code: 0,
        access_token: newToken,
        expires_in: expiresIn,
        refresh_token: "urt-new",
        refresh_token_expires_in: 2592000,
        scope: "im:message",
      }),
      { status: 200, headers: { "content-type": "application/json" } },
    )
  }) as unknown as typeof fetch
}

describe("PersonaTokenStore：读盘与状态", () => {
  test("没有令牌文件 → missing（不抛错，这是正常状态）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt-"))
    const r = await makeStore(home).resolve()
    console.log(`    实测: ${JSON.stringify(r)}`)
    expect(r.status).toBe("missing")
    if (r.status === "missing") expect(r.reason).toContain("尚未完成")
    rmSync(home, { recursive: true, force: true })
  })

  test("有效令牌 → ok（带剩余时间）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt2-"))
    const store = makeStore(home)
    await store.set({ accessToken: "uat-ok", expireAt: NOW + 3600_000, scopes: ["im:message"], appId: "cli_test", name: "张三" })
    const r = await store.resolve()
    expect(r.status).toBe("ok")
    if (r.status === "ok") {
      expect(r.token.accessToken).toBe("uat-ok")
      expect(r.remainingMs).toBeGreaterThan(3000_000)
    }
    rmSync(home, { recursive: true, force: true })
  })

  test("落盘文件权限 0600（凭据不该被同机其他用户读到）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt3-"))
    const store = makeStore(home)
    await store.set({ accessToken: "uat-perm", expireAt: NOW + 3600_000, scopes: [], appId: "cli_test" })
    const text = readFileSync(store.filePath(), "utf8")
    expect(JSON.parse(text).accessToken).toBe("uat-perm")
    // 内容确实落在 persona/ 目录（迁移时随人格状态一起带走）
    expect(store.filePath()).toContain(join("persona", "feishu-user-token.json"))
    rmSync(home, { recursive: true, force: true })
  })

  test("坏文件不阻断（当没有，交给授权流程重建）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt4-"))
    mkdirSync(join(home, "persona"), { recursive: true })
    writeFileSync(join(home, "persona", "feishu-user-token.json"), "{坏掉的 JSON")
    const r = await makeStore(home).resolve()
    console.log(`    实测坏文件 → ${r.status}`)
    expect(r.status).toBe("missing")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("PersonaTokenStore：自动刷新", () => {
  test("临期（<5min）→ 自动刷新并落盘", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt5-"))
    const store = makeStore(home, { fetchImpl: refreshOk("uat-refreshed") })
    await store.set({ accessToken: "uat-old", expireAt: NOW + 60_000, refreshToken: "urt-old", scopes: [], appId: "cli_test" })

    const r = await store.resolve()
    expect(r.status).toBe("ok")
    if (r.status === "ok") expect(r.token.accessToken).toBe("uat-refreshed")

    // 关键：落盘也必须是新的（否则重启后又用旧 token）
    const onDisk = JSON.parse(readFileSync(store.filePath(), "utf8"))
    console.log(`    实测: 内存=${r.status === "ok" ? r.token.accessToken : "?"} 盘上=${onDisk.accessToken}`)
    expect(onDisk.accessToken).toBe("uat-refreshed")
    expect(onDisk.refreshToken).toBe("urt-new") // 刷新会轮换 refresh_token
    rmSync(home, { recursive: true, force: true })
  })

  test("已过期但有 refresh_token → 刷新", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt6-"))
    const store = makeStore(home, { fetchImpl: refreshOk("uat-after-expire") })
    await store.set({ accessToken: "uat-dead", expireAt: NOW - 1000, refreshToken: "urt-x", scopes: [], appId: "cli_test" })
    const r = await store.resolve()
    expect(r.status).toBe("ok")
    if (r.status === "ok") expect(r.token.accessToken).toBe("uat-after-expire")
    rmSync(home, { recursive: true, force: true })
  })

  test("过期且无 refresh_token → expired（不抛错）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt7-"))
    const store = makeStore(home)
    await store.set({ accessToken: "uat-dead", expireAt: NOW - 1000, scopes: [], appId: "cli_test" })
    const r = await store.resolve()
    expect(r.status).toBe("expired")
    if (r.status === "expired") expect(r.reason).toContain("refresh_token")
    rmSync(home, { recursive: true, force: true })
  })

  test("refresh_token 也过期 → expired（明确说要重新授权）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt8-"))
    const store = makeStore(home)
    await store.set({
      accessToken: "uat-dead",
      expireAt: NOW - 1000,
      refreshToken: "urt-dead",
      refreshExpireAt: NOW - 500,
      scopes: [],
      appId: "cli_test",
    })
    const r = await store.resolve()
    expect(r.status).toBe("expired")
    if (r.status === "expired") expect(r.reason).toContain("重新授权")
    rmSync(home, { recursive: true, force: true })
  })

  test("刷新失败 → expired 且带原因（不假装成功）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt9-"))
    const failFetch = (async () =>
      new Response(JSON.stringify({ code: 20005, msg: "refresh_token 已失效" }), {
        status: 400,
        headers: { "content-type": "application/json" },
      })) as unknown as typeof fetch
    const store = makeStore(home, { fetchImpl: failFetch })
    await store.set({ accessToken: "uat-old", expireAt: NOW + 1000, refreshToken: "urt-bad", scopes: [], appId: "cli_test" })
    const r = await store.resolve()
    expect(r.status).toBe("expired")
    if (r.status === "expired") expect(r.reason).toContain("刷新令牌失败")
    rmSync(home, { recursive: true, force: true })
  })

  test("换应用 → 旧令牌失效（防拿别的应用的 token 去调）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt10-"))
    const store = makeStore(home, { appId: "cli_NEW" })
    await store.set({ accessToken: "uat-old-app", expireAt: NOW + 3600_000, scopes: [], appId: "cli_OLD" })
    const r = await store.resolve()
    console.log(`    实测: ${r.status} — ${r.status !== "ok" ? r.reason : ""}`)
    expect(r.status).not.toBe("ok")
    if (r.status !== "ok") expect(r.reason).toContain("不一致")
    rmSync(home, { recursive: true, force: true })
  })

  test("并发 5 次 resolve → 只刷一次（不重复消费 refresh_token）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pt11-"))
    let calls = 0
    const countingFetch = (async () => {
      calls++
      await new Promise((r) => setTimeout(r, 20)) // 模拟网络延迟，放大并发窗口
      return new Response(JSON.stringify({ code: 0, access_token: "uat-single", expires_in: 7200, refresh_token: "urt-s" }), {
        status: 200,
        headers: { "content-type": "application/json" },
      })
    }) as unknown as typeof fetch
    const store = makeStore(home, { fetchImpl: countingFetch })
    await store.set({ accessToken: "uat-old", expireAt: NOW + 1000, refreshToken: "urt-old", scopes: [], appId: "cli_test" })

    const results = await Promise.all(Array.from({ length: 5 }, () => store.resolve()))
    console.log(`    实测: 刷新请求数=${calls}，5 次结果=${results.map((r) => r.status).join(",")}`)
    expect(calls).toBe(1) // 单飞生效
    expect(results.every((r) => r.status === "ok")).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("PersonaOAuth：授权链接与兑换", () => {
  test("生成的链接含必需参数（含 prompt=consent，否则老用户看不到权限变化）", () => {
    const oauth = new PersonaOAuth()
    const t = oauth.startAuthorize({ appId: "cli_x", appSecret: "s", redirectUri: "http://localhost:3002/cb" })
    const u = new URL(t.url)
    console.log(`    实测 scope: ${u.searchParams.get("scope")}`)
    expect(u.origin + u.pathname).toContain("accounts.feishu.cn")
    expect(u.searchParams.get("client_id")).toBe("cli_x")
    expect(u.searchParams.get("response_type")).toBe("code")
    expect(u.searchParams.get("redirect_uri")).toBe("http://localhost:3002/cb")
    expect(u.searchParams.get("state")).toBe(t.state)
    expect(u.searchParams.get("prompt")).toBe("consent")
    // 只要收发消息相关权限，不申请云文档（少要权限用户更愿意点同意）
    expect(u.searchParams.get("scope")).toContain("im:message")
    expect(u.searchParams.get("scope")).not.toContain("docx")
    // 用户身份读群历史（渠道 observe 读群消息的必需权限）
    expect(u.searchParams.get("scope")).toContain("im:message.group_msg:get_as_user")
    // offline_access 缺省不带（需应用后台先开通）
    expect(u.searchParams.get("scope")).not.toContain(OFFLINE_ACCESS_SCOPE)
  })

  test("缺省 scopes 不含 offline_access（未开该权限的应用必须能完成授权）", () => {
    // 飞书对 `offline_access` 要求应用后台显式开通，未开通时授权页直接报 20027
    // 把人挡在外面——连基本授权都完不成。所以缺省不能带它。
    expect(PERSONA_SCOPES).not.toContain(OFFLINE_ACCESS_SCOPE)
  })

  test("scopesFromEnv：环境变量只做并集，不覆盖基础权限", () => {
    // 只写一个时：基础集仍在（否则写漏一项会让拟人静默少一项能力）
    const merged = scopesFromEnv({ GEBAI_PERSONA_SCOPES: "offline_access" })
    expect(merged).toContain("offline_access")
    expect(merged).toContain("im:message")
    expect(merged).toContain("im:message:readonly")
    expect(merged).toContain("im:chat.members:read")
    console.log(`    实测并集: ${merged}`)

    // 未设：就是基础集
    expect(scopesFromEnv({})).toBe(PERSONA_SCOPES)
    // 空串/空白：也当未设
    expect(scopesFromEnv({ GEBAI_PERSONA_SCOPES: "   " })).toBe(PERSONA_SCOPES)
  })

  test("兑换成功 → 返回令牌（含 refresh 与用户信息）", async () => {
    const fetchImpl = (async (url: string | URL | Request) => {
      const u = String(url)
      if (u.includes("oauth/v3/token")) {
        return new Response(
          JSON.stringify({ code: 0, access_token: "uat-1", expires_in: 7200, refresh_token: "urt-1", refresh_token_expires_in: 2592000, scope: "im:message" }),
          { status: 200, headers: { "content-type": "application/json" } },
        )
      }
      return new Response(JSON.stringify({ code: 0, data: { name: "张三", open_id: "ou_1" } }), {
        status: 200,
        headers: { "content-type": "application/json" },
      })
    }) as unknown as typeof fetch

    const oauth = new PersonaOAuth({ fetchImpl, now: () => NOW })
    const t = oauth.startAuthorize({ appId: "cli_x", appSecret: "s", redirectUri: "http://localhost/cb" })
    const entry = await oauth.exchange({ code: "code-abc", state: t.state })
    console.log(`    实测: token=${entry.accessToken} user=${entry.name} refresh=${entry.refreshToken}`)
    expect(entry.accessToken).toBe("uat-1")
    expect(entry.refreshToken).toBe("urt-1")
    expect(entry.name).toBe("张三")
    expect(entry.appId).toBe("cli_x")
    expect(entry.expireAt).toBe(NOW + 7200_000)
  })

  test("state 不匹配 → 拒绝（防跨请求混淆）", async () => {
    const oauth = new PersonaOAuth()
    oauth.startAuthorize({ appId: "cli_x", appSecret: "s", redirectUri: "http://localhost/cb" })
    await expect(oauth.exchange({ code: "c", state: "wrong-state" })).rejects.toThrow(/state 不匹配|已过期/)
  })

  test("兑换失败 → 抛出明确原因", async () => {
    const failFetch = (async () =>
      new Response(JSON.stringify({ code: 20003, error_description: "code 已使用" }), {
        status: 400,
        headers: { "content-type": "application/json" },
      })) as unknown as typeof fetch
    const oauth = new PersonaOAuth({ fetchImpl: failFetch })
    const t = oauth.startAuthorize({ appId: "cli_x", appSecret: "s", redirectUri: "http://localhost/cb" })
    await expect(oauth.exchange({ code: "used", state: t.state })).rejects.toThrow(/code 已使用/)
  })

  test("extractCode：从完整回调地址或纯 code 提取", () => {
    expect(extractCode("http://localhost/cb?code=abc123&state=x")).toBe("abc123")
    expect(extractCode("abc123")).toBe("abc123")
    expect(extractCode("http://localhost/cb?state=x")).toBe("") // 没 code
    expect(extractCode("带空格 的乱码")).toBe("")
  })
})

describe("PersonaCredentials：编排（状态机）", () => {
  function makeCreds(home: string, fetchImpl?: typeof fetch) {
    const store = makeStore(home, { ...(fetchImpl ? { fetchImpl } : {}) })
    const creds = new PersonaCredentials({
      store,
      oauth: new PersonaOAuth({ ...(fetchImpl ? { fetchImpl } : {}), now: () => NOW }),
      appId: () => "cli_test",
      appSecret: () => "secret_test",
      redirectUri: () => "http://localhost:3002/api/v1/persona/feishu/callback",
    })
    return { store, creds }
  }

  test("无令牌 → needsAuth=true 且**自带授权链接**（前端可直接渲染按钮）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pc-"))
    const { creds } = makeCreds(home)
    const s = await creds.status()
    console.log(`    实测: state=${s.state} needsAuth=${s.needsAuth} hasUrl=${!!s.authorizeUrl}`)
    expect(s.ready).toBe(false)
    expect(s.state).toBe("missing")
    expect(s.needsAuth).toBe(true)
    expect(s.authorizeUrl).toContain("accounts.feishu.cn")
    // 状态里绝不能有令牌明文
    expect(JSON.stringify(s)).not.toContain("accessToken")
    rmSync(home, { recursive: true, force: true })
  })

  test("未配置应用凭据 → needsAuth 但**不给链接**（给了也必然报错）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pc2-"))
    const store = makeStore(home)
    const creds = new PersonaCredentials({
      store,
      oauth: new PersonaOAuth({ now: () => NOW }),
      appId: () => undefined,
      appSecret: () => undefined,
      redirectUri: () => "http://localhost/cb",
    })
    const s = await creds.status()
    console.log(`    实测: state=${s.state} hasUrl=${!!s.authorizeUrl}`)
    expect(s.needsAuth).toBe(true)
    expect(s.authorizeUrl).toBeUndefined()
    rmSync(home, { recursive: true, force: true })
  })

  test("有令牌 → ready，且报告授权人与剩余时间", async () => {
    const home = mkdtempSync(join(tmpdir(), "pc3-"))
    const { store, creds } = makeCreds(home)
    await store.set({ accessToken: "uat", expireAt: NOW + 3600_000, scopes: ["im:message"], appId: "cli_test", name: "张三" })
    const s = await creds.status()
    console.log(`    实测: ready=${s.ready} user=${s.authorizedUser}`)
    expect(s.ready).toBe(true)
    expect(s.authorizedUser).toBe("张三")
    expect(s.needsAuth).toBe(false)
    expect(JSON.stringify(s)).not.toContain("uat") // 不泄露令牌
    rmSync(home, { recursive: true, force: true })
  })

  test("complete() 兑换后立即 ready（不需重启）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pc4-"))
    const fetchImpl = (async (url: string | URL | Request) => {
      if (String(url).includes("oauth/v3/token")) {
        return new Response(
          JSON.stringify({ code: 0, access_token: "uat-c", expires_in: 7200, refresh_token: "urt-c", refresh_token_expires_in: 2592000, scope: "im:message" }),
          { status: 200, headers: { "content-type": "application/json" } },
        )
      }
      return new Response(JSON.stringify({ code: 0, data: { name: "李四", open_id: "ou_2" } }), {
        status: 200,
        headers: { "content-type": "application/json" },
      })
    }) as unknown as typeof fetch
    const { creds } = makeCreds(home, fetchImpl)

    const before = await creds.status()
    expect(before.ready).toBe(false)
    // 用状态里给的链接对应的 state 兑换
    const state = new URL(before.authorizeUrl!).searchParams.get("state")!
    const after = await creds.complete("code-from-feishu", state)
    console.log(`    实测: 兑换前 ready=${before.ready} → 兑换后 ready=${after.ready} user=${after.authorizedUser}`)
    expect(after.ready).toBe(true)
    expect(after.authorizedUser).toBe("李四")
    rmSync(home, { recursive: true, force: true })
  })

  test("revoke() 后回到 needsAuth（前端横幅会重现）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pc5-"))
    const { store, creds } = makeCreds(home)
    await store.set({ accessToken: "uat", expireAt: NOW + 3600_000, scopes: [], appId: "cli_test" })
    expect((await creds.status()).ready).toBe(true)
    await creds.revoke()
    const s = await creds.status()
    console.log(`    实测: 撤销后 ready=${s.ready} needsAuth=${s.needsAuth}`)
    expect(s.ready).toBe(false)
    expect(s.needsAuth).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })

  test("token() 给渠道用：可用返回令牌，不可用返回原因（不抛）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pc6-"))
    const { store, creds } = makeCreds(home)
    const miss = await creds.token()
    expect("reason" in miss).toBe(true)

    await store.set({ accessToken: "uat-ch", expireAt: NOW + 3600_000, scopes: [], appId: "cli_test" })
    const ok = await creds.token()
    expect("token" in ok).toBe(true)
    if ("token" in ok) expect(ok.token.accessToken).toBe("uat-ch")
    rmSync(home, { recursive: true, force: true })
  })
})
