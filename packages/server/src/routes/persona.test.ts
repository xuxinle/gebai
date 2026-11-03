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
import type { Context } from "hono"
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
  workers: [],
  totals: { drivers: 1, activeDrivers: 1, running: 0, queued: 2, failed: 0, workersRunning: 0 },
}

/** 直接以给定 deps 构造路由（需要传 makeApp 未覆盖的字段时用）。 */
function registerPersonaRoutesWith(deps: Record<string, unknown>): Hono<AppEnv> {
  const app = new Hono<AppEnv>()
  registerPersonaRoutes(app, deps as never)
  return app
}

function makeApp(deps: {
  credentials?: unknown
  state?: () => Promise<PersonaStateSnapshot>
  sourceControl?: { start(name: string): Promise<boolean>; stop(name: string): Promise<boolean> }
  pushAuthCard?: () => Promise<{ ok: boolean; delivered: boolean; url?: string; error?: string }>
  requireAdmin?: (c: Context) => Response | null
}): Hono<AppEnv> {
  const app = new Hono<AppEnv>()
  registerPersonaRoutes(app, {
    credentials: (deps.credentials ?? null) as never,
    publish: () => {},
    publicBase: () => "http://localhost:3000",
    ...(deps.state ? { state: deps.state } : {}),
    ...(deps.sourceControl ? { sourceControl: deps.sourceControl } : {}),
    ...(deps.pushAuthCard ? { pushAuthCard: deps.pushAuthCard } : {}),
    ...(deps.requireAdmin ? { requireAdmin: deps.requireAdmin } : {}),
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

/**
 * 写面管理员门槛：拟人凭据/事件源是部署级资源（进程级、跨用户共享），
 * 服务模式下普通登录用户不应能撤销全局授权或停全局驱动源。
 * 本地模式（未注入 requireAdmin）不受影响。
 */
describe("写面 admin 门槛（requireAdmin）", () => {
  /** 与 app.ts 同款：模拟服务模式下非 admin 的判定。 */
  const adminOnly = (isAdmin: boolean) =>
    ((_c: Context) => (isAdmin ? null : new Response(JSON.stringify({ error: "admin only" }), { status: 403 }))) as (c: Context) => Response | null

  test("非 admin 调写面（授权/撤销/源启停）→ 403，且不动凭据", async () => {
    const creds = {
      startAuthorize: () => {
        throw new Error("不该走到这")
      },
      revoke: async () => {
        throw new Error("不该走到这")
      },
      complete: async () => {
        throw new Error("不该走到这")
      },
    }
    const log: string[] = []
    const app = makeApp({
      credentials: creds as never,
      sourceControl: {
        start: async (n) => {
          log.push(`start:${n}`)
          return true
        },
        stop: async (n) => {
          log.push(`stop:${n}`)
          return true
        },
      },
      requireAdmin: adminOnly(false),
    })
    expect((await app.request("/api/v1/persona/authorize", { method: "POST" })).status).toBe(403)
    expect((await app.request("/api/v1/persona/authorize", { method: "DELETE" })).status).toBe(403)
    expect((await app.request("/api/v1/persona/authorize/complete", { method: "POST", body: "{\"code\":\"x\"}", headers: { "content-type": "application/json" } })).status).toBe(403)
    expect((await app.request("/api/v1/persona/sources/feishu/stop", { method: "POST" })).status).toBe(403)
    expect(log).toEqual([]) // 一次都没转发
  })

  test("admin 调写面照常（门槛不误伤）", async () => {
    const creds = {
      startAuthorize: () => ({ url: "https://accounts.feishu.cn/x", scopes: "im:message", expiresInMs: 600_000 }),
      status: async () => ({ ready: false, state: "missing", message: "未授权", needsAuth: true }),
    }
    const app = makeApp({ credentials: creds as never, requireAdmin: adminOnly(true) })
    const res = await app.request("/api/v1/persona/authorize", { method: "POST" })
    expect(res.status).toBe(200)
    expect(((await res.json()) as { url: string }).url).toContain("accounts.feishu.cn")
  })

  test("读面（status/state）不设门槛：登录即可看", async () => {
    const app = makeApp({ credentials: {}, state: async () => SNAPSHOT, requireAdmin: adminOnly(false) })
    expect((await app.request("/api/v1/persona/state")).status).toBe(200)
  })

  test("装配失败时 sources 端点 404 文案带 setupError（与其它端点同口径）", async () => {
    const app = registerPersonaRoutesWith({
      credentials: null,
      publish: () => {},
      publicBase: () => "http://localhost:3000",
      setupError: "PersonaCatalog 构造炸了",
    })
    const res = await app.request("/api/v1/persona/sources/file_drop/start", { method: "POST" })
    expect(res.status).toBe(404)
    expect(((await res.json()) as { error: string }).error).toContain("PersonaCatalog 构造炸了")
  })
})

/**
 * 令牌请求走相期弹窗。
 *
 * 这条链路的关键不是「能推卡」（那是 compose 的事），而是**推失败不能连带把链接也吞了**：
 * 相期是桌面窗口，用户没开它、或它崩了，都是很常见的事。此时前端必须能降级开链接，
 * 否则用户就被卡在「已发出但看不到」——一个没有任何反馈的死胡同。
 */
describe("POST /api/v1/persona/authorize（令牌请求走相期）", () => {
  const creds = () => ({
    startAuthorize: () => ({ url: "https://accounts.feishu.cn/x", scopes: "im:message", expiresInMs: 600_000 }),
    status: async () => ({ ready: false, state: "missing", message: "未授权", needsAuth: true }),
  })

  test("推卡成功 → 链接照旧返回，card.delivered=true", async () => {
    let pushed = 0
    const app = makeApp({
      credentials: creds(),
      pushAuthCard: async () => {
        pushed++
        return { ok: true, delivered: true }
      },
    })
    const res = await app.request("/api/v1/persona/authorize", { method: "POST" })
    expect(res.status).toBe(200)
    const body = (await res.json()) as { url: string; card: { delivered: boolean } }
    expect(body.url).toContain("accounts.feishu.cn")
    expect(body.card.delivered).toBe(true)
    expect(pushed).toBe(1)
  })

  test("相期没连（delivered=false）→ 链接仍然返回（前端降级开新页）", async () => {
    const app = makeApp({
      credentials: creds(),
      pushAuthCard: async () => ({ ok: true, delivered: false, error: "相期未连接" }),
    })
    const res = await app.request("/api/v1/persona/authorize", { method: "POST" })
    expect(res.status).toBe(200)
    const body = (await res.json()) as { url: string; card: { delivered: boolean; error?: string } }
    // 关键：链接不能因为推送失败就没了
    expect(body.url).toContain("accounts.feishu.cn")
    expect(body.card.delivered).toBe(false)
    expect(body.card.error).toContain("相期未连接")
  })

  test("推卡抛错 → 授权本身仍然成功（推送是旁路，不是前置条件）", async () => {
    const app = makeApp({
      credentials: creds(),
      pushAuthCard: async () => {
        throw new Error("控制平面炸了")
      },
    })
    const res = await app.request("/api/v1/persona/authorize", { method: "POST" })
    expect(res.status).toBe(200)
    const body = (await res.json()) as { url: string; card: { delivered: boolean; error?: string } }
    expect(body.url).toContain("accounts.feishu.cn")
    expect(body.card.delivered).toBe(false)
    expect(body.card.error).toContain("控制平面炸了")
  })

  test("未启用拟人 → 404（不推卡、不生成链接）", async () => {
    let pushed = 0
    const app = makeApp({
      pushAuthCard: async () => {
        pushed++
        return { ok: true, delivered: true }
      },
    })
    const res = await app.request("/api/v1/persona/authorize", { method: "POST" })
    expect(res.status).toBe(404)
    expect(pushed).toBe(0)
  })
})
