import { describe, expect, test } from "bun:test"
import type { AgentEvent } from "@gebai/sdk"
import { EngineBotAdapter, type BotRunHandlers } from "./adapter"

/** 事件总线 fake：记录订阅回调，可主动推送事件。 */
function fakeBus() {
  let sub: ((e: AgentEvent) => void) | null = null
  return {
    subscribe: (fn: (e: AgentEvent) => void) => {
      sub = fn
      return () => {
        sub = null
      }
    },
    push: (ev: AgentEvent) => sub?.(ev),
  }
}

function makeAdapter(engine: Record<string, unknown>) {
  const bus = fakeBus()
  return { adapter: new EngineBotAdapter(engine as never, bus as never), bus }
}
const base = { sessionId: "s1", timestamp: 0 }

describe("EngineBotAdapter（飞书接口层）", () => {
  test("固定以多轮交互 + 仅最终回复运行，消息参数透传", async () => {
    let runOpts: Record<string, unknown> | undefined
    let runUser = ""
    const { adapter } = makeAdapter({
      run: async (_s: string, user: string, _p: string, opts: Record<string, unknown>) => {
        runUser = user
        runOpts = opts
      },
    })
    await adapter.run("s1", "u1", "hello", { messageId: "mid-12345678", attachments: [] }, {})
    expect(runUser).toBe("u1")
    // 多轮交互：关键操作询问用户；仅最终回复：无流式文本
    expect(runOpts!.interactionMode).toBe("multi_turn")
    expect(runOpts!.outputMode).toBe("final_only")
    expect(runOpts!.messageId).toBe("mid-12345678")
    // 通道环境注记：模型感知飞书宿主（渲染/交互/能力边界）
    expect(String(runOpts!.channelNote)).toContain("飞书")
  })

  test("事件流映射为语义回调：审批/选择/画图/最终回复/结束/错误", async () => {
    const called: string[] = []
    const handlers: BotRunHandlers = {
      onApproval: (toolCallId, tool, args, retries) => called.push(`approval:${toolCallId}:${tool}:${JSON.stringify(args)}:${retries}`),
      onChoice: (choiceId, prompt, options, multi) => called.push(`choice:${choiceId}:${String(prompt).slice(0, 6)}:${options.length}:${multi}`),
      onDraw: (renderId, _code, name) => called.push(`draw:${renderId}:${name ?? "unnamed"}`),
      onDone: (text) => called.push(`done:${text}`),
      onError: (err) => called.push(`error:${err}`),
      onEnd: () => called.push("end"),
    }
    let releaseRun: (() => void) | undefined
    const { adapter, bus } = makeAdapter({
      run: async () => {
        await new Promise<void>((resolve) => {
          releaseRun = resolve
        })
      },
    })
    const runPromise = adapter.run("s1", "u1", "hi", {}, handlers)
    // 事件推送（模拟引擎在任务运行中发布）
    bus.push({ type: "event.approval.request", ...base, payload: { toolCallId: "tc1", tool: "sh", arguments: { command: "ls" }, retries: 2 } })
    bus.push({ type: "event.choice.request", ...base, payload: { choiceId: "c1", prompt: "请选择", options: ["A", "B"], multi: false } })
    bus.push({ type: "event.draw.render", ...base, payload: { renderId: "r1", code: "@startuml", name: "flow" } })
    // 新会话过程 done（session 标记）不触发 onDone（仅最终回复）
    bus.push({ type: "event.message.done", ...base, payload: { text: "子代理过程", session: true } })
    bus.push({ type: "event.message.done", ...base, payload: { text: "最终回复" } })
    bus.push({ type: "event.task.done", ...base, payload: {} })
    expect(called).toEqual([
      'approval:tc1:sh:{"command":"ls"}:2',
      "choice:c1:请选择:2:false",
      "draw:r1:flow",
      "done:最终回复",
      "end",
    ])
    releaseRun?.()
    await runPromise
  })

  test("任务错误：onError + onEnd；其他会话事件不转发", async () => {
    const called: string[] = []
    const handlers: BotRunHandlers = {
      onError: (err) => called.push(`error:${err}`),
      onEnd: () => called.push("end"),
    }
    let releaseRun: (() => void) | undefined
    const { adapter, bus } = makeAdapter({
      run: async () => {
        await new Promise<void>((resolve) => {
          releaseRun = resolve
        })
      },
    })
    const runPromise = adapter.run("s1", "u1", "hi", {}, handlers)
    // 其他会话的事件不转发
    bus.push({ type: "event.message.done", sessionId: "other", timestamp: 0, payload: { text: "x" } })
    bus.push({ type: "event.task.error", ...base, payload: { error: "LLM 挂了" } })
    expect(called).toEqual(["error:LLM 挂了", "end"])
    releaseRun?.()
    await runPromise
  })

  test("通道开关：notifyTools 转发工具事件，默认不转发", async () => {
    const called: string[] = []
    const handlers: BotRunHandlers = {
      onToolCall: (name) => called.push(`call:${name}`),
      onToolResult: (name) => called.push(`result:${name}`),
    }
    // 默认（无开关）：工具事件不转发（仅最终回复，过程静默）
    {
      let releaseRun: (() => void) | undefined
      const { adapter, bus: b } = makeAdapter({
        run: async () => {
          await new Promise<void>((resolve) => {
            releaseRun = resolve
          })
        },
      })
      const p = adapter.run("s1", "u1", "hi", {}, handlers)
      b.push({ type: "event.tool.call", ...base, payload: { name: "read", toolCallId: "tc1" } })
      b.push({ type: "event.tool.result", ...base, payload: { name: "read", output: "ok", sessionId: "s1" } })
      expect(called).toEqual([])
      releaseRun?.()
      await p
    }
    // 开启 notifyTools：转发 onToolCall/onToolResult（含 session 标记）
    {
      let releaseRun: (() => void) | undefined
      const fake = {
        run: async () => {
          await new Promise<void>((resolve) => {
            releaseRun = resolve
          })
        },
      }
      const bus = fakeBus()
      const adapter = new EngineBotAdapter(fake as never, bus as never, { notifyTools: true })
      const p = adapter.run("s1", "u1", "hi", {}, handlers)
      bus.push({ type: "event.tool.call", ...base, payload: { name: "read", toolCallId: "tc1" } })
      bus.push({ type: "event.tool.result", ...base, payload: { name: "read", output: "ok", session: true, sessionId: "s1" } })
      expect(called).toEqual(["call:read", "result:read"])
      releaseRun?.()
      await p
    }
  })

  test("通道开关：notifyAssistant 转发中间轮文本并透传 notifyIntermediate；autoApprove 透传", async () => {
    let runOpts: Record<string, unknown> | undefined
    let releaseRun: (() => void) | undefined
    const fake = {
      run: async (_s: string, _u: string, _p: string, opts: Record<string, unknown>) => {
        runOpts = opts
        await new Promise<void>((resolve) => {
          releaseRun = resolve
        })
      },
    }
    const bus = fakeBus()
    const adapter = new EngineBotAdapter(fake as never, bus as never, { notifyAssistant: true, autoApprove: true })
    const seen: string[] = []
    const p = adapter.run("s1", "u1", "hi", {}, { onIntermediate: (t) => seen.push(t) })
    bus.push({ type: "event.message.intermediate", ...base, payload: { text: "我先查一下。", messageId: "m1", toolCalls: 1 } })
    expect(seen).toEqual(["我先查一下。"])
    expect(runOpts!.notifyIntermediate).toBe(true)
    expect(runOpts!.autoApprove).toBe(true)
    releaseRun?.()
    await p
    // 默认（未开启）：不透传 notifyIntermediate/autoApprove，不转发中间轮文本
    const fake2 = { run: async (_s: string, _u: string, _p: string, opts: Record<string, unknown>) => void (runOpts = opts) }
    const bus2 = fakeBus()
    const adapter2 = new EngineBotAdapter(fake2 as never, bus2 as never)
    const seen2: string[] = []
    const p2 = adapter2.run("s1", "u1", "hi", {}, { onIntermediate: (t) => seen2.push(t) })
    bus2.push({ type: "event.message.intermediate", ...base, payload: { text: "x" } })
    expect(seen2).toEqual([])
    expect(runOpts!.notifyIntermediate).toBeUndefined()
    expect(runOpts!.autoApprove).toBeUndefined()
    await p2
  })
})
