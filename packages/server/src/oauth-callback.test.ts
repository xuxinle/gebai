import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, readFileSync, existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createApp, SERVICE_USER, type AppDeps } from "./app"
import type { ServerConfig } from "./core/config"
import { sessionPath } from "./core/paths"
import { registerPendingAuth, getPendingAuth, USER_TOKEN_FILE } from "./sub-agents/feishu_docs/oauth"

/** 最小 deps：回调端点只依赖 config.gebaiHome 与 events.publish。 */
function makeDeps(home: string, events: Array<Record<string, unknown>>): AppDeps {
  const config = { auth: "local", binaryMode: false, devReload: false, basePath: "/", uiStyle: "classic", gebaiHome: home } as unknown as ServerConfig
  return {
    config,
    auth: { defaultUser: () => SERVICE_USER },
    events: { publish: (e: unknown) => events.push(e as Record<string, unknown>) },
  } as unknown as AppDeps
}

/** 临时替换全局 fetch（回调端点用全局 fetch 兑令牌/取用户信息），用完恢复。 */
async function withFetchMock(handler: (url: string, init?: RequestInit) => Response | Promise<Response>, fn: () => Promise<void>): Promise<void> {
  const orig = globalThis.fetch
  globalThis.fetch = (async (input: string | URL | Request, init?: RequestInit) => handler(String(input), init)) as typeof fetch
  try {
    await fn()
  } finally {
    globalThis.fetch = orig
  }
}

function oauthResponse(code: number, extra: Record<string, unknown> = {}): Response {
  return new Response(JSON.stringify({ code, ...extra }), {
    status: code === 0 ? 200 : 400,
    headers: { "content-type": "application/json" },
  })
}

describe("GET /api/v1/oauth/feishu/callback 飞书授权自动回调", () => {
  test("兑换成功：写回发起会话的令牌文件 + 发布事件 + 返回成功页（state 一次性消费）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-oauth-"))
    const events: Array<Record<string, unknown>> = []
    try {
      const app = createApp(makeDeps(home, events))
      registerPendingAuth({
        state: "state-abc",
        sessionId: "0123456789abcdef0123456789abcdef",
        user: "default",
        scopes: "docx:document offline_access",
        redirectUri: "http://localhost:3000/api/v1/oauth/feishu/callback",
        appId: "cli_oauth_app",
        appSecret: "oauth_secret",
        createdAt: Date.now(),
      })
      await withFetchMock(async (url, init) => {
        if (url.includes("/oauth/v3/token")) {
          const body = JSON.parse(String(init?.body))
          expect(body).toMatchObject({ client_id: "cli_oauth_app", client_secret: "oauth_secret", grant_type: "authorization_code", code: "code-ok" })
          expect(body.redirect_uri).toBe("http://localhost:3000/api/v1/oauth/feishu/callback")
          return oauthResponse(0, { access_token: "uat-cb", expires_in: 7200, refresh_token: "urt-cb", refresh_token_expires_in: 604800, scope: "docx:document offline_access", token_type: "Bearer" })
        }
        if (url.includes("/authen/v1/user_info")) return oauthResponse(0, { data: { name: "王五", open_id: "ou_wangwu" } })
        return oauthResponse(0, { data: {} })
      }, async () => {
        const res = await app.request("/api/v1/oauth/feishu/callback?code=code-ok&state=state-abc")
        expect(res.status).toBe(200)
        const html = await res.text()
        expect(html).toContain("飞书授权成功")
        expect(html).toContain("王五")
      })
      // 令牌写入发起会话目录（与 feishu_docs 工具共用的存储）
      const file = join(sessionPath(home, "default", "0123456789abcdef0123456789abcdef"), USER_TOKEN_FILE)
      expect(existsSync(file)).toBe(true)
      const entry = JSON.parse(readFileSync(file, "utf8")) as { accessToken: string; name: string; openId: string }
      expect(entry.accessToken).toBe("uat-cb")
      expect(entry.name).toBe("王五")
      expect(entry.openId).toBe("ou_wangwu")
      // 事件发布（WS 推送）且 state 一次性消费
      expect(events.some((e) => e.type === "oauth.completed" && e.sessionId === "0123456789abcdef0123456789abcdef" && (e.payload as Record<string, unknown>).user === "王五")).toBe(true)
      expect(getPendingAuth("state-abc")).toBeUndefined()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("state 无效/已消费：返回失败页，不写令牌文件", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-oauth-"))
    const events: Array<Record<string, unknown>> = []
    try {
      const app = createApp(makeDeps(home, events))
      const res = await app.request("/api/v1/oauth/feishu/callback?code=code-x&state=bogus")
      expect(res.status).toBe(200)
      expect(await res.text()).toContain("飞书授权失败")
      expect(existsSync(join(sessionPath(home, "default", "0123456789abcdef0123456789abcdef"), USER_TOKEN_FILE))).toBe(false)
      expect(events).toHaveLength(0)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("兑换失败（授权码已使用 20065）：失败页附可读提示且 state 释放", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-oauth-"))
    const events: Array<Record<string, unknown>> = []
    try {
      const app = createApp(makeDeps(home, events))
      registerPendingAuth({
        state: "state-fail",
        sessionId: "22222222222222222222222222222222",
        user: "default",
        scopes: "docx:document",
        redirectUri: "http://localhost:3000/api/v1/oauth/feishu/callback",
        appId: "cli_oauth_app",
        appSecret: "oauth_secret",
        createdAt: Date.now(),
      })
      await withFetchMock(async (url) => {
        if (url.includes("/oauth/v3/token")) return oauthResponse(20065, { msg: "code used" })
        return oauthResponse(0, { data: {} })
      }, async () => {
        const res = await app.request("/api/v1/oauth/feishu/callback?code=code-used&state=state-fail")
        const html = await res.text()
        expect(html).toContain("20065")
        expect(html.includes("飞书授权失败")).toBe(true)
      })
      expect(getPendingAuth("state-fail")).toBeUndefined()
      expect(existsSync(join(sessionPath(home, "default", "22222222222222222222222222222222"), USER_TOKEN_FILE))).toBe(false)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("多用户鉴权模式下回调端点免鉴权（公开）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-oauth-"))
    try {
      // 多用户模式 + 无凭据访问回调：应走豁免而非 401
      const config = { auth: "server", binaryMode: false, devReload: false, basePath: "/", uiStyle: "classic", gebaiHome: home } as unknown as ServerConfig
      const multi = createApp({ config, auth: { defaultUser: () => SERVICE_USER }, events: { publish: () => {} } } as unknown as AppDeps)
      const res = await multi.request("/api/v1/oauth/feishu/callback?code=x&state=none")
      expect(res.status).toBe(200)
      expect(await res.text()).toContain("飞书授权失败")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
