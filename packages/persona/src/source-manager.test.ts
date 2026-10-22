/**
 * 事件源扩展规范用例：与子 Agent 同款的「丢文件即注册」四套机制。
 *
 * 覆盖重点（每一条都是真实故障形态）：
 * 1. 失败隔离：一个源坏掉不能让其它源也不可用
 * 2. 字段校验：`channel` 拼错 → 路由永远匹配不上，但没有任何异常（静默失效）
 * 3. 双域覆盖：custom 同名源胜出内置（二开语义）
 * 4. 幂等键：插件显式 id 优先，缺省按 kind 推导
 * 5. 环境变量白名单：未声明即不可读（防「读告警的插件」拿到模型密钥）
 */
import { afterEach, beforeEach, describe, expect, test } from "bun:test"
import { existsSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { defaultEventSourceScanDirs, discoverEventSources, discoverySignature, validateSourceDef } from "./discovery"
import { EventSourceManager } from "./source-manager"
import { PersonaRuntime } from "./runtime"
import type { EventSourceContext, EventSourceDef, IngressEvent } from "./source"
import type { PersonaIdentity, PersonaLookup, PersonaRunner, SessionResolver } from "./ports"

let root = ""

function dirs() {
  return { builtin: join(root, "builtin"), custom: join(root, "custom") }
}

function writeSource(domain: "builtin" | "custom", name: string, body: string, ext = "ts") {
  const d = dirs()[domain]
  mkdirSync(d, { recursive: true })
  writeFileSync(join(d, `${name}.${ext}`), body)
}

beforeEach(() => {
  root = join(tmpdir(), `persona-src-${crypto.randomUUID()}`)
  mkdirSync(root, { recursive: true })
})

afterEach(() => {
  rmSync(root, { recursive: true, force: true })
})

const IDENT: PersonaIdentity = { id: "p1", name: "小霜", identity: "你是小霜", role: "应答" }

/** 最小可用运行时（source manager 测试用）。 */
function makeRuntime(opts: { routes?: boolean; emitCapture?: IngressEvent[] } = {}) {
  const lookup: PersonaLookup = {
    get: (id) => (id === "p1" ? IDENT : null),
    subscribers: () => (opts.routes === false ? [] : [{ personaId: "p1", user: "admin", driverIds: ["main"] }]),
    drivers: () => [{ id: "main", focus: "主驱动", subscriptions: ["*"] }],
    maxConcurrent: () => 2,
  }
  const runner: PersonaRunner = { isRunning: () => false, run: async () => {} }
  const sessions: SessionResolver = {
    resolveDriver: async () => ({ sessionId: "a".repeat(32), created: false, name: "x" }),
  }
  const rt = new PersonaRuntime({ lookup, runner, sessions })
  rt.start()
  return rt
}

function makeManager(rt: PersonaRuntime, opts: { enable?: string[]; disable?: string[]; env?: Record<string, string> } = {}) {
  const logs: string[] = []
  const mgr = new EventSourceManager({
    scanDirs: dirs(),
    runtime: rt,
    home: root,
    ...(opts.enable ? { enable: opts.enable } : {}),
    ...(opts.disable ? { disable: opts.disable } : {}),
    envReader: (n) => opts.env?.[n],
    log: {
      info: (m) => logs.push(`I ${m}`),
      warn: (m) => logs.push(`W ${m}`),
      error: (m) => logs.push(`E ${m}`),
    },
  })
  return { mgr, logs }
}

describe("discoverEventSources：扫描与失败隔离", () => {
  test("扫描内置域与二开域的源", async () => {
    writeSource("builtin", "alpha", `export const source = { name: "alpha", description: "a", channel: "fa", start: async () => {} }`)
    writeSource("custom", "beta", `export const source = { name: "beta", description: "b", channel: "fb", start: async () => {} }`)
    const r = await discoverEventSources(dirs())
    expect([...r.sources.keys()].sort()).toEqual(["alpha", "beta"])
    expect(r.errors.size).toBe(0)
  })

  test("单源语法/import 抛错只记错误，其它源照常注册（失败隔离）", async () => {
    writeSource("builtin", "good", `export const source = { name: "good", description: "g", channel: "fg", start: async () => {} }`)
    writeSource("builtin", "broken", `export const source = { this is not valid typescript`)
    const r = await discoverEventSources(dirs())
    expect(r.sources.has("good")).toBe(true)
    expect(r.sources.has("broken")).toBe(false)
    expect(r.errors.get("broken")?.domain).toBe("builtin")
    expect(r.errors.get("broken")?.error).toContain("加载")
  })

  test("未导出 source 记明确错误（而非静默跳过）", async () => {
    writeSource("builtin", "nosrc", `export const something = 1`)
    const r = await discoverEventSources(dirs())
    expect(r.sources.has("nosrc")).toBe(false)
    expect(r.errors.get("nosrc")?.error).toContain("未导出 source")
  })

  test("name 与文件名不一致被拒绝（防改名后配置失配）", async () => {
    writeSource("builtin", "aaa", `export const source = { name: "bbb", description: "x", channel: "f", start: async () => {} }`)
    const r = await discoverEventSources(dirs())
    expect(r.sources.size).toBe(0)
    expect(r.errors.get("aaa")?.error).toContain("不一致")
  })

  test("channel 非法被拒绝（拼错会让路由永远匹配不上）", async () => {
    writeSource("builtin", "badch", `export const source = { name: "badch", description: "x", channel: "Feishu-Bad", start: async () => {} }`)
    const r = await discoverEventSources(dirs())
    expect(r.errors.get("badch")?.error).toContain("channel")
  })

  test("测试文件与下划线前缀不参与注册", async () => {
    writeSource("builtin", "real", `export const source = { name: "real", description: "r", channel: "fr", start: async () => {} }`)
    writeSource("builtin", "ignored.test", `throw new Error("不该被加载")`)
    writeSource("builtin", "_helper", `throw new Error("辅助文件不该被加载")`)
    const r = await discoverEventSources(dirs())
    expect([...r.sources.keys()]).toEqual(["real"])
    expect(r.errors.size).toBe(0)
  })

  test("目录形态：{name}/{name}.ts", async () => {
    const d = join(dirs().builtin, "dirsrc")
    mkdirSync(d, { recursive: true })
    writeFileSync(join(d, "dirsrc.ts"), `export const source = { name: "dirsrc", description: "d", channel: "fd", start: async () => {} }`)
    const r = await discoverEventSources(dirs())
    expect(r.sources.has("dirsrc")).toBe(true)
  })

  test("custom 同名源覆盖内置（二开语义）", async () => {
    writeSource("builtin", "dup", `export const source = { name: "dup", description: "内置版", channel: "fa", start: async () => {} }`)
    writeSource("custom", "dup", `export const source = { name: "dup", description: "二开版", channel: "fb", start: async () => {} }`)
    const r = await discoverEventSources(dirs())
    expect(r.sources.get("dup")?.description).toBe("二开版")
    expect(r.sources.get("dup")?.channel).toBe("fb")
  })

  test("域不存在不算错误（未二开形态）", async () => {
    writeSource("builtin", "only", `export const source = { name: "only", description: "o", channel: "fo", start: async () => {} }`)
    const r = await discoverEventSources({ builtin: dirs().builtin, custom: join(root, "nope") })
    expect(r.sources.size).toBe(1)
    expect(r.errors.size).toBe(0)
  })
})

describe("defaultEventSourceScanDirs：默认扫描域", () => {
  /**
   * 二开域是 **仓库内 custom/persona/**（代码域，随版本控制与迁移），
   * 与 `custom/agents` 同级同语义；**不是** {GEBAI_HOME} 下的运行时目录。
   * 这条断言把该约定钉死：改路径时必然红。
   */
  test("内置域 = 包内 src/sources，二开域 = 仓库根 custom/persona", () => {
    const d = defaultEventSourceScanDirs({ packageRoot: "/repo/packages/persona" })
    expect(d.builtin).toBe(join("/repo/packages/persona", "src", "sources"))
    // 上两级即仓库根：/repo/packages/persona → /repo/packages → /repo
    expect(d.custom).toBe(join("/repo", "custom", "persona"))
  })

  test("repoRoot 可显式覆盖（产物/测试布局用）", () => {
    const d = defaultEventSourceScanDirs({ packageRoot: "/pkg", repoRoot: "/elsewhere" })
    expect(d.custom).toBe(join("/elsewhere", "custom", "persona"))
  })
})

describe("discoverySignature：热加载判定", () => {
  test("内容不变签名稳定", async () => {
    writeSource("builtin", "s1", `export const source = { name: "s1", description: "x", channel: "f", start: async () => {} }`)
    const a = await discoverySignature(dirs())
    const b = await discoverySignature(dirs())
    expect(a).toBe(b)
  })

  test("内容修改（mtime 变化）也改变签名——否则热加载永远不触发", async () => {
    const file = join(dirs().builtin, "editme.ts")
    writeSource("builtin", "editme", `export const source = { name: "editme", description: "v1", channel: "f1", start: async () => {} }`)
    const before = await discoverySignature(dirs())
    // 改内容（并显式把 mtime 推后，避免同毫秒）
    writeFileSync(file, `export const source = { name: "editme", description: "v2", channel: "f1", start: async () => {} }`)
    const future = new Date(Date.now() + 2000)
    const { utimesSync } = await import("node:fs")
    utimesSync(file, future, future)
    expect(await discoverySignature(dirs())).not.toBe(before)
  })

  test("新增文件改变签名", async () => {
    const before = await discoverySignature(dirs())
    writeSource("builtin", "newone", `export const source = { name: "newone", description: "x", channel: "f", start: async () => {} }`)
    expect(await discoverySignature(dirs())).not.toBe(before)
  })
})

describe("validateSourceDef：字段校验", () => {
  const base = { name: "x", description: "d", channel: "c", start: () => {} }

  test("合法定义返回 null", () => {
    expect(validateSourceDef(base, "x")).toBeNull()
  })

  test("未知字段被拒绝（拼错字段会静默失效）", () => {
    expect(validateSourceDef({ ...base, chanel: "typo" }, "x")).toContain("未知字段")
  })

  test("缺 description / start 被拒绝", () => {
    expect(validateSourceDef({ name: "x", channel: "c", start: () => {} }, "x")).toContain("description")
    expect(validateSourceDef({ name: "x", description: "d", channel: "c" }, "x")).toContain("start")
  })

  test("envVars 结构校验", () => {
    expect(validateSourceDef({ ...base, envVars: [{ name: "bad name", description: "d" }] }, "x")).toContain("envVars.name")
    expect(validateSourceDef({ ...base, envVars: [{ name: "OK" }] }, "x")).toContain("description")
    expect(validateSourceDef({ ...base, envVars: [{ name: "OK", description: "d" }] }, "x")).toBeNull()
  })
})

describe("EventSourceManager：生命周期与 emit 桥接", () => {
  /**
   * 用「把 ctx 交出来」的 fake 源验证桥接语义。
   *
   * 为何不用 file_drop 做这批断言：它依赖文件轮询（1.5s）与模块级实例状态，
   * 在多用例顺序执行时互相干扰，且失败时难以区分「桥接逻辑错」与「时序没等到」。
   * 桥接逻辑是纯函数式的一层，用 fake 才能精确断言语义；真实链路单独一条。
   */
  async function withCtx(
    opts: { routes?: boolean } = {},
  ): Promise<{ mgr: EventSourceManager; ctx: EventSourceContext }> {
    const home = join(root, "home-" + crypto.randomUUID())
    mkdirSync(home, { recursive: true })
    const rt = makeRuntime(opts.routes === false ? { routes: false } : {})
    let captured: EventSourceContext | null = null
    const fake: EventSourceDef = {
      name: "fake",
      description: "测试用源",
      channel: "fake",
      async start(ctx) {
        captured = ctx
      },
    }
    const mgr = new EventSourceManager({
      scanDirs: { builtin: join(root, "no-builtin"), custom: join(root, "no-custom") },
      runtime: rt,
      home,
      envReader: () => undefined,
      log: { info: () => {}, warn: () => {}, error: () => {} },
    })
    // 直接注册 fake（绕过扫描：本组只测桥接，扫描另有专门用例）
    mgr.register(fake)
    await mgr.startOne("fake")
    if (!captured) throw new Error("fake 源未被启动")
    return { mgr, ctx: captured }
  }

  const ingress = (over: Partial<IngressEvent> = {}): IngressEvent => ({
    kind: "message",
    channel: "fake",
    source: "测试源",
    summary: "测试事件",
    body: "帮我看看",
    refs: { chatId: "oc_1", messageId: "om_1" },
    ...over,
  })

  test("emit 被接管 → accepted，且计数正确", async () => {
    const { mgr, ctx } = await withCtx()
    const res = await ctx.emit(ingress())
    expect(res.status).toBe("accepted")
    const st = mgr.state().find((s) => s.name === "fake")!
    expect(st.stats.emitted).toBe(1)
    expect(st.stats.accepted).toBe(1)
  })

  test("无归属人格 → unattended（插件可见，不误吞为 accepted）", async () => {
    const { mgr, ctx } = await withCtx({ routes: false })
    const res = await ctx.emit(ingress())
    expect(res.status).toBe("unattended")
    const st = mgr.state().find((s) => s.name === "fake")!
    expect(st.stats.unattended).toBe(1)
    expect(st.stats.accepted).toBe(0)
  })

  test("同 id 重复 emit → 第二次 duplicate（幂等穿透到插件层可见）", async () => {
    const { mgr, ctx } = await withCtx()
    const e = ingress({ id: "fixed-id" })
    expect((await ctx.emit(e)).status).toBe("accepted")
    expect((await ctx.emit(e)).status).toBe("duplicate")
    const st = mgr.state().find((s) => s.name === "fake")!
    expect(st.stats.accepted).toBe(1)
    expect(st.stats.duplicate).toBe(1)
  })

  test("插件显式 id 覆盖推导（同内容不同 id = 不同事件）", async () => {
    const { ctx } = await withCtx()
    expect((await ctx.emit(ingress({ id: "a" }))).status).toBe("accepted")
    expect((await ctx.emit(ingress({ id: "b" }))).status).toBe("accepted")
  })

  test("无显式 id 时按 kind 推导幂等键（同内容重复投递被去重）", async () => {
    const { ctx } = await withCtx()
    const e = ingress({ refs: { chatId: "oc_9", messageId: "om_same" } })
    expect((await ctx.emit(e)).status).toBe("accepted")
    expect((await ctx.emit({ ...e, id: undefined })).status).toBe("duplicate")
  })

  test("ctx.log 带源名前缀", async () => {
    const logs: string[] = []
    const home = join(root, "home-" + crypto.randomUUID())
    mkdirSync(home, { recursive: true })
    const rt = makeRuntime()
    const fake: EventSourceDef = {
      name: "logger",
      description: "x",
      channel: "logger",
      async start(ctx) {
        ctx.log("info", "hello")
      },
    }
    const mgr = new EventSourceManager({
      scanDirs: { builtin: join(root, "n1"), custom: join(root, "n2") },
      runtime: rt,
      home,
      log: { info: (m) => logs.push(m), warn: (m) => logs.push(m), error: (m) => logs.push(m) },
    })
    mgr.register(fake)
    await mgr.startOne("logger")
    expect(logs.some((l) => l.includes("[logger]") && l.includes("hello"))).toBe(true)
  })
})

describe("EventSourceManager：启停名单与环境变量白名单", () => {
  test("白名单非空仅启用名单内；黑名单移除", async () => {
    for (const n of ["a", "b", "c"]) {
      writeSource("builtin", n, `export const source = { name: "${n}", description: "x", channel: "f${n}", start: async () => {} }`)
    }
    const rt = makeRuntime()
    {
      const { mgr } = makeManager(rt, { enable: ["a", "b"] })
      await mgr.start()
      expect(mgr.state().filter((s) => s.started).map((s) => s.name).sort()).toEqual(["a", "b"])
      await mgr.stop()
    }
    {
      const { mgr } = makeManager(rt, { disable: ["b"] })
      await mgr.start()
      expect(mgr.state().filter((s) => s.started).map((s) => s.name).sort()).toEqual(["a", "c"])
      await mgr.stop()
    }
  })

  test("名单中的未知名告警（拼错名单 = 源静默不启用）", async () => {
    writeSource("builtin", "real", `export const source = { name: "real", description: "x", channel: "fr", start: async () => {} }`)
    const rt = makeRuntime()
    const { mgr, logs } = makeManager(rt, { enable: ["real", "typo_name"] })
    await mgr.start()
    expect(logs.some((l) => l.includes("typo_name") && l.includes("不存在"))).toBe(true)
  })

  test("ctx.env 只允许读声明过的变量（未声明拒绝并告警）", async () => {
    writeSource(
      "builtin",
      "envsrc",
      `export const source = { name: "envsrc", description: "x", channel: "fe",
         envVars: [{ name: "MY_TOKEN", description: "令牌" }],
         start: async (ctx) => {
           globalThis.__envRead = { allowed: ctx.env("MY_TOKEN"), denied: ctx.env("GEBAI_LLM_API_KEY") }
         } }`,
    )
    const rt = makeRuntime()
    const { mgr, logs } = makeManager(rt, { env: { MY_TOKEN: "secret-ok", GEBAI_LLM_API_KEY: "must-not-leak" } })
    await mgr.start()
    const read = (globalThis as Record<string, unknown>).__envRead as { allowed?: string; denied?: string }
    expect(read.allowed).toBe("secret-ok")
    expect(read.denied).toBeUndefined() // 关键：敏感变量读不到
    expect(logs.some((l) => l.includes("GEBAI_LLM_API_KEY") && l.includes("未声明"))).toBe(true)
    delete (globalThis as Record<string, unknown>).__envRead
  })

  /**
   * 启动阶段异常**绝不冒泡**：`start()` 一旦抛出，调用方（compose 装配）会整体失败，
   * 结果是「一个源配错 → 全部源不可用 → 人格完全不被唤醒」。这是失败隔离在启动侧的对应面，
   * 与「加载期失败隔离」同等重要（一个在扫描、一个在启动）。
   */
  test("单源 start 抛错不冒泡（否则一个坏源让全部源不可用）", async () => {
    const home = join(root, "hb-" + crypto.randomUUID())
    mkdirSync(home, { recursive: true })
    const rt = makeRuntime()
    const bad: EventSourceDef = {
      name: "bad",
      description: "x",
      channel: "bad",
      async start() {
        throw new Error("连接超时")
      },
    }
    const good: EventSourceDef = { name: "good", description: "x", channel: "good", async start() {} }
    const mgr = new EventSourceManager({
      scanDirs: { builtin: join(root, "nb"), custom: join(root, "nc") },
      runtime: rt,
      home,
      log: { info: () => {}, warn: () => {}, error: () => {} },
    })
    mgr.register(good)
    mgr.register(bad)
    // 关键断言：start() 本身 resolve（不抛）
    await expect(mgr.start()).resolves.toBeUndefined()
    expect(mgr.state().find((s) => s.name === "good")!.started).toBe(true)
    expect(mgr.state().find((s) => s.name === "bad")!.started).toBe(false)
    expect(mgr.loadErrors().bad).toContain("连接超时")
  })

  /** register 是程序化入口，非法定义必须被拒（否则绕过扫描期的字段校验）。 */
  test("register 拒绝非法定义（不能绕过扫描期校验）", async () => {
    const home = join(root, "hr-" + crypto.randomUUID())
    mkdirSync(home, { recursive: true })
    const rt = makeRuntime()
    const mgr = new EventSourceManager({
      scanDirs: { builtin: join(root, "nb2"), custom: join(root, "nc2") },
      runtime: rt,
      home,
      log: { info: () => {}, warn: () => {}, error: () => {} },
    })
    // channel 非法（含大写/连字符）应被拒
    mgr.register({ name: "badsrc", description: "x", channel: "Bad-Channel", async start() {} })
    expect(mgr.list()).toHaveLength(0)
    expect(mgr.loadErrors().badsrc).toContain("注册被拒绝")
    // 合法定义可注册
    mgr.register({ name: "oksrc", description: "x", channel: "ok", async start() {} })
    expect(mgr.list().map((s) => s.name)).toEqual(["oksrc"])
  })
})

describe("EventSourceManager：热加载", () => {
  test("目录签名变化时 refreshIfChanged 重扫并重启", async () => {
    writeSource("builtin", "h1", `export const source = { name: "h1", description: "v1", channel: "f1", start: async () => {} }`)
    const rt = makeRuntime()
    const { mgr } = makeManager(rt)
    await mgr.start()
    expect(mgr.list()).toHaveLength(1)
    // 无变化 → 不重扫
    expect(await mgr.refreshIfChanged()).toBe(false)
    // 新增一个源 → 重扫
    writeSource("builtin", "h2", `export const source = { name: "h2", description: "v2", channel: "f2", start: async () => {} }`)
    expect(await mgr.refreshIfChanged()).toBe(true)
    expect(mgr.list().map((s) => s.name).sort()).toEqual(["h1", "h2"])
    expect(mgr.state().filter((s) => s.started)).toHaveLength(2)
  })

  test("热加载后删除的源不再注册", async () => {
    writeSource("builtin", "gone", `export const source = { name: "gone", description: "x", channel: "fg", start: async () => {} }`)
    const rt = makeRuntime()
    const { mgr } = makeManager(rt)
    await mgr.start()
    expect(mgr.list()).toHaveLength(1)
    rmSync(join(dirs().builtin, "gone.ts"))
    expect(await mgr.refreshIfChanged()).toBe(true)
    expect(mgr.list()).toHaveLength(0)
  })
})

describe("EventSourceManager：参考实现 file_drop", () => {
  test("内置示例源可被发现并启动（真实全链路）", async () => {
    const rt = makeRuntime()
    // 指向本包的 sources 目录（内置示例真正生效的路径）
    const builtin = join(import.meta.dir, "sources")
    const logs: string[] = []
    const mgr = new EventSourceManager({
      scanDirs: { builtin, custom: join(root, "none") },
      runtime: rt,
      home: root,
      enable: ["file_drop"],
      envReader: () => undefined,
      log: { info: (m) => logs.push(m), warn: (m) => logs.push(m), error: (m) => logs.push(m) },
    })
    await mgr.start()
    const st = mgr.state().find((s) => s.name === "file_drop")!
    expect(st.started).toBe(true)
    expect(st.channel).toBe("filedrop")
    // 投递一个 JSON → 被接管并删除
    const dropDir = join(root, "persona-drop")
    mkdirSync(dropDir, { recursive: true })
    writeFileSync(join(dropDir, "t1.json"), JSON.stringify({ kind: "message", source: "投递", summary: "测试", body: "帮我看看" }))
    // 轮询间隔 1.5s，等待首次扫描（start 内立即 poll 一次）+ 竞态余量
    const deadline = Date.now() + 4000
    while (Date.now() < deadline) {
      const cur = mgr.state().find((s) => s.name === "file_drop")!
      if (cur.stats.emitted > 0) break
      await new Promise((r) => setTimeout(r, 100))
    }
    const after = mgr.state().find((s) => s.name === "file_drop")!
    expect(after.stats.emitted).toBeGreaterThan(0)
    expect(after.stats.accepted).toBeGreaterThan(0)
    await mgr.stop()
  }, 15000)
})

describe("端到端：内置参考源 file_drop", () => {
  /**
   * 唯一一条真实链路用例：投递 JSON → file_drop 轮询 → ctx.emit → runtime 接管。
   *
   * 为何只保留一条：它依赖 1.5s 轮询间隔与模块级实例状态，多条同类用例会互相干扰
   * （前一条的 timer/停在后台仍在跑）。桥接语义的细粒度断言交由上面的 fake 源覆盖。
   */
  test("投递文件被接管并删除（真实全链路）", async () => {
    const home = join(root, "e2e-" + crypto.randomUUID())
    mkdirSync(home, { recursive: true })
    const rt = makeRuntime()
    const builtin = join(import.meta.dir, "sources")
    const logs: string[] = []
    const mgr = new EventSourceManager({
      scanDirs: { builtin, custom: join(home, "none") },
      runtime: rt,
      home,
      enable: ["file_drop"],
      envReader: () => undefined,
      log: { info: (m) => logs.push(m), warn: (m) => logs.push(m), error: (m) => logs.push(m) },
    })
    await mgr.start()
    const st = mgr.state().find((s) => s.name === "file_drop")
    expect(st?.started).toBe(true)
    expect(st?.channel).toBe("filedrop")

    const drop = join(home, "persona-drop")
    mkdirSync(drop, { recursive: true })
    writeFileSync(join(drop, "t.json"), JSON.stringify({ kind: "message", source: "投递", summary: "测试", body: "帮我看看" }))

    const deadline = Date.now() + 8000
    let stats = mgr.state().find((s) => s.name === "file_drop")!.stats
    while (Date.now() < deadline && stats.accepted === 0) {
      await new Promise((r) => setTimeout(r, 100))
      stats = mgr.state().find((s) => s.name === "file_drop")!.stats
    }
    expect(stats.emitted).toBeGreaterThan(0)
    expect(stats.accepted).toBeGreaterThan(0)
    // 处理完即删（不残留）
    expect(existsSync(join(drop, "t.json"))).toBe(false)
    await mgr.stop()
  }, 20000)
})
