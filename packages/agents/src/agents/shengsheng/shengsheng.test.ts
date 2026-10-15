import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, readFileSync, rmSync, existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { def as shengshengDef, parseControl, renderControl, dedupeState, inflightSession } from "./shengsheng"

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
    const ld = lost.data as { awaiting: unknown[]; gone: unknown[]; unresolved: unknown[] }
    expect(ld.awaiting).toHaveLength(0)
    expect(ld.gone).toHaveLength(1)
    expect(ld.unresolved).toHaveLength(0)
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

  test("根解析不跨会话泄漏：他根的 init 记忆不得成为本会话基线", async () => {
    const a = stub()
    const b = stub()
    await tool("state").execute({ action: "init", root: a.root, verify: "bun test" }, a.ctx)
    expect((await tool("state").execute({}, a.ctx)).output).toContain(a.root)

    // 会话 B 未 init：记忆若按会话 ID 存，A 的根就会成为 B 的基线（B 的读/写打到 A 的项目上）
    const readB = await tool("state").execute({}, b.ctx)
    expect(readB.output).not.toContain(a.root)
    expect(readB.output).toContain("action=init")
    const addB = await tool("state").execute({ action: "seed_add", direction: "B 的种子" }, b.ctx)
    expect(addB.output).toContain("action=init")
    expect(controlOf(a)).not.toContain("B 的种子")
    expect((await tool("state").execute({}, b.ctx)).output).toContain("action=init")
    cleanup(a)
    cleanup(b)
  })

  test("陈旧 env 不阻断：核不中即回落本会话记忆，写分支照常落地", async () => {
    const s = stub()
    const here = String(s.ctx.sessionWorkdir ?? s.ctx.workdir)
    await tool("state").execute({ action: "init", root: here, verify: "bun test" }, s.ctx)
    // 陈旧 env（指向已删路径的根）：不得据此判「初生」或让解析停在它上面
    s.sessionEnv.SHENGSHENG_ROOT = join(s.home, "gone", "final-evolve")
    const r = await tool("state").execute({}, s.ctx)
    expect(r.output).not.toContain("final-evolve")
    expect(r.output).toContain(here)
    expect(r.output).toContain("init 记忆")
    const add = await tool("state").execute({ action: "seed_add", direction: "陈旧 env 下仍可入队" }, s.ctx)
    expect(add.output).toContain("种子入队")
    expect(readFileSync(join(here, ".evolve", "control.md"), "utf8")).toContain("陈旧 env 下仍可入队")
    cleanup(s)
  })

  test("续派：session_id 指定既有 worker 会话——沿用无人值守授权，不新建、不占新建额度，且不向池外会话外扩授权", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "首单", domain: "a" }, { task: "第二单", domain: "b" }, { task: "第三单", domain: "c" }] }, s.ctx)
    const sid = s.sent[0].sessionId
    const createdBefore = s.created.length
    expect(createdBefore).toBe(3)

    // 池满（活动上限 3）也不阻断续派：续派不改池容量
    s.running.clear()
    const r = await tool("dispatch").execute({ orders: [{ task: "接着干被中断的步骤", session_id: sid }] }, s.ctx)
    expect(r.output).toContain("布雨完成")
    expect(r.output).toContain("续派")
    expect(s.created).toHaveLength(createdBefore)          // 不新建会话
    // **续跑泄漏修复的核心断言**：续派同样以无人值守姿态运行（否则需审批工具各挂 5 分钟审批超时后被跳过）
    expect(s.sent[s.sent.length - 1].opts).toEqual({ autoApprove: true, interactionMode: "none" })
    // 续派消息点名「从中断处继续」（另一关键点：worker 不得从头重做或绕开被中断的步骤）
    expect(s.sent[s.sent.length - 1].message).toContain("从中断处继续")
    // 池内仍是 3 个 worker（旧条目按会话 upsert，不重复登记）
    expect(parseControl(controlOf(s)).workers).toHaveLength(3)
    expect(parseControl(controlOf(s)).workers.find((w) => w.sessionId === sid)!.alias).toBe("a-1")

    // 授权面不外扩：session_id 只认池中登记的 worker 会话
    const stranger = await tool("dispatch").execute({ orders: [{ task: "越权单", session_id: "sess-stranger" }] }, s.ctx)
    expect(stranger.output).toContain("授权面不外扩")
    expect(s.created).toHaveLength(createdBefore)
    expect(s.sent).toHaveLength(4)                          // 未向非池会话注入
    cleanup(s)
  })

  // ------------------------------------------------------------------
  // 续派全链（fix-2 / 547039f2）：同进程内一次性锁住四条——续派≠新建（会话数不变、
  // 不占新建额度）、resume 消息分支、非池会话拒绝（授权面不外扩）、池满只挡新建。
  // 与上一条「续派」测试的分工：上条是**同语义的宽断言**；本条补齐量化与对照断言
  // （混合批次的额度记账、resume 分支的开合对照、拒绝路径的零副作用、真实存在的非池会话）。
  // ------------------------------------------------------------------

  test("续派全链：session_id 走续派而非新建——不新增会话、不占新建额度、消息点名「续派/从中断处继续」、非池会话被拒、池满只挡新建", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun run test" }, s.ctx)

    // 先立一条演进线的两个 worker（池 = 2，活动上限 = 3——留一格空位供「占额度」量化）
    await tool("dispatch").execute({ orders: [{ task: "首单：修 lint", domain: "lint" }, { task: "次单：补文档", domain: "docs" }] }, s.ctx)
    const lintSid = s.sent[0].sessionId
    const docsSid = s.sent[1].sessionId
    expect(s.created.map((c) => c.id)).toEqual([lintSid, docsSid])
    // 对照基线：正常新建单的消息**不带**续派分支（否则下面的 resume 分支断言就失去意义）
    expect(s.sent[0].message).not.toContain("续派")
    expect(s.sent[1].message).not.toContain("续派")

    // ----------------------------------------------------------------
    // ① 续派：session_id 指向池中 worker → 不新增会话、不占新建额度
    // ----------------------------------------------------------------
    s.running.clear()                                   // 两单跑完（同会话并发注入会被拒）
    const createdBefore = s.created.length
    const r = await tool("dispatch").execute({ orders: [{ task: "接着干被中断的步骤", session_id: lintSid }] }, s.ctx)
    expect(r.output).toContain("布雨完成")
    expect(r.output).toContain("续派")
    expect(s.created).toHaveLength(createdBefore)        // 不新增会话
    expect(s.sent).toHaveLength(3)
    expect(s.sent[2].sessionId).toBe(lintSid)            // 注入的仍是原会话
    // 机器可读契约：plan 标 resume、非 created（编排侧据此判额度，不能只看文本）
    const rd = r.data as { plan: Array<{ alias: string; sessionId: string; reuse: boolean; created: boolean; resume?: boolean; note?: string }> }
    expect(rd.plan).toHaveLength(1)
    expect(rd.plan[0]).toMatchObject({ alias: "lint-1", sessionId: lintSid, reuse: false, created: false, resume: true })
    expect(rd.plan[0].note).toContain("续派会话")          // 走的是续派分支（非新建、非同域复用）
    // 续派=复用会话：项目坐标 env 重注入（服务重启后 env 常已丢，续跑多发生在此之后）
    const reinject = s.envWrites.filter(([id]) => id === lintSid).pop()![1]
    expect(reinject.SHENGSHENG_ROOT).toBe(s.root)
    expect(reinject.SHENGSHENG_VERIFY).toBe("bun run test")
    // 池：同会话按会话 upsert——仍是 2 条、别名不变、轮次前推（旧轮被新轮取代）
    const st1 = parseControl(controlOf(s))
    expect(st1.workers).toHaveLength(2)
    const lintWorker = st1.workers.find((w) => w.sessionId === lintSid)!
    expect(lintWorker.alias).toBe("lint-1")
    expect(lintWorker.round).toBeGreaterThan(1)

    // **不占新建额度（量化断言）**：池 = 2、上限 3 —— 一批 [续派 + 新建] 必须放行。
    // 若续派被误算作新建，则为 2 + 2 = 4 > 3，整批被「上限 3」挡下。
    s.running.clear()
    const mixed = await tool("dispatch").execute({ orders: [{ task: "续派第二刀", session_id: lintSid }, { task: "新领域首单", domain: "perf" }] }, s.ctx)
    expect(mixed.output).toContain("布雨完成")
    expect(mixed.output).not.toContain("上限 3")
    expect(s.created).toHaveLength(createdBefore + 1)     // 只有新建那一单占额度
    expect(parseControl(controlOf(s)).workers).toHaveLength(3)

    // ----------------------------------------------------------------
    // ② resume 分支：续派消息点名「续派/从中断处继续」，且顺序在模板头部
    // ----------------------------------------------------------------
    const resumeMsg = s.sent[3].message                    // mixed 批次里的续派单
    expect(resumeMsg).toContain("续派：")
    expect(resumeMsg).toContain("从中断处继续")
    expect(resumeMsg).toContain("不要从头重做")
    expect(resumeMsg.indexOf("任务：")).toBeLessThan(resumeMsg.indexOf("续派："))
    expect(resumeMsg.indexOf("续派：")).toBeLessThan(resumeMsg.indexOf("上下文："))
    // 续派仍带完整模板与同一执行姿态（授权沿调用链随行：无人值守自动通过）
    expect(resumeMsg).toContain("任务：续派第二刀")
    expect(resumeMsg).toContain("验证：bun run test")
    expect(resumeMsg).toContain("无人值守执行")
    expect(s.sent[3].opts).toEqual({ autoApprove: true, interactionMode: "none" })
    // 对照：同一模板的**新建**单不含续派行（分支按 resume 开合，不是恒插）
    expect(s.sent[4].message).not.toContain("续派")
    expect(s.sent[4].sessionId).not.toBe(lintSid)

    // ----------------------------------------------------------------
    // ③ 非池会话被拒：授权面不外扩，且拒绝路径零副作用
    // ----------------------------------------------------------------
    s.running.clear()
    // 让「生生自己的控制会话」真实存在——存在不等于在授权面内：判据是**池中登记**
    s.sessions.set(String(s.ctx.sessionId), { id: String(s.ctx.sessionId), messages: [] })
    const ctrlBefore = controlOf(s)
    const sentBefore = s.sent.length
    const createdB = s.created.length
    for (const [label, sid] of [["不存在的会话", "sess-stranger"], ["存在但非池（生生自己的控制会话）", String(s.ctx.sessionId)]] as const) {
      const rejected = await tool("dispatch").execute({ orders: [{ task: `越权续派（${label}）`, session_id: sid }] }, s.ctx)
      expect(rejected.output).toContain("授权面不外扩")
      expect(rejected.output).toContain("不是池中登记的 worker 会话")
      expect((rejected.data as { blocked?: boolean }).blocked).toBe(true)
      // 拒绝文案须可操作：列出池中 worker 供替换为合法寻址
      expect(rejected.output).toContain(`${lintSid}`)
      expect(rejected.output).toContain("lint-1")
    }
    expect(s.sent).toHaveLength(sentBefore)               // 未向非池会话注入
    expect(s.created).toHaveLength(createdB)              // 未新建
    expect(controlOf(s)).toBe(ctrlBefore)                 // 池/在途/序号原封不动（拒绝即无副作用）

    // ----------------------------------------------------------------
    // ④ 池满（活动 worker = 3）时复用/续派不被挡——只挡新建
    // ----------------------------------------------------------------
    s.running.clear()
    await tool("collect").execute({ action: "collect", alias: "docs-1", summary: "文档轮次完成" }, s.ctx)
    expect(parseControl(controlOf(s)).workers.filter((w) => w.status === "idle")).toHaveLength(1)

    // 复用（同域 idle）放行
    const reuse = await tool("dispatch").execute({ orders: [{ task: "docs 跟进", domain: "docs" }] }, s.ctx)
    expect(reuse.output).toContain("布雨完成")
    expect(reuse.output).toContain("复用")
    expect(reuse.output).not.toContain("上限")
    expect(s.created).toHaveLength(createdB)              // 复用不新建
    expect(s.sent[s.sent.length - 1].sessionId).toBe(docsSid)

    // 续派放行
    const resumeAtFull = await tool("dispatch").execute({ orders: [{ task: "池满续派", session_id: lintSid }] }, s.ctx)
    expect(resumeAtFull.output).toContain("布雨完成")
    expect(resumeAtFull.output).toContain("续派")
    expect(resumeAtFull.output).toContain("池 3")
    expect(s.created).toHaveLength(createdB)
    expect(s.sent[s.sent.length - 1].sessionId).toBe(lintSid)

    // 新建被挡（同一池满前提下）——额度记账只算新建
    s.running.clear()
    const blocked = await tool("dispatch").execute({ orders: [{ task: "池满新单", domain: "brandnew", fresh: true }] }, s.ctx)
    expect(blocked.output).toContain("上限 3")
    const bd = blocked.data as { blocked?: boolean; workers?: number; needNew?: number; limit?: number }
    expect(bd).toMatchObject({ blocked: true, workers: 3, needNew: 1, limit: 3 })
    expect(s.created).toHaveLength(createdB)
    cleanup(s)
  })

  // ------------------------------------------------------------------
  // 主键去重：同一会话连续两轮派工后，池/在途各只有一条当前登记
  // （本机实测两次的缺陷：续派后同会话并列两条，旧条目被判「中道而废」）
  // ------------------------------------------------------------------

  test("续派去重：同一会话两轮派工后池/在途各一条当前登记（无第二条、无「不可达」假阳性）", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "第一轮", domain: "a" }] }, s.ctx)
    const sid = s.sent[0].sessionId
    const r1 = parseControl(controlOf(s)).workers[0].round

    // 第一轮跑完（空闲）后**续派同一会话**——下一条在途必须取代上一条，而不是并列
    s.running.clear()
    const d = await tool("dispatch").execute({ orders: [{ task: "第二轮", session_id: sid }] }, s.ctx)
    expect(d.output).toContain("续派")
    expect(s.created).toHaveLength(1)                        // 未新建

    const st = parseControl(controlOf(s))
    expect(st.workers).toHaveLength(1)                       // 池：同会话只一条
    expect(st.workers[0]).toMatchObject({ alias: "a-1", sessionId: sid })
    expect(st.workers[0].round).toBeGreaterThan(r1)           // 旧轮已被新轮取代
    expect(st.inflight).toHaveLength(1)                       // 在途：同会话只一条
    expect(st.inflight[0]).toMatchObject({ alias: "a-1", sessionId: sid, direction: "第二轮" })
    expect(st.workers[0].alias).toBe(st.inflight[0].alias)    // 池与在途同指一个「工人」

    // 在途自带会话列 → 不依赖别名也核得出会话（“先探会话再判生死”的前提）
    expect(inflightSession(st.inflight[0], st.workers)).toBe(sid)

    // **假阳性回归**：会话可达且仍在运行 → 不得进「中道而废」、不得报「虚假产能」
    s.running.add(sid)
    const running = await tool("state").execute({}, s.ctx)
    expect(running.output).not.toContain("中道而废")
    expect(running.output).not.toContain("虚假产能")
    expect((running.data as { lost?: unknown[] }).lost).toBeUndefined()
    expect((running.data as { gone: unknown[] }).gone).toHaveLength(0)
    expect((running.data as { awaiting: unknown[] }).awaiting).toHaveLength(0)   // 运行中不是「待收藏」

    // 跑完转空闲 → 待收藏线索恰好一条（不是两条、也不是中道而废）
    s.running.delete(sid)
    const done = await tool("state").execute({}, s.ctx)
    const dd = done.data as { awaiting: unknown[]; gone: unknown[]; unresolved: unknown[] }
    expect(dd.awaiting).toHaveLength(1)
    expect(dd.gone).toHaveLength(0)
    expect(dd.unresolved).toHaveLength(0)
    expect(done.output).not.toContain("中道而废")

    // 定向清理：两路寻址（session_id / alias）命中的是**同一条当前轮**，并回工命中结果
    const bySid = await tool("collect").execute({ action: "review", session_id: sid }, s.ctx)
    expect(bySid.output).toContain(`按 session_id=${sid} 命中本条`)
    expect(bySid.output).toContain("别名 a-1")
    expect(bySid.output).toContain("在途 有")
    const byAlias = await tool("collect").execute({ action: "review", alias: "a-1" }, s.ctx)
    expect(byAlias.output).toContain("worker a-1")
    expect(byAlias.output).toContain("在途登记：第二轮")            // 别名与 session_id 命中同一条

    // 别名与 session_id 矛盾时不猜（否则会拿另一个会话的报告入账）
    await tool("dispatch").execute({ orders: [{ task: "别的线", domain: "b" }] }, s.ctx)
    const clash = await tool("collect").execute({ action: "collect", alias: "b-1", session_id: sid, summary: "x" }, s.ctx)
    expect(clash.output).toContain("请只传一个")                     // 工具不猜：指向不同 worker 就拒绝

    // 收藏后池一条 idle、在途清空（b-1 是另一条演进线，仍在途）
    const c = await tool("collect").execute({ action: "collect", session_id: sid, summary: "第二轮要点" }, s.ctx)
    expect(c.output).toContain("收藏完成")
    const after = parseControl(controlOf(s))
    expect(after.inflight.map((i) => i.alias)).toEqual(["b-1"])            // 只清掉该会话那一条
    expect(after.workers.find((w) => w.sessionId === sid)!.status).toBe("idle")
    cleanup(s)
  })

  test("登记归一：同会话并列多条（旧格式/人工误改）读侧即归一，陈旧在途可定向清理、不误判可达", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "第一轮", domain: "a" }] }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "别的线", domain: "b" }] }, s.ctx)
    const sid = s.sent[0].sessionId
    s.running.clear()

    // 直接在手写的 control.md 里重建缺陷现场：同一会话并列两条池条目 + 两条在途
    //（旧版工具就这样写；且旧格式在途没有「会话」列——归一必须兑住它）
    const bad = [
      "## 项目坐标",
      `- 项目根：${s.root}`,
      "",
      "## worker 池",
      "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |",
      "| --- | --- | --- | --- | --- | --- | --- |",
      `| a-1 | ${sid} | a | 第一轮 | 1000 | 1 | running |`,
      `| a-9 | ${sid} | a | 第一轮（旧轮） | 900 | 1 | running |`,   // 同轮并列：靠后偏胜
      "| b-1 | sess2 | b | 别的线 | 1000 | 2 | idle |",
      "",
      "## 在途",
      "| 别名 | 方向 | 预期产出 | 派发时间 | 轮次 |",
      "| --- | --- | --- | --- | --- |",
      `| a-1 | 第二轮 |  | 1000 | 3 |`,                             // 别名不在池中（池只剩 a-9）：无从核实
      "| ghost-1 | 无处安放的在途 |  | 800 | 4 |",                     // 同上——旧格式没会话列
      "",
      "## 种子队列",
      "（空）",
      "",
      "## 收藏待办",
      "（空）",
      "",
      "## 断点",
      "（无）",
      "",
    ].join("\n")
    await Bun.write(join(s.root, ".evolve", "control.md"), bad)

    // 读侧归一：并列条目只留一条（会话为主键 → 同一会话两个别名是同一“工人”的两个名字）
    const st = parseControl(bad)
    expect(st.workers).toHaveLength(2)
    expect(st.workers.filter((w) => w.sessionId === sid)).toHaveLength(1)
    // 旧格式在途（无会话列）：归一**只能靠池解析别名**——两条的别名都不在池（同会话幸存者是 a-9），
    // 故无从判定同源而全部保留（宁多留一条，也不错杀一条真实在途：它们进「无从核实」而非「中道而废」）
    expect(st.inflight.map((i) => i.alias)).toEqual(["a-1", "ghost-1"])
    expect(inflightSession(st.inflight[0], st.workers)).toBeUndefined()
    expect(inflightSession(st.inflight[1], st.workers)).toBeUndefined()
    // 归一幂等：再渲染再解析不再变
    expect(parseControl(renderControl(st))).toEqual(st)

    // 读态：池中会话均可达 → 无「中道而废」；两条无会话线索的在途进「无从核实」（不得谎报可达、也不得谎报已死）
    const read = await tool("state").execute({}, s.ctx)
    const rd = read.data as { awaiting: unknown[]; gone: { alias: string }[]; unresolved: { alias: string }[] }
    expect(rd.gone).toHaveLength(0)
    expect(rd.unresolved.map((i) => i.alias)).toEqual(["a-1", "ghost-1"])
    expect(rd.awaiting).toHaveLength(0)
    expect(read.output).toContain("无从核实")
    expect(read.output).not.toContain("中道而废")            // 归属不明≠确已死：不得借“不可达”之名诬告
    expect(read.output).not.toContain("虚假产能")

    // 陈旧在途定向清理：**仅剩别名可寻**（池中无对应 worker）——用 alias 精准清这一条
    const wrong = await tool("collect").execute({ action: "review", alias: "ghost-1" }, s.ctx)
    expect(wrong.output).toContain("只在「在途」里的陈旧登记")
    const cleaned = await tool("collect").execute({ action: "retire", alias: "ghost-1", mode: "idle", reason: "陈旧残留" }, s.ctx)
    expect(cleaned.output).toContain("已清掉陈旧在途登记")
    const after = parseControl(controlOf(s))
    expect(after.inflight.map((i) => i.alias)).toEqual(["a-1"])            // 只清掉那一条
    expect(after.seeds.some((x) => x.direction === "无处安放的在途")).toBe(true)
    expect(after.workers.find((w) => w.sessionId === sid)!.alias).toBe("a-9")

    // 别名指向池中在途时不误清：review 按别名命中真 worker
    const real = await tool("collect").execute({ action: "review", alias: "a-9" }, s.ctx)
    expect(real.output).toContain("worker a-9")
    cleanup(s)
  })

  test("去重契约：池按会话主键（新轮取代旧轮）、在途同会话只留最新一轮（无会话线索者不误杀）", () => {
    const w = (alias: string, sessionId: string, round: number): { alias: string; sessionId: string; domain: string; task: string; dispatchedAt: number; round: number; status: "running" | "idle" } =>
      ({ alias, sessionId, domain: "d", task: "t", dispatchedAt: 1, round, status: "running" })
    const base = { coord: {}, breath: 0, seq: 0, seeds: [], pending: [], workers: [], inflight: [], note: undefined } as unknown as Parameters<typeof dedupeState>[0]

    // 池：同会话两条 → 留 round 更大者；同轮并列 → 靠后者偏胜；无会话 id 的条目一律保留（不误杀）
    const r1 = dedupeState({ ...base, workers: [w("a-1", "s1", 2), w("a-2", "s1", 5), w("b-1", "s2", 1), w("c-1", "", 1)] })
    expect(r1.workers.map((x) => x.alias)).toEqual(["a-2", "b-1", "c-1"])
    expect(r1.dropped).toBe(1)
    const tie = dedupeState({ ...base, workers: [w("a-1", "s1", 3), w("a-2", "s1", 3)] })
    expect(tie.workers.map((x) => x.alias)).toEqual(["a-2"])

    // 在途：会话线索取自带列（旧格式回落到池解析）——同会话只留最新轮，无从判定者保留
    const f = (alias: string, round: number, sessionId?: string) => ({ alias, sessionId, direction: `d${round}`, deliverable: "", dispatchedAt: round, round })
    const r2 = dedupeState({
      ...base,
      workers: [w("a-1", "s1", 3), w("b-1", "s2", 1)],
      inflight: [f("a-1", 3), f("a-1", 4), f("b-1", 1), f("ghost", 9), f("old", 1, "s1")],
    })
    // a-1 两条 → 留轮次 4；old 自带 s1 且轮次 1 → 被 a-1/r4 取代；ghost 无线索 → 保留
    expect(r2.inflight.map((x) => `${x.alias}:${x.round}`)).toEqual(["a-1:4", "b-1:1", "ghost:9"])
    // 归一不就地改调用方持有的数组
    const src = [w("a-1", "s1", 2), w("a-2", "s1", 5)]
    dedupeState({ ...base, workers: src })
    expect(src).toHaveLength(2)
  })
})
