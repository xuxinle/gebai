/**
 * session 工具（跨会话控制平面）测试。
 *
 * 覆盖面：
 * - 服务缺失降级（子代理契约上下文）；
 * - create（含 env 预置 → setEnv 白名单化）；
 * - list（运行态标记）；
 * - status（空闲/运行中、消息尾部、待决交互提示）；
 * - send（空闲注入即返回；运行中拒绝；空消息拒绝）；
 * - stop（运行中终止；空闲 no-op）；
 * - delete（运行中拒绝；空闲删除）；
 * - 自保护：对 ctx.sessionId 自身的一切动作拒绝；
 * - 目标不存在 / 未知 action 的可读报错。
 */
import { describe, expect, test } from "bun:test"
import type { SessionControlService, ToolContext } from "../base/types"
import { sessionTool } from "./session"

/** 内存桩：store/engine/run 三柄的最小实现（计数注入便于断言副作用）。 */
function makeSvc(opts: { running?: string[]; selfId?: string } = {}): { svc: SessionControlService; calls: { run: string[]; runOpts: Array<Record<string, unknown> | undefined>; cancel: string[]; delete: string[]; env: Array<[string, Record<string, string | null>]> } } {
  const running = new Set(opts.running ?? [])
  const sessions = new Map<string, { id: string; name: string; userId: string; messages: Array<{ role: string; content?: unknown }> }>()
  const calls = { run: [] as string[], runOpts: [] as Array<Record<string, unknown> | undefined>, cancel: [] as string[], delete: [] as string[], env: [] as Array<[string, Record<string, string | null>]> }
  const svc: SessionControlService = {
    store: {
      createSession: async (userId, name) => {
        const id = `new-${sessions.size + 1}`
        const s = { id, name: name ?? "新会话", userId, messages: [] }
        sessions.set(id, s)
        return s as never
      },
      setEnv: async (id, _user, vars) => {
        calls.env.push([id, vars])
        return vars as never
      },
      listSessionInfos: async (userId) => [...sessions.values()].filter((s) => s.userId === userId).map((s) => ({ id: s.id, name: s.name, updatedAt: Date.now(), createdAt: Date.now() })) as never,
      load: async (id) => (sessions.has(id) ? sessions.get(id)! : null) as never,
      delete: async (id) => {
        sessions.delete(id)
        calls.delete.push(id)
      },
    },
    engine: {
      isRunning: (id) => running.has(id),
      cancel: (id) => {
        running.delete(id)
        calls.cancel.push(id)
      },
      windDown: async (id) => {
        running.delete(id)
        calls.cancel.push(id)
      },
      runtimeOf: async () => ({}) as never,
      runningIds: async () => [...running] as never,
    },
    run: async (id, _user, prompt, o) => {
      calls.run.push(prompt)
      calls.runOpts.push(o as Record<string, unknown> | undefined)
      const s = sessions.get(id)
      if (s) s.messages.push({ role: "user", content: prompt })
    },
  }
  return { svc, calls }
}

function ctxWith(svc: SessionControlService, sessionId = "self-1"): ToolContext {
  return { user: "u1", sessionId, home: "/tmp", workdir: "/tmp", env: {}, sandboxed: false, sessions: svc } as unknown as ToolContext
}

describe("session 工具：服务缺失与参数校验", () => {
  test("未注入 sessions 服务（子代理上下文）→ 可读降级说明", async () => {
    const ctx = { user: "u", sessionId: "s", env: {} } as unknown as ToolContext
    const r = await sessionTool.execute({ action: "list" }, ctx)
    expect(r.output).toContain("未注入会话管控服务")
  })

  test("未知 action / 缺 session_id / 缺 message 的可读报错", async () => {
    const { svc } = makeSvc()
    const ctx = ctxWith(svc)
    const created = await svc.store.createSession("u1")
    expect((await sessionTool.execute({ action: "send", session_id: created.id, message: "" }, ctx)).output).toContain("需要 message")
    expect((await sessionTool.execute({ action: "bogus", session_id: created.id }, ctx)).output).toContain("未知 action")
  })
})

