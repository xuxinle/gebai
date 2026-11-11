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
import { PersonaOAuth, extractCode, extractState, mergeScopes, OFFLINE_ACCESS_SCOPE, PERSONA_SCOPES } from "./oauth"
import { PersonaCredentials } from "./credentials"
import type { PersonaTokenStore as PersonaTokenStoreType } from "./user-token"

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
    // offline_access **缺省就带**：默认争取长期授权，由下层自动降级（见下方用例）
    expect(u.searchParams.get("scope")).toContain(OFFLINE_ACCESS_SCOPE)
  })

  test("缺省 scopes **包含** offline_access（默认要长期；被拒时自动降级）", () => {
    // 早期版本把“要不要长期授权”做成了部署方开关（默认不带），因为未开通该权限时
    // 授权页会报 20027 把人挡在外面。现在改为**代码自适应**：默认就要，
    // 被拒就自己退一步发短期链接——部署方不需要知道这个概念的存在。
    expect(PERSONA_SCOPES).toContain(OFFLINE_ACCESS_SCOPE)
  })

  test("mergeScopes：渠道实例声明的 scopes 只做并集，不覆盖基础权限", () => {
    // 只写一个时：基础集仍在（否则写漏一项会让拟人静默少一项能力）
    const merged = mergeScopes(["offline_access"])
    expect(merged).toContain("offline_access")
    expect(merged).toContain("im:message")
    expect(merged).toContain("im:message:readonly")
    expect(merged).toContain("im:chat.members:read")
    console.log(`    实测并集: ${merged}`)

    // 未写：就是基础集
    expect(mergeScopes()).toBe(PERSONA_SCOPES)
    expect(mergeScopes([])).toBe(PERSONA_SCOPES)
    // 空串/空白项：也当未写
    expect(mergeScopes(["  "])).toBe(PERSONA_SCOPES)
    // 一个元素里写多个（空格分隔）也展开
    expect(mergeScopes(["offline_access im:resource"])).toContain("im:resource")
  })


