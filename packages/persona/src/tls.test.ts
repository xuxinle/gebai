/**
 * 拟人模式飞书请求的 TLS 策略回归测试。
 *
 * 锁住的事：**授权链必须走 feishuFetch，不能用原生 fetch**。
 *
 * 为什么值得专门测：这两者行为在正常网络下**完全一致**，只有企业内网出口改写
 * 证书链时才分叉。所以一旦有人把 `?? feishuFetch` 顺手写回 `?? fetch`，
 * 所有单测照旧全绿，故障只在部署环境出现——而现象是
 * 「授权页能开、扫码也成功、回调到了服务端才失败」，
 * 错误文案是 `unable to verify the first certificate`，极难归因到这一行。
 */
import { describe, expect, test, afterEach } from "bun:test"
import { feishuFetch, feishuTlsInsecure } from "./tls"
import { PersonaOAuth } from "./oauth"
import { PersonaTokenStore } from "./user-token"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"

const KEY = "GEBAI_FEISHU_INSECURE_TLS"

afterEach(() => {
  delete process.env[KEY]
})

describe("tls：开关语义与 @gebai/agents 同构", () => {
  test("默认关闭；true/1 开启；其余值关闭", () => {
    delete process.env[KEY]
    expect(feishuTlsInsecure()).toBe(false)
    process.env[KEY] = "true"
    expect(feishuTlsInsecure()).toBe(true)
    process.env[KEY] = "1"
    expect(feishuTlsInsecure()).toBe(true)
    for (const v of ["false", "0", "on", "yes", ""]) {
      process.env[KEY] = v
      expect(feishuTlsInsecure()).toBe(false)
    }
  })

  test("关闭时不改写 init（不该给所有请求塞一个没人要的 tls 字段）", async () => {
    delete process.env[KEY]
    const orig = globalThis.fetch
    let seen: unknown
    globalThis.fetch = (async (_u: unknown, init?: unknown) => {
      seen = init
      return new Response("ok")
    }) as typeof fetch
    try {
      await feishuFetch("https://open.feishu.cn/x")
      expect(seen).toBeUndefined()
    } finally {
      globalThis.fetch = orig
    }
  })

  test("开启时附加 tls.rejectUnauthorized=false，且保留原 init 字段", async () => {
    process.env[KEY] = "true"
    const orig = globalThis.fetch
    let seen: unknown
    globalThis.fetch = (async (_u: unknown, init?: unknown) => {
      seen = init
      return new Response("ok")
    }) as typeof fetch
    try {
      await feishuFetch("https://accounts.feishu.cn/oauth/v3/token", { method: "POST" })
      expect(seen).toEqual({ method: "POST", tls: { rejectUnauthorized: false } })
    } finally {
      globalThis.fetch = orig
    }
  })
})

describe("PersonaOAuth：授权链必须走 feishuFetch（而非原生 fetch）", () => {
  /**
   * 直接断言「默认注入的是谁」：不给 fetchImpl 时，实例内部拿到的必须是 feishuFetch。
   *
   * 这是本文件存在的主要理由。测不到的组合（老写法）会让开关在内网场景静默失效，
   * 而那正是这个开关唯一的用途。
   */
  test("缺省 fetchImpl === feishuFetch", () => {
    const oauth = new PersonaOAuth() as unknown as { fetchImpl: unknown }
    expect(oauth.fetchImpl).toBe(feishuFetch)
  })

  test("显式传 fetchImpl 时尊重注入（测试/替代实现不受影响）", () => {
    const custom = (async () => new Response("x")) as unknown as typeof fetch
    const oauth = new PersonaOAuth({ fetchImpl: custom }) as unknown as { fetchImpl: unknown }
    expect(oauth.fetchImpl).toBe(custom)
  })

  test("真实兑换路径：开关开启时请求带 tls.rejectUnauthorized=false", async () => {
    process.env[KEY] = "true"
    const calls: Array<{ url: string; init?: RequestInit }> = []
    const oauth = new PersonaOAuth({
      fetchImpl: (async (url: string, init?: RequestInit) => {
        // 走一遍 feishuFetch 的改造，验证它确实被用在兑换这一步
        return feishuFetch(url, init)
      }) as unknown as typeof fetch,
    })
    const orig = globalThis.fetch
    globalThis.fetch = (async (url: unknown, init?: unknown) => {
      calls.push({ url: String(url), init: init as RequestInit })
      // 兑换成功响应（避免走用户信息补全的分支报错）
      return new Response(JSON.stringify({ access_token: "at", expires_in: 7200, scope: ["im:message"] }), { status: 200 })
    }) as typeof fetch
    try {
      const ticket = oauth.startAuthorize({ appId: "cli_x", appSecret: "s", redirectUri: "http://localhost:3000/cb" })
      await oauth.exchange({ code: "the-code", state: ticket.state })
      expect(calls.length).toBeGreaterThan(0)
      const tokenCall = calls.find((c) => c.url.includes("/oauth/v3/token"))
      expect(tokenCall).toBeDefined()
      expect((tokenCall!.init as { tls?: unknown }).tls).toEqual({ rejectUnauthorized: false })
    } finally {
      globalThis.fetch = orig
    }
  })
})

describe("PersonaTokenStore：刷新令牌走同一条 TLS 路径", () => {
  test("缺省 fetchImpl === feishuFetch（刷新打的是同一个 accounts 域）", () => {
    const home = mkdtempSync(join(tmpdir(), "persona-tls-"))
    try {
      const store = new PersonaTokenStore({ home }) as unknown as { fetchImpl: unknown }
      expect(store.fetchImpl).toBe(feishuFetch)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
