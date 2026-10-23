/**
 * 飞书渠道的输入面：**自建长连接**（不与会话机器人共用）。
 *
 * 这一组用注入的假长连接验证三件容易做错的事：
 *
 * 1. **不复用**：凭据必须是独立变量（不回落 `GEBAI_FEISHU_*`），否则两个机器人撞在同一应用上
 * 2. **防自触发**：机器人自己发的消息必须忽略（否则发一条消息又会触发自己，无限循环）
 * 3. **缺实现要如实失败**：拿不到长连接注入时明确报错，而不是静默「配了但收不到」
 */
import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { ChannelRegistry } from "./index"
import type { IngressEvent } from "./index"
import { channel as feishu } from "./channels/feishu"

/** 一条飞书 schema 2.0 消息事件。 */
function msgEvent(opts: {
  messageId?: string
  chatId?: string
  text?: string
  senderType?: string
  chatType?: string
  messageType?: string
} = {}): Record<string, unknown> {
  return {
    header: { event_type: "im.message.receive_v1", create_time: "1792728000000" },
    event: {
      sender: { sender_id: { open_id: "ou_zhangsan" }, sender_type: opts.senderType ?? "user" },
      message: {
        message_id: opts.messageId ?? "om_1",
        chat_id: opts.chatId ?? "oc_support",
        chat_type: opts.chatType ?? "group",
        message_type: opts.messageType ?? "text",
        content: JSON.stringify({ text: opts.text ?? "任务为什么一直 pending" }),
      },
    },
  }
}

interface FakeConn {
  started: number
  stopped: number
  fire(ev: Record<string, unknown>): void
}

/**
 * 测试环境把飞书 HTTP 换成本地吐牌（`start()` 会验一次 tenant_access_token，
 * 不该让单测真的打外网）。返回一个恢复函数。
 */
function stubFeishuFetch(): () => void {
  const real = globalThis.fetch
  globalThis.fetch = (async (url: string | URL | Request) => {
    const u = String(url)
    if (u.includes("tenant_access_token")) {
      return new Response(JSON.stringify({ code: 0, msg: "ok", tenant_access_token: "t-fake", expire: 7200 }), {
        status: 200,
        headers: { "content-type": "application/json" },
      })
    }
    return new Response(JSON.stringify({ code: 0, msg: "ok", data: {} }), { status: 200, headers: { "content-type": "application/json" } })
  }) as typeof fetch
  return () => {
    globalThis.fetch = real
  }
}

function makeRegistry(
  home: string,
  env: Record<string, string>,
  opts: { withConn?: boolean; sessionAppId?: string } = {},
): { reg: ChannelRegistry; conns: FakeConn[]; events: IngressEvent[] } {
  const conns: FakeConn[] = []
  const events: IngressEvent[] = []
  const reg = new ChannelRegistry({
    scanDirs: { builtin: join(home, "__n1__"), custom: join(home, "__n2__") },
    home,
    envReader: (n) => env[n],
    deps: {
      ...(opts.withConn === false
        ? {}
        : {
            feishuLongConn: (o: { appId: string; appSecret: string; onEvent: (ev: Record<string, unknown>) => void }) => {
              const c: FakeConn = {
                started: 0,
                stopped: 0,
                fire: (ev) => o.onEvent(ev),
              }
              conns.push(c)
              return {
                async start() {
                  c.started++
                },
                stop() {
                  c.stopped++
                },
              }
            },
          }),
      ...(opts.sessionAppId ? { feishuSessionAppId: opts.sessionAppId } : {}),
    },
    log: { info: () => {}, warn: () => {}, error: () => {} },
  })
  reg.register(feishu)
  // 捕获事件（不经 persona，直接看渠道推了什么）
  reg.attachDispatcher({
    async deliver(ch, _dk, ev) {
      events.push(ev)
      void ch
      return { status: "accepted" } as never
    },
  })
  return { reg, conns, events }
}

