/**
 * 人格轮次运行器：`EnginePersonaRunner`。
 *
 * 两个必须证明的点：
 * 1. **注入只走既有 `channelNote`**——engine 不需要任何 persona 分支（开关关闭时行为不变的根基）
 * 2. 出站纪律真的写进了注记——否则人格会把回复写在正文里，而**没有人转达**（最容易被忽略、
 *    后果最直接的一条）
 */
import { describe, expect, test } from "bun:test"
import { InterjectionMailbox } from "@gebai/persona"
import type { PersonaEvent } from "@gebai/persona"
import { EnginePersonaRunner, personaNote } from "./persona-runner"

function ev(over: Partial<PersonaEvent> = {}): PersonaEvent {
  return {
    id: "e1",
    kind: "message",
    personaId: "",
    channel: "feishu",
    context: { source: "飞书群「支持群」", summary: "张三问任务为什么 pending", refs: { chatId: "oc_support", messageId: "om_1" } },
    body: "我的训练任务一直 pending",
    receivedAt: Date.now(),
    ...over,
  }
}

const IDENTITY = { id: "xiaoshuang", name: "小霜", identity: "你是小霜，训练平台的值班工程师。", role: "运维" }
const DRIVER = { id: "support", focus: "答支持群的问题", subscriptions: ["message"] }

interface Call {
  sessionId: string
  user: string
  prompt: string
  opts?: Record<string, unknown>
}

function makeEngine(opts: { running?: boolean; throwOnRun?: Error } = {}) {
  const calls: Call[] = []
  return {
    calls,
    engine: {
      isRunning: () => opts.running ?? false,
      async run(sessionId: string, user: string, prompt: string, o?: Record<string, unknown>) {
        if (opts.throwOnRun) throw opts.throwOnRun
        calls.push({ sessionId, user, prompt, opts: o })
      },
    },
  }
}

function runInput(over: Partial<Parameters<EnginePersonaRunner["run"]>[0]> = {}) {
  const events = (over.events as PersonaEvent[] | undefined) ?? [ev()]
  return {
    personaId: "xiaoshuang",
    driver: DRIVER,
    identity: IDENTITY,
    user: "admin",
    events,
    event: events[0]!,
    round: 1,
    sessionId: "sess-1",
    signal: new AbortController().signal,
    mailbox: new InterjectionMailbox(),
    ...over,
  }
}

describe("personaNote：注入注记的必备内容", () => {
  const note = personaNote({ ...runInput(), now: 0 } as never)

  test("包含身份原文（不改写部署方写的权威文本）", () => {
    expect(note).toContain("你是小霜，训练平台的值班工程师。")
  })

  test("包含角色与关注面（否则同一人格的分身行为没有区别）", () => {
    expect(note).toContain("运维")
    expect(note).toContain("答支持群的问题")
    expect(note).toContain("support")
  })

  test("写明出站纪律：要回复就自己发 act", () => {
    expect(note).toContain("act(")
    expect(note).toContain("send")
    // 这条是关键：写在正文里的回复没人能看到
    expect(note).toContain("没有")
  })

  test("写明「不回复也是合法的」（避免为产出而发消息）", () => {
    expect(note).toContain("什么都不做")
  })

  test("写明不确定要问人（不要自己猜测执行）", () => {
    expect(note).toContain("先")
  })

  test("写明自组织：三个 driver_* 工具与「临时的事不要建分身」的取舍", () => {
    // 不写进去时模型根本不知道自己能开/撤分工——工具在表里，但它只会“这次先顶着”
    for (const t of ["driver_list", "driver_create", "driver_retire"]) expect(note).toContain(t)
    expect(note).toContain("reason") // 审计必需项要强调
    expect(note).toContain("静态定义的撤不掉") // 否则模型会反复试一个注定失败的调用
    expect(note).toContain("一时的事不要建分身") // 反方向的纪律（防分身泛滥）
  })

  test("未给兄弟分身时不出多分身段（不拿空表制造无用提示）", () => {
    expect(note).not.toContain("其它分身")
  })

  test("给了兄弟分身：逐个列出并排除自己", () => {
    const withSiblings = personaNote({
      ...runInput(),
      siblings: [
        { id: "support", focus: "答支持群的问题", subscriptions: ["message"] },
        { id: "alerts", focus: "盯告警并判断是否升级", subscriptions: ["webhook"] },
      ],
      now: 0,
    } as never)
    expect(withSiblings).toContain("其它分身")
    expect(withSiblings).toContain("alerts")
    expect(withSiblings).toContain("盯告警并判断是否升级")
    // 自己不出现在兄弟列表里（否则它会以为“还有一个我”在干同样的事）
    const siblingSection = withSiblings.slice(withSiblings.indexOf("其它分身"))
    expect(siblingSection).not.toContain("`support`")
  })
})

