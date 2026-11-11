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
import { TrieRouter } from "hono/router/trie-router"
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
  scheduleControl?: {
    list(): Promise<
      Array<{
        key: string
        personaId: string
        driverId: string
        scheduleId: string
        enabled: boolean
        cron: string
        description?: string
        timezone?: string
        lastRunAt?: number
      }>
    >
    setEnabled(key: string, enabled: boolean): Promise<{ ok: boolean; error?: string }>
    run(key: string): Promise<{ ok: boolean; error?: string; eventId?: string }>
  }
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
    ...(deps.scheduleControl ? { scheduleControl: deps.scheduleControl } : {}),
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
    // 用任意源名验证权限门槛（不依赖具体源是否存在：无权限时连转发都不该发生）
    expect((await app.request("/api/v1/persona/sources/anything/stop", { method: "POST" })).status).toBe(403)
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

/**
 * 定时任务控制面：启停与手动运行。
 *
 * 这几条都是「界面点了应该有反应」的底层保证。要特别钉住两个边界：
 * 1. **key 不存在** → 404 而不是默默接收（写错 key 静默成功 = 以为设好了其实没有）
 * 2. **停用状态下能 run** → 不是意外，是刻意（先验证再决定要不要一直跑）
 */
describe("定时任务控制面 /api/v1/persona/schedules", () => {
  /** 与 app.ts 同款的门槛桩（本 describe 不带入上层作用域的 adminOnly）。 */
  const adminOnly = (isAdmin: boolean) =>
    ((_c: Context) => (isAdmin ? null : new Response(JSON.stringify({ error: "admin only" }), { status: 403 }))) as (
      c: Context,
    ) => Response | null

  const row = (over: Record<string, unknown> = {}) => ({
    key: "xiaoshuang/report/morning",
    personaId: "xiaoshuang",
    driverId: "report",
    scheduleId: "morning",
    enabled: true,
    cron: "0 9 * * *",
    description: "早报",
    ...over,
  })

  const control = (opts: { rows?: unknown[]; ok?: boolean; error?: string; eventId?: string } = {}) => {
    const log: string[] = []
    return {
      log,
      api: {
        list: async () => (opts.rows ?? [row()]) as never,
        setEnabled: async (key: string, enabled: boolean) => {
          log.push(`set:${key}:${enabled}`)
          return opts.ok === false ? { ok: false, error: opts.error ?? "写盘失败" } : { ok: true }
        },
        run: async (key: string) => {
          log.push(`run:${key}`)
          return opts.ok === false
            ? { ok: false, error: opts.error ?? "没接管" }
            : { ok: true, eventId: opts.eventId ?? "task:x@manual:1" }
        },
      },
    }
  }

  test("未注入控制面 → 404（与未启用拟人同面对待）", async () => {
    const app = makeApp({ credentials: {} })
    expect((await app.request("/api/v1/persona/schedules")).status).toBe(404)
    expect(
      (await app.request("/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/enable", { method: "POST" })).status,
    ).toBe(404)
  })

  test("列表返回全部声明（含已停用）", async () => {
    const c = control({ rows: [row(), row({ key: "b/y/m", enabled: false })] })
    const res = await makeApp({ credentials: {}, scheduleControl: c.api }).request("/api/v1/persona/schedules")
    expect(res.status).toBe(200)
    const body = (await res.json()) as { schedules: Array<{ key: string; enabled: boolean }> }
    // 停用的必须也在：否则界面没地方把它重新打开
    expect(body.schedules).toHaveLength(2)
    expect(body.schedules[1]).toMatchObject({ enabled: false })
  })

  test("enable / disable 转发到控制面", async () => {
    const c = control()
    const app = makeApp({ credentials: {}, scheduleControl: c.api })
    expect((await app.request("/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/enable", { method: "POST" })).status).toBe(200)
    expect((await app.request("/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/disable", { method: "POST" })).status).toBe(200)
    expect(c.log).toEqual([
      "set:xiaoshuang/report/morning:true",
      "set:xiaoshuang/report/morning:false",
    ])
  })

  test("run 转发并回 eventId", async () => {
    const c = control({ eventId: "task:abc@manual:9" })
    const res = await makeApp({ credentials: {}, scheduleControl: c.api }).request(
      "/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/run",
      { method: "POST" },
    )
    expect(res.status).toBe(200)
    expect(((await res.json()) as { eventId: string }).eventId).toBe("task:abc@manual:9")
  })

  test("停用状态下也能 run（刻意支持：先验证再决定要不要一直跑）", async () => {
    const c = control({ rows: [row({ enabled: false })] })
    const res = await makeApp({ credentials: {}, scheduleControl: c.api }).request(
      "/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/run",
      { method: "POST" },
    )
    expect(res.status).toBe(200)
    expect(c.log).toEqual(["run:xiaoshuang/report/morning"])
  })

  test("未知 key → 404 且列出可用的（写错不能静默成功）", async () => {
    const c = control()
    const res = await makeApp({ credentials: {}, scheduleControl: c.api }).request(
      "/api/v1/persona/schedules/xiaoshuang%2Freport%2Ftypo/run",
      { method: "POST" },
    )
    expect(res.status).toBe(404)
    const body = (await res.json()) as { error: string; available: string[] }
    expect(body.error).toContain("未找到")
    expect(body.available).toContain("xiaoshuang/report/morning")
    // 一次都没转发
    expect(c.log).toEqual([])
  })

  test("非法动作 → 400（不做猜测性路由）", async () => {
    const c = control()
    const res = await makeApp({ credentials: {}, scheduleControl: c.api }).request(
      "/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/restart",
      { method: "POST" },
    )
    expect(res.status).toBe(400)
    expect(c.log).toEqual([])
  })

  test("控制面说失败（写盘报错 / 事件未被接管）→ 4xx 并透出原因", async () => {
    const app = makeApp({ credentials: {}, scheduleControl: control({ ok: false, error: "磁盘只读" }).api })
    const dis = await app.request("/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/disable", {
      method: "POST",
    })
    expect(dis.status).toBe(400)
    expect(((await dis.json()) as { error: string }).error).toContain("磁盘只读")
    const run = await app.request("/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/run", {
      method: "POST",
    })
    expect(run.status).toBe(400)
  })

  test("服务模式：非 admin 不能启停/触发（部署级资源）", async () => {
    const c = control()
    const app = makeApp({ credentials: {}, scheduleControl: c.api, requireAdmin: adminOnly(false) })
    expect(
      (await app.request("/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/disable", { method: "POST" })).status,
    ).toBe(403)
    expect((await app.request("/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/run", { method: "POST" })).status).toBe(403)
    expect(c.log).toEqual([])
  })

  test("服务模式：admin 可以启停", async () => {
    const c = control()
    const app = makeApp({ credentials: {}, scheduleControl: c.api, requireAdmin: adminOnly(true) })
    expect(
      (await app.request("/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/disable", { method: "POST" })).status,
    ).toBe(200)
  })

  /**
   * 路由器兼容性：线上跑的是 **TrieRouter**，不是 RegExpRouter。
   *
   * 背景（真实事故）：这条路由曾写 `:key{.+}`，而 `TrieRouter.getPattern()` 把
   * `:key{.+}` 解析成 `[label, "key", /(?:)/]`，`matcher.exec()` 永远 null → 该路由
   * 在线上**永久不可达**，但单测用默认 RegExpRouter 全绿。下列用例就是钉住这件事：
   * 同一处理器在 TrieRouter 下必须照常工作。
   */
  describe("TrieRouter（线上实际生效的路由器）下启用可用", () => {
    const trieApp = (deps: Record<string, unknown>) => {
      const app = new Hono<AppEnv>({ router: new TrieRouter() as never })
      registerPersonaRoutes(app, deps as never)
      return app
    }

    test("URL 编码成单段的 key 能命中（前端 encodeURIComponent 形态）", async () => {
      const c = control()
      const app = trieApp({ credentials: {}, publish: () => {}, publicBase: () => "http://x", scheduleControl: c.api })
      const res = await app.request("/api/v1/persona/schedules/xiaoshuang%2Freport%2Fmorning/disable", {
        method: "POST",
      })
      expect(res.status).toBe(200)
      expect(c.log).toEqual(["set:xiaoshuang/report/morning:false"])
    })

    test("未编码的三段 key 也能命中（curl / 终端手敲形态）", async () => {
      const c = control()
      const app = trieApp({ credentials: {}, publish: () => {}, publicBase: () => "http://x", scheduleControl: c.api })
      const res = await app.request("/api/v1/persona/schedules/xiaoshuang/report/morning/enable", { method: "POST" })
      expect(res.status).toBe(200)
      // 三段拼回的 key 必须**逐字等于** scheduleKey() 的形态，否则查表永远落空
      expect(c.log).toEqual(["set:xiaoshuang/report/morning:true"])
    })

    test("key 含 % 不因二次解码而 500（param 已解码一次）", async () => {
      // `a%2525%2Fb%2Fm` 解码一次 = `a%25/b/m`（`%25` 是字面的 “%”）。
      // 旧实现又 decodeURIComponent 一次 → `%/b` 是非法转义 → URIError → 500。
      const c = control({ rows: [row({ key: "a%25/b/m" })] })
      const app = trieApp({ credentials: {}, publish: () => {}, publicBase: () => "http://x", scheduleControl: c.api })
      const res = await app.request("/api/v1/persona/schedules/a%2525%2Fb%2Fm/disable", { method: "POST" })
      expect(res.status).toBe(200)
      expect(c.log).toEqual(["set:a%25/b/m:false"])
    })
  })
})
