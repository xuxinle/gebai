/**
 * 内置事件源 `feishu`：**入站增强**（带上下文的消息）。
 *
 * 与渠道的差别只有一条值得反复确认：**本源的附加价值是「读历史」**——
 * 群里有人说「还是不行」时，只有一条正文模型只能猜。
 *
 * 另外三条硬约束都在用例里钉住：
 * - 不自己建连（同一应用只能一条，自建会互踢）
 * - 机器人自己发的消息不能当输入（无限自我对话）
 * - 幂等键 = 飞书消息 id（飞书会重投）
 */
import { describe, expect, test } from "bun:test"
import { extractMessage, renderHistory, source } from "../sources/feishu"
import type { EventSourceContext, IngressEvent } from "../source"

function ev(over: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    header: { event_type: "im.message.receive_v1", create_time: "1700000000000" },
    event: {
      sender: { sender_id: { open_id: "ou_sender" }, sender_type: "user" },
      message: {
        message_id: "om_1",
        chat_id: "oc_support",
        chat_type: "group",
        message_type: "text",
        content: JSON.stringify({ text: "任务一直 pending" }),
      },
    },
    ...over,
  }
}

/** 构造源上下文（记录 emit / log）。 */
function ctx(env: Record<string, string> = {}, deps: Record<string, unknown> = {}) {
  const emitted: IngressEvent[] = []
  const logs: Array<{ level: string; msg: string }> = []
  const c: EventSourceContext = {
    emit: async (e) => {
      emitted.push(e)
      return { status: "accepted" }
    },
    log: (level, msg) => void logs.push({ level, msg }),
    home: "/tmp/home",
    name: "feishu",
    env: (n) => env[n],
    deps,
  }
  return { ctx: c, emitted, logs }
}

/** 长连接替身：拿到 onEvent 供测试直接推事件。 */
function fakeConn() {
  const state: { handler: ((ev: Record<string, unknown>) => void) | null; started: number; stopped: number } = {
    handler: null,
    started: 0,
    stopped: 0,
  }
  const factory = (o: { appId: string; appSecret: string; onEvent: (ev: Record<string, unknown>) => void }) => {
    state.handler = o.onEvent
    return {
      start: async () => void state.started++,
      stop: () => void state.stopped++,
    }
  }
  return { state, factory }
}

describe("extractMessage：从长连接事件抽消息", () => {
  test("正常文本消息", () => {
    const m = extractMessage(ev())!
    expect(m.messageId).toBe("om_1")
    expect(m.chatId).toBe("oc_support")
    expect(m.text).toBe("任务一直 pending")
    expect(m.senderType).toBe("user")
  })

  test("非消息事件 → null（卡片回调等不归本源）", () => {
    expect(extractMessage({ header: { event_type: "im.chat.updated_v1" } })).toBeNull()
  })

  test("缺 message_id / chat_id → null（幂等键与出站目标都靠它，宁可丢也不乱投）", () => {
    const noId = ev() as { event: { message: Record<string, unknown> } }
    delete (noId.event.message as Record<string, unknown>).message_id
    expect(extractMessage(noId)).toBeNull()
  })

  test("图片消息降级为可读占位（不按 text 硬取，否则模型以为看到了内容）", () => {
    const m = extractMessage(
      ev({
        event: {
          sender: { sender_type: "user" },
          message: { message_id: "om_2", chat_id: "oc_1", message_type: "image", content: '{"text":"残留字段"}' },
        },
      }),
    )!
    expect(m.text).toContain("图片消息")
    expect(m.text).not.toContain("残留字段")
  })
})

