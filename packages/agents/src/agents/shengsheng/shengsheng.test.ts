import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, readFileSync, rmSync, existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { def as shengshengDef, parseControl, renderControl, dedupeState, inflightSession, isSessionIdShape, tableBlock } from "./shengsheng"
// 版本化解析模块（本单新抽）：判代/位置读取/升级函数与其版本声明——单测直接锁它的契约
import {
  parseInflightRow,
  parseInflightLine,
  migrateInflight,
  parsePoolRow,
  migratePool,
  selectInflightVer,
  selectPoolVer,
  inflightHeader,
  inflightCells,
  poolHeader,
  poolCells,
  INFLIGHT_WRITE,
  POOL_WRITE,
  fieldOrder,
  writeRow,
  POOL_VERSIONS,
  INFLIGHT_VERSIONS,
  INFLIGHT_CURRENT,
  POOL_CURRENT,
  type InflightVersion,
  type FieldMap,
} from "./control-format"

/** 测试内复刻 `inflightIdentity`（未导出）：断言按会话 id 认帐时用同一判据。 */
const inflightIdentityForTest = (i: { alias: string; sessionId?: string }, workers: Array<{ alias: string; sessionId: string }>): string =>
  i.sessionId || workers.find((w) => w.alias === i.alias)?.sessionId || i.alias

/** 旧格式在途条目的只读夹具（真实数据形态：无会话列、方向是含裸竖线的工单原文）。
 *  夹具只读——用例把它读进来后在临时目录重建现场，**任何测试都不得回写该文件**。 */
