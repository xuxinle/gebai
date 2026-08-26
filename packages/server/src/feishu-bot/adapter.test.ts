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
})
