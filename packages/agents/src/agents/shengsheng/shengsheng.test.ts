import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, readFileSync, rmSync, existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { def as shengshengDef, parseControl, renderControl } from "./shengsheng"

const tool = (n: string) => shengshengDef.tools![n]

interface Stub {
  ctx: ToolContext
  root: string
  home: string
  created: Array<{ id: string; name?: string }>
  sent: Array<{ sessionId: string; message: string; opts?: Record<string, unknown> }>
  deleted: string[]
  stopped: string[]
  envWrites: Array<[string, Record<string, string | null>]>
  /** 会话环境变量（setEnv 写入；后续调用随 ctx.env 生效——与引擎同语义）。 */
  sessionEnv: Record<string, string>
  tasks: Array<Record<string, unknown>>
  notified: Array<{ text: string; id?: string }>
  sessions: Map<string, { id: string; name?: string; messages: unknown[]; ctxTokens?: number }>
  running: Set<string>
  /** 让 resolvePath 拒绝的绝对路径前缀（模拟沙箱边界）。 */
  denied: string[]
}

function stub(opts: { root?: string; taskName?: string; withTask?: Record<string, unknown> } = {}): Stub {
  const home = mkdtempSync(join(tmpdir(), "gebai-shengsheng-"))
  const sessionDir = join(home, "users", "admin", "sessions", "s1")
  const tmp = join(sessionDir, "tmp")
  mkdirSync(tmp, { recursive: true })
  const root = opts.root ?? join(home, "proj")
  mkdirSync(root, { recursive: true })

      const s: Stub = {
      home,
      root,
      ctx: undefined as unknown as ToolContext,
      created: [],
      sent: [],
      deleted: [],
      stopped: [],
      envWrites: [],
      // 会话 env 语义：写入在**后续调用**注入 ctx.env（同会话下一轮/下一息生效）——测试即时生效以便同调用链验证
      sessionEnv: {},
      tasks: opts.withTask ? [opts.withTask] : [],
    notified: [],
    sessions: new Map(),
    running: new Set(),
    denied: [],
  }

  const resolveSafe = (p: string): string => {
    const abs = resolve(p)
    for (const d of s.denied) if (abs.startsWith(resolve(d))) throw new Error(`ENOENT: ${abs} 会话沙箱外路径`)
    return abs
  }

  const ctx = {
    user: "admin",
    sessionId: "s1",
    workdir: tmp,
    sessionWorkdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: resolveSafe,    readFile: async (p: string) => readFileSync(resolveSafe(p), "utf8"),
    readBinaryFile: async () => new Uint8Array(),
    writeFile: async (p: string, content: string) => {
      const abs = resolveSafe(p)
      mkdirSync(dirname(abs), { recursive: true })
      await Bun.write(abs, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r: { path: string }) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => root,
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
    sessions: {
      store: {
        createSession: async (_userId: string, name?: string) => {
          const id = `sess${s.created.length + 1}`
          s.created.push({ id, name })
          s.sessions.set(id, { id, name, messages: [] })
          return { id, name }
        },
        setEnv: async (id: string, _userId: string, vars: Record<string, string | null>) => {
          s.envWrites.push([id, vars])
          for (const [k, v] of Object.entries(vars)) if (typeof v === "string") s.sessionEnv[k] = v
          return vars as Record<string, string>
        },
        load: async (id: string) => s.sessions.get(id) ?? null,
        delete: async (id: string) => {
          s.deleted.push(id)
          s.sessions.delete(id)
        },
      },
      engine: {
        isRunning: (id: string) => s.running.has(id),
        cancel: (id: string) => {
          s.stopped.push(id)
          s.running.delete(id)
        },
        runtimeOf: async () =>
          Object.fromEntries([...s.running].map((id) => [id, { startedAt: Date.now() - 60_000, pending: [], toolCalls: 2 }])),
      },
          run: async (id: string, _user: string, message: string, opts?: Record<string, unknown>) => {
      s.sent.push({ sessionId: id, message, opts })
      // 引擎语义：run 注册任务 → isRunning 为真，直到本次运行结束
      s.running.add(id)
    },
    },
    tasks: {
      add: async (input: Record<string, unknown>) => {
        const t = { id: `t${s.tasks.length + 1}`, enabled: true, state: "idle", runCount: 0, kind: input.kind, schedule: input.schedule, name: input.name, ...input }
        s.tasks.push(t)
        return t
      },
      list: async () => s.tasks,
      update: async (id: string, patch: Record<string, unknown>) => {
        const t = s.tasks.find((x) => x.id === id)
        if (!t) return null
        Object.assign(t, patch)
        return t
      },
      notify: async (input: { text: string; title?: string }, id?: string) => {
        s.notified.push({ text: input.text, id })
        return { taskId: id ?? "t?", delivered: 1, errors: [] }
      },
    },
  } as unknown as ToolContext
  s.ctx = ctx
  // 会话 env 读取面：与引擎同语义（setEnv 写入的状态在后续调用注入 ctx.env）
  Object.defineProperty(ctx, "env", { get: () => s.sessionEnv, configurable: true })
  return s
}

function cleanup(s: Stub): void {
  rmSync(s.home, { recursive: true, force: true })
}

const controlOf = (s: Stub) => readFileSync(join(s.root, ".evolve", "control.md"), "utf8")
const ledgerOf = (s: Stub) => readFileSync(join(s.root, ".evolve", "ledger.md"), "utf8")

describe("shengsheng sub-agent（演进调度中枢）", () => {
  test("工具面精简：三工具覆盖一息，不复刻已有能力", () => {
    expect(Object.keys(shengshengDef.tools!)).toEqual(["state", "dispatch", "collect"])
    for (const t of ["read", "write", "edit", "sh", "py", "file", "list", "status", "send", "create", "notify", "run"]) {
      expect(Object.keys(shengshengDef.tools!)).not.toContain(t)
    }
    // 回收动作免审批；布雨独立会话/停用任务需审批
    expect(shengshengDef.requiresApproval).toEqual({ dispatch: true, collect: false })
    expect(shengshengDef.preload).toBe(false)
    expect(shengshengDef.envVars?.every((v) => v.name.startsWith("SHENGSHENG_"))).toBe(true)
    expect(shengshengDef.systemPrompt).toContain("生生")
  })

  test("control.md 往返幂等：render → parse 保真，未知字段不丢结构", () => {
    const md = renderControl({
      coord: { root: "/p", verify: "bun test", seedDoc: "docs/seed.md" },
      breath: 7,
      seq: 12,
      workers: [{ alias: "lint-1", sessionId: "abc", domain: "lint", task: "修 lint", dispatchedAt: 111, round: 12, status: "running" }],
      inflight: [{ alias: "lint-1", direction: "修 lint", deliverable: "补丁", dispatchedAt: 111, round: 12 }],
      seeds: [{ priority: "P1", direction: "docs 补齐", source: "台账", constraint: "只分析" }],
      pending: [],
      note: "观复到 lint-1",
    })
    const back = parseControl(md)
    expect(back.coord).toEqual({ root: "/p", verify: "bun test", seedDoc: "docs/seed.md" })
    expect(back.breath).toBe(7)
    expect(back.seq).toBe(12)
    expect(back.workers[0]).toMatchObject({ alias: "lint-1", sessionId: "abc", status: "running", round: 12 })
    expect(back.inflight[0]).toMatchObject({ alias: "lint-1", round: 12 })
    expect(back.seeds[0]).toMatchObject({ priority: "P1", direction: "docs 补齐" })
    expect(back.note).toBe("观复到 lint-1")
    expect(renderControl(back)).toBe(md)
    // 空态也保留节结构（可人工查看）
    expect(renderControl({ coord: {}, breath: 0, seq: 0, workers: [], inflight: [], seeds: [], pending: [] })).toContain("## 种子队列")
  })

  test("初生：control.md 缺失时引导 init，init 固化坐标并落盘", async () => {
    const s = stub()
    const read = await tool("state").execute({}, s.ctx)
    expect(read.output).toContain("action=init")

    const init = await tool("state").execute({ action: "init", root: s.root, verify: "bun test", seed_doc: "docs/seed.md" }, s.ctx)
    expect(init.output).toContain("项目坐标已固化")
    const md = controlOf(s)
    expect(md).toContain("项目根")
    expect(md).toContain("bun test")
    // 幂等：再次 init 沿用既有状态而非清空
    const again = await tool("state").execute({ action: "init", root: s.root }, s.ctx)
    expect(again.output).toContain("沿用既有状态")
    expect(controlOf(s)).toContain("bun test")
    cleanup(s)
  })

  test("择种：seed_add 去重、seed_done 唯一定位（不唯一拒绝）、note 推进呼吸", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    const a1 = await tool("state").execute({ action: "seed_add", direction: "修 A 模块缺陷", priority: "P0", source: "种子库", constraint: "可动手" }, s.ctx)
    expect(a1.output).toContain("种子入队")
    const dup = await tool("state").execute({ action: "seed_add", direction: "修 A 模块缺陷" }, s.ctx)
    expect(dup.output).toContain("已存在")
    await tool("state").execute({ action: "seed_add", direction: "修 A 模块文档" }, s.ctx)
    const amb = await tool("state").execute({ action: "seed_done", match: "修 A 模块" }, s.ctx)
    expect(amb.output).toContain("匹配不唯一")
    const done = await tool("state").execute({ action: "seed_done", match: "修 A 模块缺陷" }, s.ctx)
    expect(done.output).toContain("种子销账")
    const note = await tool("state").execute({ action: "note", note: "布雨 1 单，一单在途" }, s.ctx)
    expect(note.output).toContain("断点已落盘")
    const state = parseControl(controlOf(s))
    expect(state.seeds).toHaveLength(1)
    expect(state.note).toBe("布雨 1 单，一单在途")
    cleanup(s)
  })

  test("布雨：复用同域 idle worker（派工消息自包含），fresh 时新建并注入项目坐标 env", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun run test", seed_doc: "docs/seed.md", rules: "AGENTS.md", preload: "装载 code 子Agent" }, s.ctx)
    // 第一单：无 idle worker → 新建
    const first = await tool("dispatch").execute({ orders: [{ task: "修 lint 报错", domain: "lint", deliver: "lint 清理补丁" }] }, s.ctx)
    expect(first.output).toContain("布雨完成")
    expect(s.created).toHaveLength(1)
    expect(s.created[0].name).toBe("evolve-w-lint-1")
    expect(s.sent).toHaveLength(1)
    const msg = s.sent[0].message
    expect(msg).toContain("任务：修 lint 报错")
    expect(msg).toContain("验证：bun run test")
    expect(msg).toContain("纪律：AGENTS.md")
    expect(msg).toContain("预载：装载 code 子Agent")
    expect(msg).toContain("单任务完成即止")
    expect(msg).toContain("建议下一条")
    expect(msg).toContain("无人值守执行")
    // 派工即授权：worker 以无人值守姿态运行（否则实时审批会挂到超时，每单必死）
    expect(s.sent[0].opts).toEqual({ autoApprove: true, interactionMode: "none" })
    // 新会话注入项目坐标 env（服务重启即空，可重复注入）
    const newSessionEnv = s.envWrites.find(([id]) => id === s.created[0].id)![1]
    expect(newSessionEnv.SHENGSHENG_ROOT).toBe(s.root)
    expect(newSessionEnv.SHENGSHENG_VERIFY).toBe("bun run test")
    expect(first.output).toContain("不要等")

    // worker 跑完 → 标 idle → 同域第二单直接复用（不新建）
    s.running.clear()
    const st = parseControl(controlOf(s))
    st.workers[0].status = "idle"
    await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st))
    const second = await tool("dispatch").execute({ orders: [{ task: "补 lint 用例", domain: "lint" }] }, s.ctx)
    expect(second.output).toContain("复用")
    expect(s.created).toHaveLength(1)
    expect(s.sent).toHaveLength(2)
    expect(s.sent[1].sessionId).toBe(s.sent[0].sessionId)

    // fresh：强制新建
    const third = await tool("dispatch").execute({ orders: [{ task: "换前提重做", domain: "lint", fresh: true }] }, s.ctx)
    expect(third.output).toContain("新建")
    expect(s.created).toHaveLength(2)

    // 活动 worker 上限（3）挡新增
    const fifth = await tool("dispatch").execute({ orders: [{ task: "第四单", domain: "docs" }, { task: "第五单", domain: "perf" }] }, s.ctx)
    expect(fifth.output).toContain(`上限 ${3}`)
    expect(s.created).toHaveLength(2)

    // preview：只出计划不派工
    s.ctx.env.SHENGSHENG_DISPATCH = "preview"
    const preview = await tool("dispatch").execute({ orders: [{ task: "预检单", domain: "lint" }] }, s.ctx)
    expect(preview.output).toContain("布雨预检")
    expect(s.sent).toHaveLength(3)
    cleanup(s)
  })

  test("复用不改名：同域第二单沿用原别名（旧别名仍可收藏，会话不被重复登记）", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "第 1 单", domain: "lint" }] }, s.ctx)
    const first = parseControl(controlOf(s))
    expect(first.workers[0].alias).toBe("lint-1")
    s.running.clear()
    await tool("collect").execute({ action: "collect", alias: "lint-1", summary: "第一轮" }, s.ctx)

    // 同域第二单：应复用 lint-1，而非派生 lint-2（避免同一会话多别名、旧别名失效）
    const d2 = await tool("dispatch").execute({ orders: [{ task: "第 2 单", domain: "lint" }] }, s.ctx)
    expect(d2.output).toContain("lint-1")
    expect(d2.output).toContain("复用")
    const st = parseControl(controlOf(s))
    expect(st.workers).toHaveLength(1)
    expect(st.workers[0].alias).toBe("lint-1")
    expect(st.workers[0].round).toBe(2)          // 轮次递增
    expect(st.seq).toBe(2)                        // 派工序号推进
    // 旧别名仍有效（工具回显与派工消息里一直是这个名字）
    s.running.clear()
    const c2 = await tool("collect").execute({ action: "collect", alias: "lint-1", summary: "第二轮" }, s.ctx)
    expect(c2.output).toContain("收藏完成")
    const led = ledgerOf(s)
    expect(led).toContain("第一轮")
    expect(led).toContain("第二轮")
    expect(led).toContain(":r1 -->")              // 幂等键按轮次（不是时间戳）
    expect(led).toContain(":r2 -->")
    expect(led).toContain("轮次 2")
    cleanup(s)
  })

  test("幂等键按轮次：同一轮重复收藏被拦，新轮可入账", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "分析 A", domain: "a" }] }, s.ctx)
    s.running.clear()
    const c1 = await tool("collect").execute({ action: "collect", alias: "a-1", summary: "第一轮要点" }, s.ctx)
    expect(c1.output).toContain("收藏完成")
    // 同一轮再收（在途已清，回退到 worker.round）——键仍为 r1，应拦下
    const again = await tool("collect").execute({ action: "collect", alias: "a-1", summary: "重复要点" }, s.ctx)
    expect(again.output).toContain("幂等跳过")
    expect(ledgerOf(s)).not.toContain("重复要点")
    cleanup(s)
  })

  test("复用代数提示：同一会话承接多单后按单数提示换新", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    // 伪装一个已承接 REUSE_GENERATION_HINT 轮的 idle worker
    await tool("dispatch").execute({ orders: [{ task: "首单", domain: "g" }] }, s.ctx)
    const st0 = parseControl(controlOf(s))
    st0.workers[0].status = "idle"
    st0.workers[0].round = 8
    st0.seq = 8
    await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st0))
    s.running.clear()
    const d = await tool("dispatch").execute({ orders: [{ task: "续单", domain: "g" }] }, s.ctx)
    expect(d.output).toContain("已承接")
    expect(d.output).toContain("退役换新")
    cleanup(s)
  })

  test("collect：台账读改写串行（同轮并发只入账一次）", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "并发分析", domain: "c" }] }, s.ctx)
    s.running.clear()
    const [r1, r2] = await Promise.all([
      tool("collect").execute({ action: "collect", alias: "c-1", summary: "并发要点" }, s.ctx),
      tool("collect").execute({ action: "collect", alias: "c-1", summary: "并发要点" }, s.ctx),
    ])
    const okCount = [r1, r2].filter((r) => r.output.includes("收藏完成")).length
    const skipCount = [r1, r2].filter((r) => r.output.includes("幂等跳过")).length
    expect(okCount).toBe(1)
    expect(skipCount).toBe(1)
    const hits = (ledgerOf(s).match(/并发要点/g) ?? []).length
    expect(hits).toBe(1)
    cleanup(s)
  })

  test("派工消息：预载与上下文撞同一路径时给出区分说明", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test", seed_doc: "AGENTS.md" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "占位", domain: "x" }] }, s.ctx)
    const st = parseControl(controlOf(s))
    st.workers[0].status = "idle"
    await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st))
    s.running.clear()
    // 上下文与预载同为 AGENTS.md（坐标里 seedDoc=AGENTS.md，preload 也传同值）
    await tool("dispatch").execute({ orders: [{ task: "撞路径", domain: "y", preload: "AGENTS.md" }] }, s.ctx)
    const msg = s.sent[s.sent.length - 1].message
    expect(msg).toContain("AGENTS.md")
    expect(msg).toContain("分别理解")
    cleanup(s)
  })

  test("收藏：台账追加幂等 + worker 标 idle + 在途清除 + 建议下一条入种子", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "修 lint 报错", domain: "lint", deliver: "补丁 + 回归" }] }, s.ctx)

    const review = await tool("collect").execute({ action: "review", alias: "lint-1" }, s.ctx)
    expect(review.output).toContain("运行中")
    expect(review.output).toContain("处置建议")

    // worker 跑完（会话转空闲）后再收藏
    s.running.clear()

    const collect = await tool("collect").execute({ action: "collect", alias: "lint-1", summary: "## 交付\n修掉 12 处 lint\n\n## 验证\nbun test 全绿", next: "补 lint CI 门禁" }, s.ctx)
    expect(collect.output).toContain("收藏完成")
    const ledger = ledgerOf(s)
    expect(ledger).toContain("修 lint 报错")
    expect(ledger).toContain("bun test 全绿")
    expect(ledger).toContain("建议下一条：补 lint CI 门禁")
    const st = parseControl(controlOf(s))
    expect(st.workers[0].status).toBe("idle")
    expect(st.inflight).toHaveLength(0)
    expect(st.seeds.some((x) => x.direction === "补 lint CI 门禁")).toBe(true)

    // 幂等：同一 worker 再收藏不入账第二次
    // 同一轮重复收藏（先 mark 已读，避开防误改）：键含轮次号 → 必拦
    void readFileSync(join(s.root, ".evolve", "ledger.md"), "utf8")
    const again = await tool("collect").execute({ action: "collect", alias: "lint-1", summary: "重复要点" }, s.ctx)
    expect(again.output).toContain("幂等跳过")
    expect(ledgerOf(s)).not.toContain("重复要点")
    cleanup(s)
  })

  test("退役：idle 清池并把在途方向退回种子；stop 终止运行中的 worker", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "分析 A", domain: "a" }] }, s.ctx)

    const gone = await tool("collect").execute({ action: "retire", alias: "a-1", mode: "idle", reason: "领域已终" }, s.ctx)
    expect(gone.output).toContain("已退役")
    const st = parseControl(controlOf(s))
    expect(st.workers).toHaveLength(0)
    expect(st.seeds.some((x) => x.direction === "分析 A")).toBe(true)

    // stop：运行中 → 终止并退役（run 后会话处于运行态）
    await tool("dispatch").execute({ orders: [{ task: "分析 B", domain: "b" }] }, s.ctx)
    const sb = parseControl(controlOf(s))
    const stopped = await tool("collect").execute({ action: "retire", alias: sb.workers[0].alias, mode: "stop", reason: "超 40 分钟" }, s.ctx)
    expect(stopped.output).toContain("已终止并退役")
    expect(s.stopped).toHaveLength(1)

    // delete：会话删除进归档
    await tool("dispatch").execute({ orders: [{ task: "分析 C", domain: "c" }] }, s.ctx)
    const sc = parseControl(controlOf(s))
    const before = sc.workers.length
    s.running.delete(sc.workers.find((w) => w.domain === "c")!.sessionId)
    const del = await tool("collect").execute({ action: "retire", session_id: sc.workers.find((w) => w.domain === "c")!.sessionId, mode: "delete" }, s.ctx)
    expect(del.output).toContain("已删除会话并退役")
    expect(s.deleted.length).toBe(1)
    expect(parseControl(controlOf(s)).workers).toHaveLength(before - 1)
    cleanup(s)
  })

  test("读态：待收藏线索（在途但已空闲）+ 池实时态一览", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "分析 A", domain: "a" }] }, s.ctx)
    const running = await tool("state").execute({}, s.ctx)
    expect(running.output).toContain("运行中")
    expect(running.output).not.toContain("待收藏线索")
    // worker 跑完（会话转空闲）→ 在途未回收 → 待收藏线索出现
    s.running.clear()

    // worker 空闲 → 待收藏线索出现（在途未回收）
    const st = parseControl(controlOf(s))
    st.workers[0].status = "idle"
    await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st))
    const idle = await tool("state").execute({}, s.ctx)
    expect(idle.output).toContain("待收藏线索")
    expect((idle.data as { awaiting: unknown[] }).awaiting).toHaveLength(1)

    // 会话被外部删除 → 如实标注；**不得**再当作「待收藏线索」（无消息尾可 review、无交付物可入账）
    s.sessions.clear()
    const lost = await tool("state").execute({}, s.ctx)
    expect(lost.output).toContain("会话不存在")
    expect(lost.output).not.toContain("待收藏线索")
    expect(lost.output).toContain("中道而废")
    expect(lost.output).toContain("虚假产能")
    const ld = lost.data as { awaiting: unknown[]; lost: unknown[] }
    expect(ld.awaiting).toHaveLength(0)
    expect(ld.lost).toHaveLength(1)
    cleanup(s)
  })

  test("心跳任务：ensure 幂等创建（sticky + 预载 shengsheng + 项目 env），已有则不新建", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test", seed_doc: "docs/seed.md" }, s.ctx)
    const initEnv = s.envWrites.find(([id]) => id === "s1")![1]
    expect(initEnv.SHENGSHENG_ROOT).toBe(s.root)
    expect(s.ctx.env.SHENGSHENG_ROOT).toBe(s.root)
    // 同一息内 env 尚未进入本 run 的 ctx（引擎在 run 开始时解析一次）也不影响解析：本进程 init 记忆兑底
    delete s.sessionEnv.SHENGSHENG_ROOT
    const sameBreath = await tool("state").execute({}, s.ctx)
    expect(sameBreath.output).toContain("本会话 init 记忆")
    s.sessionEnv.SHENGSHENG_ROOT = s.root
    expect(s.envWrites[0][1].SHENGSHENG_ROOT).toBe(s.root)
    expect(s.ctx.env.SHENGSHENG_ROOT).toBe(s.root)

    const created = await tool("state").execute({ action: "task", op: "ensure", schedule: "@every 5m" }, s.ctx)
    expect(created.output).toContain("心跳任务已创建")
    expect(s.tasks).toHaveLength(1)
    expect(s.tasks[0]).toMatchObject({ kind: "scheduled", runner: "prompt", target: "sticky", schedule: "@every 5m", notifyOn: "model" })
    expect(s.tasks[0].agents).toEqual(["shengsheng"])
    expect(String(s.tasks[0].prompt)).toContain(s.root)
    expect((s.tasks[0].env as Record<string, string>).SHENGSHENG_ROOT).toBe(s.root)

    const again = await tool("state").execute({ action: "task", op: "ensure", schedule: "@every 5m" }, s.ctx)
    expect(again.output).toContain("已存在（幂等，未新建）")
    expect(s.tasks).toHaveLength(1)

    // 停用后 ensure 恢复启用
    s.tasks[0].enabled = false
    const resumed = await tool("state").execute({ action: "task", op: "ensure" }, s.ctx)
    expect(resumed.output).toContain("启用")
    expect(s.tasks[0].enabled).toBe(true)
    cleanup(s)
  })

  test("观澜：pause 记台账 + 通知用户 + 停用任务", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("state").execute({ action: "task", op: "ensure" }, s.ctx)

    const paused = await tool("state").execute({ action: "task", op: "pause", reason: "种子只剩需人工决策项" }, s.ctx)
    expect(paused.output).toContain("演进已暂停")
    expect(s.tasks[0].enabled).toBe(false)
    expect(s.notified[0].text).toContain("种子只剩需人工决策项")
    expect(ledgerOf(s)).toContain("演进暂停")
    expect(parseControl(controlOf(s)).note).toContain("演进暂停")

    // 恢复
    const resumed = await tool("state").execute({ action: "task", op: "resume" }, s.ctx)
    expect(resumed.output).toContain("已启用")
    expect(s.tasks[0].enabled).toBe(true)
    cleanup(s)
  })

  test("审批门控：state 的 task ensure/pause 需审批，读态与收藏免审批", async () => {
    const ra = tool("state").requiresApproval as (a: Record<string, unknown>, c: ToolContext) => boolean
    expect(ra({}, {} as ToolContext)).toBe(false)
    expect(ra({ action: "init" }, {} as ToolContext)).toBe(false)
    expect(ra({ action: "task", op: "status" }, {} as ToolContext)).toBe(false)
    expect(ra({ action: "task", op: "ensure" }, {} as ToolContext)).toBe(true)
    expect(ra({ action: "task", op: "pause" }, {} as ToolContext)).toBe(true)
    expect(tool("collect").requiresApproval).toBe(false)
    expect(tool("dispatch").requiresApproval).toBe(true)
  })

  test("沙箱边界：项目根在会话外时如实报错，不绕过约束", async () => {
    const s = stub()
    s.denied.push("/outside")
    const r = await tool("state").execute({ action: "init", root: "/outside/proj" }, s.ctx)
    expect(r.output).toContain("沙箱约束")
    expect(r.output).not.toContain("已固化")
    cleanup(s)
  })

  test("异常路径：未初始化 / 缺参 / 未知 worker 都给出可操作引导", async () => {
    const s = stub()
    const noInit = await tool("dispatch").execute({ orders: [{ task: "x" }] }, s.ctx)
    expect(noInit.output).toContain("action=init")
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    expect((await tool("dispatch").execute({}, s.ctx)).output).toContain("需要 orders")
    expect((await tool("state").execute({ action: "seed_add" }, s.ctx)).output).toContain("需要 direction")
    const unknown = await tool("collect").execute({ action: "review", alias: "nope" }, s.ctx)
    expect(unknown.output).toContain("未在池中找到")
    expect((await tool("collect").execute({ action: "review" }, s.ctx)).output).toContain("未在池中找到")
    // 未初始化时不再赘述池内容
    expect(existsSync(join(s.root, ".evolve", "control.md"))).toBe(true)
    cleanup(s)
  })
})
