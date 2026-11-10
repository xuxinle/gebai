/**
 * 路由表护栏：钉住「注册成功但永远匹配不上」这类静默失效。
 *
 * 事故背景见 `core/base/route-guard.ts` 文件头：`POST /persona/schedules/:key{.+}/:action`
 * 在线上永久 404，而所有单测全绿——因为单测用的精简 app 里 RegExpRouter 能生效，
 * 线上却因「同层静态段 + 参数段」被降级到 TrieRouter，而 TrieRouter 不认 `{.+}`。
 *
 * 这组用例守护三件事：
 * 1. 整站路由表不出现 `{...}` 正则参数（被降级后必然失效的写法）；
 * 2. 线上实际生效的路由器**确实是 TrieRouter**（若哪天变了，护栏的推理前提就变了，
 *    需要重新审视——所以它是断言而不是注释）；
 * 3. `createApp` 装配出的真实应用里，人格定时任务的三个动作都能命中处理器
 *    （不是靠构造精简 app 证明，而是拿真 app 打真路径）。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createApp, SERVICE_USER, type AppDeps } from "../../app"
import { assertRouteTable, describeRouter } from "./route-guard"
import type { ServerConfig } from "./config"

/** 最小 deps：只装配路由，不触碰运行时（与 app-ui.test.ts 同一手法）。 */
function makeDeps(overrides: Partial<ServerConfig> = {}): AppDeps {
  const config = {
    auth: "local",
    binaryMode: false,
    devReload: false,
    uiStyle: "matrix",
    ...overrides,
  } as unknown as ServerConfig
  return { config, auth: { defaultUser: () => SERVICE_USER } } as unknown as AppDeps
}

/** 人格定时控制面的桩：只记调用，不碰磁盘。 */
function stubScheduleControl(rows: Array<{ key: string }>) {
  const log: string[] = []
  const api = {
    list: async () =>
      rows.map((r) => {
        const [personaId = "", driverId = "", scheduleId = ""] = r.key.split("/")
        return {
          key: r.key,
          personaId,
          driverId,
          scheduleId,
          enabled: true,
          cron: "0 9 * * *",
        }
      }) as never,
    setEnabled: async (key: string, enabled: boolean) => {
      log.push(`set:${key}:${enabled}`)
      return { ok: true }
    },
    run: async (key: string) => {
      log.push(`run:${key}`)
      return { ok: true, eventId: "task:x@manual:1" }
    },
  }
  return { log, api }
}

describe("路由表护栏", () => {
  test("整站路由表不含跨斜杠正则参数（{...}）", () => {
    const app = createApp(makeDeps())
    // 装配期已断言过一次，这里显式再查：失败信息里会列出具体是哪条路由
    expect(() => assertRouteTable(app)).not.toThrow()
    const offenders = (app.routes as Array<{ path: string; method: string }>).filter((r) => r.path.includes("{"))
    expect(offenders).toEqual([])
  })

  test("实际生效的路由器是 TrieRouter（护栏的推理前提：变了就得重审这条约束）", () => {
    const app = createApp(makeDeps())
    const { router } = describeRouter(app)
    // 「同层静态段 + 参数段」（/tasks/queue 与 /tasks/:id）是既定 API 契约，
    // RegExpRouter 对这种组合直接拒收 → SmartRouter 必然降级到 TrieRouter。
    // 断言它：若某天升到能处理该组合的版本，才能重新允许 `{...}` 写法。
    expect(router).toBe("SmartRouter + TrieRouter")
  })

  test("真 app 打真路径：定时任务 enable / disable / run 三个动作都能命中处理器", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-route-guard-"))
    const c = stubScheduleControl([{ key: "finops_auditor/audit_todo/auto_todo" }])
    try {
      const deps = {
        ...makeDeps(),
        personaScheduleControl: c.api,
        // 处理器成功后会 `deps.publish(...)` 广播事件；最小 deps 里没有事件总线，补一个空实现
        events: { publish: () => {}, subscribe: () => () => {} },
      } as unknown as AppDeps
      const app = createApp(deps)
      const base = "/api/v1/persona/schedules/finops_auditor%2Faudit_todo%2Fauto_todo"
      for (const action of ["enable", "disable", "run"]) {
        const res = await app.request(`${base}/${action}`, { method: "POST" })
        // 关键的否定证据：不能是 Hono 的 text/plain 404（那说明路由根本没匹配上）
        expect(res.headers.get("content-type") ?? "").toContain("application/json")
        expect(res.status).toBe(200)
      }
      expect(c.log).toEqual([
        "set:finops_auditor/audit_todo/auto_todo:true",
        "set:finops_auditor/audit_todo/auto_todo:false",
        "run:finops_auditor/audit_todo/auto_todo",
      ])
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("/api/health 透出实际路由器（排查「某端点为何 404」的第一线索）", async () => {
    const app = createApp(makeDeps())
    const body = (await (await app.request("/api/health")).json()) as { router?: string; total?: number }
    expect(body.router).toBe("SmartRouter + TrieRouter")
    expect(body.total ?? 0).toBeGreaterThan(50)
  })
})
