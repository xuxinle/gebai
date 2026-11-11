/**
 * `GET /api/v1/channels/state`：渠道状态**不跟拟人开关走**。
 *
 * 为什么这条值得单测：渠道状态原先只塞在 `/persona/state` 里，而那个端点在
 * “没配人格”时返回 404——于是「渠道因为撞车没起来」这种与人格毫无关系的故障，
 * 在界面上完全不可见。这个端点存在的唯一理由就是**拆掉这个耦合**，
 * 所以用例要同时钉住两件事：拟人没配时它照样返回渠道；没注入注册表时如实 404。
 */
import { describe, expect, test } from "bun:test"
import { Hono } from "hono"
import type { AppEnv } from "../app"
import { registerPersonaRoutes } from "./persona"

function appWith(deps: Record<string, unknown>): Hono<AppEnv> {
  const app = new Hono<AppEnv>()
  registerPersonaRoutes(app, deps as never)
  return app
}

describe("/api/v1/channels/state", () => {
  test("拟人未配置（credentials=null、无 state）也能拿到渠道状态", async () => {
    const app = appWith({
      credentials: null,
      publish: () => {},
      publicBase: () => "http://localhost:3000",
      channelState: () => [
        { name: "feishu_main", description: "飞书群", started: true, listening: true },
        { name: "local", description: "本机渠道", started: false, error: "启动失败: 缺凭据" },
      ],
    })
    const res = await app.request("/api/v1/channels/state")
    console.log(`    实测: ${res.status} ${await res.clone().text()}`)
    expect(res.status).toBe(200)
    const body = (await res.json()) as { channels: Array<Record<string, unknown>> }
    expect(body.channels.map((c) => c.name)).toEqual(["feishu_main", "local"])
    // 失败原因要带出来：这才是“它为什么不说话”的直接答案
    expect(body.channels[1]!.error).toContain("缺凭据")
    // 同时确认 personas 端点仍是 404（两个端点语义不同，不能一起哑掉）
    const persona = await app.request("/api/v1/persona/state")
    expect(persona.status).toBe(404)
  })

  test("未注入渠道注册表 → 404 并说明（不假装返回空数组）", async () => {
    const app = appWith({ credentials: null, publish: () => {}, publicBase: () => "http://x" })
    const res = await app.request("/api/v1/channels/state")
    console.log(`    实测: ${res.status} ${await res.clone().text()}`)
    expect(res.status).toBe(404)
    expect(((await res.json()) as { error: string }).error).toContain("未注入渠道注册表")
  })

  test("取状态抛错 → 500 并带原因（不让它变成空白的 200）", async () => {
    const app = appWith({
      credentials: null,
      publish: () => {},
      publicBase: () => "http://x",
      channelState: () => {
        throw new Error("注册表炸了")
      },
    })
    const res = await app.request("/api/v1/channels/state")
    expect(res.status).toBe(500)
    expect(((await res.json()) as { error: string }).error).toContain("注册表炸了")
  })
})
