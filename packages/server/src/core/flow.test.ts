import { describe, expect, test } from "bun:test"
import { mkdtempSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import { evalExpr, parseExpr, resolveTemplate, runFlow, scanFlowApprovals, normalizeSteps, FLOW_FOREACH_MAX, FLOW_WHILE_HARD_MAX } from "./flow"
import type { Tool, ToolContext } from "./types"
import { createGlobalTools, shTool } from "./tools"

function baseCtx(home: string): ToolContext {
  return {
    user: "default",
    sessionId: "s1",
    workdir: home,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => resolve(home, p),
    readFile: async () => "",
    readBinaryFile: async () => new Uint8Array(),
    writeFile: async () => {},
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => {
      throw new Error("未知预置项目")
    },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    runNewSession: async () => ({ output: "ok", archive: { runId: "r", agents: ["x"], input: "", output: "ok", messages: [] } }),
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
    waitForCapture: async () => null,
  }
}

/** 构造带 mock 工具注册表的执行环境；calls 记录每次工具调用的 (name, params)。 */
function flowCtx(tools: Record<string, Tool>, calls?: Array<{ name: string; params: Record<string, unknown> }>, opts?: { safeMode?: boolean }): ToolContext {
  const home = mkdtempSync(join(tmpdir(), "gebai-flow-"))
  const c = baseCtx(home)
  c.safeMode = opts?.safeMode
  c.registry = {
    schemas: () => [],
    resolve: (name) => (tools[name] ? { name, tool: tools[name] } : undefined),
    getAgentNames: () => [],
  }
  const origExecute = Object.fromEntries(Object.entries(tools).map(([n, t]) => [n, t.execute]))
  for (const [name, tool] of Object.entries(tools)) {
    tool.execute = async (params, ctx) => {
      calls?.push({ name, params })
      return origExecute[name](params, ctx)
    }
  }
  return c
}

const tool = (name: string, fn: (args: Record<string, unknown>) => unknown, extra: Partial<Tool> = {}): Tool => ({
  name,
  description: "",
  parameters: { type: "object", properties: {} },
  async execute(args) {
    const r = await fn(args)
    return typeof r === "string" ? { output: r } : { output: JSON.stringify(r), data: r }
  },
  ...extra,
})

describe("flow 表达式", () => {
  const scope = {
    s1: { id: "s1", tool: "t", kind: "tool", status: "done", output: "hello", data: { n: 3, list: [1, 2, 3], path: "a/b.txt", ok: true } },
    item: { name: "x", tags: ["a", "b"] },
    input: "seed",
  }
  test("引用与路径访问", () => {
    expect(evalExpr("s1.data.n", scope)).toBe(3)
    expect(evalExpr("s1.data.list[1]", scope)).toBe(2)
    expect(evalExpr("s1.data.list.length", scope)).toBe(3)
    expect(evalExpr("s1.output", scope)).toBe("hello")
    expect(evalExpr("item.tags.length", scope)).toBe(2)
    expect(evalExpr("s1.data.missing", scope)).toBeUndefined()
    expect(evalExpr("input", scope)).toBe("seed")
  })
  test("比较与逻辑", () => {
    expect(evalExpr("s1.data.n == 3", scope)).toBe(true)
    expect(evalExpr("s1.data.n == '3'", scope)).toBe(true)
    expect(evalExpr("s1.data.n != 4", scope)).toBe(true)
    expect(evalExpr("s1.data.n > 2 && s1.data.ok", scope)).toBe(true)
    expect(evalExpr("!s1.data.ok || false", scope)).toBe(false)
    expect(evalExpr("(s1.data.n > 10 || s1.data.ok) && s1.data.list.length >= 3", scope)).toBe(true)
    expect(evalExpr("'b' > 'a'", scope)).toBe(true)
  })
  test("函数 len/contains/exists", () => {
    expect(evalExpr("len(s1.data.list)", scope)).toBe(3)
    expect(evalExpr("contains(s1.data.list, 2)", scope)).toBe(true)
    expect(evalExpr("contains(s1.output, 'ell')", scope)).toBe(true)
    expect(evalExpr("exists(s1.data.path)", scope)).toBe(true)
    expect(evalExpr("exists(s1.data.missing)", scope)).toBe(false)
  })
  test("空数组视为假", () => {
    const s = { ...scope, s1: { ...scope.s1, data: { ...scope.s1.data, empty: [] } } }
    expect(evalExpr("s1.data.empty && true", s)).toBe(false)
    expect(evalExpr("s1.data.list && true", scope)).toBe(true)
  })
  test("解析错误抛出", () => {
    expect(() => parseExpr("s1.data.n ==")).toThrow()
    expect(() => parseExpr("1 +")).toThrow()
    expect(() => evalExpr("unknownFn(1)", scope)).toThrow(/未知函数/)
  })
})

describe("flow 模板插值", () => {
  const scope = { s1: { data: { n: 3, list: [1, 2] } } as unknown }
  test("整值引用保类型", () => {
    expect(resolveTemplate("{{s1.data.n}}", scope)).toBe(3)
    expect(resolveTemplate("{{ s1.data.list }}", scope)).toEqual([1, 2])
  })
  test("混排字符串拼接", () => {
    expect(resolveTemplate("第{{s1.data.n}}项", scope)).toBe("第3项")
  })
  test("对象与数组递归插值", () => {
    expect(resolveTemplate({ a: "{{s1.data.n}}", b: ["x{{s1.data.n}}"] }, scope)).toEqual({ a: 3, b: ["x3"] })
  })
  test("无模板字符串原样", () => {
    expect(resolveTemplate("plain", scope)).toBe("plain")
  })
})

describe("runFlow 数据流编排", () => {
  test("旧版线性格式兼容：stdin 自动注入与 JSON 字段映射", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        producer: tool("producer", () => ({ path: "data.json", extra: 1 })),
        mapper: tool("mapper", () => "mapped"),
        sh: tool("sh", () => "out", { parameters: { type: "object", properties: { command: { type: "string" }, input: { type: "string" } } } }),
        cat: tool("sh2", () => "cat"),
      },
      calls,
    )
    // JSON 字段映射：producer 的 path 注入 mapper 的 path 参数（mapper 声明 path 入参）
    const t = (c.registry.resolve("mapper")!.tool as Tool)
    t.parameters = { type: "object", properties: { path: { type: "string" } } }
    const r = await runFlow({ steps: [{ tool: "producer" }, { tool: "mapper" }] }, c)
    expect(r.output).toContain("producer")
    expect(r.output).toContain("mapped")
    expect(calls[1].params.path).toBe("data.json")
    expect(calls[1].params.extra).toBeUndefined()
    expect(Array.isArray((r.data as { steps: unknown[] }).steps)).toBe(true)
  })

  test("sh/py 步骤 stdin 自动注入", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx({ sh: tool("sh", (a) => `cmd:${a.command}:in:${a.input ?? ""}`) }, calls)
    await runFlow({ steps: [{ tool: "sh", params: { command: "echo a" } }, { tool: "sh", params: { command: "cat" } }] }, c)
    expect(calls[1].params.input).toBe("cmd:echo a:in:")
  })

  test("id 引用 + input 显式映射（改名/多对一）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        list: tool("list", () => ({ files: ["a.md", "b.md"] })),
        read: tool("read", () => "content"),
        write: tool("write", (a) => `wrote:${a.path}:${a.content}`),
      },
      calls,
    )
    const r = await runFlow(
      {
        steps: [
          { id: "list", tool: "list" },
          { id: "first", tool: "read", input: { path: "{{list.data.files[0]}}" } },
          { tool: "write", input: { path: "all.md", content: "{{first.output}}" } },
        ],
      },
      c,
    )
    expect(calls[1].params.path).toBe("a.md")
    expect(calls[2].params.content).toBe("content")
    expect(r.output).toContain("wrote:all.md:content")
    // 显式映射抑制自动注入：write 不再收到 input 字段
    expect(calls[2].params.input).toBeUndefined()
  })

  test("when 条件分支：为假跳过且不影响后续", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        probe: tool("probe", () => ({ ok: false })),
        heavy: tool("heavy", () => "ran"),
        next: tool("next", (a) => `next:${a.input ?? ""}`),
      },
      calls,
    )
    const r = await runFlow(
      {
        steps: [
          { id: "probe", tool: "probe" },
          { tool: "heavy", when: "probe.data.ok == true" },
          { tool: "next" },
        ],
      },
      c,
    )
    expect(calls.map((x) => x.name)).toEqual(["probe", "next"])
    expect(r.output).toContain("跳过")
    // 跳过步骤不更新 prev：next 的自动注入基于 probe 的输出
    expect(calls[1].params.input).toBe(JSON.stringify({ ok: false }))
  })

  test("when/while 条件支持 {{}} 包裹与混排语法（与裸表达式等价）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        gen: tool("gen", () => ({ stdout: "5" })),
        hit: tool("hit", (a) => `hit:${a.input ?? ""}`),
        miss: tool("miss", () => "miss"),
      },
      calls,
    )
    const r = await runFlow(
      {
        steps: [
          { id: "gen", tool: "gen" },
          { tool: "hit", when: "{{gen.data.stdout}} == '5'" }, // 混排包裹
          { tool: "miss", when: "{{gen.data.stdout}} == '6'" }, // 为假跳过
          { tool: "hit", when: "{{exists(gen)}}" }, // 整值包裹
        ],
      },
      c,
    )
    expect(calls.map((x) => x.name)).toEqual(["gen", "hit", "hit"])
    expect(r.output).toContain("跳过")
  })

  test("foreach 数字（非字符串）按次数循环", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx({ tick: tool("tick", () => "t") }, calls)
    await runFlow({ steps: [{ foreach: 3, steps: [{ tool: "tick" }] }] }, c)
    expect(calls.length).toBe(3)
  })

  test("foreach JSON 数组文本自动解析（脚本 stdout 形态）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        gen: tool("gen", () => ({ stdout: "[10,20,30]" })),
        tick: tool("tick", (a) => `t:${a.v ?? ""}`),
      },
      calls,
    )
    await runFlow(
      {
        steps: [
          { id: "gen", tool: "gen" },
          { foreach: "{{gen.data.stdout}}", steps: [{ tool: "tick", input: { v: "{{item}}" } }] },
        ],
      },
      c,
    )
    expect(calls.filter((x) => x.name === "tick").map((x) => x.params.v)).toEqual([10, 20, 30])
  })

  test("foreach 裸 JSON 数组文本直接解析（无需表达式包裹）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx({ tick: tool("tick", (a) => `t:${a.v ?? ""}`) }, calls)
    await runFlow({ steps: [{ foreach: "[10,20,30]", steps: [{ tool: "tick", input: { v: "{{item}}" } }] }] }, c)
    expect(calls.map((x) => x.params.v)).toEqual([10, 20, 30])
    // 非法文本报错信息可读（附原始解析错误与正确写法）
    await expect(runFlow({ steps: [{ id: "g", foreach: "[a,b]", steps: [{ tool: "tick" }] }] }, c)).rejects.toThrow(/foreach 无效/)
  })

  test("foreach 快照语义：循环体修改源数组不影响迭代次数", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    // 源数组为共享引用：循环体内 shrink 原地缩短它（模拟「删除后重新查询/原地清理」类流程）
    const shared = ["a", "b", "c", "d", "e"]
    const c = flowCtx(
      {
        src: tool("src", () => ({ files: shared })),
        shrink: tool("shrink", () => {
          shared.splice(0, 2)
          return { files: shared }
        }),
        del: tool("del", (a) => `del:${a.id ?? ""}`),
      },
      calls,
    )
    const r = await runFlow(
      {
        steps: [
          { id: "src", tool: "src" },
          {
            id: "g",
            foreach: "{{src.data.files}}",
            steps: [
              { tool: "del", input: { id: "{{item}}" } },
              { tool: "shrink" },
            ],
          },
        ],
      },
      c,
    )
    const g = ((r.data as { steps: Array<{ id: string; runs?: number }> }).steps).find((s) => s.id === "g")!
    expect(g.runs).toBe(5) // 快照：迭代次数固定为求值时的长度，不随源数组缩短
    expect(calls.filter((x) => x.name === "del").map((x) => x.params.id)).toEqual(["a", "b", "c", "d", "e"])
  })

  test("嵌套 foreach：内层 item/index 遮蔽外层同名引用（作用域语义）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        add: tool("add", (a) => `add:${a.title}`),
      },
      calls,
    )
    await runFlow(
      {
        steps: [
          {
            foreach: "2",
            steps: [
              {
                foreach: "2",
                steps: [{ tool: "add", input: { title: "inner-{{item}}-idx{{index}}" } }],
              },
            ],
          },
        ],
      },
      c,
    )
    // 内层 item/index 完全遮蔽外层：4 次 add 标题均为内层值（0/1）
    expect(calls.map((x) => x.params.title)).toEqual(["inner-0-idx0", "inner-1-idx1", "inner-0-idx0", "inner-1-idx1"])
  })

  test("步骤 input 字符串作为 input 参数模板值（组内 {{item}} 直传 stdin）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        list: tool("list", () => ({ files: ["a.md", "b.md"] })),
        cat: tool("cat", (a) => `cat:${a.input ?? ""}`),
      },
      calls,
    )
    await runFlow(
      {
        steps: [
          { id: "list", tool: "list" },
          { foreach: "{{list.data.files}}", steps: [{ tool: "cat", input: "{{item}}" }] },
        ],
      },
      c,
    )
    expect(calls.filter((x) => x.name === "cat").map((x) => x.params.input)).toEqual(["a.md", "b.md"])
  })

  test("foreach 对象项经 input 直传 stdin 保留为对象（工具侧序列化为 JSON 文本）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        cat: tool("cat", (a) => `cat:${a.input ?? ""}`),
      },
      calls,
    )
    await runFlow(
      {
        steps: [
          { foreach: '[{"name":"a","v":1},{"name":"b","v":2}]', steps: [{ tool: "cat", input: "{{item}}" }] },
        ],
      },
      c,
    )
    // 整值 {{item}} 保原始类型：对象原样传入（py/sh 工具侧 JSON 化），不丢类型
    expect(calls.map((x) => x.params.input)).toEqual([{ name: "a", v: 1 }, { name: "b", v: 2 }])
  })

  test("引用不存在的路径解析为空（不报错，可用 exists() 判定）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        gen: tool("gen", () => ({ ok: true })),
        echo: tool("echo", (a) => `in:[${a.input ?? ""}]`),
        probe: tool("probe", (a) => `probe:${a.path ?? "none"}`),
      },
      calls,
    )
    await runFlow(
      {
        steps: [
          { id: "gen", tool: "gen" },
          { tool: "probe", input: { path: "{{gen.stdout}}" } }, // 步骤对象无 stdout 字段 → 空串
          { tool: "echo", input: "{{gen.missing.deep}}" }, // 缺失路径 → 空串
          { tool: "echo", when: "!exists(gen.data.nope)" }, // exists() 判缺失
        ],
      },
      c,
    )
    // 整值引用缺失路径 → undefined 原样传递（类型保真）；混排字符串缺失 → 空串拼接
    expect(calls[1].params.path).toBeUndefined()
    expect(calls[2].params.input).toBeUndefined()
    expect(calls.length).toBe(4) // when 为真（缺失判定成立）→ 第 4 步执行
  })

  test("foreach 数据循环：item/index 引用与扇出收集", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        list: tool("list", () => ({ files: [{ path: "a.md" }, { path: "b.md" }, { path: "c.md" }] })),
        read: tool("read", (a) => `content-of-${a.path}`),
      },
      calls,
    )
    const r = await runFlow(
      {
        steps: [
          { id: "list", tool: "list" },
          {
            id: "batch",
            foreach: "{{list.data.files}}",
            steps: [{ tool: "read", input: { path: "{{item.path}}" } }, { tool: "read", input: { path: "idx-{{index}}.txt" } }],
          },
        ],
      },
      c,
    )
    expect(calls.filter((x) => x.params.path === "a.md").length).toBe(1)
    expect(calls.some((x) => x.params.path === "idx-2.txt")).toBe(true)
    const steps = (r.data as { steps: Array<{ id: string; data: unknown; runs?: number }> }).steps
    const batch = steps.find((s) => s.id === "batch")!
    expect(batch.runs).toBe(3)
    expect((batch.data as unknown[]).length).toBe(3)
  })

  test("foreach 计数模式", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx({ tick: tool("tick", (a) => `t${a.i ?? ""}`) }, calls)
    const r = await runFlow({ steps: [{ id: "g", foreach: "3", steps: [{ tool: "tick", input: { i: "{{index}}" } }] }] }, c)
    expect(calls.length).toBe(3)
    const g = ((r.data as { steps: Array<{ id: string; runs?: number }> }).steps).find((s) => s.id === "g")!
    expect(g.runs).toBe(3)
  })

  test("while 条件循环与轮数上限", async () => {
    let n = 0
    const c = flowCtx({ tick: tool("tick", () => ({ n: ++n, more: n < 10 })) })
    const r = await runFlow({ steps: [{ id: "g", while: "g.data.more == true", maxLoops: 3, steps: [{ tool: "tick" }] }] }, c)
    const g = ((r.data as { steps: Array<{ id: string; runs?: number }> }).steps).find((s) => s.id === "g")!
    expect(g.runs).toBe(3)
    expect(r.output).toContain("已达轮数上限 3")
  })

  test("optional 容错：失败继续；未声明则中断并报位置", async () => {
    const boom: Tool = {
      name: "boom",
      description: "",
      parameters: { type: "object", properties: {} },
      async execute() {
        throw new Error("炸了")
      },
    }
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c1 = flowCtx({ boom, after: tool("after", () => "after") }, calls)
    const r1 = await runFlow({ steps: [{ id: "b", tool: "boom", optional: true }, { tool: "after" }] }, c1)
    expect(r1.output).toContain("失败")
    expect(calls.map((x) => x.name)).toContain("after")
    const c2 = flowCtx({ boom, after: tool("after", () => "after") }, calls)
    await expect(runFlow({ steps: [{ id: "b2", tool: "boom" }, { tool: "after" }] }, c2)).rejects.toThrow(/b2.*失败.*炸了/)
  })

  test("未知工具报错", async () => {
    const c = flowCtx({})
    await expect(runFlow({ steps: [{ tool: "nope" }] }, c)).rejects.toThrow(/未知工具 nope/)
  })

  test("安全模式：cron 调度类 step 层硬阻断；风险工具 step 放行（降级在工具 execute 内）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx({ cron_add: tool("cron_add", () => "added"), sh: tool("sh", () => "ran") }, calls, { safeMode: true })
    const r = await runFlow({ steps: [{ tool: "cron_add", params: {} }, { tool: "sh", params: { command: "ls" } }] }, c)
    // cron 调度无法降级：step 层拦截返回限制信息；sh 等风险工具不再一刀切拦截（工具内白名单降级）
    expect(calls.map((x) => x.name)).toEqual(["sh"])
    expect(r.output).toContain("安全模式")
    expect(r.output).toContain("ran")
  })

  test("规模上限：foreach 超限报错", async () => {
    const c = flowCtx({ list: tool("list", () => ({ items: Array.from({ length: FLOW_FOREACH_MAX + 1 }, (_, i) => i) })), t: tool("t", () => "x") })
    await expect(
      runFlow({ steps: [{ id: "l", tool: "list" }, { foreach: "{{l.data.items}}", steps: [{ tool: "t" }] }] }, c),
    ).rejects.toThrow(/超上限/)
  })

  test("步骤校验：保留名/重复 id/缺 foreach|while", async () => {
    expect(() => normalizeSteps([{ id: "prev", tool: "x" }])).toThrow(/保留名/)
    expect(() => normalizeSteps([{ id: "a", tool: "x" }, { id: "a", tool: "x" }])).toThrow(/重复/)
    expect(() => normalizeSteps([{ steps: [{ tool: "x" }] }])).toThrow(/foreach 或 while/)
    expect(() => normalizeSteps("bad")).toThrow(/数组/)
    expect(() => normalizeSteps([{ tool: "x" }, { id: "s1", tool: "y" }])).not.toThrow() // 自动 id 跳过显式 id 不冲突
  })

  test("while 硬上限钳制", async () => {
    let n = 0
    const c = flowCtx({ t: tool("t", () => ({ more: ++n < 999 })) })
    const r = await runFlow({ steps: [{ while: "true", maxLoops: 999, steps: [{ tool: "t" }] }] }, c)
    expect(r.output).toContain(`已达轮数上限 ${FLOW_WHILE_HARD_MAX}`)
  })

  test("timeout 整体超时：步骤边界中止并返回已执行部分（不抛错）", async () => {
    const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx(
      {
        fast: tool("fast", () => "f"),
        slow: tool("slow", async () => {
          await sleep(100)
          return "s"
        }),
        never: tool("never", () => "n"),
      },
      calls,
    )
    const r = await runFlow(
      { steps: [{ tool: "fast" }, { tool: "slow" }, { tool: "never" }], timeout: 0.09 },
      c,
    )
    // fast 立即 + slow 100ms：slow 执行前未超预算（<90ms），slow 后预算耗尽：never 未执行；
    // 超时作为部分结果返回而非错误
    expect(calls.map((x) => x.name)).toEqual(["fast", "slow"])
    expect(r.output).toContain("执行超时")
    const data = r.data as { timedOut?: boolean; executed?: number }
    expect(data.timedOut).toBe(true)
    expect(data.executed).toBe(2)
  })

  test("timeout 截断失控 while 循环（轮首检查）", async () => {
    const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))
    let n = 0
    const c = flowCtx({
      t: tool("t", async () => {
        await sleep(50)
        return { more: ++n < 999 }
      }),
    })
    const r = await runFlow({ steps: [{ id: "g", while: "{{g.data.more}}", steps: [{ tool: "t" }] }], timeout: 0.2 }, c)
    // 每轮约 50ms：第 5 轮前（≈200ms）超预算截断，执行 4 轮后停止
    expect(r.output).toContain("执行超时")
    expect((r.data as { executed?: number }).executed).toBe(4)
    expect(n).toBeLessThan(999)
  })

  test("sh strict: non-zero exit interrupts flow; optional tolerates", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const sh: Tool = {
      name: "sh",
      description: "",
      parameters: { type: "object", properties: {} },
      requiresApproval: true,
      async execute(args) {
        const strict = args.strict === true
        const fail = String(args.command ?? "").includes("fail")
        if (strict && fail) throw new Error("命令执行失败（exit 1）：boom")
        return { output: `ok:${args.command}`, data: { stdout: `out:${args.command}`, exitCode: fail ? 1 : 0 } }
      },
    }
    const c = flowCtx({ sh, after: tool("after", () => "after") }, calls)
    // strict 非 0 中断整个 flow
    await expect(
      runFlow({ steps: [{ id: "s", tool: "sh", params: { command: "fail", strict: true } }, { tool: "after" }] }, c),
    ).rejects.toThrow(/exit 1/)
    // optional 容错：strict 失败不中断
    const r = await runFlow({ steps: [{ id: "s", tool: "sh", params: { command: "fail", strict: true }, optional: true }, { tool: "after" }] }, c)
    expect(r.output).toContain("after")
    // 非 strict 默认不中断（非 0 是正常结果）
    const r2 = await runFlow({ steps: [{ id: "s", tool: "sh", params: { command: "fail" } }, { tool: "after" }] }, c)
    expect(r2.output).toContain("after")
  })

  test("timeout 非法/缺省不限制（行为不变）", async () => {
    const calls: Array<{ name: string; params: Record<string, unknown> }> = []
    const c = flowCtx({ t: tool("t", () => "x") }, calls)
    // 非法值（0/负数/非数字）与缺省等价：全部步骤照常执行
    await runFlow({ steps: [{ tool: "t" }, { tool: "t" }], timeout: 0 }, c)
    await runFlow({ steps: [{ tool: "t" }], timeout: -5 }, c)
    await runFlow({ steps: [{ tool: "t" }], timeout: "abc" }, c)
    await runFlow({ steps: [{ tool: "t" }] }, c)
    expect(calls.length).toBe(5)
  })
})