const CREDS = {
  GEBAI_CHANNELS_FEISHU_APP_ID: "cli_channel_bot",
  GEBAI_CHANNELS_FEISHU_APP_SECRET: "secret_channel",
  GEBAI_CHANNELS_FEISHU_CHATS: "oc_support",
}

// `start()` 会真实验一次 tenant_access_token（生产上这是对的：启动期就暴露凭据问题）。
// 单测不该打外网，故整个文件统一吐牌。
let restoreFetch: () => void = () => {}
beforeAll(() => {
  restoreFetch = stubFeishuFetch()
})
afterAll(() => {
  restoreFetch()
})

describe("凭据：独立机器人，不回落 GEBAI_FEISHU_*", () => {
  test("只配 GEBAI_FEISHU_APP_ID 不配渠道变量 → 渠道不可用（不复用会话机器人）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-cred-"))
    const { reg } = makeRegistry(home, { GEBAI_FEISHU_APP_ID: "cli_session_bot", GEBAI_FEISHU_APP_SECRET: "x" })
    await reg.init()
    const st = reg.state().find((s) => s.name === "feishu")!
    console.log(`    实测: started=${st.started} error=${st.error}`)
    expect(st.started).toBe(false)
    expect(st.error ?? "").toContain("GEBAI_CHANNELS_FEISHU_APP_ID")
    expect(st.error ?? "").toContain("不回落")
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("配了独立凭据 → 可用", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-cred2-"))
    const { reg, conns } = makeRegistry(home, CREDS)
    await reg.init()
    const st = reg.state().find((s) => s.name === "feishu")!
    console.log(`    实测: started=${st.started} 收[${st.listens}] listening=${st.listening}`)
    expect(st.started).toBe(true)
    expect(st.listens).toEqual(["messages"])
    expect(st.listening).toBe(true)
    expect(conns.length).toBe(1)
    expect(conns[0]!.started).toBe(1)
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("渠道 AppID 与会话机器人相同 → 拒绝启动（同应用两条长连接会互踢）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-clash-"))
    const { reg } = makeRegistry(
      home,
      { ...CREDS, GEBAI_CHANNELS_FEISHU_APP_ID: "cli_same_app" },
      { sessionAppId: "cli_same_app" },
    )
    await reg.init()
    const st = reg.state().find((s) => s.name === "feishu")!
    console.log(`    实测: started=${st.started} error=${st.error}`)
    expect(st.started).toBe(false)
    expect(st.error ?? "").toContain("两条长连接")
    expect(st.error ?? "").toContain("互踢")
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("未注入长连接实现 → 如实报错（不静默「配了但收不到」）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-noconn-"))
    const { reg } = makeRegistry(home, CREDS, { withConn: false })
    await reg.init()
    const st = reg.state().find((s) => s.name === "feishu")!
    console.log(`    实测: started=${st.started} listening=${st.listening} error=${st.error}`)
    // 写/读面照常起（令牌验过），但输入面起不来 → 降级而非全损
    expect(st.started).toBe(true)
    expect(st.listening).toBe(false)
    expect(st.error ?? "").toContain("feishuLongConn")
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })
})

