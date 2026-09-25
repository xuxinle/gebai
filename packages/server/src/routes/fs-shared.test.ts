/**
 * 根上下文装配（`routes/fs-shared.ts` 的 buildRootContext）：env 三层来源与预置项目解析。
 *
 * 关键回归点：工作台根清单的 env 必须含**进程全局层**（服务端 .env / 系统注入）——`{AGENT}_PROJECTS`
 * 通常配在那里，只合并「会话内存态 ∪ 请求携带」会让模型侧（EnvManager.resolve）看得到项目、
 * 工作台的根选择菜单里却没有（提示词与 UI 不一致，用户报的就是这个）。
 */
import { describe, expect, test } from "bun:test"
import { buildRootContext } from "./fs-shared"
import { SERVICE_USER, type AppDeps } from "../app"
import type { ServerConfig } from "../core/base/config"

/** 假 deps：engine.workbenchProjects 按真实口径从 env 解析 `{AGENT}_PROJECTS`，并记录收到的 env。 */
function makeDeps(opts: { sessionEnv?: Record<string, string>; globalEnv?: Record<string, string> } = {}) {
  const seen: Array<Record<string, string>> = []
  const globalEnv = opts.globalEnv ?? {}
  const deps = {
    config: { gebaiHome: "/tmp/gebai-home", fsWrite: true } as unknown as ServerConfig,
    sandbox: { enforcedFor: () => true, isExempt: () => true },
    env: { resolve: async () => ({ ...globalEnv, ...(opts.sessionEnv ?? {}) }) },
    store: { getEnv: async () => opts.sessionEnv ?? {} },
    engine: {
      workbenchProjects: (_user: string, env: Record<string, string>) => {
        seen.push(env)
        const raw = env.CODE_PROJECTS
        if (!raw) return []
        try {
          const list = JSON.parse(raw) as Array<{ name: string; path: string }>
          return Array.isArray(list) ? list : []
        } catch {
          return []
        }
      },
    },
  } as unknown as AppDeps
  return { deps, seen }
}

describe("buildRootContext 的 env 合并", () => {
  test("进程全局 env（.env）里的预置项目进根清单（回归：旧实现只看会话/请求 env）", async () => {
    const { deps } = makeDeps({ globalEnv: { CODE_PROJECTS: '[{"name":"clipwin","path":"/srv/clipwin"}]' } })
    const ctx = await buildRootContext(deps, SERVICE_USER, { sessionId: "s1" })
    expect(ctx.projects.map((p) => p.name)).toEqual(["clipwin"])
  })

  test("无会话上下文（未传 sessionId）时同样能解析（直读进程 env 快照）", async () => {
    const saved = process.env.CODE_PROJECTS
    process.env.CODE_PROJECTS = '[{"name":"gebai","path":"/srv/gebai"}]'
    try {
      // 无 sessionId 不走 EnvManager（无会话可解析），直接取进程 env 快照
      const { deps } = makeDeps()
      const ctx = await buildRootContext(deps, SERVICE_USER, {})
      expect(ctx.projects.map((p) => p.name)).toEqual(["gebai"])
    } finally {
      if (saved === undefined) delete process.env.CODE_PROJECTS
      else process.env.CODE_PROJECTS = saved
    }
  })

  test("请求携带（浏览器本地 env）优先级最高：覆盖进程层与会话层", async () => {
    const { deps, seen } = makeDeps({
      globalEnv: { CODE_PROJECTS: '[{"name":"from-process","path":"/a"}]' },
      sessionEnv: { CODE_PROJECTS: '[{"name":"from-session","path":"/b"}]' },
    })
    const ctx = await buildRootContext(deps, SERVICE_USER, {
      sessionId: "s1",
      envInput: { CODE_PROJECTS: '[{"name":"from-request","path":"/c"}]' },
    })
    expect(ctx.projects.map((p) => p.name)).toEqual(["from-request"])
    // 传给 engine 的就是合并后的那一份（单点合并，不各自读环境）
    expect(JSON.parse(seen.at(-1)!.CODE_PROJECTS)[0].name).toBe("from-request")
  })

  test("envInput 支持 JSON 字符串形态（?env= 查询参数）", async () => {
    const { deps } = makeDeps({ globalEnv: {} })
    const ctx = await buildRootContext(deps, SERVICE_USER, {
      envInput: JSON.stringify({ CODE_PROJECTS: '[{"name":"q","path":"/q"}]' }),
    })
    expect(ctx.projects.map((p) => p.name)).toEqual(["q"])
  })
})