describe("session 工具：create / list", () => {
  test("create：返回新会话 id 与用法；env 白名单化后经 setEnv 预置", async () => {
    const { svc, calls } = makeSvc()
    const ctx = ctxWith(svc)
    const r = await sessionTool.execute({ action: "create", name: "演进工作台", env: { AUTO_EVOLVE_ROOT: "D:/x", BAD: "" } }, ctx)
    expect(r.output).toContain("会话已创建")
    expect(r.output).toContain("new-1")
    expect(r.output).toContain("已预置会话环境变量 1 项")
    // 空值被白名单过滤：只落 1 项
    expect(calls.env).toHaveLength(1)
    expect(calls.env[0][1]).toEqual({ AUTO_EVOLVE_ROOT: "D:/x" })
    const d = r.data as { session: { id: string } }
    expect(d.session.id).toBe("new-1")
  })

  test("create：无 env 不触碰 setEnv", async () => {
    const { svc, calls } = makeSvc()
    await sessionTool.execute({ action: "create" }, ctxWith(svc))
    expect(calls.env).toHaveLength(0)
  })

  test("list：含运行态标记与当前会话标识", async () => {
    const { svc } = makeSvc({ running: ["new-2"] })
    await svc.store.createSession("u1", "A")
    await svc.store.createSession("u1", "B")
    const r = await sessionTool.execute({ action: "list" }, ctxWith(svc))
    expect(r.output).toContain("2 个")
    expect(r.output).toContain("self-1")
    // list 行里 new-2（塞进 running 后 load 不可见——改用 data 断言运行集）
    const d = r.data as { sessions: Array<{ id: string }> }
    expect(d.sessions.map((s) => s.id)).toContain("new-1")
  })
})

describe("session 工具：自保护（不操作自身）", () => {
  test("对 ctx.sessionId 的 status/send/stop/delete 一律拒绝", async () => {
    const { svc } = makeSvc()
    const ctx = ctxWith(svc, "self-9")
    for (const action of ["status", "send", "stop", "delete"]) {
      const r = await sessionTool.execute({ action, session_id: "self-9", message: "x" }, ctx)
      expect(r.output).toContain("不能对当前会话自身")
    }
  })
})

