/**
 * 输入面（渠道 → 调度内核）：单元 + 端到端。
 *
 * 这一组是「输入面抽象成立」的判据：**三段职责必须能分开测、分开坏**。
 *
 * | 段 | 归谁 | 若混在一起会怎样 |
 * |---|---|---|
 * | 「外面发生了什么」 | 渠道（只推 `IngressEvent`） | 每个源重实现幂等与路由，必然分叉 |
 * | 幂等键 / 渠道标识 / 类别归一 | 桥接（唯一实现） | 同一条消息在不同源里判重规则不同 |
 * | 路由到哪个人格、进哪个会话 | 内核（persona） | 渠道被迫知道人格与会话，插件拿到越权能力 |
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { ChannelInputBridge, ChannelRegistry, eventIdFor } from "./index"
import type { IngressEvent } from "./index"
import { channel as localChannel } from "./channels/local"
import { PersonaRuntime } from "../../persona/src/runtime"
import type { DriverDef, PersonaIdentity, PersonaLookup, PersonaRunner, PersonaRunInput, SessionResolver } from "../../persona/src/ports"

// ── 内核桩（真 PersonaRuntime，只把 Port 换成内存实现）──
const IDENT: PersonaIdentity = { id: "xiaoshuang", name: "小霜", identity: "你是小霜", role: "运维" }
const DRIVERS: DriverDef[] = [{ id: "support", focus: "答支持群", subscriptions: ["message"], maxConcurrent: 1 }]

function makeKernel(): { runtime: PersonaRuntime; runs: PersonaRunInput[] } {
  const runs: PersonaRunInput[] = []
  const lookup: PersonaLookup = {
    get: (pid) => (pid === "xiaoshuang" ? IDENT : null),
    drivers: () => DRIVERS,
    subscribers: (e) =>
      // 只订阅 message 类（channel 名不参与，验证类别归一真的生效）
      DRIVERS.some((d) => d.subscriptions.includes(e.kind.toLowerCase()))
        ? [{ personaId: "xiaoshuang", user: "admin", driverIds: ["support"] }]
        : [],
    maxConcurrent: () => 2,
  }
  const sessions: SessionResolver = {
    async resolveDriver({ driverId }) {
      return { sessionId: `sess-${driverId}`, created: false, name: driverId }
    },
  }
  const runner: PersonaRunner = {
    isRunning: () => false,
    async run(input) {
      runs.push(input)
    },
  }
  const runtime = new PersonaRuntime({ lookup, runner, sessions, log: { info: () => {}, warn: () => {} } })
  runtime.start()
  return { runtime, runs }
}

function makeRegistry(home: string): ChannelRegistry {
  const reg = new ChannelRegistry({
    scanDirs: { builtin: join(home, "__n1__"), custom: join(home, "__n2__") },
    home,
    envReader: (n) =>
      n === "GEBAI_CHANNELS_LOCAL_DROP_DIR" ? join(home, "channel-drop")
        : n === "GEBAI_CHANNELS_LOCAL_INBOX_DIR"
          ? join(home, "persona-inbox")
          : undefined,
    log: { info: () => {}, warn: () => {}, error: () => {} },
  })
  reg.register(localChannel)
  return reg
}

/** 等条件成立（真等渠道轮询，1.5s 一轮）。 */
async function until(cond: () => boolean, ms = 8000): Promise<void> {
  const deadline = Date.now() + ms
  while (Date.now() < deadline) {
    if (cond()) return
    await new Promise((r) => setTimeout(r, 100))
  }
}

describe("eventIdFor：幂等键推导（桥接层唯一实现）", () => {
  const base = { summary: "s", body: "b", refs: { chatId: "oc_1", messageId: "om_1" } }

  test("渠道显式 id 优先（最稳的形态）", () => {
    expect(eventIdFor("feishu", "message", { ...base, id: "om_1" })).toBe("om_1")
  })

  test("同内容兜底哈希稳定（飞书重投能去重）", () => {
    expect(eventIdFor("feishu", "message", base)).toBe(eventIdFor("feishu", "message", base))
  })

  test("内容变了 → 键变（不误吞不同消息）", () => {
    expect(eventIdFor("feishu", "message", { ...base, body: "b2" })).not.toBe(eventIdFor("feishu", "message", base))
  })

  test("不同渠道同内容 → 键不同（不跨渠道误吞）", () => {
    expect(eventIdFor("local", "message", base)).not.toBe(eventIdFor("feishu", "message", base))
  })

  test("缺 refs 也不炸（兜底路径健壮）", () => {
    expect(eventIdFor("x", "webhook", { summary: "s", body: "b" })).toContain("x:webhook:")
  })
})

