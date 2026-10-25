/**
 * 拟人模式路由：凭据授权（既有）+ 运行态快照 / 事件源手动启停（本轮新增）。
 *
 * 新增两处端点的设计取向在用例里钉住：
 *
 * 1. **未启用就 404**（与凭据端点同一开关语义）——不返回一个全零的假快照，
 *    那会让「拟人模式没开」看起来像「开了但什么都没发生」，排查方向直接跑偏。
 * 2. **源名只做精确匹配**——URL 里的名字会被拿去查表，不做任何路径拼接或通配，
 *    非法字符直接 400。
 */
import { describe, expect, test } from "bun:test"
import { Hono } from "hono"
import type { AppEnv } from "../app"
import type { PersonaStateSnapshot } from "../core/session/persona-status"
import { registerPersonaRoutes } from "./persona"

const SNAPSHOT: PersonaStateSnapshot = {
  enabled: true,
  generatedAt: 1_700_000_000_000,
  personas: [
    {
      id: "xiaoshuang",
      name: "小霜",
      role: "值班工程师",
      drivers: [
        {
          id: "support",
          focus: "答支持群",
          subscriptions: ["message"],
          origin: "static",
          active: true,
          running: 0,
          queued: 2,
          accepted: 5,
          deduped: 1,
          overflowed: 0,
          failed: 0,
          sessionId: "s1",
          sessionRunning: false,
        },
      ],
    },
  ],
  sources: [],
  sourceErrors: {},
  channelInput: {},
  credentials: { ready: true, state: "ok", message: "已授权", needsAuth: false },
  totals: { drivers: 1, activeDrivers: 1, running: 0, queued: 2, failed: 0 },
}

function makeApp(deps: {
  credentials?: unknown
  state?: () => Promise<PersonaStateSnapshot>
  sourceControl?: { start(name: string): Promise<boolean>; stop(name: string): Promise<boolean> }
}): Hono<AppEnv> {
  const app = new Hono<AppEnv>()
  registerPersonaRoutes(app, {
    credentials: (deps.credentials ?? null) as never,
    publish: () => {},
    publicBase: () => "http://localhost:3000",
    ...(deps.state ? { state: deps.state } : {}),
    ...(deps.sourceControl ? { sourceControl: deps.sourceControl } : {}),
  } as never)
  return app
}

describe("GET /api/v1/persona/state", () => {
  test("未启用 → 404（不返回假快照）", async () => {
    const res = await makeApp({}).request("/api/v1/persona/state")
    expect(res.status).toBe(404)
    expect(((await res.json()) as { error: string }).error).toContain("未启用")
  })

  test("启用 → 原样返回快照", async () => {
    const res = await makeApp({ credentials: {}, state: async () => SNAPSHOT }).request("/api/v1/persona/state")
    expect(res.status).toBe(200)
    const body = (await res.json()) as PersonaStateSnapshot
    expect(body.personas[0]!.drivers[0]!.queued).toBe(2)
    expect(body.totals.activeDrivers).toBe(1)
  })

  test("快照生成抛错 → 500 且带根因（不吞成空对象）", async () => {
    const res = await makeApp({
      credentials: {},
      state: async () => {
        throw new Error("目录不可读")
      },
    }).request("/api/v1/persona/state")
    expect(res.status).toBe(500)
    expect(((await res.json()) as { error: string }).error).toContain("目录不可读")
  })
})

describe("POST /api/v1/persona/sources/:name/:action", () => {
  const control = (log: string[], ok = true) => ({
    start: async (name: string) => {
      log.push(`start:${name}`)
      return ok
    },
    stop: async (name: string) => {
      log.push(`stop:${name}`)
      return ok
    },
  })

  test("未注入控制器 → 404", async () => {
    const res = await makeApp({ credentials: {} }).request("/api/v1/persona/sources/file_drop/start", { method: "POST" })
    expect(res.status).toBe(404)
  })

  test("start / stop 转发到管理器", async () => {
    const log: string[] = []
    const app = makeApp({ credentials: {}, sourceControl: control(log) })
    expect((await app.request("/api/v1/persona/sources/file_drop/start", { method: "POST" })).status).toBe(200)
    expect((await app.request("/api/v1/persona/sources/file_drop/stop", { method: "POST" })).status).toBe(200)
    expect(log).toEqual(["start:file_drop", "stop:file_drop"])
  })

  test("管理器说没有（不存在/已在跑）→ 404 且说明该看哪里", async () => {
    const app = makeApp({ credentials: {}, sourceControl: control([], false) })
    const res = await app.request("/api/v1/persona/sources/nope/start", { method: "POST" })
    expect(res.status).toBe(404)
    expect(((await res.json()) as { error: string }).error).toContain("sourceErrors")
  })

  test("非法动作 → 400（不做猜测性路由）", async () => {
    const log: string[] = []
    const app = makeApp({ credentials: {}, sourceControl: control(log) })
    const res = await app.request("/api/v1/persona/sources/file_drop/restart", { method: "POST" })
    expect(res.status).toBe(400)
    expect(log).toEqual([]) // 一次都没转发
  })

  test("非法源名 → 400（名字来自 URL，只接受 [a-z0-9_]+）", async () => {
    const log: string[] = []
    const app = makeApp({ credentials: {}, sourceControl: control(log) })
    const res = await app.request("/api/v1/persona/sources/..%2Fetc/start", { method: "POST" })
    expect(res.status).toBe(400)
    expect(log).toEqual([])
  })
})