const LEGACY_FIXTURE = readFileSync(join(import.meta.dir, "fixtures", "control-legacy-inflight.md"), "utf8")

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
  // 续派全链（fix-2 / 547039f2）：同进程内一次性锁住——续派≠新建（会话数不变、不占新建
  // 额度）、resume 消息分支、非池会话拒绝（授权面不外扩，**拒绝文案逐字钉死**）、池满只挡新建、
  // 续派的**池登记后果**（idle 条目被标回 running 且不新增条目）。
  // 与上一条「续派」测试的分工：上条是**同语义的宽断言**；本条补齐量化与对照断言
  // （混合批次的额度记账、resume 分支的开合对照、拒绝路径的零副作用、真实存在的非池会话）。
  // 边界分支（空池文案、session_id×fresh 冲突、preview 下的续派与授权前置）见紧随其后的两条测试。
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
    // **文案钉死**：模板逐字固定，池清单格式为 `别名=会话id` 以「、」连接。
    // 拒绝文案是人机共同的可检索落点（「授权面不外扩」这句必须能被搜到），随意改字会让
    // 这条保证失去锚点；清单段动态取池（避开批次重排导致的顺序脆弱，只钉格式不钉次序）。
    const poolList = parseControl(controlOf(s)).workers.map((w) => `${w.alias}=${w.sessionId}`).join("、")
    expect(poolList).toContain("lint-1=")               // 清单非空（空池变体是另一支文案，见下条测试）
    for (const [label, sid] of [["不存在的会话", "sess-stranger"], ["存在但非池（生生自己的控制会话）", String(s.ctx.sessionId)]] as const) {
      const rejected = await tool("dispatch").execute({ orders: [{ task: `越权续派（${label}）`, session_id: sid }] }, s.ctx)
      expect(rejected.output).toBe(
        `orders[].session_id=${sid} 不是池中登记的 worker 会话——续派只允许本演进线派生的 worker（授权面不外扩）。池中 worker：${poolList}。接手别的会话请按别名/领域复用或 fresh=true 新建。`,
      )
      // 机器可读面同样钉死（编排侧据 blocked 分流，不解析文案）
      expect(rejected.data).toEqual({ blocked: true, unknownSession: sid })
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

    // ----------------------------------------------------------------
    // ⑤ 续派的池登记后果：打到「已存在但 idle」的 worker → 池条目**标回 running**、
    //    别名/领域/首派时间沿用、轮次前推、不新增池条目（同一会话仍恰一条）
    // ----------------------------------------------------------------
    s.running.clear()
    // 中断恢复的现实场景：worker 跑完、上一轮已收藏（池中 idle），此时才有人续派它
    await tool("collect").execute({ action: "collect", alias: "lint-1", summary: "上一轮要点" }, s.ctx)
    const beforePool = parseControl(controlOf(s))
    const lintBefore = beforePool.workers.find((w) => w.sessionId === lintSid)!
    expect(lintBefore.status).toBe("idle")                                   // 前提：确是 idle
    expect(beforePool.inflight.some((i) => i.sessionId === lintSid)).toBe(false)
    const poolBefore = beforePool.workers.length

    const resumedIdle = await tool("dispatch").execute({ orders: [{ task: "空闲后续派", session_id: lintSid }] }, s.ctx)
    expect(resumedIdle.output).toContain("布雨完成")
    expect(resumedIdle.output).toContain("续派")
    const afterPool = parseControl(controlOf(s))
    expect(afterPool.workers).toHaveLength(poolBefore)                        // **不新增池条目**
    const lintAfter = afterPool.workers.filter((w) => w.sessionId === lintSid)
    expect(lintAfter).toHaveLength(1)                                        // 同会话恰一条（upsert 取代，不并列）
    expect(lintAfter[0]).toMatchObject({
      alias: lintBefore.alias,                                               // 别名沿用（工人身份不变）
      domain: lintBefore.domain,                                             // 领域沿用（未另传 domain）
      status: "running",                                                     // **标回 running**——空闲复用后又在干活
      dispatchedAt: lintBefore.dispatchedAt,                                 // 首派时间保留（不是本次派工时间）
    })
    expect(lintAfter[0].round).toBeGreaterThan(lintBefore.round)             // 轮次前推（新轮取代旧轮）
    // 在途：同会话恰一条，方向为本次任务（闲置后再派=一条新在途，不是两条）
    const inflightNow = afterPool.inflight.filter((i) => i.sessionId === lintSid)
    expect(inflightNow).toHaveLength(1)
    expect(inflightNow[0].direction).toBe("空闲后续派")
    cleanup(s)
  })

  test("续派拒绝文案钉死（空池变体）：清单段退化为「（空）」——两步分支同属契约，且不新建不注入", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    // 池空时走同一条拒绝，只是清单段换成「（空）」——两支文案都要能搜到「授权面不外扩」
    const r = await tool("dispatch").execute({ orders: [{ task: "空池越权续派", session_id: "sess-x" }] }, s.ctx)
    expect(r.output).toBe(
      "orders[].session_id=sess-x 不是池中登记的 worker 会话——续派只允许本演进线派生的 worker（授权面不外扩）。池中 worker：（空）。接手别的会话请按别名/领域复用或 fresh=true 新建。",
    )
    expect(r.data).toEqual({ blocked: true, unknownSession: "sess-x" })
    expect(s.created).toHaveLength(0)                    // 不新建
    expect(s.sent).toHaveLength(0)                       // 不注入
    cleanup(s)
  })

  test("续派边界：session_id×fresh 冲突——显式寻址胜出不新建；preview 下续派只出标签、非池仍被拒", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "首单", domain: "a" }] }, s.ctx)
    const sid = s.sent[0].sessionId
    s.running.clear()

    // **冲突输入**：session_id（点名一个具体会话）与 fresh（「别复用，开新的」）语义相反。
    // 期望语义：**显式寻址胜出**——fresh 是自动匹配阶段的偏好提示，一旦点名了具体会话就不再适用；
    // 若让 fresh 胜出而另开新会话，用户点名要的那个 worker 反被晾着，且凭空多占一格额度。
    const createdBefore = s.created.length
    const both = await tool("dispatch").execute({ orders: [{ task: "同传续派", session_id: sid, fresh: true }] }, s.ctx)
    expect(both.output).toContain("布雨完成")
    expect(both.output).toContain("续派")
    expect(both.output).not.toContain("新建")
    expect(s.created).toHaveLength(createdBefore)                     // 未新建
    expect(s.sent[s.sent.length - 1].sessionId).toBe(sid)             // 注回被点名的那个会话
    const bp = both.data as { plan: Array<{ created: boolean; resume?: boolean }> }
    expect(bp.plan[0]).toMatchObject({ created: false, resume: true })
    expect(s.sent[s.sent.length - 1].message).toContain("从中断处继续")
    expect(parseControl(controlOf(s)).workers).toHaveLength(1)        // 池不因冲突输入膨胀

    // preview：续派只出计划标签（`续派会话 <sid>`，非「新建」），不派工、不新建
    s.running.clear()
    const sentBefore = s.sent.length
    s.ctx.env.SHENGSHENG_DISPATCH = "preview"
    try {
      const pre = await tool("dispatch").execute({ orders: [{ task: "预检续派", session_id: sid }] }, s.ctx)
      expect(pre.output).toContain("布雨预检")
      expect(pre.output).toContain(`续派会话 ${sid}`)
      expect(pre.output).not.toContain("新建")
      expect(s.sent).toHaveLength(sentBefore)                         // 预检不派工
      expect((pre.data as { plan: Array<{ created: boolean; resume?: boolean }> }).plan[0]).toMatchObject({ created: false, resume: true })

      // **授权前置**：非池会话在 preview 下**同样**被拒——预检不得把越权请求摊成一份
      // 「看起来能跑」的计划（授权判定在计划构造段，早于 preview 返回，也早于容量检查）
      const preStranger = await tool("dispatch").execute({ orders: [{ task: "预检越权", session_id: "sess-x" }] }, s.ctx)
      expect(preStranger.output).toContain("授权面不外扩")
      expect(preStranger.output).not.toContain("布雨预检")
      expect((preStranger.data as { blocked?: boolean }).blocked).toBe(true)
      expect(s.sent).toHaveLength(sentBefore)
      expect(s.created).toHaveLength(createdBefore)
    } finally {
      delete s.ctx.env.SHENGSHENG_DISPATCH
    }
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

  // ------------------------------------------------------------------
  // 跨模块全场景回归（续派路径 × 池去重）：一条测试、同一进程、一个场景走到底。
  //
  // 为什么把「续派四契约」与「池去重三重登记一致」两个视角合并成一条：分开写时，
  // 「续派后池一条 + 在途一条」与「收藏/退役能把那条清掉」各自都能被**局部**满足——
  // 池按会话 id upsert 了、在途按轮次归了一了，看上去都对；可一旦两处的**主键判据不同**
  // （池按会话登记、在途只按别名过滤/寻址），残留条目就卡在夹缝里：写侧过滤不掉、读侧归
  // 并不掉、collect/retire 也删不掉。「同一会话两轮派工」是唯一能同时打到这三处的场景。
  //
  // 盘上预备的是**旧版工具写下的别名漂移残留**（在本条测试里手写重建：旧版 `ensureUniqueAlias`
  // 不认 revived，续派会给同一会话派一个新别名；在途 upsert 只按别名过滤，旧行因此留在盘上；
  // 两张表各自按轮次归一，幸存者可以不是「同一条」）：同一会话两个别名（池 a-9 / 在途 a-1），
  // 且在途轮次**高于**本次续派将写入的轮次——对本次续派最不利的排序。若实现只是「恰好因为
  // 轮次更大而胜出」，这里就会露出来。
  // ------------------------------------------------------------------

  test("跨模块全场景：同一会话两轮派工（第二轮续派）——池/在途各一条当前登记、续派后标回 running、无「中道而废」假阳性，收藏与退役按会话 id 真正清空登记", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun run test" }, s.ctx)

    // 第一轮：走真实路径新建 worker 并派工（不是手写盘）
    await tool("dispatch").execute({ orders: [{ task: "第一轮：摸底", domain: "drift", deliver: "基线数据" }] }, s.ctx)
    const sid = s.sent[0].sessionId
    expect(s.created).toHaveLength(1)
    s.running.clear()                                   // 第一轮跑完，worker 空闲

    // 盘上残留（旧版工具形态）：池 a-9@3（该会话）＋ 在途 a-1@9（同一会话、别名漂移、轮次更高）
    await Bun.write(join(s.root, ".evolve", "control.md"), [
      "## 项目坐标",
      `- 项目根：${s.root}`,
      "",
      "## 计数",
      "- 呼吸：1",
      "- 派工序号：5",
      "",
      "## worker 池",
      "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |",
      "| --- | --- | --- | --- | --- | --- | --- |",
      `| a-9 | ${sid} | drift | 第一轮：摸底 | 1000 | 3 | idle |`,
      "",
      "## 在途",
      "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |",
      "| --- | --- | --- | --- | --- | --- |",
      `| a-1 | ${sid} | 第一轮：摸底 |  | 2000 | 9 |`,
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
    ].join("\n"))
    const before = parseControl(controlOf(s))
    expect(before.workers.map((w) => w.alias)).toEqual(["a-9"])
    expect(before.inflight.map((i) => i.alias)).toEqual(["a-1"])
    expect(inflightSession(before.inflight[0], before.workers)).toBe(sid)      // 会话线索在 → 判定得了

    // ---------- ① 续派（第二轮）：不新建、消息走 resume 分支、同一执行姿态 ----------
    const createdBefore = s.created.length
    const r = await tool("dispatch").execute({ orders: [{ task: "第二轮：从中断处继续", session_id: sid }] }, s.ctx)
    expect(r.output).toContain("布雨完成")
    expect(r.output).toContain("续派")
    expect(s.created).toHaveLength(createdBefore)                              // 续派不新建会话
    expect(s.sent[s.sent.length - 1].sessionId).toBe(sid)
    expect(s.sent[s.sent.length - 1].message).toContain("从中断处继续")
    expect(s.sent[s.sent.length - 1].opts).toEqual({ autoApprove: true, interactionMode: "none" })
    expect((r.data as { plan: Array<{ created: boolean; resume?: boolean }> }).plan[0]).toMatchObject({ created: false, resume: true })

    // ---------- ② 三重登记一致：池/在途各只一条当前登记，且同指一个「工人」 ----------
    const st = parseControl(controlOf(s))
    const poolHits = st.workers.filter((w) => w.sessionId === sid)
    const flightHits = st.inflight.filter((i) => inflightSession(i, st.workers) === sid)
    expect(poolHits).toHaveLength(1)                                          // 池：无重复登记
    expect(flightHits).toHaveLength(1)                                        // 在途：无重复登记
    expect(st.workers).toHaveLength(1)
    expect(st.inflight).toHaveLength(1)
    expect(flightHits[0].alias).toBe(poolHits[0].alias)                       // 别名不漂移（同一会话两个身份即裂缝）
    expect(flightHits[0].direction).toBe("第二轮：从中断处继续")                // 在途记的是**本轮**方向，不是旧轮
    expect(flightHits[0].round).toBe(poolHits[0].round)                       // 轮次同源（幂等键据此，漂移即重复入账）

    // ---------- ③ 续派后该会话标记回 running（空闲续派不是「留在 idle」）----------
    expect(poolHits[0].status).toBe("running")

    // ---------- ④ 读态：无「中道而废」假阳性、无「虚假产能」、运行中不算待收藏 ----------
    const read = await tool("state").execute({}, s.ctx)
    const rd = read.data as { gone: unknown[]; unresolved: unknown[]; awaiting: unknown[] }
    expect(rd.gone).toHaveLength(0)
    expect(rd.unresolved).toHaveLength(0)
    expect(read.output).not.toContain("中道而废")
    expect(read.output).not.toContain("虚假产能")
    expect(rd.awaiting).toHaveLength(0)                                       // 会话正在运行 → 不是「待收藏」

    s.running.clear()                                                         // 第二轮跑完
    const done = await tool("state").execute({}, s.ctx)
    expect((done.data as { awaiting: unknown[] }).awaiting).toHaveLength(1)    // 待收藏线索恰一条（不是两条）
    expect(done.output).not.toContain("中道而废")

    // ---------- ⑤ 收藏（按会话 id 寻址）：该会话在途清空、留池标 idle ----------
    const c = await tool("collect").execute({ action: "collect", session_id: sid, summary: "第二轮要点" }, s.ctx)
    expect(c.output).toContain("收藏完成")
    const afterCollect = parseControl(controlOf(s))
    expect(afterCollect.inflight).toHaveLength(0)                             // 清得掉：别名漂移不得成为豁免
    expect(afterCollect.workers.find((w) => w.sessionId === sid)!.status).toBe("idle")
    expect((await tool("state").execute({}, s.ctx)).output).not.toContain("待收藏线索")

    // ---------- ⑥ 退役（按会话 id 寻址）：控制面不得再留该会话任何登记 ----------
    const rt = await tool("collect").execute({ action: "retire", session_id: sid, mode: "idle", reason: "全场景回归" }, s.ctx)
    expect(rt.output).toContain("已退役")
    const afterRetire = parseControl(controlOf(s))
    expect(afterRetire.workers.some((w) => w.sessionId === sid)).toBe(false)
    expect(afterRetire.inflight.some((i) => inflightSession(i, afterRetire.workers) === sid)).toBe(false)

    // ---------- ⑦ 授权面不放宽：该会话退役后即池外，续派请求仍被拒且零副作用 ----------
    const beforeReject = controlOf(s)
    const rejected = await tool("dispatch").execute({ orders: [{ task: "越权续派", session_id: sid }] }, s.ctx)
    expect(rejected.output).toContain("授权面不外扩")
    expect(rejected.output).toContain("池中 worker：（空）")
    expect(rejected.data).toEqual({ blocked: true, unknownSession: sid })
    expect(s.sent).toHaveLength(2)                                            // 只派过两轮（越权单未注入）
    expect(controlOf(s)).toBe(beforeReject)                                   // 拒绝即无副作用

    // ---------- ⑧ 盘上残留不经续派、直接进回收：清理路径自己就得按会话 id 认帐 ----------
    // 为什么单靠①-⑥还不够：续派会在写侧把残留**顺带清掉**，于是后面的收藏/退役面对的是一张干净表，
    // 那两处的旧实现（只按别名过滤）就永远测不出来了。而现实中完全可能没人续派——生生开一息，
    // 读态报出待收藏，直接 collect/retire。故这里在**同一场景内**把漂移残留重铺一次直接走回收。
    const reseedDrift = async (poolAlias: string, poolRound: number, flightAlias: string, flightRound: number): Promise<void> => {
      await Bun.write(join(s.root, ".evolve", "control.md"), [
        "## 项目坐标", `- 项目根：${s.root}`, "", "## 计数", "- 呼吸：2", "- 派工序号：12", "",
        "## worker 池", "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |", "| --- | --- | --- | --- | --- | --- | --- |",
        `| ${poolAlias} | ${sid} | drift | 第三轮：收尾 | 3000 | ${poolRound} | idle |`, "",
        "## 在途", "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- | --- |",
        `| ${flightAlias} | ${sid} | 第三轮：收尾 |  | 4000 | ${flightRound} |`, "",
        "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", "",
      ].join("\n"))
    }

    // ⑧-a 收藏：只按别名清不掉漂移残留（会重新被读态报成「待收藏」——收藏过的又冒出来）
    await reseedDrift("drift-9", 6, "drift-1", 7)
    const c2 = await tool("collect").execute({ action: "collect", session_id: sid, summary: "直接回收（未经续派）" }, s.ctx)
    expect(c2.output).toContain("收藏完成")
    const st8 = parseControl(controlOf(s))
    expect(st8.inflight.filter((i) => inflightSession(i, st8.workers) === sid)).toHaveLength(0)
    expect(st8.workers.find((w) => w.sessionId === sid)!.status).toBe("idle")
    // 旧名残留被清时，它在途方向必须退回种子队列（否则该方向连同进度从登记里消失）
    expect(st8.seeds.map((x) => x.direction)).toContain("第三轮：收尾")
    const read8 = await tool("state").execute({}, s.ctx)
    expect(read8.output).not.toContain("待收藏线索")
    expect(read8.output).not.toContain("无从核实")

    // ⑧-b 退役：同一判据得站在清理侧——池与在途不得再留该会话任何痕迹
    await reseedDrift("drift-8", 6, "drift-2", 7)
    const rt2 = await tool("collect").execute({ action: "retire", session_id: sid, mode: "idle", reason: "清理漂移残留" }, s.ctx)
    expect(rt2.output).toContain("已退役")
    const st9 = parseControl(controlOf(s))
    expect(st9.workers.some((w) => w.sessionId === sid)).toBe(false)
    expect(st9.inflight.filter((i) => inflightIdentityForTest(i, st9.workers) === sid)).toHaveLength(0)

    // ⑧-c 新旧两代格式**同一张在途表并存**：新版那行自带会话列，旧版残行没有。
    // 旧行只能靠**池解析别名**换回会话——归一必须把两行归到同一个会话，否则同一会话又一次并列两条。
    // 这是「旧格式兼容那一档」的辨别格：身份键若只认自带列，旧行退化为用**别名**当身份（与新版那行的
    // 会话 id 不同键）→ 两行不同键 → 全部保留。
    await Bun.write(join(s.root, ".evolve", "control.md"), [
      "## 项目坐标", `- 项目根：${s.root}`, "", "## 计数", "- 呼吸：2", "- 派工序号：21", "",
      "## worker 池", "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |", "| --- | --- | --- | --- | --- | --- | --- |",
      `| drift-7 | ${sid} | drift | 第三轮 | 3000 | 20 | idle |`, "",
      "## 在途", "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- | --- |",
      `| drift-7 | ${sid} | 第三轮 |  | 4000 | 21 |`,
      "| drift-7 | 第三轮 |  | 3500 | 19 |", "",
      "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", "",
    ].join("\n"))
    const st7 = parseControl(controlOf(s))
    expect(st7.workers).toHaveLength(1)
    expect(st7.inflight).toHaveLength(1)                                      // 两行同会话（旧行经池解析）→ 归一为一条
    expect(st7.inflight[0].round).toBe(21)                                    // 幸存者是轮次更高的那一条
    const c7 = await tool("collect").execute({ action: "collect", session_id: sid, summary: "新旧格式并存残留" }, s.ctx)
    expect(c7.output).toContain("收藏完成")
    const read7 = await tool("state").execute({}, s.ctx)
    expect((read7.data as { awaiting: unknown[] }).awaiting).toHaveLength(0)
    expect((read7.data as { unresolved: unknown[] }).unresolved).toHaveLength(0)

    // ---------- ⑨ 旧格式在途（无会话列）：凭池解析别名也得归到该会话名下 ----------
    // 旧版工具写的表没「会话」列；在途身份若只看自带列，这条就永远核不出会话——留着它，
    // 读态会把它报成「无从核实」，而它事实上就是池里那个工人未收的那一单。
    await Bun.write(join(s.root, ".evolve", "control.md"), [
      "## 项目坐标", `- 项目根：${s.root}`, "", "## 计数", "- 呼吸：3", "- 派工序号：13", "",
      "## worker 池", "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |", "| --- | --- | --- | --- | --- | --- | --- |",
      `| drift-3 | ${sid} | drift | 第四轮 | 5000 | 8 | idle |`, "",
      "## 在途", "| 别名 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- |",
      "| drift-3 | 第四轮 |  | 6000 | 9 |", "",
      "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", "",
    ].join("\n"))
    const st10 = parseControl(controlOf(s))
    expect(inflightSession(st10.inflight[0], st10.workers)).toBe(sid)          // 旧格式经池解析仍核得出会话
    const read10 = await tool("state").execute({}, s.ctx)
    expect((read10.data as { unresolved: unknown[] }).unresolved).toHaveLength(0)   // 不得谎报「无从核实」
    expect((read10.data as { awaiting: unknown[] }).awaiting).toHaveLength(1)       // 它就是待收藏那一单
    const c3 = await tool("collect").execute({ action: "collect", session_id: sid, summary: "旧格式残留" }, s.ctx)
    expect(c3.output).toContain("收藏完成")
    expect(parseControl(controlOf(s)).inflight).toHaveLength(0)

    // ---------- ⑩ 寻址取会话（主键）而非别名（显示名）----------
    // 池条目已被清、只剩在途残留时，同时传「陈旧别名 + 会话 id」：命中说明必须回工 **session_id**
    // （别名是那个工人已改掉的旧名，拿它当寻址依据会把「同一个工人的旧名」当成另一个工人）。
    await Bun.write(join(s.root, ".evolve", "control.md"), [
      "## 项目坐标", `- 项目根：${s.root}`, "", "## 计数", "- 呼吸：3", "- 派工序号：13", "",
      "## worker 池", "（空）", "",
      "## 在途", "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- | --- |",
      `| drift-4 | ${sid} | 第五轮 |  | 7000 | 10 |`, "",
      "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", "",
    ].join("\n"))
    const staleRt = await tool("collect").execute({ action: "retire", alias: "drift-4", session_id: sid, mode: "idle", reason: "定向清理" }, s.ctx)
    expect(staleRt.output).toContain("已清掉陈旧在途登记")
    expect(staleRt.output).toContain(`会话 ${sid}`)                        // 认的是会话（主键），不是那个旧别名
    expect(staleRt.data).toMatchObject({ inflight: { alias: "drift-4", sessionId: sid } })
    expect(parseControl(controlOf(s)).inflight).toHaveLength(0)

    // ---------- ⑪ `mode=stop` 退役（超时终止那条路）也得按会话 id 带走漂移残留 ----------
    // 与 ⑥ 的分工：那条走 `mode=idle`（保留会话）、这条走 `mode=stop`（终止运行中会话）——
    // 两条分支各自清理在途，任一漏改就各自漏一条。此处池里有该会话（别名 drift-9，派工时的名字），
    // 在途残留却是旧名 drift-5；**只传 session_id 寻址**（不传别名）——别名过滤在写侧没东西可过滤。
    await Bun.write(join(s.root, ".evolve", "control.md"), [
      "## 项目坐标", `- 项目根：${s.root}`, "", "## 计数", "- 呼吸：3", "- 派工序号：14", "",
      "## worker 池", "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |", "| --- | --- | --- | --- | --- | --- | --- |",
      `| drift-9 | ${sid} | drift | 第六轮：收尾 | 7000 | 11 | running |`, "",
      "## 在途", "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- | --- |",
      `| drift-5 | ${sid} | 第六轮：收尾 |  | 8000 | 12 |`, "",
      "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", "",
    ].join("\n"))
    const stopRetire = await tool("collect").execute({ action: "retire", session_id: sid, mode: "stop", reason: "超 40 分钟" }, s.ctx)
    expect(stopRetire.output).toContain("已终止并退役")
    const st11 = parseControl(controlOf(s))
    expect(st11.inflight).toHaveLength(0)                                     // 旧名残留一并未带走
    expect(st11.workers.some((w) => w.sessionId === sid)).toBe(false)
    expect(st11.seeds.map((x) => x.direction)).toContain("第六轮：收尾")        // 方向退回种子、不得丢

    // ---------- ⑫ 寻址取会话（主键）而非别名（显示名）——两者指向**不同**在途时的取舍 ----------
    // 别名的病在于它不是身份：传进来的别名可能属于**别的会话**的在途残留。假设盘上有两条在途：
    // `ghost-1`（别人的，别名恰好等于传入别名）与 `drift-6`（本会话的，别名就是旧名）。
    // 按别名优先会清掉**别人的那条**、留下自己那条——越权且没清干净；按会话 id 优先才对。
    await Bun.write(join(s.root, ".evolve", "control.md"), [
      "## 项目坐标", `- 项目根：${s.root}`, "", "## 计数", "- 呼吸：3", "- 派工序号：15", "",
      "## worker 池", "（空）", "",
      "## 在途", "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- | --- |",
      `| ghost-1 | sess-other | 别人的方向 |  | 9000 | 4 |`,
      `| drift-6 | ${sid} | 本会话的方向 |  | 9000 | 13 |`, "",
      "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", "",
    ].join("\n"))
    const pick = await tool("collect").execute({ action: "retire", alias: "ghost-1", session_id: sid, mode: "idle", reason: "定向清理" }, s.ctx)
    expect(pick.output).toContain("已清掉陈旧在途登记")
    expect(pick.output).toContain(`会话 ${sid}`)                              // 命中的是本会话那条
    const st12 = parseControl(controlOf(s))
    expect(st12.inflight.map((i) => i.alias)).toEqual(["ghost-1"])            // 别人的那条分毫未动
    expect(st12.seeds.map((x) => x.direction)).toContain("本会话的方向")
    expect(st12.seeds.map((x) => x.direction)).not.toContain("别人的方向")
    cleanup(s)
  })

  // ------------------------------------------------------------------
  // 在途登记格式兼容：盘上的**旧格式在途条目**（无会话列）必须"不误判、不静默丢弃"
  //
  // 这条测的是迁移面而不是新增面：e1e803ce 把身份键单一化为会话 id 后，"会话列自带"成了主路，
  // 而**盘上的存量行没有这一列**（工具升级不迁移数据，旧行会一直躺到被收藏/退役）。
  // 两条历史形态最容易漏：
  //   ① 旧行靠**池解析别名**换回会话——若身份键只认自带列，同一会话就会因新旧两代键不同而并列两条；
  //   ② 旧行没有转义，**方向里一个裸竖线就多切一格**——格数在旧表里不可靠，把 5 列的旧行顶成
  //      "6 列的新版行"、把方向碎片填进会话位。①② 都会把**可达会话误判为「中道而废」**
  //      （拿登记猜真值——这正是身份键单一化要堵的假阳性，只是从旧格式这一侧重新漏进来）。
  // 夹具是只读的（fixtures/control-legacy-inflight.md），用例只读它、在临时目录重建现场。
  // ------------------------------------------------------------------

  test("格式兼容：旧格式在途（无会话列）按池解析认帐、含裸竖线不被误当新版行，读态/寻址不误判也不丢弃", async () => {
    // —— ⓪ 夹具本身：形状、列数、往返（先把夹具的形态钉住，后面的断言才有根据）——
    const fix = parseControl(LEGACY_FIXTURE)
    expect(fix.workers.map((w) => w.alias)).toEqual(["wb-1", "legacy-2"])
    // 池表的**任务**列含裸竖线也不得截断（同一张渲染器的同类病：尾列并回、正文完整）
    expect(fix.workers[1].task).toContain("mode==='view' | stat.editable===false")
    expect(fix.workers[1].round).toBe(40)                         // 轮次/时间没被多切出来的格顶掉
    expect(fix.workers[1].dispatchedAt).toBe(1792087248971)
    expect(fix.workers[1].status).toBe("running")
    // 四条在途都**无会话列**（旧格式）：自带列解析不出来，只能靠池
    expect(fix.inflight.map((i) => i.alias)).toEqual(["wb-1", "legacy-2", "wb-2", "ghost-9"])
    expect(fix.inflight.every((i) => !i.sessionId)).toBe(true)
    // 旧行不可因含裸竖线而被当新版行：方向/交付各自完整（含各自的竖线），尾列不被溢出的格顶掉
    const wb = fix.inflight.find((i) => i.alias === "wb-1")!
    const lg = fix.inflight.find((i) => i.alias === "legacy-2")!
    expect(wb.round).toBe(39)
    expect(wb.dispatchedAt).toBe(1792087248971)
    expect(lg.direction).toContain("mode==='view'")
    expect(lg.round).toBe(37)                                    // 行尾两位是时间/轮次，不是正文
    expect(lg.dispatchedAt).toBe(1792077365617)
    // 预期产出里的裸竖线**不得丢**：裸切后它落在「轮次/时间」两格之前，必须被并回预期产出
    expect(lg.deliverable).toContain("实测截图或 DOM 断言证据")
    expect(lg.direction).toContain("stat.editable===false | truncated | roots.writable===false")   // 方向不得被腰斩
    // 夹具在途段：恰 4 条数据行，且其中含裸竖线那条**按裸分隔符切会多出格**（夹具确实带着这个病）
    const flightSection: string[] = []
    let inFlight = false
    for (const l of LEGACY_FIXTURE.split(/\r?\n/)) {
      if (/^##\s/.test(l)) { inFlight = l.trim() === "## 在途"; continue }
      if (inFlight && l.trim().startsWith("|")) flightSection.push(l)
    }
    const dataRows = flightSection.filter((l) => !/^\|[\s:|-]+\|$/.test(l.trim()) && !l.includes("| 别名 |"))
    expect(dataRows).toHaveLength(4)
    const naive = (l: string) => l.trim().replace(/^\|/, "").replace(/\|$/, "").split("|")
    const pipedRow = dataRows.find((l) => l.startsWith("| legacy-2 |"))!
    expect(naive(pipedRow).length).toBeGreaterThan(6)            // 病在：裸竖线把旧行切成 7 格
    expect(fix.inflight.find((i) => i.alias === "legacy-2")!.sessionId).toBe("")   // 但读出来仍是旧格式
    // 夹具往返恒等（渲染→解析→渲染不变形：不得把旧行升级成新版行）
    expect(parseControl(renderControl(fix))).toEqual(fix)
    // 只读约束：本条用例跑完夹具**逐字节未变**（任务纪律：不得改写用户既有状态）
    expect(readFileSync(join(import.meta.dir, "fixtures", "control-legacy-inflight.md"), "utf8")).toBe(LEGACY_FIXTURE)

    // —— ① 端到端：把夹具搬进临时目录的真现场（根改成临时根），走读态与寻址 ——
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test packages/agents/src/agents/shengsheng/" }, s.ctx)
    const SID_WB = "aaaabbbbccccddddeeeeffff00001111"       // 活会话（有清单）
    const SID_LEGACY = "bbbbaaaaccccddddeeeeffff11110000"   // 活会话（有清单）
    expect(isSessionIdShape(SID_WB)).toBe(true)
    expect(isSessionIdShape(SID_LEGACY)).toBe(true)
    // 把夹具按临时根重建（只换“项目根”一行；存量旧行原样搬）
    const relocated = LEGACY_FIXTURE.replace(/^- 项目根：.*$/m, `- 项目根：${s.root}`)
    await Bun.write(join(s.root, ".evolve", "control.md"), relocated)
    // 两个池会话都**可达且空闲**——这是最不利的对照：误判成「中道而废」会直接露出来
    s.sessions.set(SID_WB, { id: SID_WB, messages: [{ role: "assistant", content: "报告：交付 X；验证 Y（尾巴够长以落入待收藏）".padEnd(260, "、") }] })
    s.sessions.set(SID_LEGACY, { id: SID_LEGACY, messages: [{ role: "assistant", content: "报告：交付 Z；验证 W（尾巴够长以落入待收藏）".padEnd(260, "、") }] })

    // 读态：可达会话的两条进「待收藏」；`ghost-9` 无任何会话线索 → 第三类「无从核实」（不诬告已死、不谎报可达）
    const read = await tool("state").execute({}, s.ctx)
    const rd = read.data as { awaiting: { alias: string }[]; gone: { alias: string }[]; unresolved: { alias: string }[] }
    expect(rd.awaiting.map((i) => i.alias).sort()).toEqual(["legacy-2", "wb-1"])
    expect(rd.gone).toHaveLength(0)
    expect(rd.unresolved.map((i) => i.alias)).toEqual(["wb-2", "ghost-9"])   // 池里没有这两个别名 → 无从核实
    expect(read.output).not.toContain("中道而废")            // 旧格式不得被读成“会话没了”
    expect(read.output).toContain("无从核实")
    // 读态也不得把旧行静默丢弃：三条都在在途一览里（且旧行的会话经池解析回了真会话）
    for (const i of fix.inflight) expect(read.output).toContain(`- ${i.alias}｜`)
    expect(read.output).toContain(`会话 ${SID_LEGACY.slice(0, 8)}…`)   // 旧行认回了池里那个会话

    // ① -a review 按 alias 命中旧行：认得它是池里那个 worker，在途登记读得出、方向完整
    const rv = await tool("collect").execute({ action: "review", alias: "legacy-2" }, s.ctx)
    expect(rv.output).toContain("worker legacy-2")
    expect(rv.output).toContain(`会话 ${SID_LEGACY}`)
    expect(rv.output).toContain("在途登记：文件工作台：状态栏「只读」标记")
    expect(rv.output).not.toContain("陈旧登记")               // 不得当“只在在途里的陈旧登记”处理

    // ① -b 旧行也能被「按会话 id」寻到（经池解析别名回工——寻址判据与会话一致）
    const rvSid = await tool("collect").execute({ action: "review", session_id: SID_LEGACY }, s.ctx)
    expect(rvSid.output).toContain("按 session_id=")
    expect(rvSid.output).toContain("worker legacy-2")

    // —— ② 「无会话可探」必须按**中道而废**而不是崩溃/误报可达 ——
    // 把池里的 wb-1 换成一条**会话已不存在**的登记（旧格式行仍无会话列），并把 legacy-2 回收掉
    s.sessions.delete(SID_LEGACY)                                  // 该会话已从存储消失（外部删除）
    await Bun.write(join(s.root, ".evolve", "control.md"), [
      "## 项目坐标", `- 项目根：${s.root}`, "",
      "## 计数", "- 呼吸：18", "- 派工序号：40", "",
      "## worker 池", "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |", "| --- | --- | --- | --- | --- | --- | --- |",
      `| wb-1 | ${SID_LEGACY} | workbench-ui | 会话已消失的历史轮 | 1792087248971 | 39 | running |`, "",
      "## 在途", "| 别名 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- |",
      "| wb-1 | 第三轮：老会话的方向 | 预期产出甲 | 1792087248971 | 39 |",
      "| ghost-9 | 只在这张文里出现过的在途方向 |  | 1792077365617 | 12 |", "",
      "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", "",
    ].join("\n"))
    const read2 = await tool("state").execute({}, s.ctx)
    const rd2 = read2.data as { awaiting: { alias: string }[]; gone: { alias: string }[]; unresolved: { alias: string }[] }
    expect(rd2.gone.map((i) => i.alias)).toEqual(["wb-1"])          // 探过且确不存在 → 中道而废
    expect(rd2.awaiting).toHaveLength(0)
    expect(rd2.unresolved.map((i) => i.alias)).toEqual(["ghost-9"]) // 没探过的仍归第三类，不混入
    expect(read2.output).toContain("中道而废")

    // ② -a 中道而废那条走 retire alias=… mode=idle：清干净且方向退回种子（旧行不得静默消失）
    const rt = await tool("collect").execute({ action: "retire", alias: "wb-1", mode: "idle", reason: "夹具：会话确已不存在" }, s.ctx)
    expect(rt.output).toContain("已退役")
    const after = parseControl(controlOf(s))
    expect(after.inflight.map((i) => i.alias)).toEqual(["ghost-9"])   // 只清那一条
    expect(after.workers).toHaveLength(0)
    expect(after.seeds.map((x) => x.direction)).toContain("第三轮：老会话的方向")

    // ② -b 无线索那条按「无从核实」的处置路径定向清理（旧行的方向仍然不许丢）
    const rtGhost = await tool("collect").execute({ action: "retire", alias: "ghost-9", mode: "idle", reason: "夹具：核不到会话" }, s.ctx)
    expect(rtGhost.output).toContain("已清掉陈旧在途登记")
    const after2 = parseControl(controlOf(s))
    expect(after2.inflight).toHaveLength(0)
    expect(after2.seeds.map((x) => x.direction)).toContain("只在这张文里出现过的在途方向")

    // —— ③ 反向对照：**新版**行（自带会话列）仍按新版读——也不得被旧格式兼容逻辑拖回去 ——
    await Bun.write(join(s.root, ".evolve", "control.md"), [
      "## 项目坐标", `- 项目根：${s.root}`, "",
      "## 计数", "- 呼吸：19", "- 派工序号：41", "",
      "## worker 池", "（空）", "",
      "## 在途", "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- | --- |",
      `| ghost-1 | ${SID_WB} | 新版行的方向 | 预期产出 | 5000 | 42 |`, "",
      "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", "",
    ].join("\n"))
    const st3 = parseControl(controlOf(s))
    expect(st3.inflight[0].sessionId).toBe(SID_WB)                     // 自带列被采纳（池里没有它也认得）
    expect(st3.inflight[0].direction).toBe("新版行的方向")
    expect(st3.inflight[0].round).toBe(42)
    // 池外 + 自带会话列：会话可达 → 待收藏；**不得**退化成“无从核实”
    s.sessions.set(SID_WB, { id: SID_WB, messages: [{ role: "assistant", content: "新版行报告：交付 Y；验证 Z（尾巴够长以落入待收藏）".padEnd(260, "、") }] })
    const read3 = await tool("state").execute({}, s.ctx)
    const rd3 = read3.data as { awaiting: { alias: string }[]; unresolved: unknown[] }
    expect(rd3.awaiting.map((i) => i.alias)).toEqual(["ghost-1"])
    expect(rd3.unresolved).toHaveLength(0)
    cleanup(s)
  })

  // ------------------------------------------------------------------
  // 旧格式行的「两难格」：一格既非会话 id 形状、也不在池里，而手写新格式行又必须被完整读回。
  //
  // 这是格式兼容里唯一真正两难的一格：同一份文本（6 格、第 2 格是自由文本）可以是
  //   ① 旧格式行的方向被一个裸竖线切成两格（实例：本仓现存 control.md 的在途表就是这种行）；
  //   ② 手写/外部工具写的**新格式行**，会话 id 不是引擎发的 32hex。
  // 两种读法不可能同时正确，只能选代价小的一侧——**选①**（按旧版读），因为：
  //   认错（把方向碎片当会话）= 拿伪线索探一个不存在的会话 → 可达会话被判「中道而废」（假阳性）；
  //   不认（丢掉真 id）= 回落到池按别名解析同一会话，**值一模一样**。
  // 下面把两侧都钉死，免得日后有人“顺手”改宽。
  // ------------------------------------------------------------------

  test("格式兼容·两难格：6 格而第 2 格不像会话 id 时按旧版读（宁可回落到池，也不拿伪线索探会话）", () => {
    const SID = "aaaabbbbccccddddeeeeffff00001111"
    const HEAD_NEW = ["| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- | --- |"]
    const HEAD_OLD = ["| 别名 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- |"]
    const POOL = ["## worker 池", "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |", "| --- | --- | --- | --- | --- | --- | --- |"]
    const TAIL = ["", "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", ""]
    const doc = (pool: string[], flightHead: string[], flight: string[]) =>
      ["## 项目坐标", "- 项目根：/x", "", "## 计数", "- 呼吸：1", "- 派工序号：9", "", ...POOL, ...pool, "", "## 在途", ...flightHead, ...flight, ...TAIL].join("\n")

    // ① 旧表头 + 方向被裸竖线切成 6 格 → 仍是旧版行（会话列不得从方向碎片里长出来）
    const legacySplit = doc([`| wb-1 | ${SID} | d | t | 100 | 3 | idle |`], HEAD_OLD, ["| wb-1 | 方向甲 | 带竖线 | 交付乙 | 100 | 3 |"])
    const stA = parseControl(legacySplit)
    const a = stA.inflight[0]
    expect(a.sessionId).toBe("")                                       // 不给方向碎片发会话名
    expect(inflightSession(a, stA.workers)).toBe(SID)                  // 但经池仍认得出会话
    expect(a.round).toBe(3)                                            // 尾列取量后两位，不被溢出的格顶掉
    expect(a.dispatchedAt).toBe(100)
    expect(a.direction).toBe("方向甲")
    expect(a.deliverable).toBe("带竖线 | 交付乙")                        // 溢出的格并回预期产出，不丢

    // ② 同一格形态但表头已升级为 6 列且本行**恰 6 格**（对位无溢格）：按新版读
    const alignedNew = doc([`| wb-1 | ${SID} | d | t | 100 | 3 | idle |`], HEAD_NEW, ["| wb-1 | 手写的会话 id | 方向甲 | 交付乙 | 100 | 3 |"])
    const b = parseControl(alignedNew).inflight[0]
    expect(b.sessionId).toBe("手写的会话 id")
    expect(b.direction).toBe("方向甲")
    expect(b.deliverable).toBe("交付乙")
    expect(b.round).toBe(3)

    // ③ 新表头但**会话是池里认得的合成 id**（非 32hex）、**且行溢了格**：仍按新版读（只靠池这一档）
    const poolSynthetic = doc(["| wb-1 | sess-abc | d | t | 100 | 3 | idle |"], HEAD_NEW, ["| wb-1 | sess-abc | 方向甲 | 带竖线 | 交付乙 | 100 | 3 |"])
    const c = parseControl(poolSynthetic).inflight[0]
    expect(c.sessionId).toBe("sess-abc")
    expect(c.direction).toBe("方向甲")
    expect(c.deliverable).toBe("带竖线 | 交付乙")

    // ④ 池表的**任务**列（同一张渲染器的同类位）溢了格也不得截断
    const poolPiped = doc(
      ["| wb-1 | sess-abc | d | 任务甲 | 带竖线 | 交付乙 | 100 | 3 | idle |"],
      HEAD_NEW, ["| wb-1 | sess-abc | 方向甲 | 交付乙 | 100 | 3 |"])
    const pw = parseControl(poolPiped).workers[0]
    expect(pw.task).toBe("任务甲 | 带竖线 | 交付乙")
    expect(pw.round).toBe(3)
    expect(pw.dispatchedAt).toBe(100)
    expect(pw.status).toBe("idle")

    // ⑤ 新版行**溢了格**（方向/预期产出里是裸竖线）：会话列照旧自带、溢出格并回、不丢
    const rawPipeNew = doc([], HEAD_NEW, [`| wb-1 | ${SID} | 方向甲 | 带竖线 | 交付乙 | 100 | 3 |`])
    const stRaw = parseControl(rawPipeNew)
    expect(stRaw.inflight[0].sessionId).toBe(SID)
    expect(stRaw.inflight[0].direction).toBe("方向甲")
    expect(stRaw.inflight[0].deliverable).toBe("带竖线 | 交付乙")
    expect(stRaw.inflight[0].round).toBe(3)

    // ⑤ 新版行但**方向恰是一串 32hex**（工单原文里带 hash）：表头说 6 列时，形状/池都对不上时
    // 「表头正对位」这一档必须站住（否则这行会被丢回旧版读，方向被当成会话）
    const alignedHex = doc([], HEAD_NEW, [`| wb-1 | ${SID} | 方向甲 | 交付乙 | 100 | 3 |`])
    const stAligned = parseControl(alignedHex)
    expect(stAligned.inflight[0].sessionId).toBe(SID)
    expect(stAligned.inflight[0].direction).toBe("方向甲")

    // ⑥ 尾列取的是**行尾两位**而非固定下标（溢格后若按 c[3]/c[4] 取会拿到正文）
    const tailProbe = doc([], HEAD_NEW, [`| wb-1 | ${SID} | 方向甲\\|甲 | 带竖线 | 交付乙\\|乙 | 100 | 7 |`])
    const tp = parseControl(tailProbe).inflight[0]
    expect(tp.dispatchedAt).toBe(100)
    expect(tp.round).toBe(7)
    expect(tp.direction).toBe("方向甲|甲")
    expect(tp.deliverable).toBe("带竖线 | 交付乙|乙")

    // ⑦ 形状档：真会话 id 即使不在池、且行**溢了格**（长度不是 6）也认——池条目被清后的残留就靠它认
    const shapeOnly = doc([], HEAD_NEW, [`| wb-1 | ${SID} | 方向甲\\|带竖线 | 交付乙 | 100 | 3 |`])
    const stShape = parseControl(shapeOnly)
    expect(stShape.inflight[0].sessionId).toBe(SID)
    expect(stShape.inflight[0].direction).toBe("方向甲|带竖线")

    // ⑧ 旧表头档不能被前几档劫走：旧表头的行无论第 2 格长得像不像会话 id，都是旧版行
    //（否则「方向恰好是一串 32hex」就会被当成会话去探一个不存在的会话）
    const legacyHexLike = doc([`| wb-1 | ${SID} | d | t | 100 | 3 | idle |`], HEAD_OLD, [`| wb-1 | ${SID} | 交付乙 | 100 | 3 |`])
    const stHex = parseControl(legacyHexLike)
    expect(stHex.inflight[0].sessionId).toBe("")
    expect(stHex.inflight[0].direction).toBe(SID)                     // 它就是方向本身
    expect(inflightSession(stHex.inflight[0], stHex.workers)).toBe(SID)   // 会话仍经池认得

    // ⑨ 表头档的**判别格**：旧表头 + 6 格行且第 2 格**确实是 32hex**——这是「形状/池/对位」三档都
    // 会点头的一格（形状像是 id、恰好也等于池里那个会话、格数也对）。但表头说这张表没有会话列，
    // 旧表里多出来的第 6 格只能来自**方向里的裸竖线**（旧渲染器就是这个病）——故仍按旧版读：
    // 方向就是那串 32hex、不得把它升格成会话（升格等于拿方向去探会话，会把可达的那条误判为已死）。
    const legacyHexSix = doc([`| wb-1 | ${SID} | d | t | 100 | 3 | idle |`], HEAD_OLD, [`| wb-1 | ${SID} | 交付甲 | 带竖线 | 100 | 3 |`])
    const stHex6 = parseControl(legacyHexSix).inflight[0]
    expect(stHex6.sessionId).toBe("")                                  // 表头说旧版 → 不升格
    expect(stHex6.direction).toBe(SID)
    expect(stHex6.deliverable).toBe("交付甲 | 带竖线")                  // 溢出格并回预期产出（不丢正文）
    expect(stHex6.dispatchedAt).toBe(100)                              // 尾列仍取行尾两位
    expect(stHex6.round).toBe(3)
    expect(inflightSession(stHex6, parseControl(legacyHexSix).workers)).toBe(SID)

    // ⑨ 新旧两代行**同一张表并存**：旧行靠池解析、新行自带列——两行归一为同一会话一条（不是并列两条）
    const mixed = doc([`| wb-1 | ${SID} | d | t | 100 | 9 | idle |`], HEAD_NEW, [
      `| wb-1 | ${SID} | 新版轮 |  | 200 | 9 |`,
      "| wb-1 | 旧版轮 |  | 150 | 3 |",
    ])
    const st = parseControl(mixed)
    expect(st.inflight).toHaveLength(1)                                // 同会话（旧行经池解析）→ 归一
    expect(st.inflight[0].round).toBe(9)                               // 幸存者是轮次更大那条
    expect(st.inflight[0].sessionId).toBe(SID)
  })

  // ------------------------------------------------------------------
  // 版本化解析：把「格式版本 + 升级函数」从散落的旧行分支里抽成显式小模块。
  //
  // 两节：① **等价性基线**——逐字节对照（语料 × 现场 × 写入），保证重构不改行为；
  //       ② **版本契约**——判代是显式可断言的（`parseInflightRow` 回 `{ ver, fields }`），
  //          多代行混排各归各代，加列只需加一支版本分支。
  // ------------------------------------------------------------------

  test("版本化解析·等价性基线：多版本语料上 parseControl/renderControl 与升级前逐字节一致（含写入等价性）", () => {
    // 黄金基线从**升级前**的实现捕获、随语料入库（只读）：重构后必须逐字节不变。
    // 比对的四维：state（解析结果）、sessions（每条在途归哪个会话——身份键不许漂）、
    // rendered（**写入等价性**：已落盘 control.md 的渲染结果不许变）、roundTrip（写入→重读恒等）。
    const golden = JSON.parse(readFileSync(join(import.meta.dir, "fixtures", "control-format-corpus", "golden.json"), "utf8")) as Record<
      string,
      { state: unknown; sessions: unknown; rendered: string; roundTrip: unknown }
    >
    expect(Object.keys(golden).sort()).toEqual(["01-new-header-mixed.md", "02-legacy-header-mixed.md"])
    for (const [file, g] of Object.entries(golden)) {
      const md = readFileSync(join(import.meta.dir, "fixtures", "control-format-corpus", file), "utf8")
      const st = parseControl(md)
      // `toStrictEqual`：**字段存在性也算契约**（旧代必须补 `sessionId: ""` 而不是缺这个键——
      // 对象形状与现行版一致是往返恒等与下游取数的前提）。
      expect(st, `${file} 解析结果等价`).toStrictEqual(g.state as never)
      expect(st.inflight.map((i) => inflightSession(i, st.workers) ?? null), `${file} 会话归属等价`).toStrictEqual(g.sessions as never)
      expect(renderControl(st), `${file} 渲染逐字节等价`).toBe(g.rendered)
      expect(parseControl(renderControl(st)), `${file} 往返恒等`).toStrictEqual(g.roundTrip as never)
      // 旧代行必带 `sessionId` 这个键（不能是缺键）——统一形状是升级函数的职责
      for (const i of st.inflight) expect(Object.keys(i)).toContain("sessionId")
    }
    // 现场 ③：本仓存量旧格式夹具（`.evolve/control.md` 的在途表就长这样）也不得变形
    const legacy = parseControl(LEGACY_FIXTURE)
    expect(renderControl(legacy)).toBe(renderControl(parseControl(renderControl(legacy))))
  })

  test("版本契约：判代是显式的（ver 可断言），多代行混排各归各代，加列只需加一支版本分支", () => {
    const SID = "aaaabbbbccccddddeeeeffff00001111"
    const pool = new Set([SID])

    // —— 版本声明表自身的契约（“加一列 = 加一支版本分支”的机制保证）——
    // 版本号严格递增、列名不重复、列数即表头声明数；现行版就是声明表的最后一条。
    for (const [name, decls] of [
      ["在途", INFLIGHT_VERSIONS as ReadonlyArray<{ ver: number; columns: readonly string[] }>],
      ["池", POOL_VERSIONS as ReadonlyArray<{ ver: number; columns: readonly string[] }>],
    ] as const) {
      expect(decls.length, `${name}至少两代`).toBeGreaterThanOrEqual(2)
      decls.forEach((d, i) => {
        expect(d.ver, `${name}版本号递增`).toBe(i + 1)
        if (i) expect(d.columns.length, `${name}只加列不减列`).toBeGreaterThanOrEqual(decls[i - 1].columns.length)
        expect(new Set(d.columns).size, `${name}列名不重复`).toBe(d.columns.length)
        if (i) for (const col of decls[i - 1].columns) expect(d.columns, `${name}既有列保留`).toContain(col)
      })
    }
    expect(INFLIGHT_CURRENT).toBe(INFLIGHT_VERSIONS[INFLIGHT_VERSIONS.length - 1].ver)
    expect(POOL_CURRENT).toBe(POOL_VERSIONS[POOL_VERSIONS.length - 1].ver)
    // 自带会话列只在末代（现行）为真——它正是本次版本化的核心差异
    expect(INFLIGHT_VERSIONS.filter((v) => v.ownSession).map((v) => v.ver)).toEqual([INFLIGHT_CURRENT])
    // **加列只动声明**的可验证保证（“判代不得写死代次/列数”——这正是散落旧行分支的病根）：
    // ① 每代的**位置声明**只能提字段名（不得出现表外的键：升级函数不认的键就是死声明）；
    // ② 判代是**纯函数**（同一输入两次调用同结果）——不与调用次序/残留状态相关；
    // ③ 判代按**声明的列数**判“本行装得下哪一代”（见下各条正向用例：v1/v2/v3 的边界就是列数）。
    const inflightKeys = new Set(["alias", "sessionId", "direction", "deliverable", "dispatchedAt", "round"])
    const poolKeys = new Set(["alias", "sessionId", "domain", "task", "dispatchedAt", "round", "status"])
    for (const d of INFLIGHT_VERSIONS) for (const k of Object.keys(d.read)) expect(inflightKeys, `在途#${d.ver} 位置声明的键须是已知字段`).toContain(k)
    for (const d of POOL_VERSIONS) for (const k of Object.keys(d.read)) expect(poolKeys, `池#${d.ver} 位置声明的键须是已知字段`).toContain(k)
    const probeRow = ["a", SID, "方向", "产出", "100", "3"]
    expect(selectInflightVer(probeRow, { declared: 6 })).toBe(selectInflightVer(probeRow, { declared: 6 }))
    expect(parseInflightRow(probeRow, { declared: 6 })).toEqual(parseInflightRow(probeRow, { declared: 6 }))
    const probePool = ["a", SID, "d", "t", "100", "7", "idle"]
    expect(selectPoolVer(probePool, { declared: 7 })).toBe(selectPoolVer(probePool, { declared: 7 }))
    // 行入口（parseInflightLine）与格入口（parseInflightRow）同源：给出同一 `{ ver, fields }`
    expect(parseInflightLine(`| a | ${SID} | 方向 | 产出 | 100 | 3 |`, { declared: 6 })).toEqual(parseInflightRow(["a", SID, "方向", "产出", "100", "3"], { declared: 6 }))

    // —— 判代（在途）：每一档证据各自的判代结果——
    // v3：自带会话列（形状档 / 同源档 / 空值档）
    expect(parseInflightRow(["a", SID, "方向", "产出", "100", "3"], { declared: 6 }).ver).toBe(3)
    expect(parseInflightRow(["a", SID, "方向", "产出", "100", "3"], { declared: 6, poolSessions: pool }).ver).toBe(3)
    expect(parseInflightRow(["a", "", "方向", "产出", "100", "3"], { declared: 6 }).ver).toBe(3)
    // v3：表头正对位（6 格且声明 6 列）——即使会话列不像 id、也不在池里
    expect(parseInflightRow(["a", "手写的会话", "方向", "产出", "100", "3"], { declared: 6 }).ver).toBe(3)
    // v3：非 32hex 但**在池里认得**（同源档）——溢了格也认
    expect(parseInflightRow(["a", "sess-abc", "方向", "溢格", "产出", "100", "3"], { declared: 6, poolSessions: new Set(["sess-abc"]) }).ver).toBe(3)
    // v2：表头说旧版（声明 5 列）——无论第 2 格长得像不像会话 id，都不升格
    expect(parseInflightRow(["a", SID, "方向", "产出", "100", "3"], { declared: 5, poolSessions: pool }).ver).toBe(2)
    // v2：无表头声明 + 6 格 + 第 2 格不像会话 id（**两难格**）——按保守一侧读旧版
    expect(parseInflightRow(["a", "方向碎片", "方向甲", "产出", "100", "3"], {}).ver).toBe(2)
    // v2：格数 ≥ 5（5 格、7 格都算）——尾列锚定对溢格也是正确读法
    expect(parseInflightRow(["a", "方向", "产出", "100", "3"], {}).ver).toBe(2)
    expect(parseInflightRow(["a", "方向", "产出1", "产出2", "100", "3"], {}).ver).toBe(2)
    // v1：无轮次列（4 格）——轮次为 0，不再拿第 3 格当时间
    expect(parseInflightRow(["a", "方向", "产出", "100"], {}).ver).toBe(1)
    // **判代先看“本行装得下哪一代”**（列数就是边界）：只 5 格的行即便第 2 格像会话 id、池里也认得，
    // 也不得升格为 v3——5 格里根本没有「会话」那一列，第 2 格就是方向本身
    //（旧行的方向恰是一串 hash 的形态）。否则方向会被当成会话→探一个不存在的会话→假阳性。
    const fiveCell = parseInflightRow(["a", SID, "方向", "产出", "100"], { declared: 6, poolSessions: pool })
    expect(fiveCell.ver).toBe(2)
    expect(fiveCell.fields.direction).toBe(SID)

    // —— 升级函数：各代 → 现行形态（字段齐全、类型定型） ——
    expect(migrateInflight(parseInflightRow(["a", "方向", "产出", "100"], {}))).toEqual({ alias: "a", sessionId: "", direction: "方向", deliverable: "产出", dispatchedAt: 100, round: 0 })
    expect(migrateInflight(parseInflightRow(["a", SID, "方向", "产出", "100", "7"], { declared: 6 }))).toEqual({ alias: "a", sessionId: SID, direction: "方向", deliverable: "产出", dispatchedAt: 100, round: 7 })

    // —— 判代（池）：表头声明即代次 ——
    expect(parsePoolRow(["a", SID, "d", "t", "100", "7", "idle"], { declared: 7 }).ver).toBe(2)
    expect(parsePoolRow(["a", SID, "d", "t", "100", "idle"], { declared: 6 }).ver).toBe(1)
    // 池表 v1 行的**历史读法**（本单只做抽取、不改语义）：v1 没有「轮次」列，而尾锚点仍按**历史上那一组**取——
    // 故 6 格 v1 行的「状态」位落到第 6 格（idle→idle）、时间位落到第 5 格（非数字→0）。
    // 这是已由既有用例盖住的旧行为，**不得在重构里「顺手改正」**（要修正须单独一单 + 专门用例）。
    expect(migratePool(parsePoolRow(["a", SID, "d", "t", "派发时间甲", "idle"], { declared: 6 })))
      .toEqual({ alias: "a", sessionId: SID, domain: "d", task: "t", dispatchedAt: 0, round: 0, status: "idle" })
    // 对照：**v2** 行（声明 7 列）在同一组锚点上是正确的——时间取第 5 格、轮次第 6 格、状态第 7 格
    expect(migratePool(parsePoolRow(["a", SID, "d", "t", "100", "7", "idle"], { declared: 7 })))
      .toEqual({ alias: "a", sessionId: SID, domain: "d", task: "t", dispatchedAt: 100, round: 7, status: "idle" })

    // —— 现行表头与格（写出侧）与版本声明同源，不会各写各的 ——
    expect(inflightHeader()).toEqual(["别名", "会话", "方向", "预期产出", "派发时间", "轮次"])
    expect(poolHeader()).toEqual(["别名", "会话", "领域", "任务", "派发时间", "轮次", "状态"])
    expect(inflightCells({ alias: "a", sessionId: undefined, direction: "d", deliverable: "p", dispatchedAt: 1, round: 2 })).toEqual(["a", "", "d", "p", "1", "2"])

    // ================================================================
    // 多版本行**同一张表混排**：判代逐行独立，各归各代，谁也不带偏谁。
    // 语料的表头是**新版**（声明 6 列）而盘上有历代写下的行（工具升级不迁移数据）：
    // ================================================================
    const POOL = ["## worker 池", "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |", "| --- | --- | --- | --- | --- | --- | --- |"]
    const TAIL = ["", "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", ""]
    const doc = (flightHead: string[], rows: string[]) =>
      ["## 项目坐标", "- 项目根：/x", "", "## 计数", "- 呼吸：1", "- 派工序号：9", "", ...POOL, `| wb-1 | ${SID} | d | t | 100 | 9 | idle |`, "", "## 在途", ...flightHead, ...rows, ...TAIL].join("\n")
    const HEAD_NEW = ["| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- | --- |"]
    // 四代形态同表（新表头下）：v1 残行 / v2 旧行 / v2 溢格旧行 / v3 自带列行 / v3 空会话列行
    // 别名各自区分：同名会因「无从核会话时退化为别名」而先被归一到一条，反而看不清单行判代。
    const mixed = doc(HEAD_NEW, [
      "| m-1 | 只有四格的残行 | 产出甲 | 700 |",                          // v1（盘上格数 < 5）
      "| m-2 | 旧行方向 | 产出乙 | 800 | 3 |",                           // v2（恰 5 格）
      "| m-3 | 旧行方向（含裸竖线）| 溢出甲 | 溢出乙 | 产出丙 | 800 | 4 |",    // v2 溢格（7 格 → 对齐档不成立）
      `| m-4 | ${SID} | 新版方向 | 产出丁 | 900 | 5 |`,                  // v3（形状/同源档）
      "| m-5 |  | 空会话列的新版行 | 产出戊 | 950 | 6 |",               // v3（空值档）
    ])
    const st = parseControl(mixed)
    const byAlias = (a: string) => st.inflight.find((i) => i.alias === a)!
    // **升级后形状统一**：不管哪一代读进来的，在途条目都是现行形态——六列全在（旧代缺的列补空值），
    // 不因代次而缺键（缺键会让下游取数与序列化各处各防一遍）。
    for (const i of st.inflight) expect(Object.keys(i).sort(), `升级后六列齐备（${i.alias}）`).toEqual(["alias", "deliverable", "direction", "dispatchedAt", "round", "sessionId"])
    // v1 行：轮次为 0（盘上无此列）；时间取第 4 格
    expect(byAlias("m-1")).toMatchObject({ direction: "只有四格的残行", deliverable: "产出甲", dispatchedAt: 700, round: 0, sessionId: "" })
    // v2 行：会话列为空、尾列取行尾两位
    expect(byAlias("m-2")).toMatchObject({ direction: "旧行方向", deliverable: "产出乙", dispatchedAt: 800, round: 3, sessionId: "" })
    // v2 溢格行：溢出的格并回交付（不丢正文）、会话列不得从碎片里长出来
    expect(byAlias("m-3")).toMatchObject({ direction: "旧行方向（含裸竖线）", deliverable: "溢出甲 | 溢出乙 | 产出丙", dispatchedAt: 800, round: 4, sessionId: "" })
    expect(inflightSession(byAlias("m-2"), st.workers)).toBeUndefined()   // 别名不在池里 → 无从核实（不谎报）
    // v3 行：会话列自带 / 空值——方向**不得**被下一格顶掉
    expect(byAlias("m-4")).toMatchObject({ direction: "新版方向", deliverable: "产出丁", dispatchedAt: 900, round: 5, sessionId: SID })
    expect(byAlias("m-5")).toMatchObject({ direction: "空会话列的新版行", deliverable: "产出戊", dispatchedAt: 950, round: 6, sessionId: "" })
    // 混排不归一为一条：只有 m-4 归到池里那个会话（另四条无会话线索 → 各留一条，不误杀）
    expect(st.inflight).toHaveLength(5)
    expect(st.inflight.filter((i) => inflightSession(i, st.workers) === SID)).toHaveLength(1)
    // 混排现场**写入等价**：渲染→重读不再变（判代稳定，不会每渲染一次就换一代）
    expect(parseControl(renderControl(st))).toEqual(st)

    // 反向：**旧表头**（声明 5 列）下的同批行仍各归各代——旧表头不因行像新版就升格
    const HEAD_OLD = ["| 别名 | 方向 | 预期产出 | 派发时间 | 轮次 |", "| --- | --- | --- | --- | --- |"]
    const old = parseControl(doc(HEAD_OLD, [
      `| wb-1 | ${SID} | 交付甲 | 800 | 3 |`,                            // 两难格：按旧版读（第 2 格是方向、不是会话）
      "| o-2 | 旧行方向 | 产出乙 | 800 | 4 |",                          // 普通旧行（别名不在池 → 无从核实）
    ]))
    expect(old.inflight.find((i) => i.alias === "wb-1")).toMatchObject({ sessionId: "", direction: SID, deliverable: "交付甲", round: 3, dispatchedAt: 800 })
    expect(inflightSession(old.inflight.find((i) => i.alias === "wb-1")!, old.workers)).toBe(SID)   // 会话仍经池认得
    expect(old.inflight.find((i) => i.alias === "o-2")).toMatchObject({ sessionId: "", direction: "旧行方向", round: 4 })
  })

  test("加一列只动声明：真的走一遍 parse → migrate → render 往返（新增字段），旧行仍能解析", () => {
    // 本用例是「版本化解析」这个抽象的**可执行证人**。它不靠文字声称“日后加列很容易”，而是真的：
    //   ① 在运行时给声明表加一族新版本（尾部加「心跳」列）；
    //   ② 用**原封不动的** `selectInflightVer` / `parseInflightRow` / `migrateInflight` /
    //      `writeRow` / `inflightHeader`（只多传一个「声明表」参数）走完
    //      **parse → migrate → render → 再解析**；
    //   ③ 断言往返等价、写出列序与声明同源、旧代行仍认得出。
    // 哪天有人把版本号/列序/列名列表写死回实现里（`c.length >= 6`、手写数组字面量那类），本条立刻红。
    //
    // 加一列**要动的地方全在下面这一块**（这正是收益：四处声明，而不是 N 处判据）：
    //   ① 版本号类型（TS 侧的联合类型）；② 一条版本声明；③ 现行版本标记（= 声明表末条，自动跟上）；
    //   ④ 写出侧取值器表加一行（新列要落盘时）。**判代 / 位置读取 / 升级 / 写出列序一行不改。**
    const SID = "aaaabbbbccccddddeeeeffff00001111"
    const V4 = {
      ver: 4 as const,
      columns: ["别名", "会话", "方向", "预期产出", "派发时间", "轮次", "心跳"],
      ownSession: true,
      read: {
        alias: { at: 0 }, sessionId: { at: 1 }, direction: { at: 2 },
        deliverable: { at: 3, overflow: true }, dispatchedAt: { fromEnd: 3 }, round: { fromEnd: 2 },
        heartbeat: { fromEnd: 1 },   // ← 新增列：**自尾锚定**（尾部定长列，行变长也不串位）
      },
    }
    // 现行（v4）行（新列待填）：正文里同时含**裸竖线**与**反斜杠**（转义感知往返）
    const full = { alias: "a4", sessionId: SID, direction: "方向 | 裸竖线 \\ C:\\Users", deliverable: "产出", dispatchedAt: 1000, round: 9 }
    const extended = [...INFLIGHT_VERSIONS, V4] as unknown as readonly InflightVersion[]
    const shuffled = [V4, ...INFLIGHT_VERSIONS] as unknown as readonly InflightVersion[]
    // 加列的**写出侧那一步**也真的走一遍：给取值器表补一行，再把它接进**真写出入口**。
    // 注意这里用的是 `inflightCells`（真公开面）而不是自己调 `writeRow`——写成后者会绕过真实现，
    // 「写出侧退回硬编码数组」这类退化就杀不死了（实测：自调 writeRow 时该变异体存活）。
    const write4 = { ...INFLIGHT_WRITE, heartbeat: (i: { direction: string }) => (i.direction.includes("心跳") ? "7" : "") }
    const cells4 = (i: import("./shengsheng").Inflight) => inflightCells(i, extended, write4)
    // 未补取值器时**当场报错**（而不是静默把 undefined 写进盘）——这正是加列最易漏的一处
    expect(() => inflightCells(full, extended), "漏取值器 → 报错（加列时最易漏的一处）").toThrow(/缺字段取值器：heartbeat/)

    // —— ① 写出侧：声明 → 列序（**由位置声明推出**，不是另拄一份数组字面量） ——
    expect(fieldOrder(V4.read, V4.columns.length)).toEqual(["alias", "sessionId", "direction", "deliverable", "dispatchedAt", "round", "heartbeat"])
    // 历代各代的列序都必须**逐列对位**（位置声明的键集与列集一致）——否则写出会静默错位
    for (const v of [...INFLIGHT_VERSIONS, V4]) {
      expect(fieldOrder(v.read, v.columns.length), `在途#${v.ver} 列序逐列覆盖`).toHaveLength(v.columns.length)
    }
    // 现行池表（v2）的列序与历史实现逐位相同
    expect(fieldOrder(POOL_VERSIONS[POOL_VERSIONS.length - 1].read, POOL_VERSIONS[POOL_VERSIONS.length - 1].columns.length))
      .toEqual(["alias", "sessionId", "domain", "task", "dispatchedAt", "round", "status"])

    // —— ② 表头：与声明同源（**现行 = 声明表末条**，扩表后自动跟上） ——
    expect(inflightHeader(extended)).toEqual([...V4.columns])
    expect(inflightHeader()).toEqual([...INFLIGHT_VERSIONS[INFLIGHT_VERSIONS.length - 1].columns])
    // 这条规则**依赖声明表按版本号有序**——故次序本身有契约（「版本契约」用例钉着 `d.ver === i+1`）：
    // 发出去的声明表里，末条必须就是最大版本号（否则写出侧会把新行写成旧格式、新列静默丢）。
    expect(INFLIGHT_CURRENT, "现行在途版 = 声明表里最大版本号").toBe(Math.max(...INFLIGHT_VERSIONS.map((v) => v.ver)) as typeof INFLIGHT_CURRENT)
    expect(POOL_CURRENT, "现行池版 = 声明表里最大版本号").toBe(Math.max(...POOL_VERSIONS.map((v) => v.ver)) as typeof POOL_CURRENT)
    // 把声明表写乱只会影响**写出侧的现行**（末条变了）；**读入侧与声明次序无关**（见 ⑤）。
    expect(inflightHeader(shuffled), "乱序后末条是一条旧代（写出侧只认末条）").toEqual([...INFLIGHT_VERSIONS[INFLIGHT_VERSIONS.length - 1].columns])

    // —— ③ 往返：**真的** parse → migrate → render → 再解析 ——
    // 走**真文档**（表头 + 分隔行 + 数据行）再从它读回来——即 `tableBlock` 那条真路，而不是自己手拼
    // 格数组（那会绕过表头识别，正是残留①藏身的地方）。
    const TAIL = ["", "## 种子队列", "（空）", "", "## 收藏待办", "（空）", "", "## 断点", "（无）", ""]
    const rowLine = (cellsOut: string[]) => `| ${cellsOut.map((c) => c.replace(/\|/g, "\\|").replace(/\r?\n/g, " ")).join(" | ")} |`
    const doc = (header: string[], rows: string[]) => [
      "## 项目坐标", "- 项目根：/drill", "", "## 计数", "- 呼吸：1", "- 派工序号：9", "",
      "## worker 池", "（空）", "",
      "## 在途", `| ${header.join(" | ")} |`, `| ${header.map(() => "---").join(" | ")} |`, ...rows,
      ...TAIL,
    ].join("\n")

    const rendered = doc(inflightHeader(extended), [rowLine(cells4(full))])
    // 写出侧真的把新列落盘了（表头 7 格、数据行 7 格）——这里新列为**空值**（方向里无「心跳」标记）
    const blk = tableBlock(rendered, "在途")
    expect(blk.declared, "表头列数被认出").toBe(7)
    expect(blk.rows[0]).toEqual([full.alias, full.sessionId, full.direction, full.deliverable, "1000", "9", ""])
    expect(rendered, "正文竖线按转义写盘").toContain("方向 \\| 裸竖线 \\ C:\\Users")
    const back = migrateInflight(parseInflightRow(blk.rows[0], { declared: blk.declared, poolSessions: new Set([SID]) }, extended))
    expect(back, "往返等价：写出→读回，逐字段相同").toEqual(full as typeof back)
    // 再走一遍（render → parse → render）——幂等，不因往返而漂
    expect(doc(inflightHeader(extended), [rowLine(cells4(back))])).toBe(rendered)

    // 新列**非空**那一档：值落在声明指定的那一格（心跳 = 最后一格）
    const hbIdx = fieldOrder(V4.read, V4.columns.length).indexOf("heartbeat")
    expect(hbIdx, "新列的声明位置就是它的输出位").toBe(6)
    const hbFull = { ...full, direction: "方向（带心跳标记）" }
    const withHb = cells4(hbFull)
    expect(withHb).toHaveLength(7)
    expect(withHb[6], "新列按声明取值").toBe("7")
    // 多字段边界：新列有值 + 正文含裸竖线（溢格并回交付列），新列表头仍对位
    const multi = doc(inflightHeader(extended), [rowLine(withHb), rowLine(cells4({ ...full, alias: "a4b", direction: "溢 | 格", deliverable: "产 | 出" }))])
    const multiBlk = tableBlock(multi, "在途")
    expect(multiBlk.declared).toBe(7)
    const multiParsed = multiBlk.rows.map((c) => migrateInflight(parseInflightRow(c, { declared: multiBlk.declared, poolSessions: new Set([SID]) }, extended)))
    expect(multiParsed[0]).toEqual(hbFull as (typeof multiParsed)[number])
    expect(multiParsed[1]).toEqual({ ...full, alias: "a4b", direction: "溢 | 格", deliverable: "产 | 出" })

    // —— 旧代行**同表混排**：各代各归各代、都不被新列带偏，且升级后形状统一 ——
    const mixed = doc(inflightHeader(extended), [
      rowLine(withHb),                                 // v4（新列有值）
      `| a3 | ${SID} | 方向三 | 产出三 | 1000 | 8 |`,   // v3（少一列）
      "| a2 | 旧行方向 | 产出二 | 1000 | 7 |",          // v2（旧代，无会话列）
      "| a1 | 方向一 | 产出一 | 700 |",                 // v1（无轮次列）
    ])
    const mixedBlock = tableBlock(mixed, "在途")
    expect(mixedBlock.rows).toHaveLength(4)
    const parsedMixed = mixedBlock.rows.map((c) => parseInflightRow(c, { declared: mixedBlock.declared, poolSessions: new Set([SID]) }, extended))
    expect(parsedMixed.map((p) => p.ver as number), "四代各归各代").toEqual([4, 3, 2, 1])
    const migrated = parsedMixed.map(migrateInflight)
    expect(migrated[0]).toEqual(hbFull as (typeof migrated)[number])
    expect(migrated[1]).toMatchObject({ alias: "a3", sessionId: SID, direction: "方向三", deliverable: "产出三", dispatchedAt: 1000, round: 8 })
    expect(migrated[2]).toMatchObject({ alias: "a2", sessionId: "", direction: "旧行方向", deliverable: "产出二", dispatchedAt: 1000, round: 7 })
    expect(migrated[3]).toMatchObject({ alias: "a1", sessionId: "", direction: "方向一", deliverable: "产出一", dispatchedAt: 700, round: 0 })
    // 升级后形状统一：旧代缺的列也**存在**（空值，而非缺键）
    for (const m of migrated) expect(Object.keys(m).sort()).toEqual(["alias", "deliverable", "direction", "dispatchedAt", "round", "sessionId"])

    // —— ④ **前置**一列同样只动声明：表头识别不得依赖列名/列序 ——
    // （残留①的回归位：旧 `tableBlock` 按「首格是不是已知列名」认表头，前置列后首格变成新列名 →
    //   表头认不出 → 表头行被当数据行读进来。现改为「分隔行的上一行即表头」，与列名无关。）
    const V5 = {
      ver: 5 as const,
      columns: ["序号", ...V4.columns],   // 8 列：前置一列（新列在**最前**，彻底改变首格）
      ownSession: true,
      read: {
        seq: { at: 0 }, alias: { at: 1 }, sessionId: { at: 2 }, direction: { at: 3 },
        deliverable: { at: 4, overflow: true }, dispatchedAt: { fromEnd: 3 }, round: { fromEnd: 2 },
        heartbeat: { fromEnd: 1 },
      },
    }
    const ext5 = [...INFLIGHT_VERSIONS, V4, V5] as unknown as readonly InflightVersion[]
    expect(V5.columns).toHaveLength(8)
    expect(fieldOrder(V5.read, V5.columns.length)).toEqual(["seq", "alias", "sessionId", "direction", "deliverable", "dispatchedAt", "round", "heartbeat"])
    const prepended = doc(inflightHeader(ext5), ["| 1 | p5 | " + SID + " | 方向五 | 产出五 | 1000 | 5 | 2 |"])
    const preBlock = tableBlock(prepended, "在途")
    expect(preBlock.declared, "前置列的表头也被认出").toBe(8)
    expect(preBlock.rows, "表头行不得被当成数据行").toHaveLength(1)
    expect(preBlock.rows[0][0], "首个数据行的首格就是它的内容").toBe("1")
    const preParsed = parseInflightRow(preBlock.rows[0], { declared: preBlock.declared }, ext5)
    expect(preParsed.ver as number).toBe(5)
    expect(preParsed.fields).toMatchObject({ seq: "1", alias: "p5", sessionId: SID, direction: "方向五", deliverable: "产出五", dispatchedAt: "1000", round: "5" })

    // —— ⑤ 声明次序**不必**等于列数次序（人工编辑/合并冲突后的常见形态） ——
    expect(shuffled[0].ver as number, "声明表确实被写乱了").toBe(4)
    expect(parseInflightRow(["a4", SID, "方向", "产出", "1000", "9", "0"], { declared: 7 }, shuffled).ver as number, "打乱声明次序也要判对").toBe(4)
    expect(parseInflightRow(["a3", SID, "方向", "产出", "1000", "8"], { declared: 7 }, shuffled).ver as number).toBe(3)

    // —— ⑥ 写出侧的**失败面**：声明与取值器/列集不同步时当场报错，而不是静默写坏盘 ——
        const gA = { alias: (i: typeof full) => i.alias }
    expect(() => writeRow(V4, gA as never, full), "取值器表整个缺新列 → 同样报错").toThrow(/缺字段取值器/)
    const tbl = (read: FieldMap) => ({ columns: ["甲", "乙"], read })
    expect(() => writeRow(tbl({ a: { at: 0 } }), { a: () => "x" }, {}), "少一列").toThrow(/未逐列覆盖/)
    expect(() => writeRow(tbl({ a: { at: 0 }, b: { at: 0 } }), { a: () => "x", b: () => "y" }, {}), "两字段同址").toThrow(/未逐列覆盖/)
    expect(() => writeRow(tbl({ a: { at: 0 }, b: { at: 5 } }), { a: () => "x", b: () => "y" }, {}), "下标越界").toThrow(/未逐列覆盖/)

    // —— ⑦ 池表加代同理（判代与写出侧都**不含版本号字面量**） ——
    expect(selectPoolVer(["a", SID, "d", "t", "100", "idle"], { declared: 6 })).toBe(1)
    expect(selectPoolVer(["a", SID, "d", "t", "100", "7", "idle"], { declared: 7 })).toBe(2)
    // 声明表里没有的列数（无表头 / 半截行）→ 回落到**现行版**，而不是硬编码撞出另一代
    expect(selectPoolVer(["a", SID, "d"], { declared: 3 })).toBe(POOL_CURRENT)
    expect(migratePool(parsePoolRow(["a", SID, "d", "t", "100", "7", "idle"], { declared: 7 })), "现行池行时间位在自尾 3（不被新增列带偏）").toEqual({ alias: "a", sessionId: SID, domain: "d", task: "t", dispatchedAt: 100, round: 7, status: "idle" })
    // 加了一代（8 列）之后，**同一个 8 列声明就认得出来了**——判代随声明表演进，不靠改代码
    // （池表加列正是「轮次」那一列的做法：插在中间并**重新锤定尾锚点**）
    const P2 = POOL_VERSIONS[POOL_VERSIONS.length - 1]
    const P3 = {
      ver: 3 as const,
      columns: ["别名", "会话", "领域", "任务", "派发时间", "轮次", "心跳", "状态"],
      hasRound: true,
      // 尾锤点**整体后移一位**：时间 = 自尾 4、轮次 = 自尾 3、心跳 = 自尾 2、状态 = 自尾 1
      // （与 74a2e6b9 给池表加「轮次」时同一手法：中间**插入一列就必须重锤定全部尾列**——
      //   漏掉任何一个，写作侧会当场报「未逐列覆盖」而不是默默写错位）
      read: { ...P2.read, dispatchedAt: { fromEnd: 4 }, round: { fromEnd: 3 }, heartbeat: { fromEnd: 2 }, status: { fromEnd: 1 } },
    }
    const ext3 = [...POOL_VERSIONS, P3] as unknown as readonly import("./control-format").PoolVersion[]
    expect(poolHeader(ext3)).toEqual([...P3.columns])   // 加代后写出侧自动跟上（末条即现行）
    // 旧声明表里 8 列是未知列数 → 回落到现行（不硬编码撞代）；本用例里它是 P3（见上显式声明）
    expect(fieldOrder(P3.read, P3.columns.length)).toEqual(["alias", "sessionId", "domain", "task", "dispatchedAt", "round", "heartbeat", "status"])
    // 池写出侧由声明推出列序，且与历史实现逐位相同
    const w = { alias: "wa", sessionId: SID, domain: "dp", task: "tk", dispatchedAt: 1000, round: 7, status: "idle" as const }
    expect(poolCells(w)).toEqual(["wa", SID, "dp", "tk", "1000", "7", "idle"])
    // 给池表加一列的写出侧：写取值器表补一行，走**真入口**（`poolCells` 扩表版）｜列序由声明推出。
    // 另加一条**历史读法不变**的回归：两代共用同一组尾锤点的旧行为不得因本单而变（见「版本契约」用例）。
    const poolWrite3 = { ...POOL_WRITE, heartbeat: (x: { round: number }) => (x.round === 7 ? "1" : "0") }
    expect(poolCells(w, ext3, poolWrite3)).toEqual(["wa", SID, "dp", "tk", "1000", "7", "1", "idle"])
    // 漏补取值器 → 当场报错（同前；而不是静默把新列写成空）
    expect(() => poolCells(w, ext3), "漏取值器 → 报错").toThrow(/缺字段取值器：heartbeat/)
    // 真声明表（v2，7 列）下写出侧仍逐位等同历史实现（不放宽、不错位）
    expect(poolCells(w, [...POOL_VERSIONS])).toEqual(["wa", SID, "dp", "tk", "1000", "7", "idle"])
    // 加代后**读出侧**才认得 8 列（同一行在旧声明表里只能回落到现行、在新声明表里才是 v3）
    expect(selectPoolVer(["wa", SID, "dp", "tk", "1000", "7", "1", "idle"], { declared: 8 }, ext3), "加代后判代随声明表演进").toBe(P3.ver as typeof POOL_CURRENT)
    const pool3Parsed = migratePool(parsePoolRow(["wa", SID, "dp", "tk", "1000", "7", "1", "idle"], { declared: 8 }, ext3))
    expect(pool3Parsed).toEqual(w)   // 新声明的状态位仍在最后一格（各行锚点均按新声明重锤定）

    // —— ⑧ 真实现今日的现行版仍是 v3/v2（本用例只在旁路演示扩表，不改现行声明） ——
    expect(INFLIGHT_CURRENT).toBe(3)
    expect(POOL_CURRENT).toBe(2)
    expect(extended.length).toBe(INFLIGHT_VERSIONS.length + 1)
  })
})