describe("装配：渠道 → 桥接 → 内核", () => {
  test("同一渠道同时声明写/读/收三面", async () => {
    const home = mkdtempSync(join(tmpdir(), "in-asm-"))
    const reg = makeRegistry(home)
    const { runtime } = makeKernel()
    reg.attachDispatcher(new ChannelInputBridge({ dispatch: runtime }))
    await reg.init()
    const st = reg.state().find((s) => s.name === "local")!
    console.log(`    实测: 写[${st.verbs}] 读[${st.observes}] 收[${st.listens}] listening=${st.listening}`)
    expect(st.verbs.length).toBeGreaterThan(0)
    expect(st.observes.length).toBeGreaterThan(0)
    expect(st.listens).toEqual(["drop", "inbox"])
    expect(st.listening).toBe(true)
    expect(reg.isListening("local")).toBe(true)
    await reg.stop()
    runtime.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("未注入桥接时不订阅（渠道安静待着，不报错）", async () => {
    const home = mkdtempSync(join(tmpdir(), "in-nobr-"))
    const reg = makeRegistry(home)
    await reg.init()
    const st = reg.state().find((s) => s.name === "local")!
    expect(st.started).toBe(true) // 写/读面可用
    expect(st.listening).toBe(false) // 输入面未起
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })
})

describe("端到端：真投递 → 真跑 driver", () => {
  test("写 JSON 进 drop 目录 → 事件穿过三段职责到 driver", async () => {
    const home = mkdtempSync(join(tmpdir(), "in-e2e-"))
    const dropDir = join(home, "channel-drop")
    mkdirSync(dropDir, { recursive: true })
    const reg = makeRegistry(home)
    const { runtime, runs } = makeKernel()
    reg.attachDispatcher(new ChannelInputBridge({ dispatch: runtime }))
    await reg.init()

    writeFileSync(
      join(dropDir, "m1.json"),
      JSON.stringify({
        summary: "张三反馈任务起不来",
        body: "我这个训练任务一直 pending，帮我看看",
        source: "飞书群「支持群」",
        kind: "message",
        refs: { chatId: "oc_support", messageId: "om_1", senderName: "张三" },
      }),
    )
    await until(() => runs.length > 0)

    expect(runs.length).toBe(1)
    const e = runs[0]!.events[0]!
    console.log(`    driver 看到: id=${e.id} kind=${e.kind} channel=${e.channel} source=${e.context.source}`)
    expect(e.channel).toBe("local") // 渠道标识由注册表给，渠道不能自己宣布
    expect(e.kind).toBe("message") // 类别归一
    expect(e.context.source).toBe("飞书群「支持群」") // 来源透传
    expect(e.body).toContain("pending")
    expect(e.id.startsWith("filedrop:")).toBe(true)

    await reg.stop()
    runtime.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("inbox：人工投喂文本也能走同一条链路", async () => {
    const home = mkdtempSync(join(tmpdir(), "in-inbox-"))
    const inboxDir = join(home, "persona-inbox")
    mkdirSync(inboxDir, { recursive: true })
    const reg = makeRegistry(home)
    const { runtime, runs } = makeKernel()
    reg.attachDispatcher(new ChannelInputBridge({ dispatch: runtime }))
    await reg.init()

    writeFileSync(join(inboxDir, "note.md"), "# 请把慢查询加到监控\n阈值 3s，超过告警。\n")
    await until(() => runs.length > 0)

    expect(runs.length).toBe(1)
    const e = runs[0]!.events[0]!
    console.log(`    实测: summary=${JSON.stringify(e.context.summary)}`)
    expect(e.context.summary).toContain("慢查询")
    expect(e.context.summary).not.toContain("#") // 去掉 markdown 标题前缀
    expect(e.body).toContain("阈值 3s")

    await reg.stop()
    runtime.stop()
    rmSync(home, { recursive: true, force: true })
  })
})

describe("裁决语义：渠道能看见内核的结果", () => {
  test("重投 → duplicate（内核幂等生效，渠道计数可见）", async () => {
    const home = mkdtempSync(join(tmpdir(), "in-dup-"))
    const reg = makeRegistry(home)
    const { runtime } = makeKernel()
    const bridge = new ChannelInputBridge({ dispatch: runtime })
    reg.attachDispatcher(bridge)
    await reg.init()

    const before = bridge.stats("local").duplicate
    const ev: IngressEvent = { summary: "重投测试", body: "x", id: "dup-1" }
    const r1 = await bridge.deliver("local", undefined, ev)
    const r2 = await bridge.deliver("local", undefined, ev)
    console.log(`    实测: 首次=${r1.status} 重投=${r2.status} duplicate计数=${before}→${bridge.stats("local").duplicate}`)
    expect(r1.status).toBe("accepted")
    expect(r2.status).toBe("duplicate")
    expect(bridge.stats("local").duplicate).toBe(before + 1)

    await reg.stop()
    runtime.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("无人订阅 → unrouted（不误吞，也不自行处理）", async () => {
    const home = mkdtempSync(join(tmpdir(), "in-unr-"))
    const reg = makeRegistry(home)
    const { runtime, runs } = makeKernel()
    const bridge = new ChannelInputBridge({ dispatch: runtime })
    reg.attachDispatcher(bridge)
    await reg.init()

    // 内核桩只订阅 message；这里投 schedule 类
    const r = await bridge.deliver("local", "schedule", { summary: "定时到点", body: "巡检", id: "sch-1" })
    console.log(`    实测: status=${r.status}`)
    expect(r.status).toBe("unrouted")
    expect(runs.length).toBe(0) // 没有偷偷跑 driver
    expect(bridge.stats("local").unrouted).toBeGreaterThanOrEqual(1)

    await reg.stop()
    runtime.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("内核未启动 → unavailable（渠道记日志，不崩轮询循环）", async () => {
    const lookup: PersonaLookup = { get: () => IDENT, drivers: () => DRIVERS, subscribers: () => [], maxConcurrent: () => 1 }
    const runtime = new PersonaRuntime({
      lookup,
      runner: { isRunning: () => false, run: async () => {} },
      sessions: {
        async resolveDriver() {
          return { sessionId: "s", created: false, name: "n" }
        },
      },
    })
    const bridge = new ChannelInputBridge({ dispatch: runtime })
    const r = await bridge.deliver("local", undefined, { summary: "s", body: "b", id: "x-1" })
    console.log(`    实测: status=${r.status} reason=${r.status === "accepted" ? "-" : r.reason}`)
    expect(r.status).toBe("unavailable")
    expect(bridge.stats("local").unavailable).toBe(1)
  })
})

describe("治理：渠道拿不到内核能力", () => {
  test("ChannelContext 只有五个安全字段，没有 emit", () => {
    const home = mkdtempSync(join(tmpdir(), "in-gov-"))
    const reg = makeRegistry(home)
    const keys = Object.keys(reg.context(localChannel)).sort()
    console.log(`    实测: ctx 键 = ${keys.join(", ")}`)
    expect(keys).toEqual(["deps", "env", "home", "log", "name"])
    expect(keys).not.toContain("emit") // sink 单独传入，渠道不能自己造事件
    rmSync(home, { recursive: true, force: true })
  })

  test("sink 绑定渠道名：强塞 channel/personaId 也改不了路由", async () => {
    const home = mkdtempSync(join(tmpdir(), "in-bind-"))
    const reg = makeRegistry(home)
    const { runtime, runs } = makeKernel()
    const bridge = new ChannelInputBridge({ dispatch: runtime })
    reg.attachDispatcher(bridge)
    await reg.init()

    // IngressEvent 根本没有 channel/personaId 字段，这里强塞验证「即使塞了也无效」
    const ev = { summary: "伪造渠道", body: "x", id: "forge-1", channel: "feishu", personaId: "attacker" } as unknown as IngressEvent
    await bridge.deliver("local", undefined, ev)
    await until(() => runs.length > 0, 3000)
    expect(runs.length).toBe(1)
    const e = runs[0]!.events[0]!
    console.log(`    实测: 强塞 channel=feishu → 实际 channel=${e.channel}；强塞 personaId=attacker → 实际=${JSON.stringify(e.personaId)}`)
    expect(e.channel).toBe("local") // 注册表给的名字胜出
    // 内核在投递时会把 personaId 改写为**路由命中的那个**（这是它的职责）；
    // 关键是「attacker 未被采纳」，而不是必须为空串。
    expect(e.personaId).not.toBe("attacker")

    await reg.stop()
    runtime.stop()
    rmSync(home, { recursive: true, force: true })
  })
})

describe("生命周期：stopListen 真的摘订阅", () => {
  test("stop 后定时器停掉，新文件不再产生事件", async () => {
    const home = mkdtempSync(join(tmpdir(), "in-stop-"))
    const dropDir = join(home, "channel-drop")
    mkdirSync(dropDir, { recursive: true })
    const reg = makeRegistry(home)
    const { runtime, runs } = makeKernel()
    reg.attachDispatcher(new ChannelInputBridge({ dispatch: runtime }))
    await reg.init()
    await reg.stop()
    expect(reg.isListening("local")).toBe(false)

    writeFileSync(join(dropDir, "after-stop.json"), JSON.stringify({ summary: "停后投递", body: "不该被处理" }))
    await new Promise((r) => setTimeout(r, 2500))
    console.log(`    实测: 停止后运行数=${runs.length}`)
    expect(runs.length).toBe(0)
    runtime.stop()
    rmSync(home, { recursive: true, force: true })
  })

  test("热加载（refreshIfChanged）后订阅仍存活", async () => {
    const home = mkdtempSync(join(tmpdir(), "in-hot-"))
    const dropDir = join(home, "channel-drop")
    mkdirSync(dropDir, { recursive: true })
    const reg = makeRegistry(home)
    const { runtime, runs } = makeKernel()
    reg.attachDispatcher(new ChannelInputBridge({ dispatch: runtime }))
    await reg.init()

    // 签名未变 → 不重扫，订阅不受影响
    expect(await reg.refreshIfChanged()).toBe(false)
    expect(reg.isListening("local")).toBe(true)

    writeFileSync(join(dropDir, "after-refresh.json"), JSON.stringify({ summary: "刷新后投递", body: "应被处理", kind: "message" }))
    await until(() => runs.length > 0)
    console.log(`    实测: refresh 后运行数=${runs.length}`)
    expect(runs.length).toBe(1)

    await reg.stop()
    runtime.stop()
    rmSync(home, { recursive: true, force: true })
  })
})