describe("EnginePersonaRunner：只经既有 channelNote 注入", () => {
  test("每轮开始前调一次热加载检查（否则「放文件即注册」只是一句文档）", async () => {
    const { engine } = makeEngine()
    let refreshes = 0
    const r = new EnginePersonaRunner({ engine, refreshBeforeRun: async () => void refreshes++ })
    await r.run(runInput())
    expect(refreshes).toBe(1)
  })

  test("热加载检查抛错不阻断本轮（目录不可读不该让已到达的事件丢掉）", async () => {
    const { engine, calls } = makeEngine()
    const r = new EnginePersonaRunner({
      engine,
      refreshBeforeRun: async () => {
        throw new Error("源目录不可读")
      },
    })
    await r.run(runInput())
    expect(calls.length).toBe(1) // 引擎照跑
    expect(calls[0]!.prompt).toContain("pending")
  })

  test("不传 refreshBeforeRun 时不报错（旧调用方可直接用）", async () => {
    const { engine, calls } = makeEngine()
    await new EnginePersonaRunner({ engine }).run(runInput())
    expect(calls.length).toBe(1)
  })

  test("跑一轮 → engine.run 收到 prompt 与 channelNote（无其他新增入参）", async () => {
    const { engine, calls } = makeEngine()
    const r = new EnginePersonaRunner({ engine })
    await r.run(runInput())
    expect(calls.length).toBe(1)
    const c = calls[0]!
    console.log(`    实测 opts 键: ${Object.keys(c.opts ?? {}).sort().join(", ")}`)
    expect(c.sessionId).toBe("sess-1")
    expect(c.user).toBe("admin")
    expect(c.prompt).toContain("pending")
    // 关键：只用了引擎已有的 channelNote（不新增 persona 专用字段）
    expect(Object.keys(c.opts ?? {}).sort()).toEqual(["autoApprove", "channelNote"])
    expect(String(c.opts?.channelNote)).toContain("答支持群的问题")
  })

  test("isRunning 直接转发引擎", () => {
    const { engine } = makeEngine({ running: true })
    expect(new EnginePersonaRunner({ engine }).isRunning("s")).toBe(true)
    expect(new EnginePersonaRunner({ engine: makeEngine({ running: false }).engine }).isRunning("s")).toBe(false)
  })

  test("autoApprove 可控：默认 true（无人值守没人守审批卡）", async () => {
    const a = makeEngine()
    await new EnginePersonaRunner({ engine: a.engine }).run(runInput())
    expect(a.calls[0]!.opts?.autoApprove).toBe(true)

    const b = makeEngine()
    await new EnginePersonaRunner({ engine: b.engine, autoApprove: false }).run(runInput())
    expect(b.calls[0]!.opts?.autoApprove).toBeUndefined()
  })
})

describe("EnginePersonaRunner：事件合批与附件", () => {
  test("多事件按到达顺序进 prompt", async () => {
    const { engine, calls } = makeEngine()
    const r = new EnginePersonaRunner({ engine })
    await r.run(
      runInput({
        events: [ev({ id: "a", body: "第一句" }), ev({ id: "b", body: "第二句" })],
      }),
    )
    const p = calls[0]!.prompt
    console.log(`    实测 prompt 前 120 字: ${JSON.stringify(p.slice(0, 120))}`)
    expect(p.indexOf("第一句")).toBeLessThan(p.indexOf("第二句"))
  })

  test("运行期间插话被 drain 进本轮（不等下一轮）", async () => {
    const { engine, calls } = makeEngine()
    const r = new EnginePersonaRunner({ engine })
    const mailbox = new InterjectionMailbox()
    mailbox.push(ev({ id: "later", body: "补充一句：还有一个任务也不行" }))
    await r.run(runInput({ events: [ev({ id: "first", body: "初始事件" })], mailbox }))
    const p = calls[0]!.prompt
    console.log(`    实测 drain 后 prompt 含插话=${p.includes("还有一个任务也不行")}`)
    expect(p).toContain("初始事件")
    expect(p).toContain("还有一个任务也不行")
    expect(mailbox.drain()).toEqual([]) // 已取走
  })

  test("附件合并传给引擎（人格看到的和用户发的是同一份）", async () => {
    const { engine, calls } = makeEngine()
    const att = { name: "a.png", mime: "image/png", data: new Uint8Array([1, 2]) }
    await new EnginePersonaRunner({ engine }).run(runInput({ events: [ev({ attachments: [att] })] }))
    expect((calls[0]!.opts?.attachments as unknown[]).length).toBe(1)
  })

  test("无附件时不下发 attachments 字段（不污染既有调用形态）", async () => {
    const { engine, calls } = makeEngine()
    await new EnginePersonaRunner({ engine }).run(runInput())
    expect("attachments" in (calls[0]!.opts ?? {})).toBe(false)
  })
})

describe("EnginePersonaRunner：失败如实上抛（不吞）", () => {
  test("引擎抛错 → runner 抛错（调度器据此记失败，不假装成功）", async () => {
    const { engine } = makeEngine({ throwOnRun: new Error("模型 500") })
    const r = new EnginePersonaRunner({ engine })
    await expect(r.run(runInput())).rejects.toThrow("模型 500")
  })
})