describe("scanFlowApprovals 审批扫描", () => {
  const risky: Tool = { name: "sh", description: "", parameters: { type: "object", properties: {} }, requiresApproval: true, async execute() { return { output: "" } } }
  const safe: Tool = { name: "read", description: "", parameters: { type: "object", properties: {} }, async execute() { return { output: "" } } }
  test("内部含需审批工具 → 整体需审批（含嵌套循环体）", async () => {
    const c = flowCtx({ read: safe, sh: risky })
    expect(await scanFlowApprovals([{ tool: "read" }], c)).toBe(false)
    expect(await scanFlowApprovals([{ tool: "read" }, { tool: "sh" }], c)).toBe(true)
    expect(await scanFlowApprovals([{ foreach: "3", steps: [{ tool: "sh" }] }], c)).toBe(true)
  })
  test("函数式 requiresApproval 的编排工具同样参与判定", async () => {
    const inner: Tool = {
      name: "orchestrator",
      description: "",
      parameters: { type: "object", properties: {} },
      requiresApproval: async () => true,
      async execute() {
        return { output: "" }
      },
    }
    const c = flowCtx({ orchestrator: inner })
    expect(await scanFlowApprovals([{ tool: "orchestrator" }], c)).toBe(true)
  })
  test("结构非法按需审批（fail-safe）", async () => {
    const c = flowCtx({})
    expect(await scanFlowApprovals("bad", c)).toBe(true)
  })
  test("flow 工具自身的动态审批接线", async () => {
    const c = flowCtx({ read: safe, sh: risky })
    const flow = createGlobalTools().flow
    expect(typeof flow.requiresApproval).toBe("function")
    expect(await (flow.requiresApproval as (a: unknown, ctx: ToolContext) => Promise<boolean>)({ steps: [{ tool: "read" }] }, c)).toBe(false)
    expect(await (flow.requiresApproval as (a: unknown, ctx: ToolContext) => Promise<boolean>)({ steps: [{ tool: "sh" }] }, c)).toBe(true)
  })
  test("内层脚本工具按次免审（approval:false）传导为 flow 整体免审", async () => {
    // 真实 sh 工具：缺省需审批 → flow 整体需审批；步骤声明 approval:false → 该步免审 → 全免审步骤时 flow 免审
    const c = flowCtx({ read: safe, sh: shTool })
    expect(await scanFlowApprovals([{ tool: "sh", params: { command: "ls" } }], c)).toBe(true)
    expect(await scanFlowApprovals([{ tool: "read" }, { tool: "sh", params: { command: "ls", approval: false } }], c)).toBe(false)
  })
})