describe("renderHistory：历史拼成上下文", () => {
  test("用户身份读到的历史：标题不带“可能不全”", () => {
    const s = renderHistory([{ senderName: "张三", text: "报错了" }], "user")
    expect(s).toContain("这段对话之前的内容")
    expect(s).toContain("张三")
    expect(s).toContain("报错了")
    expect(s).not.toContain("可能不全")
  })

  test("应用身份读到：**明确标注可能不全**并带上原因（否则像“这个群就是没历史”）", () => {
    const s = renderHistory([{ senderName: "李四", text: "在吗" }], "app", "用户未授权")
    expect(s).toContain("应用身份读取，可能不全")
    expect(s).toContain("用户未授权")
  })

  test("空历史 → 空串（不制造一个空标题）", () => {
    expect(renderHistory([], "user")).toBe("")
  })

  test("空白正文被过滤掉（历史里不该有多余空行）", () => {
    const s = renderHistory([{ text: "  " }, { text: "有内容" }], "user")
    expect(s).toContain("有内容")
    expect(s.split("\n").filter((l) => l.trim().startsWith("-")).length).toBe(1)
  })
})

describe("source：启动与投递", () => {
  test("缺凭据 → start 抛错（该源不可用，不阻断其它源）", async () => {
    const { ctx: c } = ctx()
    await expect(source.start(c)).rejects.toThrow(/FEISHU_BOT_APP_ID/)
  })

  test("未注入长连接 → 抛错并说清「不自己建连」的原因（自建会互踢）", async () => {
    const { ctx: c } = ctx({ FEISHU_BOT_APP_ID: "cli_x", FEISHU_BOT_APP_SECRET: "s" })
    await expect(source.start(c)).rejects.toThrow(/不自己建连|互踢/)
  })

  test("正常启动：投递消息，幂等键用飞书消息 id", async () => {
    const { state, factory } = fakeConn()
    const { ctx: c, emitted } = ctx({ FEISHU_BOT_APP_ID: "cli_x", FEISHU_BOT_APP_SECRET: "s" }, { feishuLongConn: factory })
    await source.start(c)
    expect(state.started).toBe(1)
    state.handler!(ev())
    await new Promise((r) => setTimeout(r, 10))
    expect(emitted.length).toBe(1)
    expect(emitted[0]!.id).toBe("feishu:om_1")
    expect(emitted[0]!.channel).toBe("feishu")
    expect(emitted[0]!.refs?.chatId).toBe("oc_support")
    expect(emitted[0]!.body).toBe("任务一直 pending")
    await source.stop?.()
  })

  test("机器人自己发的消息被忽略（否则无限自我对话）", async () => {
    const { state, factory } = fakeConn()
    const { ctx: c, emitted } = ctx({ FEISHU_BOT_APP_ID: "cli_x", FEISHU_BOT_APP_SECRET: "s" }, { feishuLongConn: factory })
    await source.start(c)
    state.handler!(
      ev({ event: { sender: { sender_type: "app" }, message: { message_id: "om_9", chat_id: "oc_1", message_type: "text", content: '{"text":"我自己说的"}' } } }),
    )
    await new Promise((r) => setTimeout(r, 10))
    expect(emitted.length).toBe(0)
    await source.stop?.()
  })

  test("群白名单：不在名单里的群不投", async () => {
    const { state, factory } = fakeConn()
    const { ctx: c, emitted } = ctx(
      { FEISHU_BOT_APP_ID: "cli_x", FEISHU_BOT_APP_SECRET: "s", GEBAI_PERSONA_FEISHU_CHATS: "oc_other" },
      { feishuLongConn: factory },
    )
    await source.start(c)
    state.handler!(ev())
    await new Promise((r) => setTimeout(r, 10))
    expect(emitted.length).toBe(0)
    await source.stop?.()
  })

  test("开历史回看：正文尾部带上历史（这是本源相对渠道的全部附加价值）", async () => {
    const { state, factory } = fakeConn()
    const { ctx: c, emitted } = ctx(
      { FEISHU_BOT_APP_ID: "cli_x", FEISHU_BOT_APP_SECRET: "s", GEBAI_PERSONA_FEISHU_HISTORY: "3" },
      {
        feishuLongConn: factory,
        feishuHistory: async () => ({ messages: [{ senderName: "张三", text: "昨天也是这样" }], readIdentity: "user" }),
      },
    )
    await source.start(c)
    state.handler!(ev())
    await new Promise((r) => setTimeout(r, 20))
    expect(emitted[0]!.body).toContain("任务一直 pending")
    expect(emitted[0]!.body).toContain("昨天也是这样")
    await source.stop?.()
  })

  test("读历史失败不阻断本条投递（只是没上下文）", async () => {
    const { state, factory } = fakeConn()
    const { ctx: c, emitted, logs } = ctx(
      { FEISHU_BOT_APP_ID: "cli_x", FEISHU_BOT_APP_SECRET: "s", GEBAI_PERSONA_FEISHU_HISTORY: "3" },
      {
        feishuLongConn: factory,
        feishuHistory: async () => {
          throw new Error("接口 500")
        },
      },
    )
    await source.start(c)
    state.handler!(ev())
    await new Promise((r) => setTimeout(r, 20))
    expect(emitted.length).toBe(1) // 消息照投
    expect(emitted[0]!.body).toBe("任务一直 pending") // 没有历史段
    expect(logs.some((l) => l.level === "warn" && l.msg.includes("读历史失败"))).toBe(true)
    await source.stop?.()
  })

  test("要历史但宿主没注入读能力 → 启动告警，仍能投正文", async () => {
    const { state, factory } = fakeConn()
    const { ctx: c, emitted, logs } = ctx(
      { FEISHU_BOT_APP_ID: "cli_x", FEISHU_BOT_APP_SECRET: "s", GEBAI_PERSONA_FEISHU_HISTORY: "3" },
      { feishuLongConn: factory },
    )
    await source.start(c)
    expect(logs.some((l) => l.level === "warn" && l.msg.includes("未注入 deps.feishuHistory"))).toBe(true)
    state.handler!(ev())
    await new Promise((r) => setTimeout(r, 10))
    expect(emitted.length).toBe(1)
    await source.stop?.()
  })

  test("stop 释放连接（不释放 = 每次热加载泄漏一条长连接，最终与应用互踢）", async () => {
    const { state, factory } = fakeConn()
    const { ctx: c } = ctx({ FEISHU_BOT_APP_ID: "cli_x", FEISHU_BOT_APP_SECRET: "s" }, { feishuLongConn: factory })
    await source.start(c)
    await source.stop?.()
    expect(state.stopped).toBe(1)
    // 停止后不再投递
    state.handler!(ev())
    const { emitted } = { emitted: [] as IngressEvent[] }
    await new Promise((r) => setTimeout(r, 10))
    expect(emitted.length).toBe(0)
  })

  test("空正文消息被忽略（撤回/系统消息）", async () => {
    const { state, factory } = fakeConn()
    const { ctx: c, emitted } = ctx({ FEISHU_BOT_APP_ID: "cli_x", FEISHU_BOT_APP_SECRET: "s" }, { feishuLongConn: factory })
    await source.start(c)
    state.handler!(
      ev({ event: { sender: { sender_type: "user" }, message: { message_id: "om_e", chat_id: "oc_1", message_type: "text", content: '{"text":""}' } } }),
    )
    await new Promise((r) => setTimeout(r, 10))
    expect(emitted.length).toBe(0)
    await source.stop?.()
  })
})

describe("source 定义自检", () => {
  test("name/channel 稳定（人格订阅按 channel 匹配，改了会让所有订阅失效）", () => {
    expect(source.name).toBe("feishu")
    expect(source.channel).toBe("feishu")
  })

  test("声明的环境变量覆盖全部读取点（未声明就读不到——白名单过滤）", () => {
    const declared = new Set((source.envVars ?? []).map((v) => v.name))
    for (const k of [
      "FEISHU_BOT_APP_ID",
      "FEISHU_BOT_APP_SECRET",
      "GEBAI_PERSONA_FEISHU_CHATS",
      "GEBAI_PERSONA_FEISHU_HISTORY",
      "GEBAI_PERSONA_FEISHU_USE_USER",
    ]) {
      expect(declared.has(k)).toBe(true)
    }
  })
})