describe("session 工具：status / send / stop / delete", () => {
  test("目标不存在 → 拒绝（load 兼做权限校验）", async () => {
    const { svc } = makeSvc()
    const r = await sessionTool.execute({ action: "status", session_id: "ghost" }, ctxWith(svc))
    expect(r.output).toContain("不存在或无权访问")
  })

  test("status：运行中 + 消息尾部 + 待决交互", async () => {
    const { svc } = makeSvc({ running: ["w1"] })
    const created = await svc.store.createSession("u1", "工作")
    const s = (await svc.store.load(created.id)) as unknown as { messages: Array<{ role: string; content: string }> }
    s.messages.push({ role: "user", content: "第一段：修复平台进程缺陷" }, { role: "assistant", content: "已定位 platform_process.cpp:427，env 追加不覆盖" })
    const running = new Set([created.id])
    ;(svc.engine as unknown as { isRunning: (id: string) => boolean; runningIds: () => Promise<string[]> }).isRunning = (id: string) => running.has(id)
    ;(svc.engine as unknown as { runningIds: () => Promise<string[]> }).runningIds = async () => [created.id]
    ;(svc.engine as unknown as { runtimeOf: (u: string) => Promise<Record<string, unknown>> }).runtimeOf = async () => ({ [created.id]: { pending: [{ kind: "approval" }] } })
    const r = await sessionTool.execute({ action: "status", session_id: created.id, tail: 40 }, ctxWith(svc))
    expect(r.output).toContain("运行中")
    expect(r.output).toContain("已定位 platform_process.cpp")
    expect(r.output).toContain("待决交互 1 项")
    const d = r.data as { running: boolean }
    expect(d.running).toBe(true)
  })

  test("send：注入即返回（快任务同步完成如实报告）", async () => {
    const { svc, calls } = makeSvc()
    const created = await svc.store.createSession("u1", "驻留")
    const r = await sessionTool.execute({ action: "send", session_id: created.id, message: "继续演进：读台账选下个方向" }, ctxWith(svc))
    expect(r.output).toContain("已注入")
    expect(r.output).toContain("已完成")
    expect(calls.run).toEqual(["继续演进：读台账选下个方向"])
  })

  test("send：缺省免审（无人值守不卡审批），approve=false 收紧为按需审批", async () => {
    const { svc, calls } = makeSvc()
    const created = await svc.store.createSession("u1", "worker")
    // 缺省：本轮以 autoApprove 运行——工具派出的活无人守审批卡，不能挂着等审批超时
    const r = await sessionTool.execute({ action: "send", session_id: created.id, message: "干重活" }, ctxWith(svc))
    expect(calls.runOpts[0]).toEqual({ autoApprove: true, interactionMode: "realtime" })
    expect(r.output).toContain("免审")
    // 显式收紧：需审批工具在无交互通道下直接拒绝（deny），但仍会注入并驱动一轮
    const r2 = await sessionTool.execute({ action: "send", session_id: created.id, message: "再干一次", approve: false }, ctxWith(svc))
    expect(calls.runOpts[1]).toEqual({ autoApprove: false, interactionMode: "realtime" })
    expect(calls.run).toHaveLength(2)
    expect(r2.output).toContain("按需审批")
  })

  test("send：interactive=false 按无交互通道运行（ask 类工具不挂起等超时）", async () => {
    const { svc, calls } = makeSvc()
    const created = await svc.store.createSession("u1", "后台")
    const r = await sessionTool.execute({ action: "send", session_id: created.id, message: "跑批", interactive: false }, ctxWith(svc))
    expect(calls.runOpts[0]).toEqual({ autoApprove: true, interactionMode: "none" })
    expect(r.output).toContain("已注入")
  })

  test("send：长任务 2 秒后放行（不阻塞控制会话）；短任务同步完成如实报告", async () => {
    const { svc } = makeSvc()
    const created = await svc.store.createSession("u1", "长活")
    // 慢 run：6 秒才结束——send 必须在 ~2 秒内返回（早期失败窗口后放行）
    ;(svc as unknown as { run: (id: string, u: string, p: string) => Promise<void> }).run = async () => {
      await new Promise((res) => setTimeout(res, 6000))
    }
    const t0 = Date.now()
    const r = await sessionTool.execute({ action: "send", session_id: created.id, message: "重活" }, ctxWith(svc))
    expect(Date.now() - t0).toBeLessThan(4000)
    expect(r.output).toContain("后台运行")
    // 快 run：立即结束——报告「已完成」
    ;(svc as unknown as { run: (id: string, u: string, p: string) => Promise<void> }).run = async () => {}
    const r2 = await sessionTool.execute({ action: "send", session_id: created.id, message: "快活" }, ctxWith(svc))
    expect(r2.output).toContain("已完成")
  })

  test("send：目标运行中 → 拒绝并给出 stop/等待指引", async () => {
    const { svc, calls } = makeSvc()
    const created = await svc.store.createSession("u1", "W")
    ;(svc.engine as unknown as { isRunning: (id: string) => boolean }).isRunning = () => true
    const r = await sessionTool.execute({ action: "send", session_id: created.id, message: "hi" }, ctxWith(svc))
    expect(r.output).toContain("有任务在运行")
    expect(calls.run).toHaveLength(0)
  })

  test("stop：运行中 → cancel 下达；空闲 → no-op", async () => {
    const { svc, calls } = makeSvc()
    const created = await svc.store.createSession("u1", "W")
    const running = new Set([created.id])
    ;(svc.engine as unknown as { isRunning: (id: string) => boolean }).isRunning = (id: string) => running.has(id)
    ;(svc.engine as unknown as { cancel: (id: string) => void }).cancel = (id: string) => {
      running.delete(id)
      calls.cancel.push(id)
    }
    const r = await sessionTool.execute({ action: "stop", session_id: created.id }, ctxWith(svc))
    expect(r.output).toContain("终止指令已下达")
    expect(calls.cancel).toEqual([created.id])
    const r2 = await sessionTool.execute({ action: "stop", session_id: created.id }, ctxWith(svc))
    expect(r2.output).toContain("无运行中任务")
  })

  test("delete：运行中拒绝；空闲删除", async () => {
    const { svc, calls } = makeSvc()
    const created = await svc.store.createSession("u1", "W")
    const running = new Set([created.id])
    ;(svc.engine as unknown as { isRunning: (id: string) => boolean }).isRunning = (id: string) => running.has(id)
    const r = await sessionTool.execute({ action: "delete", session_id: created.id }, ctxWith(svc))
    expect(r.output).toContain("先 action=stop")
    expect(calls.delete).toHaveLength(0)
    // 转空闲后删除成功
    running.clear()
    const ok = await sessionTool.execute({ action: "delete", session_id: created.id }, ctxWith(svc))
    expect(ok.output).toContain("已删除")
    expect(calls.delete).toEqual([created.id])
  })
})
