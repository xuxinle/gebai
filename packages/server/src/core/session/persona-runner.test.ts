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
  const cancelled: string[] = []
  return {
    calls,
    cancelled,
    engine: {
      isRunning: () => opts.running ?? false,
      cancel(sessionId: string) {
        cancelled.push(sessionId)
      },
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

  test("身份字段（自由键值）全部进提示词，不挑不筛", () => {
    const withActor = personaNote({
      ...runInput(),
      identity: {
        ...IDENTITY,
        actor: { fields: { 姓名: "张三", 花名: "小张", 工号: "10086", 部门: "基础架构", 邮箱: "z@corp.com" } },
      },
      now: 0,
    } as never)
    for (const [k, v] of Object.entries({ 姓名: "张三", 花名: "小张", 工号: "10086", 部门: "基础架构", 邮箱: "z@corp.com" })) {
      expect(withActor).toContain(k)
      expect(withActor).toContain(v)
    }
    // 不挑不筛：没有“只列姓名工号”的白名单
    expect(withActor).toContain("你扮演的是")
    // 明确禁止自称机器人（不写这句它就会说“我帮你转达”）
    expect(withActor).toContain("不要自称机器人")
  })

  test("没有身份字段时不渲染那一行（不编造）", () => {
    const bare = personaNote({ ...runInput(), identity: IDENTITY, now: 0 } as never)
    expect(bare).not.toContain("你扮演的是")
  })

  test("输出目标进提示词（含默认标记与话题 root 的传法）", () => {
    const withOutputs = personaNote({
      ...runInput(),
      driver: {
        ...DRIVER,
        outputs: [
          { channel: "feishu", address: "oc_support", label: "支持群", default: true },
          { channel: "feishu", address: "oc_topic", thread: "om_root" },
        ],
      },
      now: 0,
    } as never)
    expect(withOutputs).toContain("feishu:oc_support")
    expect(withOutputs).toContain("默认")
    expect(withOutputs).toContain('root:"om_root"')
    // 名单外目标会被拒——这条纪律不写，模型会去试别的群
    expect(withOutputs).toContain("名单之外的目标一律会被拒")
  })

  test("会话键进提示词（模型得知道自己在哪个群/话题里）", () => {
    const scoped = personaNote({ ...runInput(), sessionKey: "oc_support__t_om_1", now: 0 } as never)
    expect(scoped).toContain("oc_support__t_om_1")
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

  test("未声明 agents 时**不写**记忆段（不给没装工具的人格凭空提要求）", () => {
    // 两半必须同源：写了纪律却没装工具 → 它会去调不存在的工具；
    // 装了工具却不写纪律 → 想不起来用。这里钉住“没声明就一个字都不提”。
    expect(note).not.toContain("册府")
    expect(note).not.toContain("cefu_search")
  })

  test("声明了定时任务：写明“你自己会把自己叫醒”与每条时刻/要求", () => {
    const withSchedules = personaNote({
      ...runInput(),
      driver: {
        ...runInput().driver,
        schedules: [
          { id: "morning", cron: "0 9 * * 1-5", description: "工作日早报", prompt: "写今天的早报" },
          { id: "patrol", cron: "@every 30m", prompt: "巡检一遍告警", timezone: "Asia/Shanghai" },
        ],
      },
      now: 0,
    } as never)
    expect(withSchedules).toContain("定时任务")
    // 时刻与要求都必须写明：只给 cron 模型不知道到点干什么；只给要求它不知道什么时候做
    expect(withSchedules).toContain("0 9 * * 1-5")
    expect(withSchedules).toContain("工作日早报")
    expect(withSchedules).toContain("写今天的早报")
    expect(withSchedules).toContain("@every 30m")
    expect(withSchedules).toContain("Asia/Shanghai")
    // 「不要为了有动静而发没内容的消息」——巡检类任务最容易涞生的噪音
    expect(withSchedules).toContain("没内容的消息")
  })

  test("未声明定时：一个字节都不提（不让模型凭空以为有作息表）", () => {
    expect(note).not.toContain("定时任务")
    expect(note).not.toContain("作息")
  })

  describe("声明了 cefu 时写记忆纪律", () => {
    const withMemory = personaNote({ ...runInput(), agents: ["cefu"], now: 0 } as never)

    test("写明「答之前先查」（否则它凭印象答运维问题）", () => {
      expect(withMemory).toContain("册府")
      expect(withMemory).toContain("cefu_search")
      expect(withMemory).toContain("答之前先查")
    })

    test("写明三个写入时机（用户纠正 / 实测结论 / 问第二遍）", () => {
      // 不写清“什么时候该写”时，模型要么什么都不写，要么把闲聊也写成词条
      expect(withMemory).toContain("用户纠正")
      expect(withMemory).toContain("实测")
      expect(withMemory).toContain("问第二遍")
    })

    test("写明三条硬约束：title / category / source_ref", () => {
      // source_ref 尤其关键：没有出处的知识事后无法判断该不该信
      for (const k of ["title", "category", "source_ref"]) expect(withMemory).toContain(k)
      expect(withMemory).toContain("出处")
    })

    test("写明「能 diff 成代码的写代码，不进的册府」（防知识库变垃圾场）", () => {
      expect(withMemory).toContain("写代码")
    })

    test("只声明其它子代理时不写记忆段（cid 不匹配就不提）", () => {
      const other = personaNote({ ...runInput(), agents: ["imgproc"], now: 0 } as never)
      expect(other).not.toContain("册府")
      expect(other).not.toContain("cefu_search")
    })
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

  test("跑一轮 → engine.run 收到 prompt 与 channelNote（无 persona 专用新字段）", async () => {
    const { engine, calls } = makeEngine()
    const r = new EnginePersonaRunner({ engine })
    await r.run(runInput())
    expect(calls.length).toBe(1)
    const c = calls[0]!
    console.log(`    实测 opts 键: ${Object.keys(c.opts ?? {}).sort().join(", ")}`)
    expect(c.sessionId).toBe("sess-1")
    expect(c.user).toBe("admin")
    expect(c.prompt).toContain("pending")
    // 缺省 readonly：工具执行不弹卡 → 显式 autoApprove:true
    expect(Object.keys(c.opts ?? {}).sort()).toEqual(["autoApprove", "channelNote", "interactionMode"])
    expect(String(c.opts?.channelNote)).toContain("答支持群的问题")
  })

  test("interactionMode 必显式传：人格会话没有前端，缺省 realtime 会让 ask 推死卡并阻塞 5 分钟", async () => {
    const { engine, calls } = makeEngine()
    await new EnginePersonaRunner({ engine }).run(runInput())
    expect(calls[0]!.opts?.interactionMode).toBe("multi_turn")
  })

  test("isRunning 直接转发引擎", () => {
    const { engine } = makeEngine({ running: true })
    expect(new EnginePersonaRunner({ engine }).isRunning("s")).toBe(true)
    expect(new EnginePersonaRunner({ engine: makeEngine({ running: false }).engine }).isRunning("s")).toBe(false)
  })

  /**
   * 会话丢失必须转成 `SessionLostError` 再上抛：调度器靠类型辨认它、
   * 走换锚（清缓存重解、事件改投新会话）而不是当成毒事件重试三次后丢弃。
   */
  test("引擎报会话不存在 → 抛 SessionLostError（调度器据此换锚，不当毒事件）", async () => {
    const { engine } = makeEngine({ throwOnRun: new Error("会话不存在: sess-gone") })
    const r = new EnginePersonaRunner({ engine })
    const err = await r.run(runInput()).then(
      () => null,
      (e: unknown) => e,
    )
    expect(err).toBeTruthy()
    expect((err as { name?: string }).name).toBe("SessionLostError")
    expect((err as { sessionId?: string }).sessionId).toBe("sess-1")
  })

  /**
   * 但运行**中途**的 `session not found`（存储层，跑到一半被删）不归一：
   * 那时模型可能已发过消息/改过文件，重跑一轮会重复对外动作。
   * 让它按普通失败处理——下一轮准备阶段报「会话不存在」时再自愈。
   */
  test("运行中途的 session not found 不归一（避免重跑重复对外动作）", async () => {
    const { engine } = makeEngine({ throwOnRun: new Error("session not found: sess-1") })
    const r = new EnginePersonaRunner({ engine })
    const err = await r.run(runInput()).then(
      () => null,
      (e: unknown) => e,
    )
    expect((err as { name?: string }).name).toBe("Error")
  })

  test("其它错误原样上抛（不把磁盘满/权限错误当会话丢失）", async () => {
    const { engine } = makeEngine({ throwOnRun: new Error("磁盘满了") })
    const r = new EnginePersonaRunner({ engine })
    const err = await r.run(runInput()).then(
      () => null,
      (e: unknown) => e,
    )
    expect((err as Error).message).toBe("磁盘满了")
  })

test("工具审批：缺省名单为空 → autoApprove=true（工具全部直接执行）", async () => {
  const a = makeEngine()
  await new EnginePersonaRunner({ engine: a.engine }).run(runInput())
  expect(a.calls[0]!.opts?.autoApprove).toBe(true)
  // interactionMode 仍必须是 multi_turn：名单里的工具要靠弹窗问人，模型也可能主动 ask
  expect(a.calls[0]!.opts?.interactionMode).toBe("multi_turn")
})

test("工具审批：配了名单也仍是 autoApprove=true（拦截交给引擎的 forceApprovalFor）", async () => {
  const a = makeEngine()
  await new EnginePersonaRunner({ engine: a.engine, askTools: ["sh", "file.delete"] }).run(runInput())
  // 关键：**不能**靠 autoApprove 表达“部分工具要审”——它是会话级开关
  expect(a.calls[0]!.opts?.autoApprove).toBe(true)
  expect(a.calls[0]!.opts?.interactionMode).toBe("multi_turn")
})

test("名单支持热改（传函数）：同一个 runner 改了名单即生效", async () => {
  let list: import("@gebai/persona").AskToolPattern[] = []
  const a = makeEngine()
  const r = new EnginePersonaRunner({ engine: a.engine, askTools: () => list })
  await r.run(runInput())
  expect(r.askToolsFor("xiaoshuang","support")).toEqual([])
  list = ["sh"]
  await r.run(runInput())
  expect(r.askToolsFor("xiaoshuang","support")).toEqual(["sh"])
  // 两轮都走同一条引擎通道
  expect(a.calls.map((c) => c.opts?.autoApprove)).toEqual([true, true])
})

test("名单解析抛错 → 回到空名单（不弹卡，不把人格卡死在无人值守里）", () => {
  const r = new EnginePersonaRunner({
    engine: makeEngine().engine,
    askTools: () => {
      throw new Error("boom")
    },
  })
  expect(r.askToolsFor("xiaoshuang","support")).toEqual([])
})

test("提示词：空名单告诉它“动手不弹卡、发言要人点头”", async () => {
  const a = makeEngine()
  await new EnginePersonaRunner({ engine: a.engine, askTools: [] }).run(runInput())
  const note = String(a.calls[0]!.opts?.channelNote)
  expect(note).toContain("动手一般不用批")
  expect(note).toContain("发言要人点头")
  // 没有名单就不该提“会弹卡”，否则模型会等一张永远不出现的卡
  expect(note).not.toContain("会弹卡等人点头")
})

test("提示词：有名单时明确告诉它会弹卡、被拒不要重试", async () => {
  const a = makeEngine()
  await new EnginePersonaRunner({ engine: a.engine, askTools: ["sh", "file.delete"] }).run(runInput())
  const note = String(a.calls[0]!.opts?.channelNote)
  expect(note).toContain("2 类工具会弹卡")
  expect(note).toContain("不要重复试同一个操作")
})

 test("取消桥：signal abort → engine.cancel（调度器 cancelRunning/stop 能真正停下引擎轮）", async () => {
  const ctl = new AbortController()
  const { engine, cancelled } = makeEngine()
  // engine.run 期间 abort：监听器必须在 run 挂起时已装好（先注册后 run）
  engine.run = async () => {
    ctl.abort()
    await new Promise((r) => setTimeout(r, 10))
  }
  await new EnginePersonaRunner({ engine }).run(runInput({ signal: ctl.signal }))
  expect(cancelled).toEqual(["sess-1"])
 })

 test("取消桥：正常完成不 cancel，监听器用完即摘（不残留）", async () => {
  const ctl = new AbortController()
  const { engine, cancelled } = makeEngine()
  await new EnginePersonaRunner({ engine }).run(runInput({ signal: ctl.signal }))
  expect(cancelled).toEqual([])
  // run 结束后同一 signal 再 abort：不再触发 cancel（监听器已移除）
  ctl.abort()
  expect(cancelled).toEqual([])
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
