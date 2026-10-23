/**
 * 渠道抽象层用例。
 *
 * 重点不是「代码跑得通」，而是**抽象本身是否成立**：
 * 1. 白名单越界必须直接拒绝（fail-closed），不能退化成一个审批问题
 * 2. 声明与实现必须成对（缺一边都该在加载期报错，而不是运行期才炸）
 * 3. 失败隔离（一个渠道坏不连带全部）
 * 4. 同一套 act/observe 面能同时表达「发消息」与「写记忆」（抽象泛化性的直接证据）
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { ActionExecutor } from "./executor"
import { parseTarget } from "./channel"
import type { ChannelDef } from "./channel"
import { ChannelRegistry, validateChannelDef } from "./registry"
import { channel as localChannel } from "./channels/local"

const HOME = () => mkdtempSync(join(tmpdir(), "channels-test-"))

function makeRegistry(home: string, extra: ChannelDef[] = []): ChannelRegistry {
  const reg = new ChannelRegistry({
    // 指向不存在的目录：只用程序化注册，测试不依赖仓库里的文件
    scanDirs: { builtin: join(home, "__no_builtin__"), custom: join(home, "__no_custom__") },
    home,
  })
  reg.register(localChannel)
  for (const c of extra) reg.register(c)
  return reg
}

describe("parseTarget：target 解析", () => {
  test("正常形态", () => {
    expect(parseTarget("feishu:oc_123")).toEqual({ channel: "feishu", address: "oc_123", raw: "feishu:oc_123" })
  })

  test("只切第一个冒号（address 自身可含冒号）", () => {
    const r = parseTarget("jira:PROJ:123")
    expect("error" in r).toBe(false)
    if (!("error" in r)) {
      expect(r.channel).toBe("jira")
      expect(r.address).toBe("PROJ:123")
    }
  })

  test("缺渠道前缀 → 报错（不猜默认渠道）", () => {
    const r = parseTarget("oc_123")
    expect("error" in r).toBe(true)
    if ("error" in r) expect(r.error).toContain("缺少渠道前缀")
  })

  test("空地址 → 报错", () => {
    expect("error" in (parseTarget("feishu:"))).toBe(true)
    expect("error" in (parseTarget("feishu:   "))).toBe(true)
  })

  test("非法渠道名 → 报错", () => {
    expect("error" in parseTarget("FeiShu:oc_1")).toBe(true)
    expect("error" in parseTarget("fei-shu:oc_1")).toBe(true)
  })
})

describe("validateChannelDef：声明与实现必须成对", () => {
  const base = (over: Record<string, unknown>): unknown => ({
    name: "x",
    description: "测试渠道",
    ...over,
  })

  test("最小合法定义（只读）", () => {
    const def = {
      name: "x",
      description: "d",
      observes: { ping: { summary: "探活" } },
      async observe() {
        return { summary: "pong" }
      },
    }
    expect(validateChannelDef(def, "x")).toBeNull()
  })

  test("有声明无实现 → 拒绝", () => {
    const err = validateChannelDef(base({ verbs: { send: { summary: "发" } } }), "x")
    expect(err).toContain("缺少实现")
  })

  test("有实现无声明 → 拒绝（模型不知道能调什么）", () => {
    const err = validateChannelDef(base({ async act() { return { summary: "" } } }), "x")
    expect(err).toContain("无声明")
  })

  test("name 与文件名不一致 → 拒绝", () => {
    expect(validateChannelDef({ name: "y", description: "d" }, "x")).toContain("不一致")
  })

  test("未知字段 → 拒绝", () => {
    expect(validateChannelDef(base({ typo: 1 }), "x")).toContain("未知字段")
  })

  test("verb 名非法 → 拒绝", () => {
    const err = validateChannelDef(base({ verbs: { "Send!": { summary: "s" } }, async act() { return { summary: "" } } }), "x")
    expect(err).toContain("非法")
  })

  test("capability 缺 summary → 拒绝（模型靠它判断何时用）", () => {
    const err = validateChannelDef(base({ verbs: { send: { args: {} } }, async act() { return { summary: "" } } }), "x")
    expect(err).toContain("summary")
  })

  test("arg 类型非法 → 拒绝", () => {
    const err = validateChannelDef(
      base({ verbs: { send: { summary: "s", args: { text: { type: "text", description: "d" } } } }, async act() { return { summary: "" } } }),
      "x",
    )
    expect(err).toContain("type 非法")
  })

  test("targets 非函数 → 拒绝（白名单写成字符串会静默失效）", () => {
    expect(validateChannelDef(base({ targets: ["memory"] }), "x")).toContain("targets 必须是函数")
  })
})

describe("ActionExecutor：白名单 fail-closed", () => {
  test("越界目标直接拒绝（不转审批）", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const ex = new ActionExecutor(reg)

      const bad = await ex.act({ target: "local:somewhere_else", verb: "append", args: { section: "事", text: "x" } })
      expect(bad.ok).toBe(false)
      if (!bad.ok) {
        expect(bad.kind).toBe("rejected")
        expect(bad.reason).toContain("不在渠道")
        expect(bad.reason).toContain("memory") // 列出可用地址
      }
      // 白名单内正常
      const good = await ex.act({ target: "local:memory", verb: "append", args: { section: "事", text: "记一笔" } })
      expect(good.ok).toBe(true)
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("targets 抛错 → 拒绝（fail-safe：读不到白名单不能放行）", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home, [
        {
          name: "broken",
          description: "白名单读不出来的渠道",
          verbs: { send: { summary: "发", writes: true } },
          targets() {
            throw new Error("配置读失败")
          },
          async act() {
            return { summary: "不该走到这里" }
          },
        },
      ])
      await reg.init()
      const ex = new ActionExecutor(reg)
      const r = await ex.act({ target: "broken:any", verb: "send", args: {} })
      expect(r.ok).toBe(false)
      if (!r.ok) {
        expect(r.kind).toBe("rejected")
        expect(r.reason).toContain("fail-closed")
      }
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("targets 返回 null = 不做地址级限制（允许任意 address）", async () => {
    const home = HOME()
    try {
      let seen = ""
      const reg = makeRegistry(home, [
        {
          name: "open",
          description: "不限制地址的渠道",
          verbs: { send: { summary: "发", writes: true } },
          targets() {
            return null
          },
          async act(input) {
            seen = input.address
            return { summary: "ok" }
          },
        },
      ])
      await reg.init()
      const ex = new ActionExecutor(reg)
      const r = await ex.act({ target: "open:whatever/addr", verb: "send", args: {} })
      expect(r.ok).toBe(true)
      expect(seen).toBe("whatever/addr")
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("ActionExecutor：能力与参数裁决", () => {
  test("未知渠道 → 列出已注册渠道", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const ex = new ActionExecutor(reg)
      const r = await ex.act({ target: "nope:addr", verb: "send", args: {} })
      expect(r.ok).toBe(false)
      if (!r.ok) {
        expect(r.reason).toContain("未知渠道")
        expect(r.reason).toContain("local")
        expect(r.reason).toContain("channels_list")
      }
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("未声明的 verb → 列出可用动词", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const ex = new ActionExecutor(reg)
      const r = await ex.act({ target: "local:memory", verb: "delete", args: {} })
      expect(r.ok).toBe(false)
      if (!r.ok) {
        expect(r.reason).toContain("没有声明动作")
        expect(r.reason).toContain("append")
      }
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("缺必填参数 → 指出缺哪个（可行动的反馈）", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const ex = new ActionExecutor(reg)
      const r = await ex.act({ target: "local:memory", verb: "append", args: { section: "事" } })
      expect(r.ok).toBe(false)
      if (!r.ok) {
        expect(r.kind).toBe("rejected")
        expect(r.reason).toContain("缺少必填参数：text")
        expect(r.reason).toContain("section:string（必填）")
      }
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("渠道实现抛错 → failed（与「被拒绝」性质区分）", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home, [
        {
          name: "boom",
          description: "会炸的渠道",
          verbs: { send: { summary: "发", writes: true } },
          targets: () => ["ok"],
          async act(input) {
            if (input.address === "ok") throw new Error("对端 500")
            return { summary: "" }
          },
        },
      ])
      await reg.init()
      const ex = new ActionExecutor(reg)
      const r = await ex.act({ target: "boom:ok", verb: "send", args: {} })
      expect(r.ok).toBe(false)
      if (!r.ok) {
        expect(r.kind).toBe("failed") // 不是 rejected
        expect(r.reason).toContain("对端 500")
      }
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("ActionExecutor：审批判定（写面才问人）", () => {
  const home = HOME()
  test("写动词需审批，读能力免审", async () => {
    const reg = makeRegistry(home)
    await reg.init()
    const ex = new ActionExecutor(reg)
    expect(ex.needsApproval({ target: "local:memory", verb: "append", args: {} })).toBe(true)
    expect(ex.needsApproval({ target: "local:memory", verb: "put", args: {} })).toBe(true)
    // 未知 target/verb 不该弹审批（问了也白问，直接在执行期拒绝）
    expect(ex.needsApproval({ target: "nope:x", verb: "send", args: {} })).toBe(false)
    expect(ex.needsApproval({ target: "local:memory", verb: "nope", args: {} })).toBe(false)
    await reg.stop()
    rmSync(home, { recursive: true, force: true })
  })
})

describe("失败隔离与热加载", () => {
  test("单渠道启动失败不阻断其它渠道", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home, [
        {
          name: "dies",
          description: "启动就抛",
          observes: { ping: { summary: "探活" } },
          async start() {
            throw new Error("凭证缺失")
          },
          async observe() {
            return { summary: "pong" }
          },
        },
      ])
      await reg.init()
      const states = reg.state()
      const dies = states.find((s) => s.name === "dies")!
      const local = states.find((s) => s.name === "local")!
      expect(dies.started).toBe(false)
      expect(dies.error).toContain("凭证缺失")
      expect(local.started).toBe(true) // 不受影响
      // 不可用渠道的调用被明确拒绝（不是静默失败）
      const ex = new ActionExecutor(reg)
      const r = await ex.observe({ target: "dies:any", what: "ping", args: {} })
      expect(r.ok).toBe(false)
      if (!r.ok) expect(r.reason).toContain("不可用")
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("非法治定义被拒绝并记入 loadErrors", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      // 绕过类型：故意注册一个坏定义
      reg.register({ name: "bad", description: "" } as unknown as ChannelDef)
      expect(reg.loadErrors().bad).toContain("description")
      expect(reg.get("bad")).toBeNull()
      await reg.init()
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("state() 报出动词与观测面（面板/诊断可用）", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const st = reg.state().find((s) => s.name === "local")!
      expect(st.verbs).toContain("append")
      expect(st.verbs).toContain("put")
      expect(st.observes).toContain("all")
      expect(st.observes).toContain("list")
      expect(st.restricted).toBe(true) // 声明了 targets
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("抽象泛化性：同一套 act/observe 面表达不同性质的事", () => {
  test("「写自己的记忆」与「往外部群发消息」走同一个 act 入口", async () => {
    const home = HOME()
    try {
      const sent: Array<{ address: string; text: string }> = []
      const reg = makeRegistry(home, [
        {
          // 典型的外部渠道：与 local 性质完全不同（网络、凭据、白名单）
          name: "feishu",
          description: "飞书群",
          verbs: {
            send: { summary: "发文本到群", writes: true, args: { text: { type: "string", description: "正文", required: true } } },
            reply: { summary: "引用回复", writes: true, args: { text: { type: "string", description: "正文", required: true } } },
          },
          targets: () => ["oc_ops", "oc_support"],
          async act(input) {
            sent.push({ address: input.address, text: String(input.args.text) })
            return { summary: `已发到 ${input.address}`, ref: `om_${sent.length}` }
          },
        },
      ])
      await reg.init()
      const ex = new ActionExecutor(reg)

      // ① 外部渠道：发消息
      const a = await ex.act({ target: "feishu:oc_ops", verb: "send", args: { text: "巡检无异常" } })
      expect(a.ok).toBe(true)
      if (a.ok) expect(a.outcome.ref).toBe("om_1")

      // ② 本机渠道：写记忆 —— 完全同一个入口，只是 target/verb 不同
      const b = await ex.act({ target: "local:memory", verb: "append", args: { section: "事", text: "今天做了巡检" } })
      expect(b.ok).toBe(true)

      // ③ 动作都真的发生了
      expect(sent).toHaveLength(1)
      const mem = await ex.observe({ target: "local:memory", what: "all", args: {} })
      expect(mem.ok).toBe(true)
      if (mem.ok) expect(mem.outcome.summary).toContain("今天做了巡检")

      console.log("    ✅ 同一 act 入口：feishu:send → 外部群；local:append → 本机记忆")
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("「提交工单」不需要新框架 —— 就是 verb:submit", async () => {
    const home = HOME()
    try {
      const submitted: Array<Record<string, unknown>> = []
      const reg = makeRegistry(home, [
        {
          name: "jira",
          description: "工单系统",
          verbs: {
            submit: { summary: "开一张工单", writes: true, args: { title: { type: "string", description: "标题", required: true } } },
            update: { summary: "改工单状态", writes: true, args: { status: { type: "string", description: "新状态", required: true } } },
          },
          targets: () => ["PROJ"],
          async act(input) {
            submitted.push({ verb: input.verb, ...input.args })
            return { summary: `已${input.verb}`, ref: "PROJ-1" }
          },
        },
      ])
      await reg.init()
      const ex = new ActionExecutor(reg)

      const r1 = await ex.act({ target: "jira:PROJ", verb: "submit", args: { title: "磁盘告警跟进" } })
      const r2 = await ex.act({ target: "jira:PROJ", verb: "update", args: { status: "进行中" } })
      expect(r1.ok && r2.ok).toBe(true)
      expect(submitted).toEqual([
        { verb: "submit", title: "磁盘告警跟进" },
        { verb: "update", status: "进行中" },
      ])
      console.log("    ✅ submit_* 无需单独框架：verb=submit / verb=update 走同一 act 面")
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("local 渠道：真实读写", () => {
  test("append 建段 + observe 读回（含 sections）", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const ex = new ActionExecutor(reg)

      await ex.act({ target: "local:memory", verb: "append", args: { section: "人", text: "- 张三：训练平台负责人" } })
      await ex.act({ target: "local:memory", verb: "append", args: { section: "事", text: "- d0 队列满" } })
      await ex.act({ target: "local:memory", verb: "append", args: { section: "人", text: "- 李四：算法同学" } })

      const secs = await ex.observe({ target: "local:memory", what: "sections", args: {} })
      if (!secs.ok) throw new Error("sections 失败")
      expect(secs.outcome.data?.sections).toEqual(["人", "事"])

      const one = await ex.observe({ target: "local:memory", what: "section", args: { section: "人" } })
      if (!one.ok) throw new Error("section 失败")
      const content = String(one.outcome.data?.content ?? "")
      expect(content).toContain("张三")
      expect(content).toContain("李四") // 同段追加生效
      expect(content).not.toContain("d0 队列满") // 不串段
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("put 落盘 + list/get 读回", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const ex = new ActionExecutor(reg)

      const put = await ex.act({ target: "local:outbox", verb: "put", args: { name: "巡检报告.md", content: "# 报告\n一切正常" } })
      expect(put.ok).toBe(true)
      if (!put.ok) throw new Error("put 失败")
      const fileName = String(put.outcome.data?.name)

      const list = await ex.observe({ target: "local:outbox", what: "list", args: {} })
      if (!list.ok) throw new Error("list 失败")
      expect(JSON.stringify(list.outcome.data?.files)).toContain(fileName)

      const got = await ex.observe({ target: "local:outbox", what: "get", args: { name: fileName } })
      if (!got.ok) throw new Error("get 失败")
      expect(got.outcome.summary).toContain("一切正常")
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("get 路径穿越被拒（不接受分隔符）", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const ex = new ActionExecutor(reg)
      for (const bad of ["../secret", "a/b", "..\\x"]) {
        const r = await ex.observe({ target: "local:outbox", what: "get", args: { name: bad } })
        expect(r.ok).toBe(false)
      }
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("observe 不存在的段落 → 明确说没有（不是空成功）", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const ex = new ActionExecutor(reg)
      const r = await ex.observe({ target: "local:memory", what: "section", args: { section: "不存在的段" } })
      expect(r.ok).toBe(true)
      if (r.ok) {
        expect(r.outcome.data?.found).toBe(false)
        expect(r.outcome.summary).toContain("没有")
      }
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("并发 append 不丢内容（写串行化）", async () => {
    const home = HOME()
    try {
      const reg = makeRegistry(home)
      await reg.init()
      const ex = new ActionExecutor(reg)
      await Promise.all(
        Array.from({ length: 20 }, (_, i) =>
          ex.act({ target: "local:memory", verb: "append", args: { section: "并发", text: `- item ${i}` } }),
        ),
      )
      const r = await ex.observe({ target: "local:memory", what: "section", args: { section: "并发" } })
      if (!r.ok) throw new Error("读取失败")
      const content = String(r.outcome.data?.content ?? "")
      for (let i = 0; i < 20; i++) expect(content).toContain(`item ${i}`)
      await reg.stop()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