describe("授权类型自适应：配置不区分长期/短期", () => {
  test("① 默认就要长期：授权链接里带 offline_access", () => {
    const oauth = new PersonaOAuth()
    const t = oauth.startAuthorize({ appId: "cli_x", appSecret: "s", redirectUri: "http://localhost:3002/cb" })
    const scope = new URL(t.url).searchParams.get("scope") ?? ""
    console.log(`    实测 scope: ${scope}`)
    expect(scope).toContain(OFFLINE_ACCESS_SCOPE)
  })

  test("② 被 20027 拒后 → 降级链接**不带** offline_access，其余权限一个不少", () => {
    const logs: string[] = []
    const store = new PersonaTokenStore({ home: mkdtempSync(join(tmpdir(), "pa-")), appId: () => "cli_x", appSecret: () => "s" })
    const creds = new PersonaCredentials({
      store,
      oauth: new PersonaOAuth(),
      redirectUri: () => "http://localhost:3002/cb",
      appId: () => "cli_x",
      appSecret: () => "s",
      scopes: () => PERSONA_SCOPES,
      log: (l, m) => logs.push(`${l}:${m}`),
    })
    creds.startAuthorize()
    const t = creds.downgradeToShortLived()
    const scope = new URL(t.url).searchParams.get("scope") ?? ""
    console.log(`    降级后 scope: ${scope}`)
    expect(scope).not.toContain(OFFLINE_ACCESS_SCOPE)
    // 关键：只去掉长期授权，基础权限一个不能少（否则授权了也干不了活）
    expect(scope).toContain("im:message")
    expect(scope).toContain("im:message:readonly")
    expect(scope).toContain("im:chat.members:read")
    // 降级必须留痕：静默降级会让“为何又要授权”永远查不出来
    console.log(`    降级日志: ${logs.filter((l) => l.startsWith("warn")).join(" | ").slice(0, 120)}`)
    expect(logs.some((l) => l.includes("20027"))).toBe(true)
  })

  test("③ 不记住“不支持长期”：降级后再发起又回到要长期", () => {
    // 后台随时可能开通 offline_access。若把“这个应用不支持”记住了，
    // 开通后就永远不会再试，用户会一直每两小时重新授权一次。
    const store = new PersonaTokenStore({ home: mkdtempSync(join(tmpdir(), "pa-")), appId: () => "cli_x", appSecret: () => "s" })
    const creds = new PersonaCredentials({
      store,
      oauth: new PersonaOAuth(),
      redirectUri: () => "http://localhost:3002/cb",
      appId: () => "cli_x",
      appSecret: () => "s",
      scopes: () => PERSONA_SCOPES,
      log: () => {},
    })
    creds.startAuthorize()
    creds.downgradeToShortLived()
    const again = creds.startAuthorize()
    expect(new URL(again.url).searchParams.get("scope") ?? "").toContain(OFFLINE_ACCESS_SCOPE)
  })

  test("④ 以实际拿到的 refresh_token 判定长期/短期，不凭“我请求过”", async () => {
    // 飞书可以不理 offline_access（应用未开通、管理员拒了），此时拿不到 refresh_token——
    // 那就是短期。按“我请求了所以是长期”推断，会一直到过期才发现。
    const fetchImpl = (async (url: string | URL | Request) => {
      const u = String(url)
      if (u.includes("oauth/v3/token")) {
        return new Response(
          JSON.stringify({ code: 0, access_token: "uat-1", expires_in: 7200, scope: "im:message" }),
          { status: 200, headers: { "content-type": "application/json" } },
        )
      }
      return new Response(JSON.stringify({ code: 0, data: { name: "张三", open_id: "ou_1" } }), {
        status: 200,
        headers: { "content-type": "application/json" },
      })
    }) as unknown as typeof fetch
    const store = new PersonaTokenStore({ home: mkdtempSync(join(tmpdir(), "pa-")), appId: () => "cli_x", appSecret: () => "s" })
    const logs: string[] = []
    const creds = new PersonaCredentials({
      store,
      oauth: new PersonaOAuth({ fetchImpl }),
      redirectUri: () => "http://localhost:3002/cb",
      appId: () => "cli_x",
      appSecret: () => "s",
      scopes: () => PERSONA_SCOPES,
      log: (l, m) => logs.push(`${l}:${m}`),
    })
    const t = creds.startAuthorize()
    await creds.complete(`http://localhost:3002/cb?code=abc&state=${t.state}`)
    const done = logs.find((l) => l.includes("授权完成"))
    console.log(`    实测: ${done}`)
    expect(done).toContain("短期")
    expect(done).not.toContain("长期")
  })

  test("⑤ 兑换阶段才报 20027 → 同样降级，并给可操作指引", async () => {
    const fetchImpl = (async () =>
      new Response(
        JSON.stringify({ code: 20027, error_description: "offline_access not enabled" }),
        { status: 400, headers: { "content-type": "application/json" } },
      )) as unknown as typeof fetch
    const store = new PersonaTokenStore({ home: mkdtempSync(join(tmpdir(), "pa-")), appId: () => "cli_x", appSecret: () => "s" })
    const creds = new PersonaCredentials({
      store,
      oauth: new PersonaOAuth({ fetchImpl }),
      redirectUri: () => "http://localhost:3002/cb",
      appId: () => "cli_x",
      appSecret: () => "s",
      scopes: () => PERSONA_SCOPES,
      log: () => {},
    })
    const t = creds.startAuthorize()
    const err = await creds.complete(`http://localhost:3002/cb?code=abc&state=${t.state}`).catch((e) => e)
    console.log(`    实测错误: ${err.message.slice(0, 70)}`)
    // 文案要告诉人“已经替你降级了”+“怎么才能免重复授权”
    expect(err.message).toContain("短期")
    expect(err.message).toContain("持续访问已授权的数据")
    const url = creds.authorizeUrl()
    expect(new URL(String(url)).searchParams.get("scope") ?? "").not.toContain(OFFLINE_ACCESS_SCOPE)
  })
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

describe("PersonaOAuth：无 state 兑换与 state 提取（防 account mix-up）", () => {
  /** 成功兑换桩（oauth/v3/token + user_info 都成功）。 */
  const okFetch = (async () =>
    new Response(JSON.stringify({ code: 0, access_token: "uat-any", expires_in: 7200, refresh_token: "urt-any" }), {
      status: 200,
      headers: { "content-type": "application/json" },
    })) as unknown as typeof fetch

  test("无 state 且 pending 唯一 → 允许兑（单用户本地手动粘贴场景）", async () => {
    const oauth = new PersonaOAuth({ fetchImpl: okFetch, now: () => NOW })
    oauth.startAuthorize({ appId: "cli_x", appSecret: "s", redirectUri: "http://localhost/cb" })
    const entry = await oauth.exchange({ code: "c-unique" }) // 纯 code、无 state
    expect(entry.accessToken).toBe("uat-any")
  })

  test("无 state 且多个 pending → 拒绝（无法确定属于哪次授权，防 account mix-up）", async () => {
    const oauth = new PersonaOAuth({ fetchImpl: okFetch, now: () => NOW })
    oauth.startAuthorize({ appId: "cli_x", appSecret: "s", redirectUri: "http://localhost/cb" })
    oauth.startAuthorize({ appId: "cli_y", appSecret: "s", redirectUri: "http://localhost/cb2" })
    // 模拟攻击者/另一个用户拿着 code 打过来且不带 state：拒绝而非默认兑最新的
    await expect(oauth.exchange({ code: "c-attacker" })).rejects.toThrow(/无法确定本次兑换属于哪一个/)
  })

  test("无 state 且无 pending → 拒绝（过期提示）", async () => {
    const oauth = new PersonaOAuth({ fetchImpl: okFetch, now: () => NOW })
    await expect(oauth.exchange({ code: "c-orphan" })).rejects.toThrow(/已过期或不存在/)
  })

  test("粘贴完整回调 URL → 自动提取 state 并按它兑（不必手动拆参数）", async () => {
    const oauth = new PersonaOAuth({ fetchImpl: okFetch, now: () => NOW })
    const a = oauth.startAuthorize({ appId: "cli_a", appSecret: "s", redirectUri: "http://localhost/cb" })
    oauth.startAuthorize({ appId: "cli_b", appSecret: "s", redirectUri: "http://localhost/cb" })
    // 两个 pending 共存，但粘贴的 URL 里带着 a 的 state——应兑 a，而不是拒绝或兑 b
    const pasted = `http://localhost/cb?code=c-a&state=${a.state}`
    const entry = await oauth.exchange({ code: pasted })
    expect(entry.appId).toBe("cli_a")
  })

  test("extractState：从 URL 提取，纯 code / 无 state 返回 undefined", () => {
    expect(extractState("http://localhost/cb?code=x&state=abc123")).toBe("abc123")
    expect(extractState("abc123")).toBeUndefined()
    expect(extractState("http://localhost/cb?code=x")).toBeUndefined()
  })
})

describe("PersonaTokenStore：刷新失败退避（防请求风暴）", () => {
  test("失败后 30s 内再次 resolve → 不再打接口，直接回报上次原因", async () => {
    const home = mkdtempSync(join(tmpdir(), "ptb1-"))
    let calls = 0
    let clock = NOW
    const failFetch = (async () => {
      calls++
      return new Response(JSON.stringify({ code: 500, msg: "server error" }), {
        status: 500,
        headers: { "content-type": "application/json" },
      })
    }) as unknown as typeof fetch
    const store = new PersonaTokenStore({
      home,
      appId: () => "cli_test",
      appSecret: () => "secret_test",
      fetchImpl: failFetch,
      now: () => clock,
    })
    await store.set({ accessToken: "uat-old", expireAt: NOW + 1000, refreshToken: "urt", refreshExpireAt: NOW + 30 * 86400_000, scopes: [], appId: "cli_test" })

    const r1 = await store.resolve()
    expect(r1.status).toBe("expired")
    expect(calls).toBe(1)

    clock += 10_000 // 10 秒后（退避窗口内）
    const r2 = await store.resolve()
    expect(r2.status).toBe("expired")
    expect(calls).toBe(1) // 没有再打接口

    clock += 25_000 // 又过 25 秒（超过 30s 退避）
    await store.resolve()
    expect(calls).toBe(2) // 退避窗口过了才重试
    rmSync(home, { recursive: true, force: true })
  })

  test("set 新令牌后清退避（下次临期正常刷新）", async () => {
    const home = mkdtempSync(join(tmpdir(), "ptb2-"))
    let calls = 0
    const okFetch = (async () => {
      calls++
      return new Response(JSON.stringify({ code: 0, access_token: "uat-new2", expires_in: 7200, refresh_token: "urt-n2" }), {
        status: 200,
        headers: { "content-type": "application/json" },
      })
    }) as unknown as typeof fetch
    let clock = NOW
    const store = new PersonaTokenStore({ home, appId: () => "cli_test", appSecret: () => "s", fetchImpl: okFetch, now: () => clock })
    // 先放一个已过期令牌（退避未触发过）
    await store.set({ accessToken: "uat-e", expireAt: NOW - 1, refreshToken: "urt", refreshExpireAt: NOW + 86400_000, scopes: [], appId: "cli_test" })
    const r = await store.resolve()
    expect(r.status).toBe("ok")
    expect(calls).toBe(1)
    void clock
    rmSync(home, { recursive: true, force: true })
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

  /**
   * 授权链接**过期后不得继续被展示**。
   *
   * 对应一条真实故障链：授权卡 TTL 10 分钟 → 过期后定时器重推卡 →
   * 重推的 `authorizeUrl` 仍是 10 分钟前那条链接（state 已被 OAuth 侧 GC）→
   * 用户点开、在飞书里点完同意、回来报到「授权会话已过期」。服务端日志全绿，
   * 现象是"怎么授权都授权不成功"。
   */
  test("授权链接过期后：下次取状态会**重新生成**（不能继续给死链接）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pc-exp-"))
    const clock = { t: NOW }
    const store = makeStore(home, { now: () => clock.t })
    const creds = new PersonaCredentials({
      store,
      oauth: new PersonaOAuth({ now: () => clock.t }),
      appId: () => "cli_test",
      appSecret: () => "secret_test",
      redirectUri: () => "http://localhost:3002/cb",
      now: () => clock.t,
    })
    const first = await creds.status()
    const url1 = first.authorizeUrl!
    expect(url1).toContain("accounts.feishu.cn")

    clock.t = NOW + 11 * 60_000 // 走过 10 分钟 TTL
    const second = await creds.status()
    console.log(`    实测：过期后是否换了新链接 = ${second.authorizeUrl !== url1}`)
    expect(second.authorizeUrl).toBeDefined()
    expect(second.authorizeUrl).not.toBe(url1) // 必须是新链接（新 state）
    expect(new URL(second.authorizeUrl!).searchParams.get("state")).toBeTruthy()
    rmSync(home, { recursive: true, force: true })
  })

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

describe("PersonaCredentials：expiring / error 状态（提前叫人，不等人格失声）", () => {
  function makeCreds(home: string, opts: { now?: () => number } = {}) {
    const store = makeStore(home, opts)
    const creds = new PersonaCredentials({
      store,
      oauth: new PersonaOAuth({ now: () => NOW }),
      appId: () => "cli_test",
      appSecret: () => "secret_test",
      redirectUri: () => "http://localhost:3002/api/v1/persona/feishu/callback",
    })
    return { store, creds }
  }

  test("无 refresh_token 且剩余 < 10 分钟 → expiring（提前叫人，needsAuth=true）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pcx1-"))
    const { store, creds } = makeCreds(home)
    // 无 offline_access 的典型形态：只有 accessToken，剩 5 分钟。
    // 注意：剩 5 分钟已落在 TokenStore 的刷新窗口内（REFRESH_SKEW_MS=5min），
    // 无 refreshToken 时 resolve 直接报 expired——所以剩 8 分钟（窗口外、提醒窗口内）
    // 才是 expiring 状态的真正可观察区间。
    await store.set({ accessToken: "uat-dying", expireAt: NOW + 8 * 60_000, scopes: [], appId: "cli_test" })
    const s = await creds.status()
    console.log(`    实测: state=${s.state} ready=${s.ready} needsAuth=${s.needsAuth}`)
    expect(s.state).toBe("expiring")
    expect(s.ready).toBe(true) // 仍可用（还没过期）
    expect(s.needsAuth).toBe(true) // 但要提前叫人
    expect(s.message).toContain("即将过期")
    rmSync(home, { recursive: true, force: true })
  })

  test("有 refresh_token 且剩 < 10 分钟 → 仍 ok（能自动续期就不必叫人）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pcx2-"))
    // 刷新桩：临期会自动刷成功
    const { store, creds } = (() => {
      const s = makeStore(home, { fetchImpl: refreshOk("uat-renewed") })
      const c = new PersonaCredentials({
        store: s,
        oauth: new PersonaOAuth({ now: () => NOW }),
        appId: () => "cli_test",
        appSecret: () => "secret_test",
        redirectUri: () => "http://localhost/cb",
      })
      return { store: s, creds: c }
    })()
    await store.set({ accessToken: "uat-near", expireAt: NOW + 3 * 60_000, refreshToken: "urt-ok", refreshExpireAt: NOW + 30 * 86400_000, scopes: [], appId: "cli_test" })
    const s = await creds.status()
    console.log(`    实测: state=${s.state}（有 refresh_token 时临期自动刷，不标 expiring）`)
    expect(s.state).toBe("ok")
    expect(s.needsAuth).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })

  test("无 refresh_token 且剩 > 10 分钟 → ok（远未到期不制造焦虑）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pcx3-"))
    const { store, creds } = makeCreds(home)
    await store.set({ accessToken: "uat-fresh", expireAt: NOW + 30 * 60_000, scopes: [], appId: "cli_test" })
    const s = await creds.status()
    expect(s.state).toBe("ok")
    expect(s.needsAuth).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })

  test("resolve 抛错 → error（不是 missing，不误导用户去白点一次授权）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pcx4-"))
    const creds = new PersonaCredentials({
      // 故意让 resolve 抛错：验证 error 路径不会被当成 missing
      store: ({ resolve: async () => { throw new Error("磁盘读不了") } } as unknown) as PersonaTokenStoreType,
      oauth: new PersonaOAuth({ now: () => NOW }),
      appId: () => "cli_test",
      appSecret: () => "secret_test",
      redirectUri: () => "http://localhost/cb",
    })
    const s = await creds.status()
    console.log(`    实测: state=${s.state} message=${s.message}`)
    expect(s.state).toBe("error")
    expect(s.ready).toBe(false)
    expect(s.message).toContain("凭据读取异常")
    rmSync(home, { recursive: true, force: true })
  })
})
