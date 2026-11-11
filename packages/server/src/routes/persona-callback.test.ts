/**
 * 授权回调：**遇到 20027 自动降级为短期授权**。
 *
 * 为什么这条路径必须测：飞书对 `offline_access` 的处理是**跳转授权页之前**就拒绝
 * （回调地址带上 `?error=20027`），用户此时还没看到授权页。如果不在这里接管，
 * 用户会看到一个「飞书授权失败：20027」的死页面——并且不知道该怎么办
 * （去后台开权限？改配置？）。接管后他直接被送到短期授权链接上，无感完成。
 */
import { describe, expect, test } from "bun:test"
import { Hono } from "hono"
import type { AppEnv } from "../app"
import { registerPersonaRoutes } from "./persona"

function makeApp(creds: unknown): Hono<AppEnv> {
  const app = new Hono<AppEnv>()
  registerPersonaRoutes(app, {
    credentials: creds as never,
    publish: () => {},
    publicBase: () => "http://localhost:3000",
  } as never)
  return app
}

describe("授权回调的 20027 自适应降级", () => {
  test("error=20027 → 302 跳到**不带 offline_access** 的短期授权链接", async () => {
    let downCalls = 0
    const shortUrl =
      "https://accounts.feishu.cn/open-apis/authen/v1/authorize?client_id=cli_x&scope=im%3Amessage&state=abc"
    const creds = {
      downgradeToShortLived: () => {
        downCalls++
        return { url: shortUrl, state: "abc", scopes: "im:message", redirectUri: "http://localhost:3000/cb", expiresInMs: 600000, issuedAtMs: Date.now() }
      },
    }
    const app = makeApp(creds)
    const res = await app.request("/api/v1/persona/feishu/callback?error=20027&error_description=offline_access")
    console.log(`    实测状态=${res.status} location=${res.headers.get("location")?.slice(0, 60)}`)
    expect(res.status).toBe(302)
    expect(res.headers.get("location")).toBe(shortUrl)
    expect(downCalls).toBe(1)
  })

  test("error=20027 但凭据对象没有降级能力 → 照常显示失败页（不静默吞）", async () => {
    const app = makeApp({})
    const res = await app.request("/api/v1/persona/feishu/callback?error=20027")
    expect(res.status).toBe(200)
    const html = await res.text()
    expect(html).toContain("飞书授权失败")
    expect(html).toContain("20027")
  })

  test("其它错误码（如用户拒绝）→ **不降级**，如实显示失败", async () => {
    // 关键区分：把“用户点了拒绝”也当成“应用没开长期权限”去降级，
    // 会让真正的错误被掩盖，且白白把用户送去一次莫名的新授权。
    let downCalls = 0
    const creds = {
      downgradeToShortLived: () => {
        downCalls++
        return { url: "https://example.com", state: "x", scopes: "", redirectUri: "", expiresInMs: 1, issuedAtMs: 0 }
      },
    }
    const app = makeApp(creds)
    const res = await app.request("/api/v1/persona/feishu/callback?error=20029&error_description=user%20denied")
    expect(res.status).toBe(200)
    expect(downCalls).toBe(0)
    expect(await res.text()).toContain("飞书授权失败")
  })
})