describe("入站：消息抽取与治理", () => {
  test("标准文本消息 → 归一成 IngressEvent（幂等键=消息 id）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-in-"))
    const { reg, conns, events } = makeRegistry(home, CREDS)
    await reg.init()

    conns[0]!.fire(msgEvent({ messageId: "om_abc", text: "任务为什么一直 pending" }))
    await new Promise((r) => setTimeout(r, 100))
    expect(events.length).toBe(1)
    const e = events[0]!
    console.log(`    实测: id=${e.id} summary=${JSON.stringify(e.summary)}`)
    console.log(`          refs=${JSON.stringify(e.refs)}`)
    expect(e.id).toBe("feishu:om_abc") // 显式幂等键（飞书会重投同一消息）
    expect(e.body).toBe("任务为什么一直 pending")
    expect(e.refs?.chatId).toBe("oc_support")
    expect(e.refs?.messageId).toBe("om_abc")
    expect(e.refs?.senderId).toBe("ou_zhangsan")
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("机器人自己发的消息 → 忽略（防自触发循环）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-self-"))
    const { reg, conns, events } = makeRegistry(home, CREDS)
    await reg.init()

    conns[0]!.fire(msgEvent({ messageId: "om_self", senderType: "app" }))
    await new Promise((r) => setTimeout(r, 100))
    console.log(`    实测: senderType=app 的事件数=${events.length}`)
    expect(events.length).toBe(0) // 否则发一条消息 → 又触发自己 → 无限循环

    // 用户的消息照常
    conns[0]!.fire(msgEvent({ messageId: "om_user", senderType: "user" }))
    await new Promise((r) => setTimeout(r, 100))
    expect(events.length).toBe(1)
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("非白名单群 → 忽略（入站也守白名单）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-chat-"))
    const { reg, conns, events } = makeRegistry(home, CREDS)
    await reg.init()

    conns[0]!.fire(msgEvent({ messageId: "om_out", chatId: "oc_other" }))
    await new Promise((r) => setTimeout(r, 100))
    console.log(`    实测: 非白名单群事件数=${events.length}`)
    expect(events.length).toBe(0)

    conns[0]!.fire(msgEvent({ messageId: "om_in", chatId: "oc_support" }))
    await new Promise((r) => setTimeout(r, 100))
    expect(events.length).toBe(1)
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("非消息事件 → 不归本渠道（卡片回调等留给会话机器人）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-card-"))
    const { reg, conns, events } = makeRegistry(home, CREDS)
    await reg.init()

    conns[0]!.fire({ header: { event_type: "card.action.trigger" }, event: {} })
    await new Promise((r) => setTimeout(r, 100))
    console.log(`    实测: 卡片事件后 events=${events.length}`)
    expect(events.length).toBe(0)
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("非文本消息 → 降级为可读说明（不静默丢弃）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-img-"))
    const { reg, conns, events } = makeRegistry(home, CREDS)
    await reg.init()

    conns[0]!.fire(msgEvent({ messageId: "om_img", messageType: "image" }))
    await new Promise((r) => setTimeout(r, 100))
    expect(events.length).toBe(1)
    console.log(`    实测: body=${JSON.stringify(events[0]!.body)}`)
    expect(events[0]!.body).toContain("image")
    expect(events[0]!.body).toContain("om_img") // 给出可追查的 id
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("缺 message_id/chat_id → 不投（幂等键与出站目标都靠它）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-noid-"))
    const { reg, conns, events } = makeRegistry(home, CREDS)
    await reg.init()

    const broken = msgEvent() as { event: { message: Record<string, unknown> } }
    delete broken.event.message.message_id
    conns[0]!.fire(broken as unknown as Record<string, unknown>)
    await new Promise((r) => setTimeout(r, 100))
    console.log(`    实测: 缺 message_id 后 events=${events.length}`)
    expect(events.length).toBe(0) // 宁可丢也不乱投（无法去重、无法回复）
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("单聊消息 → 来源标为「与我的单聊」", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-p2p-"))
    const { reg, conns, events } = makeRegistry(home, CREDS)
    await reg.init()

    conns[0]!.fire(msgEvent({ messageId: "om_p2p", chatType: "p2p", chatId: "oc_support" }))
    await new Promise((r) => setTimeout(r, 100))
    expect(events.length).toBe(1)
    console.log(`    实测: source=${JSON.stringify(events[0]!.source)}`)
    expect(events[0]!.source).toContain("单聊")
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })
})

describe("生命周期：stopListen 断开自己的长连接", () => {
  test("stop 后长连接被 stop（不影响其它渠道）", async () => {
    const home = mkdtempSync(join(tmpdir(), "fs-stop-"))
    const { reg, conns } = makeRegistry(home, CREDS)
    await reg.init()
    expect(conns[0]!.started).toBe(1)
    expect(reg.isListening("feishu")).toBe(true)

    await reg.stop()
    console.log(`    实测: started=${conns[0]!.started} stopped=${conns[0]!.stopped}`)
    expect(conns[0]!.stopped).toBe(1)
    expect(reg.isListening("feishu")).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })
})
