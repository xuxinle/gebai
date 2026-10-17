import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, readFileSync, rmSync, existsSync, renameSync, writeFileSync } from "node:fs"
import { spawn, spawnSync } from "node:child_process"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { def as shengshengDef, parseControl, renderControl, dedupeState, inflightSession, isSessionIdShape, tableBlock, SECTION_HEADINGS, activeWorkerLimit, ctxScale, ctxVerdict, ctxLine, ctxScaleLine, DEFAULT_MODEL_CONTEXT_WINDOW, DEFAULT_REUSE_ADVISORY_LIMIT, DEFAULT_EXCLUSIVE_RESOURCE, DEFAULT_LEASE_MINUTES, LEASE_START_GRACE_MS, resourceKey, repoResourceName, renderSharedLeases, parseSharedLeases } from "./shengsheng"
// 问卦内核（纯数据/纯函数）与测试用常量直引，不经 def（工具面断言与内核断言分开两把尺）
import {
  buildHexagram,
  hexagramName,
  hourIndexOf,
  pickCandidate,
  trigramOrderOf,
  trigramOf,
  TRIGRAMS,
  HEXAGRAM_SAYINGS,
  LINE_LABELS,
  LINE_SEMANTICS,
  OPERATION_FAMILIES,
  type DivineInputs,
} from "./hexagrams"
import { questionHash, DIVINE_WARN_COUNT, DIVINE_HASH_LEN } from "./shengsheng"
// 提交范围（宽范围暂存的纪律与判据）——直接引纯函数，不绕工具面：判据在纯函数层与工具层各验一遍
import {
  WIDE_STAGING_FORMS,
  commitProtocolLine,
  mentionCovers,
  mentionPaths,
  parseLastCommit,
  parseStatusPorcelain,
  scopeAudit,
  stagingCommands,
  untrackedOf,
} from "./commit-scope"
// `pgrep -f` 自匹配活锁的判据（纯函数直引：文本判据与进程表判据各验一遍，不绕工具面）
import { aliveMatching, detectPgrepLock, disjoinTail, extractPatterns, isSelfMatch, isWaitCommand } from "./pgrep-lock"
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
  /** 引擎自报的上下文窗口（真引擎的 `contextWindow()`）；**0 = 不报**【缺省】——用例可覆盖。 */
  contextWindow: number
  /** 让 resolvePath 拒绝的绝对路径前缀（模拟沙箱边界）。 */
  denied: string[]
  /** 命令钩子（缺省真跑 `/bin/sh -c`）：模拟“没装 git”这类环境。 */
  commandHook?: (cmd: string, opts?: { workdir?: string }) => Promise<{ stdout: string; stderr: string; code: number }>
}

function stub(opts: { root?: string; taskName?: string; withTask?: Record<string, unknown>; sessionId?: string; idPrefix?: string } = {}): Stub {
  const home = mkdtempSync(join(tmpdir(), "gebai-shengsheng-"))
  const sessionDir = join(home, "users", "admin", "sessions", "s1")
  const tmp = join(sessionDir, "tmp")
  mkdirSync(tmp, { recursive: true })
  const root = opts.root ?? join(home, "proj")
  mkdirSync(root, { recursive: true })
  // 会话 id 与派生 id 前缀可定制：**两条演进线共存**的用例里两边必须是不同会话
  // （否则“被自己挡”与“被别人挡”会混为一谈——那恰好是本组判据的分界线）。
  const selfSid = opts.sessionId ?? "s1"
  const idPrefix = opts.idPrefix ?? "sess"

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
    contextWindow: 0,
    denied: [],
  }

  const resolveSafe = (p: string): string => {
    const abs = resolve(p)
    for (const d of s.denied) if (abs.startsWith(resolve(d))) throw new Error(`ENOENT: ${abs} 会话沙箱外路径`)
    return abs
  }

  const ctx = {
    user: "admin",
    sessionId: selfSid,
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
    deleteFile: async (p: string) => {
      rmSync(resolveSafe(p), { force: true })
    },
    moveFile: async (from: string, to: string) => {
      const a = resolveSafe(from)
      const b = resolveSafe(to)
      mkdirSync(dirname(b), { recursive: true })
      renameSync(a, b)
    },
    // 真跑命令（仓级租约要 `git rev-parse --show-toplevel`）——测试里一律真实，不用桩：
    // 「探测不到就应当安静降级」那条判据若用桩跑，桩会把它自己写的答案当成真相（CONVENTIONS §7.2）。
    runCommand: async (cmd: string, o?: { workdir?: string }) => {
      if (s.commandHook) return s.commandHook(cmd, o)
      const r = spawnSync("/bin/sh", ["-c", cmd], { cwd: o?.workdir ?? tmp, encoding: "utf8", timeout: 5000 })
      return { stdout: r.stdout ?? "", stderr: r.stderr ?? "", code: r.status ?? 1 }
    },
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
          const id = `${idPrefix}${s.created.length + 1}`
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
        // 真引擎有这个方法（`engine.contextWindow()`）：缺省桩里**不提供**，让用例走 env/缺省值那条路；
        // 需要验「引擎自报优先」的用例自己覆盖 `s.contextWindow`。
        contextWindow: () => s.contextWindow,
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
  test("工具面精简：四工具覆盖一息，不复刻已有能力", () => {
    expect(Object.keys(shengshengDef.tools!)).toEqual(["state", "dispatch", "collect", "divine"])
    for (const t of ["read", "write", "edit", "sh", "py", "file", "list", "status", "send", "create", "notify", "run"]) {
      expect(Object.keys(shengshengDef.tools!)).not.toContain(t)
    }
    // 回收与原问免审批；布雨独立会话/停用任务需审批
    expect(shengshengDef.requiresApproval).toEqual({ dispatch: true, collect: false })
    // divine 的免审批走**工具级**声明（与 state 同型），不进 requiresApproval 映射：
    // 映射是子Agent 级覆盖表，两键（dispatch/collect）是既有契约，加键会动到别人的断言面。
    expect(tool("divine").requiresApproval).toBe(false)
    expect(shengshengDef.preload).toBe(false)
    expect(shengshengDef.envVars?.every((v) => v.name.startsWith("SHENGSHENG_"))).toBe(true)
    expect(shengshengDef.systemPrompt).toContain("生生")
    // 负载敏感的测量与门禁必须先声明独占（本单新增的一节，不是散文而是可断言的契约）：
    // 两度实证的事故都源于“两个 worker 各自以为自己是唯一在跑的”——提示词里没有这一节，
    // 模型就只会靠“自觉等负载降”（那恰好是两次事故的共同前提）。
    expect(shengshengDef.systemPrompt).toContain("必须声明独占")
    expect(shengshengDef.systemPrompt).toContain("数字作废条件")
    expect(shengshengDef.systemPrompt).toContain("cpu-exclusive")
    // 机制类交付必须有反向断言（2026-10-17）：本条纪律是**派工/收单的硬规**，不是散文——
    // 它必须真的在提示词里，并且带三项可判别的内容（三种反向形态 / 三缺一不算交付 / 工单句式）。
    // 断言用**字面文本**：与 `shengsheng.md` 同源时用它做断言会把差异抹平（CONVENTIONS §7.2）。
    //
    // ⚠ 这里必须锁**整句**而不是关键词：本条纪律在第六节（立规）与第七节（收单口径第四条）
    // 各出现一次，只锁「三缺一不算交付」「机制类交付必须有反向断言」这类短语时，把第六节改成
    // 「建议补一条（可选）」**竟不会变红**（实测：软回退后 98 pass / 0 fail）——因为短语在第七节
    // 的交叉引用里还在。所以锁的是「带 `必须` 的整句 + 不可选」这两处语义，软退化成「可选」即红。
    expect(shengshengDef.systemPrompt).toContain(
      "机制类交付必须有反向断言——正向断言只证「该做的做了」，反向断言才证「不该做的一件都没做」。",
    )
    expect(shengshengDef.systemPrompt).toContain("不是加分项，缺了就是半个证人")
    // 三种反向形态各自的名字（缺任一种就少了一整类“不可碰”的检验面）
    expect(shengshengDef.systemPrompt).toContain("不得 X")
    expect(shengshengDef.systemPrompt).toContain("其它 Y 必须不受影响")
    expect(shengshengDef.systemPrompt).toContain("不属于自己的 Z 必须仍存在")
    // 工单模板：`deliver` 必含反向断言（不是可选项）+ 可照抄句式
    expect(shengshengDef.systemPrompt).toContain("`deliver` 里必须含一条反向断言（不是可选项）")
    // 正文那句也要锁：只锁小标题时，把正文从「必须再写一条」软退成「建议再加一条」**不变红**
    // （实测 drill：只锁小标题时 98 pass / 0 fail；补上这句后同一回退当场 1 fail）
    expect(shengshengDef.systemPrompt).toContain("必须再写一条反向验收")
    expect(shengshengDef.systemPrompt).toContain("反向验收：构造 <不属于本单的对象>，本单动作后它**必须不受影响**（否则会 <具体祸及>）")
    // 两个正面标杆必须点名（“只讲道理不给样本”的纪律没人会照做），且必须带那句“跨线祸及”的自述
    expect(shengshengDef.systemPrompt).toContain("跨线祸及比不杀更坏")
    expect(shengshengDef.systemPrompt).toContain("不会把所有人互相锁死")
    expect(shengshengDef.systemPrompt).toContain("三缺一不算交付")
    // 为什么会漏：正向断言只证「该做的做了」——反向那一半必须点名，否则模型只会验正向。
    expect(shengshengDef.systemPrompt).toContain("不该碰的东西有没有被碰到")
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
      leases: [],
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
    expect(renderControl({ coord: {}, breath: 0, seq: 0, workers: [], inflight: [], seeds: [], pending: [], leases: [] })).toContain("## 种子队列")
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
    // 呼吸**真的推进了一息**（工具描述与提示词都写着「note 推进呼吸」——
    // 名字里写着的东西必须有断言守着，否则「写了机制 ≠ 机制生效」在此重演）
    expect(note.output).toContain("呼吸 1")
    expect((note.data as { breath: number }).breath).toBe(1)
    const state = parseControl(controlOf(s))
    expect(state.breath).toBe(1)          // 落盘真值同步（不只是返回文案）
    expect(state.seeds).toHaveLength(1)
    expect(state.note).toBe("布雨 1 单，一单在途")
    // 再落一次 → 再到下一息（单调递增，不重置、不卡住）
    const next = await tool("state").execute({ action: "note", note: "第二息" }, s.ctx)
    expect((next.data as { breath: number }).breath).toBe(2)
    expect(parseControl(controlOf(s)).breath).toBe(2)
    // 读态如实报出当前呼吸（与落盘一致）
    expect((await tool("state").execute({}, s.ctx)).output).toContain("呼吸：2")
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

    // 活动 worker 上限（3）：池已 2，本批 2 单新建 → **放行 1 单、顺延 1 单**（不整批拒绝）
    const fifth = await tool("dispatch").execute({ orders: [{ task: "第四单", domain: "docs" }, { task: "第五单", domain: "perf" }] }, s.ctx)
    expect(fifth.output).toContain("布雨完成")
    expect(fifth.output).toContain("上限 3")
    expect(fifth.output).toContain("未派")
    expect(s.created).toHaveLength(3)

    // preview：只出计划不派工
    s.ctx.env.SHENGSHENG_DISPATCH = "preview"
    const preview = await tool("dispatch").execute({ orders: [{ task: "预检单", domain: "lint" }] }, s.ctx)
    expect(preview.output).toContain("布雨预检")
    expect(s.sent).toHaveLength(4)
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

  // ------------------------------------------------------------------
  // 断点节往返保真：断点是**自由文本节**（正文由模型写），而 `sectionText` 原先一律在
  // 任何 `^##\s` 行处截断——正文里一行手写小标题（`## 下一步`、粘贴的 md 片段）会被当成
  // 节边界，断点被**静默截断**（读进来只剩前半段，下次 note 写回时后半段永久丢失）。
  // 边界改为只认**规范节标题**（SECTION_HEADINGS）。
  // ------------------------------------------------------------------

  test("断点往返保真：正文里的 `## ` 行是内容不是结构（不被静默截断、写回不丢后半段）", async () => {
    const s = stub()
    const md = renderControl({
      coord: { root: "C:/p", verify: "bun test" },
      breath: 3,
      seq: 9,
      workers: [],
      inflight: [],
      seeds: [],
      pending: [],
      leases: [],
      note: "前半段：观到三单在跑。\n\n## 下一步\n\n- 收 ss-1；- 布一雨。\n\n尾段：别忘了核提交数。",
    })
    // 写进真现场（与工具同一路径），再用**真工具**读回来
    mkdirSync(join(s.root, ".evolve"), { recursive: true })
    await Bun.write(join(s.root, ".evolve", "control.md"), md)
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)

    const read = await tool("state").execute({}, s.ctx)
    const note = (read.data as { note: string | null }).note ?? ""
    expect(note).toContain("## 下一步")            // 非规范 `## ` 行是内容
    expect(note).toContain("尾段：别忘了核提交数")   // **后半段没被截掉**（修复前这里丢）

    // 往返：再落一次断点（带上尾巴）→ 重读仍在（不静默丢）
    await tool("state").execute({ action: "note", note: `${note}\n\n（复核：三单已收）` }, s.ctx)
    const again = await tool("state").execute({}, s.ctx)
    const back = (again.data as { note: string | null }).note ?? ""
    expect(back).toContain("## 下一步")
    expect(back).toContain("尾段：别忘了核提交数")
    expect(back).toContain("（复核：三单已收）")

    // 对照：**规范**节标题仍是边界——后一节的正文不得泄进断点
    expect(back).not.toContain("## 种子队列")
    // 再往返一次恒等（解析 → 渲染 → 解析）
    const once = parseControl(controlOf(s))
    expect(parseControl(renderControl(once)).note).toBe(once.note)
    cleanup(s)
  })

  test("节结构单源：写出侧节标题集 == 解析侧认的规范标题集（次序一致，两边漂开即报）", () => {
    const md = renderControl({ coord: {}, breath: 0, seq: 0, workers: [], inflight: [], seeds: [], pending: [], leases: [] })
    const written = md.split(/\r?\n/).filter((l) => /^##\s/.test(l.trim())).map((l) => l.trim().slice(3).trim())
    // 逐字且**次序一致**：渲染器写出的每个节都在语料里，语料里的每个节也都被写出
    expect(written).toEqual([...SECTION_HEADINGS])
    expect(new Set(written).size).toBe(SECTION_HEADINGS.length)
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

  test("心跳任务按项目归属：同名任务属他项时，ensure 不复用、pause 不停用（跨项目撞名护栏）", async () => {
    const s = stub()
    const other = join(s.home, "other-proj")
    mkdirSync(other, { recursive: true })
    // 盘上先有一条**别的项目**的心跳任务（历史缺省名，正是撞名现场）
    s.tasks.push({ id: "t-other", name: "SHENGSHENG_EVOLVE", kind: "scheduled", runner: "prompt", enabled: true, state: "idle", runCount: 0, schedule: "@every 3m", env: { SHENGSHENG_ROOT: other } })
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    const otherBefore = JSON.stringify(s.tasks[0])

    // status：如实报「本项目尚无心跳任务」，并点名他项任务——不把它当本项目的心跳
    const st = await tool("state").execute({ action: "task", op: "status" }, s.ctx)
    expect(st.output).toContain("尚无心跳任务")
    expect(st.output).toContain("其他项目")
    expect(st.output).toContain(other)

    // ensure：不复用他项任务，为本项目**新建**（名字按项目派生）
    const ens = await tool("state").execute({ action: "task", op: "ensure" }, s.ctx)
    expect(ens.output).toContain("心跳任务已创建")
    expect(s.tasks).toHaveLength(2)
    const mine = s.tasks.find((t) => t.id !== "t-other")!
    expect((mine.env as Record<string, string>).SHENGSHENG_ROOT).toBe(s.root)
    expect(String(mine.name)).toContain("::")
    expect(String(mine.name)).not.toBe("SHENGSHENG_EVOLVE")
    expect(ens.output).toContain("其他项目")          // 同批一并提示他项任务
    // 他项任务**一字未动**（未被 patch 启用态、未被改名）
    expect(JSON.stringify(s.tasks.find((t) => t.id === "t-other"))).toBe(otherBefore)

    // ensure 幂等：再调一次仍命中本项目那条
    const again = await tool("state").execute({ action: "task", op: "ensure" }, s.ctx)
    expect(again.output).toContain("已存在（幂等，未新建）")
    expect(again.output).toContain("与本项目一致")
    expect(s.tasks).toHaveLength(2)

    // pause：只停本项目的心跳，**他项任务不得被停用**
    const pa = await tool("state").execute({ action: "task", op: "pause", reason: "种子柟" }, s.ctx)
    expect(pa.output).toContain("演进已暂停")
    expect(s.tasks.find((t) => t.id === "t-other")!.enabled).toBe(true)
    expect(mine.enabled).toBe(false)

    // 显式点名他项任务 id：直接拒绝（不复用、不停用、也不顺手另建）
    const byId = await tool("state").execute({ action: "task", op: "ensure", task_id: "t-other" }, s.ctx)
    expect(byId.output).toContain("拒绝")
    expect(byId.output).toContain("不属于本项目")
    expect(s.tasks.find((t) => t.id === "t-other")!.enabled).toBe(true)
    expect(s.tasks).toHaveLength(2)
    cleanup(s)
  })

  test("心跳任务升级：旧版任务归属可从唤醒 prompt 回读，ensure 不另建", async () => {
    const s = stub()
    // 旧版形态：任务级 env 无 root，但唤醒 prompt 带「项目根：」（本仓升级前的真实形态）
    s.tasks.push({ id: "t-legacy", name: "SHENGSHENG_EVOLVE", kind: "scheduled", runner: "prompt", enabled: true, state: "idle", runCount: 0, schedule: "@every 3m", env: {}, prompt: `SHENGSHENG_EVOLVE 心跳：生生，执行一息。\n项目根：${s.root}\n一息流程：…` })
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    const ens = await tool("state").execute({ action: "task", op: "ensure" }, s.ctx)
    expect(ens.output).toContain("已存在（幂等，未新建）")
    expect(s.tasks).toHaveLength(1)
    // 归属证据补齐（env 落 root）——下息起不再依赖 prompt 回读
    expect((s.tasks[0].env as Record<string, string>).SHENGSHENG_ROOT).toBe(s.root)
    cleanup(s)
  })

  test("并发：SHENGSHENG_MAX_WORKERS 解析与放宽（缺省仍是 3，误设被夹）", () => {
    expect(activeWorkerLimit(undefined)).toBe(3)
    expect(activeWorkerLimit({})).toBe(3)
    expect(activeWorkerLimit({ SHENGSHENG_MAX_WORKERS: "6" })).toBe(6)
    expect(activeWorkerLimit({ SHENGSHENG_MAX_WORKERS: "2|6" })).toBe(2)
    expect(activeWorkerLimit({ SHENGSHENG_MAX_WORKERS: "|6" })).toBe(6)     // 只给硬顶
    expect(activeWorkerLimit({ SHENGSHENG_MAX_WORKERS: "999" })).toBe(64)   // 误设被夹到硬顶
    expect(activeWorkerLimit({ SHENGSHENG_MAX_WORKERS: "abc" })).toBe(3)    // 非法回落缺省
    expect(activeWorkerLimit({ SHENGSHENG_MAX_WORKERS: "0" })).toBe(1)
  })

  test("并发：放宽后一批多单同时派出；缺省下池满只顺延新建、复用与续派不受限", async () => {
    // 放宽：4 单全出
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    s.ctx.env.SHENGSHENG_MAX_WORKERS = "5"
    const wide = await tool("dispatch").execute({ orders: [
      { task: "并发 1", domain: "a" }, { task: "并发 2", domain: "b" }, { task: "并发 3", domain: "c" }, { task: "并发 4", domain: "d" },
    ] }, s.ctx)
    expect(wide.output).toContain("布雨完成")
    expect(wide.output).not.toContain("未派")
    expect(s.created).toHaveLength(4)
    expect(parseControl(controlOf(s)).workers).toHaveLength(4)
    cleanup(s)

    // 缺省（3）：同样的四单出 3、顺延 1（不再整批拒绝）
    const s2 = stub()
    await tool("state").execute({ action: "init", root: s2.root, verify: "bun test" }, s2.ctx)
    const narrow = await tool("dispatch").execute({ orders: [
      { task: "默认 1", domain: "a" }, { task: "默认 2", domain: "b" }, { task: "默认 3", domain: "c" }, { task: "默认 4", domain: "d" },
    ] }, s2.ctx)
    expect(s2.created).toHaveLength(3)
    expect(narrow.output).toContain("未派")
    expect(narrow.output).toContain("上限 3")
    // 池内 3 个 idle 时，复用仍可派出（只有新建受限）
    s2.running.clear()
    const pc = parseControl(controlOf(s2))
    for (const w of pc.workers) w.status = "idle"
    await Bun.write(join(s2.root, ".evolve", "control.md"), renderControl(pc))
    const reuse = await tool("dispatch").execute({ orders: [{ task: "复用一单", domain: "a" }] }, s2.ctx)
    expect(reuse.output).toContain("布雨完成")
    expect(s2.created).toHaveLength(3)
    cleanup(s2)
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

    // 新建超出上限——本单顺延（不再整批拒绝）：如实报「未派」且不新建
    s.running.clear()
    const blocked = await tool("dispatch").execute({ orders: [{ task: "池满新单", domain: "brandnew", fresh: true }] }, s.ctx)
    expect(blocked.output).toContain("上限 3")
    expect(blocked.output).toContain("未派")
    expect((blocked.data as { limit?: number }).limit).toBe(3)
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

  // -------------------------------------------------------------------------
  // 问卦（divine）：起卦内核 + 择的哨兵
  // -------------------------------------------------------------------------

  describe("问卦（divine）", () => {
    const base = (over: Partial<DivineInputs> = {}): DivineInputs => ({
      breath: 1, inflight: 0, running: 0, seeds: 0, seq: 0, month: 1, day: 1, hourIndex: 1, draw: 0, ...over,
    })

    test("卦表完整性：64 卦名唯一、生生断 64 句齐备、爻位语义六行", () => {
      const names = new Set<string>()
      for (let u = 1; u <= 8; u++) for (let l = 1; l <= 8; l++) names.add(hexagramName(u, l))
      expect(names.size).toBe(64)
      expect(Object.keys(HEXAGRAM_SAYINGS)).toHaveLength(64)
      for (const n of names) expect(HEXAGRAM_SAYINGS[n], `缺断语：${n}`).toBeTruthy()
      for (const k of Object.keys(HEXAGRAM_SAYINGS)) expect(names.has(k), `断语表有陌生卦名：${k}`).toBe(true)
      expect(LINE_SEMANTICS).toHaveLength(6)
      expect(LINE_LABELS).toEqual(["初", "二", "三", "四", "五", "上"])
      // 八卦表：序、名、三爻同源（序→爻是其二进制自下而上）
      for (const t of TRIGRAMS) {
        expect(t.order).toBeGreaterThanOrEqual(1)
        expect(t.order).toBeLessThanOrEqual(8)
        expect(trigramOrderOf(t.lines), `${t.name} 序爻同源`).toBe(t.order)
      }
      expect(TRIGRAMS.map((t) => t.name).join("")).toBe("乾兑离震巽坎艮坤")
      // 生生断必须命中操作动词族（断语是方向不是文学）——64 句均须可执行
      const noVerb = Object.entries(HEXAGRAM_SAYINGS).filter(([, s]) => !OPERATION_FAMILIES.some((f) => s.includes(f))).map(([k]) => k)
      expect(noVerb, `断语未命中任何操作族：${noVerb.join("/")}`).toEqual([])
    })

    test("成卦可复算（同输入同卦）+ 定卦与变卦正确（动爻翻转）+ 体用归属随爻位", () => {
      const i = base({ breath: 7, inflight: 1, running: 2, seeds: 3, seq: 4, month: 5, day: 6, hourIndex: 2, draw: 12345 })
      const a = buildHexagram(i)
      const b = buildHexagram({ ...i })
      expect(b).toEqual(a) // 纯函数：可复算回放
      expect(a.saying).toBe(HEXAGRAM_SAYINGS[a.name])
      expect(a.changed).toBeTruthy()
      const t = trigramOf(a.moving <= 3 ? a.lower : a.upper)
      expect(a.yong.name).toBe(t.name)
      expect(a.yong.scope).toBe(a.moving <= 3 ? "本家" : "方向库")
      expect(a.ti.name).toBe(trigramOf(a.moving <= 3 ? a.upper : a.lower).name)
      expect(a.lineSemantics).toBe(LINE_SEMANTICS[a.moving - 1])
      expect(a.moving).toBeGreaterThanOrEqual(1)
      expect(a.moving).toBeLessThanOrEqual(6)
      // 动爻不翻转则变卦即本卦（参照点）：对某些输入应出现，证明变卦非恒等也非恒异
      expect(hexagramName(a.upper, a.lower)).toBe(a.name)
    })

    test("呼吸入卦：内势同则本卦同（本卦是处境不是抽签），呼吸 +1 则本卦必不同", () => {
      const i = base({ breath: 5, inflight: 1, running: 2 })
      const at = (breath: number, draw: number) => buildHexagram({ ...i, breath, draw })
      // 同一息：掷值不同只换动爻（与变卦），本卦不动
      expect(at(5, 0).name).toBe(at(5, 999999).name)
      expect(at(5, 0).lower).toBe(at(5, 999999).lower)
      // 呼吸 +1（内势其余项不变）⇒ 下卦序 +1 ⇒ 本卦必不同
      expect(at(6, 0).lower).not.toBe(at(5, 0).lower)
      expect(at(6, 0).name).not.toBe(at(5, 0).name)
      // 内势任一量变都会改变下卦序（卦随河变）
      expect(buildHexagram({ ...i, running: 3 }).lower).not.toBe(buildHexagram({ ...i }).lower)
    })

    test("卦指分布：任意候选数都可达、无死区（旧式「卦序号+动爻」取模在 k>6 时有永不可达项）", () => {
      const h = buildHexagram(base({ breath: 3 }))
      for (const k of [1, 2, 3, 4, 5, 6, 7, 8, 9, 13]) {
        const seen = new Set<number>()
        for (let d = 0; d < 4096; d++) seen.add(pickCandidate(h, k, d))
        expect(seen.size, `k=${k} 有候选项永不可达`).toBe(k)
        for (const p of seen) {
          expect(p).toBeGreaterThanOrEqual(1)
          expect(p).toBeLessThanOrEqual(k)
        }
      }
      // 单向性：固定掷值同卦同 k ⇒ 同结果（可复算）
      expect(pickCandidate(h, 7, 12345)).toBe(pickCandidate(h, 7, 12345))
    })

    test("时辰序：子一…亥十二，跨子时边界", () => {
      expect(hourIndexOf(0)).toBe(1)   // 23:00–00:59 子
      expect(hourIndexOf(1)).toBe(2)   // 01:00–02:59 丑
      expect(hourIndexOf(2)).toBe(2)   // 丑
      expect(hourIndexOf(12)).toBe(7)  // 午
      expect(hourIndexOf(23)).toBe(1)  // 子（跨夜）
    })

    test("问卦：环境入卦 + 卦入台账 + 断卦框架（零新执行面：不派会话、不执行命令、不写 control.md）", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("state").execute({ action: "seed_add", direction: "修复验证门", priority: "P0" }, s.ctx)
      const controlBefore = controlOf(s)
      const r = await tool("divine").execute({ question: "下一步做什么" }, s.ctx)
      expect(r.output).toContain("# 卦")
      expect(r.output).toContain("环境快照")
      expect(r.output).toContain("断卦框架")
      const d = r.data as Record<string, unknown>
      expect(d.mode).toBe("开卦")
      expect(d.replayed).toBe(false)
      expect((d.hexagram as Record<string, unknown>).name).toBeTruthy()
      const env = d.env as Record<string, number | string>
      expect(env.breath).toBe(0)
      expect(env.seeds).toBe(1)
      expect(env.branch).toBeTruthy() // stub 无 .git ⇒ 「未取」（降级不抛）
      expect(ledgerOf(s)).toContain("## 卦·")
      expect(ledgerOf(s)).toContain(String(d.ledgerKey))
      // 零新执行面：不派会话、不跑命令、不动 control.md
      expect(s.sent).toHaveLength(0)
      expect(s.created).toHaveLength(0)
      expect(controlOf(s)).toBe(controlBefore)
      cleanup(s)
    })

    test("一息一卦：同息再问不掷新卦、返回前卦原文（换问句也绕不过——键不含问句哈希）", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      const first = await tool("divine").execute({ question: "下一步做什么" }, s.ctx)
      const firstCard = String((first.data as Record<string, unknown>).hexagram ? (first.data as { hexagram: { name: string } }).hexagram.name : "")
      const second = await tool("divine").execute({ question: "完全换一个说法：接下来该往哪走？" }, s.ctx)
      expect(second.output).toContain("渎则不告")
      expect((second.data as Record<string, unknown>).replayed).toBe(true)
      expect(second.output).toContain(firstCard) // 返回前卦原文
      // 台账只落了一卦（同息）
      const md = ledgerOf(s)
      expect(md.split("## 卦·").length - 1).toBe(1)
      cleanup(s)
    })

    test("定卦：传 candidates 即卦指即落（1 起序号 + 候选原文），且卦指在候选范围内", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      const cands = ["甲方向", "乙方向", "丙方向", "丁方向", "戊方向"]
      const r = await tool("divine").execute({ question: "甲乙丙丁戊证据平衡，先做哪个", candidates: cands }, s.ctx)
      const d = r.data as { mode: string; pick: { index: number; candidate: string } }
      expect(d.mode).toBe("定卦")
      expect(d.pick.index).toBeGreaterThanOrEqual(1)
      expect(d.pick.index).toBeLessThanOrEqual(cands.length)
      expect(d.pick.candidate).toBe(cands[d.pick.index - 1])
      expect(r.output).toContain("卦指")
      expect(ledgerOf(s)).toContain(d.pick.candidate)
      cleanup(s)
    })

    test("渎则不告跨息解除：note 推进呼吸后，下一息可再问", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("divine").execute({ question: "第一息" }, s.ctx)
      await tool("state").execute({ action: "note", note: "收息" }, s.ctx) // 呼吸 0 → 1
      const r = await tool("divine").execute({ question: "第二息" }, s.ctx)
      expect((r.data as Record<string, unknown>).replayed).toBe(false)
      expect(ledgerOf(s).split("## 卦·").length - 1).toBe(2)
      cleanup(s)
    })

    test("未初始化拒绝问卦：内势无据（不掷、不落档）", async () => {
      const s = stub()
      // 不 init：根下无 .evolve/control.md
      const r = await tool("divine").execute({ question: "问什么", root: s.root }, s.ctx)
      expect(r.output).toContain("未初始化")
      expect(existsSync(join(s.root, ".evolve", "ledger.md"))).toBe(false)
      cleanup(s)
    })

    test("台账键不互吞：定长问句哈希，前缀型键不误命中（appendLedger 是子串判据）", () => {
      const a = questionHash("下一步做什么")
      const b = questionHash("下一步做什么！")
      expect(a).toHaveLength(DIVINE_HASH_LEN)
      expect(b).toHaveLength(DIVINE_HASH_LEN)
      expect(a).not.toBe(b)
      // 子串判据下的关键性质：两个键不互为前缀（定长 ⇒ 恒成立）
      expect(`<!-- divine:3:${a} -->`.includes(`<!-- divine:3:${b} -->`)).toBe(false)
      // 空白归一只做「压连续空白 + 去首尾」——问句内部的空白仍视为不同问（哈希**不参与幂等**，
      // 它只是记录标识，故无需更激进的标点/全半角归一；一息一卦的闸看的是呼吸）。
      expect(questionHash("  下一步做什么 ")).toBe(a)
      expect(questionHash("下一步\t做什么")).toBe(questionHash("下一步 做什么"))
    })

    test("候选边界：空数组=开卦（不开定卦）、单个候选恒指它、全空串视同未传", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      const one = await tool("divine").execute({ question: "只有一个候选", candidates: ["唯一方向"] }, s.ctx)
      const d1 = one.data as { mode: string; pick: { index: number; candidate: string } }
      expect(d1.mode).toBe("定卦")
      expect(d1.pick).toEqual({ index: 1, candidate: "唯一方向" })
      await tool("state").execute({ action: "note", note: "收息" }, s.ctx)
      // 空数组 / 全空串：视同未传（开卦）——定卦模式不得因空清单而成「指了个空」
      const empty = await tool("divine").execute({ question: "空清单", candidates: [] }, s.ctx)
      expect((empty.data as Record<string, unknown>).mode).toBe("开卦")
      expect((empty.data as Record<string, unknown>).pick).toBeNull()
      await tool("state").execute({ action: "note", note: "收息" }, s.ctx)
      const blank = await tool("divine").execute({ question: "全空串", candidates: ["  ", ""] }, s.ctx)
      expect((blank.data as Record<string, unknown>).mode).toBe("开卦")
      cleanup(s)
    })

    test("卦史滥用提示：近窗内问得密则附一句提醒（不拒）", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      let last = ""
      for (let i = 0; i < DIVINE_WARN_COUNT + 1; i++) {
        last = (await tool("divine").execute({ question: `第 ${i} 问` }, s.ctx)).output
        await tool("state").execute({ action: "note", note: "收息" }, s.ctx)
      }
      expect(last).toContain("问卦是渎")
      cleanup(s)
    })
  })

  // ═══════════════════════════════════════════════════════════════════════════
  // 真往返探针撞出的两处缺陷（2026-10-17）
  //
  // 背景：这两条不是读代码想出来的，而是在**真起一条演进线**（给霜天项目 init + 播种 +
  // 布雨）时用工具真往返撞出来的——当时 52 个既有用例全绿，两条都没被覆盖（「绿而无效」）。
  // 探针的事实：
  //   · 连派两个**全新会话** → 第二个的「轮次」写成 2（首派应为 1）；
  //   · 一次回复里并行发两个 `state` 调用（系统提示明确「同一次回复的多个工具调用
  //     会并行执行」）→ 后写者带**陈旧基线**覆盖前者，静默丢一条：
  //     实测「两个 seed_add 只留一个」「`note` + `seed_add` 并行 → 呼吸仍为 0、断点为空」。
  // ═══════════════════════════════════════════════════════════════════════════

  describe("并发写与轮次语义（真往返撞出的缺陷）", () => {
    test("并发 seed_add：两条种子都要在（后写者不得带陈旧基线覆盖前者）", async () => {
      // 反例（修复前实测）：只留一条——`并发甲` 消失。
      // 根因：`appendLedger` 有路径级串行（withLedgerLock），而 `saveState` **没有**——
      // 两者读写的是同一份 `.evolve/`，串行语义却不一致。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await Promise.all([
        tool("state").execute({ action: "seed_add", direction: "并发甲", priority: "P1" }, s.ctx),
        tool("state").execute({ action: "seed_add", direction: "并发乙", priority: "P2" }, s.ctx),
      ])
      const seeds = parseControl(controlOf(s)).seeds.map((x) => x.direction)
      expect(seeds).toContain("并发甲")
      expect(seeds).toContain("并发乙")
      cleanup(s)
    })

    test("并发 note + seed_add：呼吸必须推进且断点必须落盘（note 不得被整条吞掉）", async () => {
      // 反例（修复前实测）：`呼吸 = 0`、`断点 = （无）`——note 的整次写入被 seed_add 的
      // 陈旧基线覆盖。呼吸是「这条河跑了多久」的唯一机器可读计数，被吞即历史断档。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await Promise.all([
        tool("state").execute({ action: "note", note: "并行断点" }, s.ctx),
        tool("state").execute({ action: "seed_add", direction: "并行种子" }, s.ctx),
      ])
      const st = parseControl(controlOf(s))
      expect(st.breath).toBe(1)
      expect(st.note).toBe("并行断点")
      expect(st.seeds.map((x) => x.direction)).toContain("并行种子")
      cleanup(s)
    })

    test("并发 dispatch + seed_add：派工登记与种子入队互不覆盖", async () => {
      // 跨工具并发（同一息里「择种 + 布雨」是自然写法）：两条都必须落地。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await Promise.all([
        tool("dispatch").execute({ orders: [{ task: "并行派工", domain: "par" }] }, s.ctx),
        tool("state").execute({ action: "seed_add", direction: "并行播种", priority: "P1" }, s.ctx),
      ])
      const st = parseControl(controlOf(s))
      expect(st.workers.length).toBe(1)
      expect(st.inflight.length).toBe(1)
      expect(st.seeds.map((x) => x.direction)).toContain("并行播种")
      cleanup(s)
    })

    test("轮次语义：全新会话首派 round=1（round 是「该会话承接的第几轮」，不是全局派工序号）", async () => {
      // 反例（修复前实测）：批内两单 → `a-1:1` 而 `b-1:2`（乙方首派却拿到全局序号 2）。
      // 为何这不是显示问题：`round` 同时是 ①读侧归一的胜负判据（dedupeBySession 取最大轮）
      // ②复用代数 `prev.round + 1` ③收藏幂等键 `collect:<sid>:r<round>`——语义错位会让
      // 「同会话轮次」与「全局序号」的大小关系相反时选错条目、幂等键漂移。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute(
        { orders: [{ task: "首单甲", domain: "a" }, { task: "首单乙", domain: "b" }] },
        s.ctx,
      )
      const st = parseControl(controlOf(s))
      expect(st.workers.length).toBe(2)
      for (const w of st.workers) expect(w.round).toBe(1)
      // 在途与池同源写入：轮次也必须一致（都是各自会话的首轮）。
      for (const i of st.inflight) expect(i.round).toBe(1)
      // 全局派工序号仍单调递增（它是台账幂等键的跨会话唯一源，语义不并入 round）。
      expect(st.seq).toBe(2)
      cleanup(s)
    })

    test("跨层反哺：业务线撞出的调度层问题必须能回流到平台线的种子队列", async () => {
      // 自举闭环缺的那一环：worker 撞出的问题若属于**调度层**（派工丢写 / 轮次错 / 种子泄漏 /
      // 跨线污染 / 会话不可达…），它不属于被演进的那个项目——写进该项目的文档永远没人看。
      // 没有回流，“用生生干活”就无法让生生自己变好：三层链条（息→平台→业务→反哺平台）就断了。
      const s = stub()
      // 业务线（模拟霜天）
      const bizRoot = join(s.home, "biz")
      mkdirSync(bizRoot, { recursive: true })
      await tool("state").execute({ action: "init", root: bizRoot, verify: "v" }, s.ctx)
      await tool("state").execute({ action: "seed_add", direction: "业务方向", priority: "P1" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "业务单", domain: "biz" }] }, s.ctx)
      const sid = parseControl(readFileSync(join(bizRoot, ".evolve", "control.md"), "utf8")).workers[0].sessionId
      // 平台线（模拟生生自己那条线）
      const platRoot = join(s.home, "plat")
      mkdirSync(platRoot, { recursive: true })
      await tool("state").execute({ action: "init", root: platRoot, verify: "v" }, s.ctx)

      // worker 已空闲（可收藏），并撞出了一个调度层问题
      s.running.delete(sid)
      const r = await tool("collect").execute(
        {
          action: "collect",
          session_id: sid,
          root: bizRoot,
          summary: "交付了一个业务修复；顺带撞出：派工时种子不销账",
          next: "派工后种子不销账 → 下一息会重复派同一方向",
          feed_to: platRoot,
        },
        s.ctx,
      )
      // 业务线的 next → 业务线队列；调度层问题 → 平台线队列
      const biz = parseControl(readFileSync(join(bizRoot, ".evolve", "control.md"), "utf8"))
      const plat = parseControl(readFileSync(join(platRoot, ".evolve", "control.md"), "utf8"))
      expect(biz.seeds.map((x) => x.direction)).toContain("业务方向") // 未派出的那条还在
      expect(plat.seeds.map((x) => x.direction)).toContain("派工后种子不销账 → 下一息会重复派同一方向")
      // 溯源：来源必须标明从哪条线反哺来的（否则平台线无法判断这条是谁撞出的）
      const fed = plat.seeds.find((x) => x.direction.includes("派工后种子不销账"))!
      expect(fed.source).toContain("反哺")
      expect(fed.source).toContain(bizRoot)
      expect(r.output).toContain("已反哺")
      cleanup(s)
    })

    test("跨层反哺：目标线已有同向种子时幂等不重复；未给 next 或目标同根时如实拒绝", async () => {
      const s = stub()
      const bizRoot = join(s.home, "biz2")
      const platRoot = join(s.home, "plat2")
      mkdirSync(bizRoot, { recursive: true })
      mkdirSync(platRoot, { recursive: true })
      await tool("state").execute({ action: "init", root: bizRoot, verify: "v" }, s.ctx)
      await tool("state").execute({ action: "seed_add", direction: "单", priority: "P1" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "单", domain: "d" }] }, s.ctx)
      const sid = parseControl(readFileSync(join(bizRoot, ".evolve", "control.md"), "utf8")).workers[0].sessionId
      await tool("state").execute({ action: "init", root: platRoot, verify: "v" }, s.ctx)
      await tool("state").execute({ action: "seed_add", direction: "平台已有方向" }, s.ctx)
      s.running.delete(sid)

      // 幂等：同方向第二次不重复入队
      const r1 = await tool("collect").execute(
        { action: "collect", session_id: sid, root: bizRoot, summary: "s1", next: "平台已有方向", feed_to: platRoot }, s.ctx,
      )
      const plat1 = parseControl(readFileSync(join(platRoot, ".evolve", "control.md"), "utf8"))
      expect(plat1.seeds.filter((x) => x.direction === "平台已有方向").length).toBe(1)
      expect(r1.output).toContain("已在目标队列")

      // 只给目标不给 next：不得凭空造一条种子。
      // ⚠ 必须换一个新 worker——同一会话同一轮的台账键已入账，第二次 collect 会被幂等拦下，
      // 根本走不到反哺分支（本轮实测踩到：拿旧 worker 断言“反哺未落盘”，实际拿到的是幂等提示）。
      await tool("state").execute({ action: "seed_add", direction: "第二单方向", priority: "P1", root: bizRoot }, s.ctx)
      s.running.clear()
      // ⚠ dispatch 必须显式带 root：会话 env 此刻已指 plat2（上一步 init 改写了），
      // 不带 root 会派到平台线去（本轮实测踩到，worker 找不到）。
      await tool("dispatch").execute({ orders: [{ task: "第二单方向", domain: "d2" }], root: bizRoot }, s.ctx)
      const sid2 = parseControl(readFileSync(join(bizRoot, ".evolve", "control.md"), "utf8"))
        .workers.find((w) => w.domain === "d2")!.sessionId
      s.running.delete(sid2)
      const before = parseControl(readFileSync(join(platRoot, ".evolve", "control.md"), "utf8")).seeds.length
      const r2 = await tool("collect").execute(
        { action: "collect", session_id: sid2, root: bizRoot, summary: "s2", feed_to: platRoot }, s.ctx,
      )
      expect(r2.output).toContain("反哺未落盘")
      // 关键：目标线种子数不得变（凭空造一条是比不反哺更坏的错）
      const after = parseControl(readFileSync(join(platRoot, ".evolve", "control.md"), "utf8"))
      expect(after.seeds.length).toBe(before)
      cleanup(s)
    })

    test("init 不跨线继承：新根的初生状态不得带入另一条演进线的池/在途/种子", async () => {
      // 实测事故：会话 env SHENGSHENG_ROOT 指 A（上一条演进线）时 `init root=B`——
      // loadState 的回落链读到了 A 的状态（回显“沿用既有状态：池 2…”），而坐标却写进 B，
      // 于是 B 的 control.md 凭空多出 A 的两个 worker（跨演进线污染）。
      // 更阴的是回显看起来正常，下息才发现 worker 不是自己的。
      // 判据：坐标是**每个根自己的一件事**——目标根没有 control.md 就是初生，不回落。
      const s = stub()
      // A 线：有 worker 与种子
      const rootA = join(s.home, "projA")
      mkdirSync(rootA, { recursive: true })
      await tool("state").execute({ action: "init", root: rootA, verify: "va" }, s.ctx)
      await tool("state").execute({ action: "seed_add", direction: "A 的方向" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "A 的单", domain: "a" }] }, s.ctx)
      const aState = parseControl(readFileSync(join(rootA, ".evolve", "control.md"), "utf8"))
      expect(aState.workers.length).toBe(1)

      // 会话 env 仍指 A（模拟沙箱内残留的上一条线坐标），但 init 传 B
      s.sessionEnv.SHENGSHENG_ROOT = rootA
      const rootB = join(s.home, "projB")
      mkdirSync(rootB, { recursive: true })
      const r = await tool("state").execute({ action: "init", root: rootB, verify: "vb" }, s.ctx)
      const bState = parseControl(readFileSync(join(rootB, ".evolve", "control.md"), "utf8"))
      // B 必须是初生
      expect(r.output).toContain("初生")
      expect(bState.workers.length).toBe(0)
      expect(bState.inflight.length).toBe(0)
      expect(bState.seeds.length).toBe(0)
      // A 不受影响（没被改写成 B）
      const aAfter = parseControl(readFileSync(join(rootA, ".evolve", "control.md"), "utf8"))
      expect(aAfter.coord.root).toBe(rootA)
      expect(aAfter.workers.length).toBe(1)
      cleanup(s)
    })

    test("派出去即销账：对应种子离开队列（否则下一息会把它再派一次）", async () => {
      // 章程规定「择定的方向先 seed_add 落笔，再布雨」——派完后种子若留在队列，下一息读态会把它
      // 当“待派方向”**再派一次**（真重复烧一条 worker 命）。
      // 实测（本轮真演进）：种子 1 条 + 派它后 → 种子仍在、在途也有一份，同一方向两处并存。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("state").execute({ action: "seed_add", direction: "方向甲：修 X", priority: "P1" }, s.ctx)
      await tool("state").execute({ action: "seed_add", direction: "方向乙：另一件", priority: "P2" }, s.ctx)
      const d = await tool("dispatch").execute({ orders: [{ task: "方向甲：修 X", domain: "x" }] }, s.ctx)
      const st = parseControl(controlOf(s))
      // 被派的那条销账，另一条不动
      expect(st.seeds.map((x) => x.direction)).toEqual(["方向乙：另一件"])
      // 不得两处并存（种子 + 在途 同一方向）
      expect(st.inflight.some((i) => st.seeds.some((x) => x.direction === i.direction))).toBe(false)
      // 回工：让生生知道种子被销了（否则它无法确认“这条不会再被派一次”）
      expect(d.output).toContain("种子已销账")
      cleanup(s)
    })

    test("运行中也要报上下文水位：review 的处置建议不得在运行态漏掉 ctx 判据", async () => {
      // 实测（真 worker）：已跑到 ctx 171K（过复用建议线 150K），而 review 只说了「运行中 18 分钟，
      // 未超 40 → 勿扰，下息再看」——因为 ctx 提醒当时只写在「非运行中」分支里。
      // 为何这是缺陷而不是遗漏：运行中恰恰**最该**看到它（还在消费、上下文只会更长），
      // 而「勿扰」的默认建议会把它推得更深。
      //
      // ⚠ 口径修正（本单）：171K 过的是**复用建议线**（150K），**不是模型窗口**（512K）——
      // 旧用例断言的「上下文已超上限」正是那条假误报。现在断言的是正确语义：
      // 「可复用，但收益递减」，且**不得**说成“超上限”。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "长跑任务", domain: "long" }] }, s.ctx)
      const st = parseControl(controlOf(s))
      const sid = st.workers[0].sessionId
      // 造现场：worker 正在运行，且 ctx 过了复用建议线
      const sess = s.sessions.get(sid)!
      sess.ctxTokens = 171_000
      s.running.add(sid)
      const r = await tool("collect").execute({ action: "review", session_id: sid }, s.ctx)
      // 建议必须指向可执行动作（抢救结论 → 退役），而不是只描述现象
      expect(r.output).toContain("retire mode=stop")
      // 关键：不得给出“勿扰”作为**唯一**建议（ctx 要进来）
      expect(r.output).not.toMatch(/勿扰，下息再看\s*$/m)
      // 两个量都在：过的是建议线，不是窗口
      expect(r.output).toContain("复用建议线")
      expect(r.output).toContain("模型窗口")
      expect(r.output).not.toContain("超上限")
      cleanup(s)
    })

    test("上下文水位：两个量分开报（分子/分母各自点名），不再只报一个数并叫它“上限”", async () => {
      // 沿革：① 最早只报裸 `ctx 55400`（分子标了、分母没说是什么量）；
      // ② 后来报 `ctx 55.4K / 150.0K（37%）`——分母有了，但它是**复用建议线**、却被当成“上限”用；
      // ③ 本单拆成两个量，且**两个都报**。真误报就出在 ②：ctx 224K 被判“超上限”，而它只占真窗口 512K 的 44%。
      const scale = ctxScale({})
      expect(scale.window).toBe(DEFAULT_MODEL_CONTEXT_WINDOW)
      expect(scale.window).toBe(512_000)
      expect(scale.advisory).toBe(150_000)
      expect(scale.advisory).toBe(DEFAULT_REUSE_ADVISORY_LIMIT)
      // 行文：两个量各自点名 + 各自百分比
      expect(ctxLine(55_400, scale)).toBe("ctx 55.4K｜复用建议线 150.0K（37%）｜模型窗口 512.0K（11%）")
      expect(ctxLine(undefined, scale)).toBe("") // 读不到就什么都不报，不编造 0%
      // 224K：过建议线、远未过窗口
      const v = ctxVerdict(224_000, scale)!
      expect(v.level).toBe("over-advisory")
      expect(v.advisoryPct).toBe(149)
      expect(v.windowPct).toBe(44)
      expect(v.action).toContain("可复用，但收益递减")
      expect(v.action).not.toContain("必须退役")
      // 520K：真过窗口
      const over = ctxVerdict(520_000, scale)!
      expect(over.level).toBe("over-window")
      expect(over.action).toContain("必须退役")
      expect(over.action).not.toContain("可复用")
      // 两个量都可 env 覆盖（且回显报出数源：缺省值 ≠ 引擎真值）
      const custom = ctxScale({ SHENGSHENG_MODEL_CTX_WINDOW: "128000", SHENGSHENG_REUSE_ADVISORY_CTX: "100000" })
      expect(custom.window).toBe(128_000)
      expect(custom.advisory).toBe(100_000)
      expect(custom.windowSource).toBe("env SHENGSHENG_MODEL_CTX_WINDOW")
      // 引擎自报优先于 env/缺省（.env 改了 MAX_CONTEXT 时不得静默沿用硬编码的 512K）
      const probed = ctxScale({ SHENGSHENG_MODEL_CTX_WINDOW: "128000" }, { contextWindow: () => 200_000 })
      expect(probed.window).toBe(200_000)
      expect(probed.windowSource).toBe("引擎自报")
      // 真工具走一遍：引擎自报 200K 时，ctx 224K 就从「过建议线」变成「过窗口」（必须退役）——
      // 这条钉住“窗口不是写死的 512K”：否则 .env 改成 128K/200K 时工具会静默说错话。
      const s2 = stub()
      s2.contextWindow = 200_000
      await tool("state").execute({ action: "init", root: s2.root, verify: "bun test" }, s2.ctx)
      await tool("dispatch").execute({ orders: [{ task: "分析 B", domain: "b" }] }, s2.ctx)
      const sid2 = parseControl(controlOf(s2)).workers[0].sessionId
      s2.running.clear()
      s2.sessions.get(sid2)!.ctxTokens = 224_000
      const rp = await tool("collect").execute({ action: "review", session_id: sid2 }, s2.ctx)
      expect(rp.output).toContain("模型窗口 200.0K（112%）")
      expect(rp.output).toContain("必须退役")
      cleanup(s2)
      // 窗口未知时**明说未知**、不得编造一个数（旧缺陷的另一面：把建议线当窗口报）
      const unknown = ctxVerdict(224_000, ctxScale({ SHENGSHENG_MODEL_CTX_WINDOW: "0" }))!
      expect(unknown.line).toContain("模型窗口：未知")
      expect(unknown.level).toBe("over-advisory")
      // 两个量各自是什么（state 读态头部那一行）：必须点名两个量且给出 env 可改的名字
      const sm = ctxScaleLine(ctxScale({}))
      expect(sm).toContain("复用建议线 150.0K")
      expect(sm).toContain("SHENGSHENG_REUSE_ADVISORY_CTX")
      expect(sm).toContain("模型窗口 512.0K")
      expect(sm).toContain("SHENGSHENG_MODEL_CTX_WINDOW")
      expect(sm).toContain("必须退役")
      cleanup(stub())
    })

    test("review 口径：ctx 224K 报「可复用但收益递减」（不得说“超上限”）；ctx 520K 报「必须退役」", async () => {
      // 本单的主判据（真工具往返）：同一条 review 路径，两个 ctx 档必须给出**两种不同的处置**。
      // 旧实现在两档上都只能说同一句话（“上下文已过肥…超上限”）——因为分母只有一个 150K。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "分析 A", domain: "a" }] }, s.ctx)
      const sid = parseControl(controlOf(s)).workers[0].sessionId
      s.running.clear() // 空闲（可复用就是在这种状态下被决定的）

      // —— ① 224K：过建议线、占窗口 44% ——
      s.sessions.get(sid)!.ctxTokens = 224_000
      const r1 = await tool("collect").execute({ action: "review", session_id: sid }, s.ctx)
      expect(r1.output).toContain("ctx 224.0K")
      expect(r1.output).toContain("复用建议线 150.0K（149%）")
      expect(r1.output).toContain("模型窗口 512.0K（44%）")
      expect(r1.output).toContain("可复用，但收益递减")
      // ⚠ 三句错话一句也不许出现：它们都是把建议线当窗口的产物
      expect(r1.output).not.toContain("超上限")
      expect(r1.output).not.toContain("已过肥")
      expect(r1.output).not.toContain("必须退役")
      // data 面也分开给（调用方不必解析文案）
      const d1 = (r1.data as { ctx: { level: string; advisoryPct: number; windowPct: number } }).ctx
      expect(d1.level).toBe("over-advisory")
      expect([d1.advisoryPct, d1.windowPct]).toEqual([149, 44])

      // —— ② 520K：真过窗口 ——
      s.sessions.get(sid)!.ctxTokens = 520_000
      const r2 = await tool("collect").execute({ action: "review", session_id: sid }, s.ctx)
      expect(r2.output).toContain("模型窗口 512.0K（102%）")
      expect(r2.output).toContain("必须退役")
      expect(r2.output).not.toContain("可复用，但收益递减")
      const d2 = (r2.data as { ctx: { level: string } }).ctx
      expect(d2.level).toBe("over-window")
      cleanup(s)
    })

    test("读态与收藏的口径：凡回显 ctx 处必须两个量都报（或明写是哪个量）", async () => {
      // state 池行、collect 回显都不能只给一个数/一个百分比——那是“一个数字两个量”的入口。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "修 X", domain: "x" }] }, s.ctx)
      const sid = parseControl(controlOf(s)).workers[0].sessionId
      s.running.clear()
      s.sessions.get(sid)!.ctxTokens = 224_000

      const st = await tool("state").execute({}, s.ctx)
      expect(st.output).toContain("上下文口径")
      expect(st.output).toContain("复用建议线 150.0K")
      expect(st.output).toContain("模型窗口 512.0K")
      expect(st.output).toContain("ctx 224.0K｜复用建议线 150.0K（149%）｜模型窗口 512.0K（44%）")
      expect(st.output).not.toContain("超上限")

      const c = await tool("collect").execute({ action: "collect", alias: "x-1", summary: "## 交付\n完成" }, s.ctx)
      expect(c.output).toContain("复用建议线 150.0K（149%）")
      expect(c.output).toContain("模型窗口 512.0K（44%）")
      cleanup(s)
    })

    test("轮次语义：同会话复用递增到 2，另一会话首派仍为 1（两者互不干扰）", async () => {
      // 这条钉住「round 只随**本会话**的承接数递增」——修复前它会随全局序号跳号。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "甲首单", domain: "a" }] }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "乙首单", domain: "b" }] }, s.ctx)
      const st0 = parseControl(controlOf(s))
      // 甲转为 idle 后可复用（同域）
      st0.workers.find((w) => w.domain === "a")!.status = "idle"
      await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st0))
      s.running.clear()
      await tool("dispatch").execute({ orders: [{ task: "甲复单", domain: "a" }] }, s.ctx)
      const st = parseControl(controlOf(s))
      const a = st.workers.find((w) => w.domain === "a")!
      const b = st.workers.find((w) => w.domain === "b")!
      expect(a.round).toBe(2) // 甲承接了第 2 单
      expect(b.round).toBe(1) // 乙仍是首派——修复前这里是全局序号（3）
      cleanup(s)
    })
  })

  // ==================================================================
  // 独占资源租约（两度实证：负载敏感的两套测量互污染；一次门禁侥幸没被污染）
  //
  // 两件真事故（都是本仓实际发生的，不是假想）：
  //   ① x11-2buf「等机器空闲后开始测量」与 red-case「等负载降下来再跑 A/B」在等**同一种稀缺资源**，
  //      谁先看到负载降谁先跑，另一个随后也跑——两套测量互相污染，而两个结论都依赖「负载干净」。
  //   ② red-case 为复现 pty 竞态**故意**起 10 个并发 st build（load 峰值 55），而 x11-2buf 正在同一台机
  //      跑最终门禁 st test；那次 184 passed / 0 failed **只因时间错开**才没被污染——纯偶然，不是设计。
  //
  // 下面每条用例都走**真工具往返**（dispatch/collect/state + 真 control.md），不喂手拼对象。
  // ==================================================================
  describe("独占资源租约（负载敏感测量与门禁）", () => {
    /** 让池里某 worker 转 idle 并清掉运行态（模拟「worker 跑完/被人工置 idle」）。 */
    const markIdle = async (s: Stub, alias: string): Promise<void> => {
      const st = parseControl(controlOf(s))
      const w = st.workers.find((x) => x.alias === alias)!
      w.status = "idle"
      await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st))
      s.running.delete(w.sessionId)
    }

    test("独占租约：同一批两个工单都申请 cpu-exclusive——第二个被顺延且回显说清（被谁占、何时到期、下息再试）", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      // 容量先放宽：本用例的判据是**资源**冲突，不是活动 worker 上限（否则「未派」可能来自容量而非租约）
      s.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      const dirs = [
        "负载敏感测量 A：x11 双缓冲基线跑 20 轮 A/B（前后测 uptime，期间无人构建）",
        "负载敏感测量 B：pty 复现的最终门禁 st test（要求负载干净）",
      ]
      for (const d of dirs) await tool("state").execute({ action: "seed_add", direction: d, priority: "P1" }, s.ctx)

      const r = await tool("dispatch").execute({
        orders: [
          { task: dirs[0], domain: "x11", resource: DEFAULT_EXCLUSIVE_RESOURCE },
          { task: dirs[1], domain: "pty", resource: DEFAULT_EXCLUSIVE_RESOURCE },
        ],
      }, s.ctx)
      // 只代出去一单（第二单在**锁内**看到租约已被占 → 不派、不新建会话）
      expect(s.sent).toHaveLength(1)
      expect(s.created).toHaveLength(1)
      const st = parseControl(controlOf(s))
      expect(st.leases).toHaveLength(1)
      expect(st.leases[0].resource).toBe(DEFAULT_EXCLUSIVE_RESOURCE)
      expect(st.leases[0].alias).toBe(st.workers[0].alias)
      expect(st.leases[0].sessionId).toBe(st.workers[0].sessionId)
      // 回显三件事：被谁占着、何时到期、下息再试
      expect(r.output).toContain("未派")
      expect(r.output).toContain(`被 ${st.workers[0].alias} 占着`)
      expect(r.output).toContain("到期")
      expect(r.output).toContain("下息再试")
      // 方向不得丢：被顺延的那条必须还在种子队列（与「额度只挡新建、不静默丢」同型）
      expect(st.seeds.map((x) => x.direction)).toContain(dirs[1])
      // 派出去的那条销账、且种子仍在队列里的那条注明是被顺延退货的
      expect(st.seeds.map((x) => x.direction)).not.toContain(dirs[0])
      cleanup(s)
    })

    test("独占租约：A 持租约期间 B 派工不得开跑（不持资源的普通单不受影响；同一持有者续派不受限）", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      s.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      // A：负载敏感的门禁（exclusive:true 是 resource 的简写）
      const a = await tool("dispatch").execute({ orders: [{ task: "st test 最终门禁", domain: "gate", exclusive: true }] }, s.ctx)
      expect(a.output).toContain("布雨完成")
      expect(a.output).toContain("独占")
      const holder = parseControl(controlOf(s)).workers[0]
      expect(s.sent).toHaveLength(1)

      // B：另一个 worker 申请同一资源 → 不得开跑（sent 不增加、不新建会话）
      s.running.clear()
      const b = await tool("dispatch").execute({
        orders: [{ task: "另一路负载敏感测量：20 轮 A/B", domain: "perf", fresh: true, resource: "cpu-exclusive" }],
      }, s.ctx)
      expect(s.sent).toHaveLength(1)
      expect(s.created).toHaveLength(1)
      expect(b.output).toContain("未派")
      expect(b.output).toContain(`被 ${holder.alias} 占着`)
      expect(b.output).toContain("下息再试")
      // 方向落进种子（下息可重派）
      expect(parseControl(controlOf(s)).seeds.map((x) => x.direction)).toContain("另一路负载敏感测量：20 轮 A/B")

      // 对照组：**不持资源**的普通单照派（租约不挡无关工单——否则一条租约会堵住整条院子）
      const plain = await tool("dispatch").execute({ orders: [{ task: "读码定位符号", domain: "docs", fresh: true }] }, s.ctx)
      expect(plain.output).toContain("布雨完成")
      expect(s.sent).toHaveLength(2)
      expect(parseControl(controlOf(s)).leases).toHaveLength(1)

      // 同一持有者续派：它自己持着租约，续期而**不重复占**、也不被自己挡
      s.running.clear()
      const renew = await tool("dispatch").execute({
        orders: [{ task: "续跑门禁的最后一组", session_id: holder.sessionId, resource: "cpu-exclusive" }],
      }, s.ctx)
      expect(renew.output).toContain("布雨完成")
      expect(renew.output).not.toContain("未派")
      const afterRenew = parseControl(controlOf(s))
      expect(afterRenew.leases).toHaveLength(1)          // 仍是同一张租约（不并列两条）
      expect(afterRenew.leases[0].alias).toBe(holder.alias)
      expect(afterRenew.leases[0].expireAt).toBeGreaterThan(holder.dispatchedAt + DEFAULT_LEASE_MINUTES * 60_000 - 1)
      cleanup(s)
    })

    test("独占租约：collect/retire 自动释放，释放后他人可派", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      s.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      await tool("dispatch").execute({ orders: [{ task: "门禁一", domain: "gate", exclusive: true }] }, s.ctx)
      const holder = parseControl(controlOf(s)).workers[0]
      expect(parseControl(controlOf(s)).leases).toHaveLength(1)

      // collect：报告入台账 → 自动释放租约（回显要说出来，不静默）
      s.running.clear()
      const c = await tool("collect").execute({ action: "collect", alias: holder.alias, summary: "门禁 184 passed / 0 failed（前后 uptime 已记）" }, s.ctx)
      expect(c.output).toContain("租约已释放")
      expect(c.output).toContain(DEFAULT_EXCLUSIVE_RESOURCE)
      expect(parseControl(controlOf(s)).leases).toHaveLength(0)

      // 释放后另一人可派（不再被挡）
      const other = await tool("dispatch").execute({ orders: [{ task: "门禁二", domain: "gate2", fresh: true, resource: "cpu-exclusive" }] }, s.ctx)
      expect(other.output).toContain("布雨完成")
      expect(other.output).not.toContain("未派")
      expect(parseControl(controlOf(s)).leases).toHaveLength(1)

      // retire（mode=idle）：同样释放（否则一条死租约会永久堵住院子）
      // 目标必须是**当前持者**（gate-1 的租约已在 collect 时释放，再 retire 它无租约可释放——那是个空断言）
      const stNow = parseControl(controlOf(s))
      const holder2 = stNow.leases[0].alias
      const w2 = stNow.workers.find((x) => x.alias === holder2)!
      s.running.clear()
      const rt = await tool("collect").execute({ action: "retire", alias: w2.alias, mode: "idle", reason: "领域已终" }, s.ctx)
      expect(rt.output).toContain("租约已释放")
      expect(parseControl(controlOf(s)).leases).toHaveLength(0)
      cleanup(s)
    })

    test("独占租约：超期回收——持有者 idle / 会话不存在 / 已到期，读态必须报出回收与原因（死租约不得永久堵院）", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      s.ctx.env.SHENGSHENG_MAX_WORKERS = "6"

      // ① 持有者被置 idle（收藏路径绕过/人工改盘）→ 读态必须报「持有者已空闲」并回收
      await tool("dispatch").execute({ orders: [{ task: "测量甲", domain: "a", exclusive: true }] }, s.ctx)
      const sidA = parseControl(controlOf(s)).workers.find((w) => w.alias === parseControl(controlOf(s)).leases[0].alias)!.sessionId
      await markIdle(s, parseControl(controlOf(s)).leases[0].alias)
      const r1 = await tool("state").execute({}, s.ctx)
      expect(r1.output).toContain("租约回收")
      expect(r1.output).toContain("持有者已空闲")
      expect(r1.output).toContain("cpu-exclusive")
      expect(parseControl(controlOf(s)).leases).toHaveLength(0)

      // ② 持有者会话被外部删除 → 读态必须报「持有者会话已不存在」
      s.running.clear()
      await tool("dispatch").execute({ orders: [{ task: "测量乙", domain: "b", fresh: true, exclusive: true }] }, s.ctx)
      const leaseB = parseControl(controlOf(s)).leases[0]
      s.sessions.delete(leaseB.sessionId)   // 外部删除（服务重启/人工清理）
      const r2 = await tool("state").execute({}, s.ctx)
      expect(r2.output).toContain("租约回收")
      expect(r2.output).toContain("持有者会话已不存在")
      expect(r2.output).toContain(leaseB.alias)
      expect(parseControl(controlOf(s)).leases).toHaveLength(0)

      // ③ 真到期（改盘上的到期时刻，走 parse→render 真往返）：报「已到期」
      s.running.clear()
      await tool("dispatch").execute({ orders: [{ task: "测量丙", domain: "c", fresh: true, exclusive: true }] }, s.ctx)
      const st = parseControl(controlOf(s))
      st.leases[0].expireAt = Date.now() - 1000
      await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st))
      const r3 = await tool("state").execute({}, s.ctx)
      expect(r3.output).toContain("租约回收")
      expect(r3.output).toContain("已到期")
      expect(parseControl(controlOf(s)).leases).toHaveLength(0)
      // 回收后他人可派（院子不再被堵）
      const again = await tool("dispatch").execute({ orders: [{ task: "测量丁", domain: "d", fresh: true, exclusive: true }] }, s.ctx)
      expect(again.output).not.toContain("未派")
      void sidA
      cleanup(s)
    })

    test("独占租约：资源名归一——\"  CPU-Exclusive \" 与 \"cpu-exclusive\" 是同一资源（一个空格绕不过去）", async () => {
      // 起因（验收点名）：`resourceKey` 的注释说「否则一个空格就能绕开租约」，可**没有任何用例真从这个
      // 角度撞过它**——注释不是证据。这里走真派工往返：第一单用带空格+大写的资源名，第二单用规范名。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      s.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      expect(resourceKey("  CPU-Exclusive ")).toBe(DEFAULT_EXCLUSIVE_RESOURCE)
      expect(resourceKey("CPU-EXCLUSIVE")).toBe(resourceKey("cpu-exclusive"))
      const a = await tool("dispatch").execute({
        orders: [{ task: "x11 双缓冲基线（负载敏感）", domain: "x11", resource: "  CPU-Exclusive " }],
      }, s.ctx)
      expect(a.output).toContain("布雨完成")
      // 盘上落的是**归一后**的名字（写侧 upsert 与读侧归一同一套判据）
      const leased = parseControl(controlOf(s)).leases
      expect(leased).toHaveLength(1)
      expect(leased[0].resource).toBe(DEFAULT_EXCLUSIVE_RESOURCE)
      // 另一个人用规范名申请同一资源 → 必须被挡（空格与大小写都不得成为绕开的路）
      s.running.clear()
      const b = await tool("dispatch").execute({
        orders: [{ task: "pty 最终门禁（要求负载干净）", domain: "pty", fresh: true, resource: "cpu-exclusive" }],
      }, s.ctx)
      expect(b.output).toContain("未派")
      expect(b.output).toContain("占着")
      expect(s.sent).toHaveLength(1)                                  // 第二个真没开跑
      expect(parseControl(controlOf(s)).leases[0].alias).toBe(parseControl(controlOf(s)).workers[0].alias)  // 持者未被顶掉
      cleanup(s)
    })

    test("独占租约：SHENGSHENG_LEASE_MINUTES 真决定盘上到期时刻（缺省 90 → 设 1 即 60s）", async () => {
      // 到期是**兜底**判据（防“持有者悄悄死掉而登记没被碰”），而租期由 env 现算——env 若不生效，
      // 到期兜底在长租期下等于不存在（90 分钟 vs 1 分钟是“能不能等”的量级差别）。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "缺省租期测量", domain: "d0", exclusive: true }] }, s.ctx)
      const d0 = parseControl(controlOf(s)).leases[0]
      expect(d0.expireAt - d0.start).toBe(DEFAULT_LEASE_MINUTES * 60_000)
      // env=1：**同一持有者续期**（续期语义：start 保留、expireAt 前推）——租期必须变成 1 分钟。
      // 注：换人会**被挡**（那正是判据），所以这里用 session_id 续派，不是新建。
      s.running.clear()
      s.ctx.env.SHENGSHENG_LEASE_MINUTES = "1"
      const w0 = parseControl(controlOf(s)).workers[0]
      const renew = await tool("dispatch").execute({ orders: [{ task: "缺省租期测量（续期）", session_id: w0.sessionId, resource: "cpu-exclusive" }] }, s.ctx)
      expect(renew.output).not.toContain("未派")
      const d1 = parseControl(controlOf(s)).leases[0]
      expect(d1.sessionId).toBe(w0.sessionId)   // 仍是同一持者（不并列两条）
      // 注意续期语义：`start` 保留（起租时刻不变），`expireAt = 当前派发时刻 + 租期`
      // → 所以“租期”要按 expireAt − now 验，不能按 expireAt − start 验（那样会多出前一次领租到现在的那几毫秒）。
      expect(d1.start).toBe(d0.start)
      expect(d1.expireAt - Date.now()).toBeGreaterThan(55_000)
      expect(d1.expireAt - Date.now()).toBeLessThanOrEqual(60_000)
      // 未到期且持有者仍在跑 → **不得**回收（兜底不得砍掉正在干的活）
      s.running.clear()
      s.running.add(d1.sessionId)
      const alive = await tool("state").execute({}, s.ctx)
      expect(alive.output).not.toContain("租约回收")
      expect(parseControl(controlOf(s)).leases).toHaveLength(1)
      // 到期时刻已过（真 parse→render 往返改盘）→ 必须回收且说「已到期」
      const st = parseControl(controlOf(s))
      st.leases[0].expireAt = Date.now() - 1_000
      await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st))
      const expired = await tool("state").execute({}, s.ctx)
      expect(expired.output).toContain("租约回收")
      expect(expired.output).toContain("已到期")
      expect(parseControl(controlOf(s)).leases).toHaveLength(0)
      cleanup(s)
    })

    test("独占租约：启动宽限的**另一面**——没过宽限不回收，过了宽限（会话可达但不在跑）必须回收", async () => {
      // 已有用例只锁了“刚派出不得被同一息撕掉”这一面。它的反面同样是判据：宽限**过期**后，
      // 若仍不回收，一个“会话还在跑、池里也没标 idle、登记还挂着”的死租约就会一直堵院——
      // 而这条真值（会话可达但空闲）恰恰是宽度之外最常见的中道而废形态。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "刚开跑的负载敏感测量", domain: "g", exclusive: true }] }, s.ctx)
      const lease = parseControl(controlOf(s)).leases[0]
      // 池里**仍标 running**（没人收藏、没人置 idle），只是会话此刻不在跑
      s.running.clear()
      const fresh = await tool("state").execute({}, s.ctx)
      expect(fresh.output).not.toContain("租约回收")
      expect(parseControl(controlOf(s)).leases).toHaveLength(1)
      // 起租时刻前推到宽限之外（真 parse→render 往返改盘）→ 这时才该回收
      const st = parseControl(controlOf(s))
      st.leases[0].start = Date.now() - (LEASE_START_GRACE_MS + 60_000)
      await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st))
      const aged = await tool("state").execute({}, s.ctx)
      expect(aged.output).toContain("租约回收")
      expect(aged.output).toContain("已过启动宽限")
      expect(aged.output).toContain(lease.alias)
      expect(parseControl(controlOf(s)).leases).toHaveLength(0)
      // 回收后他人可拿（院子没被白堵）
      const again = await tool("dispatch").execute({ orders: [{ task: "后来的负载敏感测量", domain: "h", fresh: true, exclusive: true }] }, s.ctx)
      expect(again.output).not.toContain("未派")
      cleanup(s)
    })

    test("独占租约：control.md 往返保真（含竖线的用途不丢、缺节时按空表读、不悬空租约）", () => {
      const lease = {
        resource: DEFAULT_EXCLUSIVE_RESOURCE,
        alias: "gate-1",
        sessionId: "0123456789abcdef0123456789abcdef",
        start: 1_700_000_000_000,
        expireAt: 1_700_000_000_000 + DEFAULT_LEASE_MINUTES * 60_000,
        purpose: "st test 门禁｜前后 uptime｜期间无他人构建",
      }
      const md = renderControl({ coord: {}, breath: 0, seq: 0, workers: [], inflight: [], seeds: [], pending: [], leases: [lease] })
      expect(md).toContain("## 资源租约")
      const back = parseControl(md)
      expect(back.leases).toEqual([lease])
      expect(renderControl(back)).toBe(md)   // 往返恒等
      // 旧 control.md（无此节）读成空表（升级不迁移数据：旧文件必须照旧可读）
      expect(parseControl("# x\n\n## 计数\n- 呼吸：1\n\n## 种子队列\n（空）\n").leases).toEqual([])
      // 空集合写占位（保留节结构供人工查看）
      expect(md).toContain("（空）")
      expect(parseControl(md).leases).toHaveLength(1)
      // 手改出的同资源两条（并列）= 一条租约两个持者：读侧归一为靠后一条（不住让死条目堵院）
      const dup = `${md.trimEnd()}\n| 资源名 | 副本-1 | | 1 | 9999999999999 | 后写的 |\n`
      const norm = parseControl(dup.replace(/\n\| 资源名 \|[^\n]*\n/, "\n"))
      expect(norm.leases).toHaveLength(1)
    })

    test("独占租约：同一息两次并发 dispatch 只能一位拿到（租约领取在锁内、先于 svc.run）", async () => {
      // 为何单独一条（不是上一批用例的子集）：真实形态是**同一息里模型发两个 dispatch 调用**
      // （系统提示已明说“同一次回复里的多个工具调用是并行执行”）。若领取不在锁内，两个调用各自读到
      // “无人持租约”就一起开跑了——那正是 load 峰值 55 而门禁在跑的那次事故的重演。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      s.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      const [r1, r2] = await Promise.all([
        tool("dispatch").execute({ orders: [{ task: "并发测量甲", domain: "a", fresh: true, exclusive: true }] }, s.ctx),
        tool("dispatch").execute({ orders: [{ task: "并发测量乙", domain: "b", fresh: true, exclusive: true }] }, s.ctx),
      ])
      const leased = parseControl(controlOf(s)).leases
      expect(leased).toHaveLength(1)          // 同一资源只剩一位持者
      expect(s.sent).toHaveLength(1)          // 只有一个开跑
      const texts = [r1.output, r2.output]
      expect(texts.filter((t) => t.includes("未派")).length).toBe(1)
      // 另一路真派出去了（有派工回执行；注：两路回显的标题行都是“布雨完成”，不能拿它当判据）
      const dispatched = texts.find((t) => !t.includes("未派"))!
      expect(dispatched).toContain("轮次 1")
      expect(dispatched).toContain("独占 cpu-exclusive")
      // 被挡下的那单方向进了种子（不丢）
      const blockedTask = s.sent[0].message.includes("并发测量甲") ? "并发测量乙" : "并发测量甲"
      expect(parseControl(controlOf(s)).seeds.map((x) => x.direction)).toContain(blockedTask)
      cleanup(s)
    })

    test("独占租约：台账幂等重放也要释放租约（否则死租约只能等超期）", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "门禁重放", domain: "gate", exclusive: true }] }, s.ctx)
      const holder = parseControl(controlOf(s)).workers[0]
      s.running.clear()
      const first = await tool("collect").execute({ action: "collect", alias: holder.alias, summary: "第一次报告" }, s.ctx)
      expect(first.output).toContain("租约已释放")
      // 人工把租约**写回**（模拟“已收过但租约漏释放”的历史现场，如旧版工具/中途崩溃）
      const st = parseControl(controlOf(s))
      st.leases = [{ resource: DEFAULT_EXCLUSIVE_RESOURCE, alias: holder.alias, sessionId: holder.sessionId, start: Date.now(), expireAt: Date.now() + 3_600_000, purpose: "遗留死租约" }]
      await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st))
      // 重放同一条收藏（幂等命中）——也必须把租约收回去
      const replay = await tool("collect").execute({ action: "collect", alias: holder.alias, summary: "第二次报告（重放）" }, s.ctx)
      expect(replay.output).toContain("幂等跳过")
      expect(replay.output).toContain("租约已释放")
      expect(parseControl(controlOf(s)).leases).toHaveLength(0)
      cleanup(s)
    })

    test("独占租约：刚派出的单不得被同一息的读态回收（启动宽限——引擎拉起会话有延迟）", async () => {
      // 实测风险：dispatch 返回后引擎把会话拉起来是有延迟的，而 dispatch 末尾与下一息开头都会立刻读态。
      // 若"未运行就回收"，自己刚领的租约会被自己同一息撕掉，紧接着的第二单就进来了（又变成两套测量互污染）。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      s.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      await tool("dispatch").execute({ orders: [{ task: "长测量", domain: "bench", exclusive: true }] }, s.ctx)
      // 关键：模拟“会话尚未被引擎报告为运行中”（stub 的 run 已置 running，这里手动抹掉）
      s.running.clear()
      const r = await tool("state").execute({}, s.ctx)
      expect(r.output).toContain("租约 1")
      expect(r.output).not.toContain("租约回收")
      expect(parseControl(controlOf(s)).leases).toHaveLength(1)
      // 但**池登记已标 idle**（worker 真交付/被收）时当即可信，不受宽限约束
      const st = parseControl(controlOf(s))
      st.workers[0].status = "idle"
      await Bun.write(join(s.root, ".evolve", "control.md"), renderControl(st))
      const r2 = await tool("state").execute({}, s.ctx)
      expect(r2.output).toContain("租约回收")
      expect(parseControl(controlOf(s)).leases).toHaveLength(0)
      cleanup(s)
    })

    test("独占租约：预检（preview）看得见冲突——不派工只出计划时也要说出会被顺延", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      s.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      await tool("dispatch").execute({ orders: [{ task: "先派出的门禁", domain: "gate", exclusive: true }] }, s.ctx)
      const holder = parseControl(controlOf(s)).leases[0].alias
      s.running.clear()
      s.ctx.env.SHENGSHENG_DISPATCH = "preview"
      const prev = await tool("dispatch").execute({ orders: [{ task: "第二路测量", domain: "perf", fresh: true, resource: "cpu-exclusive" }] }, s.ctx)
      expect(prev.output).toContain("预检")
      expect(prev.output).toContain(`被 ${holder} 占着`)
      expect(s.sent).toHaveLength(1)      // 预检不派工
      cleanup(s)
    })
  })

  // ==================================================================
  // 仓级资源租约（同一个 git 仓上的多条演进线）
  //
  // 现场（本仓真值，两条线同时活着）：
  //   /home/x/code/gebai/.evolve            ← 歌白线（会话 7dd7b830）
  //   /home/x/code/gebai/shuangtian/.evolve ← 霜天线
  //   两者的 `git rev-parse --show-toplevel` 都是 /home/x/code/gebai
  // ⇒ 两条线各有自己的 control.md，却共享**同一棵树、同一批文件、同一台机器**，而租约互相看不见。
  // 代价（当日实测）：一条线在跑 `tsc --noEmit`（196% CPU），另一条正在等 load<2 做性能测量；
  // 一条在 `reverse_verify_kill_drill.py` 里把 shengsheng.ts 切成回退态，另一条正准备提交同一文件。
  //
  // 下面每条用例都用**两个真实项目根 + 同一个真 git 仓**（git init 真跑），不喂手拼对象：
  // 只测内部函数的话，“探测不到 git 顶层就安静降级”与“不同仓互不影响”这两条根本无从验。
  // ==================================================================
  describe("仓级资源租约（同一 git 仓上的多条演进线）", () => {
    /** 造一个真 git 仓，其下两个子目录各是一条演进线的项目根（各自 init 过 control.md）。
     *  `git` 命令真跑（夹具里 `git init -q` 一个空仓就够——`rev-parse --show-toplevel` 只看目录结构）。 */
    const twoLines = async () => {
      const home = mkdtempSync(join(tmpdir(), "gebai-repolease-"))
      const repo = join(home, "repo")
      mkdirSync(repo, { recursive: true })
      const g = spawnSync("git", ["init", "-q", repo], { encoding: "utf8" })
      if (g.status !== 0) throw new Error(`git init 失败：${g.stderr}`)
      const rootA = join(repo, "line-a")
      const rootB = join(repo, "shuangtian")   // 故意取与真实现场同形的名（被测的是关系，不是名字）
      mkdirSync(rootA, { recursive: true })
      mkdirSync(rootB, { recursive: true })
      const a = stub({ root: rootA, sessionId: "sessA", idPrefix: "a" })
      const b = stub({ root: rootB, sessionId: "sessB", idPrefix: "b" })
      // 两条线各自立坐标（真写 .evolve/control.md）
      await tool("state").execute({ action: "init", root: rootA, verify: "bun test" }, a.ctx)
      await tool("state").execute({ action: "init", root: rootB, verify: "bun test" }, b.ctx)
      return { home, repo, rootA, rootB, a, b, cleanup: () => { cleanup(a); cleanup(b); rmSync(home, { recursive: true, force: true }) } }
    }
    const sharedFile = (repo: string) => join(repo, ".evolve-shared", "leases.json")
    const sharedOf = (repo: string) => parseSharedLeases(readFileSync(sharedFile(repo), "utf8"))

    test("仓级租约：资源名带 git 顶层；同仓 A 根占着 → B 根派工被顺延且回显说清（被哪条线、何时到期）", async () => {
      const t = await twoLines()
      t.a.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      t.b.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      // A 根（歌白线）领仓级租约
      const ra = await tool("dispatch").execute({ orders: [{ task: "歌白线：tsc --noEmit 全量门禁", domain: "gate", exclusive: true }], root: t.rootA }, t.a.ctx)
      expect(ra.output).toContain("布雨完成")
      expect(ra.output).toContain("仓级")
      // 共享表在 **git 顶层**，不在 A 根自己的 .evolve/
      expect(existsSync(sharedFile(t.repo))).toBe(true)
      expect(existsSync(join(t.rootA, ".evolve-shared"))).toBe(false)
      const sh = sharedOf(t.repo)
      expect(sh.leases).toHaveLength(1)
      // 资源名带 git 顶层（这是跨线可见的唯一依据）
      expect(sh.leases[0].resource).toBe(`repo:${t.repo}:${DEFAULT_EXCLUSIVE_RESOURCE}`)
      expect(sh.leases[0].root).toBe(t.rootA)
      // 本地镜像也写上了（同一资源名）：一条线自己读态时也知道自己持着什么
      expect(parseControl(controlOf(t.a)).leases.map((l) => l.resource)).toContain(`repo:${t.repo}:${DEFAULT_EXCLUSIVE_RESOURCE}`)

      // B 根（霜天线）派同一资源 → 必须被顺延（不能开跑）
      const rb = await tool("dispatch").execute({ orders: [{ task: "霜天线：等 load<2 做性能测量", domain: "perf", fresh: true, exclusive: true }], root: t.rootB }, t.b.ctx)
      expect(t.b.sent).toHaveLength(0)              // 真没开跑
      expect(rb.output).toContain("未派")
      expect(rb.output).toContain("跨线")            // 回显必须标出这是**另一条线**挡的
      expect(rb.output).toContain("line-a")          // 说清是哪条线（A 根名）
      expect(rb.output).toContain("到期")
      expect(rb.output).toContain("下息再试")
      // 方向不得丢：退回 B 自己的种子队列
      expect(parseControl(controlOf(t.b)).seeds.map((x) => x.direction)).toContain("霜天线：等 load<2 做性能测量")
      // B 的本地表**不应**凭空多出一条自己的租约（它没拿到）
      expect(parseControl(controlOf(t.b)).leases).toHaveLength(0)
      t.cleanup()
    })

    test("仓级租约：不同 git 仓的 A/B 互不相挡（防止把所有人互相锁死）", async () => {
      // 这一条与上一条是**同一枚硬币的两面**：只验前一面（同仓要挡）时，把资源名写成全局常量也能过，
      // 而那会把整台机器上的所有仓串成一串——一个陌生仓库的门禁能挡住你的测量。
      const t = await twoLines()
      const other = mkdtempSync(join(tmpdir(), "gebai-otherrepo-"))
      const g = spawnSync("git", ["init", "-q", other], { encoding: "utf8" })
      if (g.status !== 0) throw new Error(`git init 失败：${g.stderr}`)
      const rootC = join(other, "line-c")
      mkdirSync(rootC, { recursive: true })
      const c = stub({ root: rootC, sessionId: "sessC", idPrefix: "c" })
      await tool("state").execute({ action: "init", root: rootC, verify: "bun test" }, c.ctx)
      t.a.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      c.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      // A 根（repo）先占
      await tool("dispatch").execute({ orders: [{ task: "repo 内的门禁", domain: "gate", exclusive: true }], root: t.rootA }, t.a.ctx)
      expect(sharedOf(t.repo).leases).toHaveLength(1)
      // 另一个 git 仓的 C 根：必须照派（两个仓是两台不同的工作树）
      const rc = await tool("dispatch").execute({ orders: [{ task: "另一个仓的负载敏感测量", domain: "perf", fresh: true, exclusive: true }], root: rootC }, c.ctx)
      expect(c.sent).toHaveLength(1)
      expect(rc.output).not.toContain("未派")
      expect(rc.output).not.toContain("跨线")
      // 两个仓各自有自己的共享表（不是一个全局表）
      expect(existsSync(sharedFile(other))).toBe(true)
      expect(sharedOf(other).leases).toHaveLength(1)
      expect(sharedOf(other).leases[0].resource).toBe(`repo:${other}:${DEFAULT_EXCLUSIVE_RESOURCE}`)
      expect(existsSync(sharedFile(t.repo))).toBe(true)
      expect(sharedOf(t.repo).leases[0].resource).toBe(`repo:${t.repo}:${DEFAULT_EXCLUSIVE_RESOURCE}`)
      cleanup(c)
      rmSync(other, { recursive: true, force: true })
      t.cleanup()
    })

    test("仓级租约：B 根读态能看见 A 根的仓级租约（两份合并展示，并写明仓级租约域路径）", async () => {
      // 「一"份文件、各写各的域」的关键证据：B 自己的 control.md 里**没有**这条，但它读态看得到。
      const t = await twoLines()
      t.a.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      await tool("dispatch").execute({ orders: [{ task: "A 线：st test 最终门禁", domain: "gate", exclusive: true }], root: t.rootA }, t.a.ctx)
      const holder = sharedOf(t.repo).leases[0]
      const rb = await tool("state").execute({ root: t.rootB }, t.b.ctx)
      expect(rb.output).toContain("仓级租约域")
      expect(rb.output).toContain(sharedFile(t.repo))
      expect(rb.output).toContain(holder.alias)
      expect(rb.output).toContain("line-a")            // 哪条线在持着
      expect(rb.output).toContain("（含仓级 1）")         // 计数口径明说了
      const data = rb.data as { repoLeases: Array<{ resource: string; root: string }>; repoTop: string | null }
      expect(data.repoTop).toBe(t.repo)
      expect(data.repoLeases.map((l) => l.resource)).toEqual([holder.resource])
      // B 自己的 control.md 里没有这条租约（各写各的域）
      expect(parseControl(controlOf(t.b)).leases).toHaveLength(0)
      t.cleanup()
    })

    test("仓级租约：A 的 worker 被收藏后，共享表释放 → B 根立刻能领", async () => {
      const t = await twoLines()
      t.a.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      t.b.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      await tool("dispatch").execute({ orders: [{ task: "A 线门禁", domain: "gate", exclusive: true }], root: t.rootA }, t.a.ctx)
      const holder = sharedOf(t.repo).leases[0]
      t.a.running.clear()
      t.a.sessions.get(holder.sessionId)!.messages = [{ role: "assistant", content: "门禁 184 passed / 0 failed" }]
      const col = await tool("collect").execute({ action: "collect", alias: holder.alias, summary: "门禁绿", root: t.rootA }, t.a.ctx)
      expect(col.output).toContain("租约已释放")
      // 同一资源在两个域各有一条（本地镜像 + 仓级）——回显不得列两遍（那会让读者以为真有两份资源被释放）
      expect(col.output.split("cpu-exclusive").length - 1).toBe(1)
      expect(sharedOf(t.repo).leases).toHaveLength(0)
      // B 现在能领到
      const rb = await tool("dispatch").execute({ orders: [{ task: "B 线测量", domain: "perf", fresh: true, exclusive: true }], root: t.rootB }, t.b.ctx)
      expect(t.b.sent).toHaveLength(1)
      expect(rb.output).not.toContain("未派")
      expect(sharedOf(t.repo).leases).toHaveLength(1)
      expect(sharedOf(t.repo).leases[0].root).toBe(t.rootB)
      t.cleanup()
    })

    test("仓级租约：超期回收跨线——B 根读态能回收 A 根留下的死租约并报出原因", async () => {
      // 为何必须跨线回收：仓级持者可能在另一条线上，若那条线已停（心跳被停用/会话被删），
      // 就**没人会去收**——而死租约堵住的恰好是“要独占才敢做”的那类活。
      const t = await twoLines()
      t.a.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      await tool("dispatch").execute({ orders: [{ task: "A 线长测量", domain: "bench", exclusive: true }], root: t.rootA }, t.a.ctx)
      const lease = sharedOf(t.repo).leases[0]
      // 把到期时刻改成已过（真读→改→写共享表，走测试自己的渲染器而不是生产代码里的私有函数）
      const doc = JSON.parse(readFileSync(sharedFile(t.repo), "utf8")) as { version: number; leases: Array<Record<string, unknown>> }
      doc.leases[0].expireAt = Date.now() - 1_000
      mkdirSync(dirname(sharedFile(t.repo)), { recursive: true })
      await Bun.write(sharedFile(t.repo), `${JSON.stringify(doc, null, 2)}\n`)
      // B 读态：必须报出「仓级租约回收」与原因（跨线可见的回收）
      const rb = await tool("state").execute({ root: t.rootB }, t.b.ctx)
      expect(rb.output).toContain("仓级租约回收")
      expect(rb.output).toContain("已到期")
      expect(rb.output).toContain(lease.alias)
      expect(rb.output).toContain("line-a")
      expect(sharedOf(t.repo).leases).toHaveLength(0)
      // 回收后 B 能领（院子真交回了）
      const rd = await tool("dispatch").execute({ orders: [{ task: "B 线测量（接上）", domain: "perf", fresh: true, exclusive: true }], root: t.rootB }, t.b.ctx)
      expect(rd.output).not.toContain("未派")
      expect(sharedOf(t.repo).leases[0].root).toBe(t.rootB)
      t.cleanup()
    })

    test("仓级租约：拿不到 git 顶层时安静退化（dispatch 照常能派，只在本地域互斥并明说降级）", async () => {
      // 硬要求：拿不到 git 顶层或目录不可写时**不能因此让 dispatch 起不来**。
      // 本用例把 `git` 命令打空（模拟“没装 git / 不在仓内”）——真仓里的 git 仍在，所以断言的是退化后的行为。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      s.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      s.commandHook = async () => ({ stdout: "", stderr: "git: command not found", code: 127 })
      const r1 = await tool("dispatch").execute({ orders: [{ task: "没 git 也能派：门禁", domain: "gate", exclusive: true }] }, s.ctx)
      expect(r1.output).toContain("布雨完成")
      expect(r1.output).toContain("仓级租约域未启用")
      expect(r1.output).not.toContain("未派")
      expect(s.sent).toHaveLength(1)
      // 本地域照旧互斥（退化不是“取消互斥”，而是“只在本线生效”）
      s.running.clear()
      const r2 = await tool("dispatch").execute({ orders: [{ task: "没 git 也能派：第二个门禁", domain: "p", fresh: true, exclusive: true }] }, s.ctx)
      expect(r2.output).toContain("未派")
      expect(s.sent).toHaveLength(1)
      // 读态明说“仓级未启用”，不谎报跨线互斥生效
      const st = await tool("state").execute({}, s.ctx)
      expect(st.output).toContain("未启用")
      expect((st.data as { repoTop: string | null }).repoTop).toBe(null)
      cleanup(s)
    })

    test("仓级租约：共享表损坏时不覆盖、不崩——按“仓级不可用”降级，本地域照旧互斥", async () => {
      // 损坏可能来自：另一版工具、手改、或写到一半的旧实现。覆盖它就把别的线的租约抹掉了。
      const t = await twoLines()
      mkdirSync(dirname(sharedFile(t.repo)), { recursive: true })
      await Bun.write(sharedFile(t.repo), "{ 这不是 JSON")
      t.b.ctx.env.SHENGSHENG_MAX_WORKERS = "6"
      const rb = await tool("dispatch").execute({ orders: [{ task: "损坏表下的门禁", domain: "gate", exclusive: true }], root: t.rootB }, t.b.ctx)
      expect(rb.output).toContain("布雨完成")
      expect(rb.output).toContain("跨线互斥**未生效**")
      expect(t.b.sent).toHaveLength(1)
      expect(readFileSync(sharedFile(t.repo), "utf8")).toBe("{ 这不是 JSON")   // 未被覆盖
      // 本地域照旧互斥
      t.b.running.clear()
      const rb2 = await tool("dispatch").execute({ orders: [{ task: "第二个门禁", domain: "p", fresh: true, exclusive: true }], root: t.rootB }, t.b.ctx)
      expect(rb2.output).toContain("未派")
      t.cleanup()
    })

    test("仓级租约：git 顶层把“同一棵树的不同子目录”判为同域，把“不同仓”判为异域（资源名层）", async () => {
      // 纯函数层锁住名字规则：它是跨线可见性的**唯一依据**，也是“不把所有人串成一串”的唯一分界线。
      expect(repoResourceName("/home/x/code/gebai", "cpu-exclusive")).toBe("repo:/home/x/code/gebai:cpu-exclusive")
      // 归一：大写/空格/尾分隔符不得成为绕开或另立一域的路
      expect(repoResourceName("/home/x/code/gebai/", "  CPU-Exclusive ")).toBe(repoResourceName("/home/x/code/gebai", "cpu-exclusive"))
      expect(repoResourceName("/home/x/code/gebai", "cpu-exclusive")).not.toBe(repoResourceName("/home/x/code/other", "cpu-exclusive"))
      // 共享表解析：坏 JSON / 未知版本 / 残行都必须**报错或丢弃**，不得当空表静默放过
      expect(parseSharedLeases("not json").error).toContain("不是合法 JSON")
      expect(parseSharedLeases(JSON.stringify({ version: 99, leases: [] })).error).toContain("版本")
      expect(parseSharedLeases(null).leases).toEqual([])
      const md = renderSharedLeases([
        { resource: "repo:/r:cpu-exclusive", alias: "a-1", sessionId: "s1", start: 1, expireAt: 2, purpose: "p", root: "/r" },
      ])
      const back = parseSharedLeases(md)
      expect(back.leases).toHaveLength(1)
      expect(back.leases[0]).toMatchObject({ resource: "repo:/r:cpu-exclusive", alias: "a-1", root: "/r" })
      // 往返恒等（写→读→写 字节不变：共享表是可 diff 的，形态必须确定）
      expect(renderSharedLeases(back.leases)).toBe(md)
    })
  })

  // ==================================================================
  // 提交范围：宽范围暂存会带走别人的东西（2026-10-17 本仓真事故）
  //
  // 现场：worker `shared-lease` 提交 `856ba9bb` 时用 `git add -A`，把**另一个仍在跑的 worker**
  // （`retire-tree`，384 消息）的两份未完成交付物一起提交了：
  //   A packages/agents/src/agents/shengsheng/proc-tree.ts                    (+596)
  //   A packages/agents/src/agents/shengsheng/shengsheng-retire-tree.test.ts  (+573)
  // 根因不是粗心，是**守卫的缺口**：`edit`/`write` 的防陈旧覆盖守卫只管已跟踪文件，
  // 未跟踪的新文件（`??`）不受任何保护——**谁 add 谁带走**。
  //
  // 下面每条用例都用**真 git 仓**（`git init` + 真提交 + 真 `git add`），不喂手拼对象：
  // 被测的正是「git 到底把哪些文件放进了提交」这个事实，桩在这里会把差异抹平
  // （CONVENTIONS §7.2：桩必须复现被测的那一段真实行为）。
  // ==================================================================
  describe("提交范围（宽范围暂存会带走别人的东西）", () => {
    /** 真跑 git（夹具的仓是自己 init 的，故自带一次性的身份配置）。 */
    const git = (root: string, args: string[]) =>
      spawnSync("git", ["-C", root, "-c", "user.email=t@t", "-c", "user.name=t", ...args], { encoding: "utf8" })

    /** 造一个真 git 仓：`.gitignore` + `src/a.ts` 已在首个提交里（`.evolve/` 被忽略）。
     *  返回后：工作树干净、有一条提交、`repoTop === root`（根即顶层）。 */
    const repoWithCommit = (root: string) => {
      const init = spawnSync("git", ["init", "-q", root], { encoding: "utf8" })
      if (init.status !== 0) throw new Error(`git init 失败：${init.stderr}`)
      mkdirSync(join(root, "src"), { recursive: true })
      writeFileSync(join(root, ".gitignore"), ".evolve/\n")
      writeFileSync(join(root, "src", "a.ts"), "export const a = 1\n")
      git(root, ["add", "--", ".gitignore", "src/a.ts"])
      git(root, ["commit", "-q", "-m", "init"])
    }

    /** 提交里到底有哪些文件（**直接问 git**，不从被测代码里读——两者互为对照）。 */
    const commitFiles = (root: string) =>
      (git(root, ["show", "--name-only", "--pretty=format:", "HEAD"]).stdout ?? "")
        .split(/\r?\n/)
        .map((s) => s.trim())
        .filter(Boolean)

    test("宽范围暂存：别人的未跟踪新文件被一起提交 → review 报「疑似宽范围暂存」并点名该文件", async () => {
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      repoWithCommit(s.root)
      // 另一个 worker 的成果还在工作树里（`??`，未提交）——它不知道自己的活会被别人带走
      writeFileSync(join(s.root, "src", "other-worker.ts"), "export const other = 1\n")
      // 本单只该动 a.ts（工单里点明的就是它）
      await tool("dispatch").execute({ orders: [{ task: "改 src/a.ts 的解析", domain: "a", deliver: "src/a.ts 的补丁 + 回归" }] }, s.ctx)
      s.running.clear()
      // 事故重演的**关键一步**：宽范围暂存（`git add -A`）——把别人的 `??` 文件一起带走
      writeFileSync(join(s.root, "src", "a.ts"), "export const a = 2\n")
      git(s.root, ["add", "-A"])
      git(s.root, ["commit", "-q", "-m", "fix: a 的解析"])
      // 前置事实（用例自己先核一遍：事故真复现了，后面才有东西可测）
      expect(commitFiles(s.root)).toContain("src/other-worker.ts")

      const rv = await tool("collect").execute({ action: "review", alias: "a-1" }, s.ctx)
      expect(rv.output).toContain("疑似宽范围暂存带走了别人的改动")
      expect(rv.output).toContain("src/other-worker.ts")   // 必须点出**是哪个**文件
      expect(rv.output).toContain("+1")                      // 带增删行数（`856ba9bb` 那种 +596 的形状）
      expect(rv.output).toContain("不要 revert")              // 处置：先记归属，不是回滚（回滚会抹掉已落盘成果）
      // 结构化字段也能核（控制面/前端按字段用，不靠抠文本）
      const data = rv.data as { commitScope?: { outOfScope: string[]; inScope: number } }
      expect(data.commitScope?.outOfScope).toContain("src/other-worker.ts")
      expect(data.commitScope?.inScope).toBe(1)             // a.ts 在工单范围内
      cleanup(s)
    })

    test("反向用例：别人的 `??` 文件不得被带走——只 add 自己列出的路径，且 review 不误报", async () => {
      // 这条与上一条是**同一枚硬币的两面**。只验「宽范围暂存会被抓到」时，把检测写成
      // 「永远报异常」也能过——而那种实现会让每一条正常提交都报警，警报疲劳之后真事故就看不见了。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      repoWithCommit(s.root)
      writeFileSync(join(s.root, "src", "other-worker.ts"), "export const other = 1\n")
      await tool("dispatch").execute({ orders: [{ task: "改 src/a.ts 的解析", domain: "a", deliver: "src/a.ts 的补丁 + 回归" }] }, s.ctx)
      s.running.clear()

      // 纪律形态：逐个列路径——**命令由 `stagingCommands()` 生成后真跑**，不是用例自己手写一份：
      // 否则「纪律退回 `git add -A`」这条回退与断言之间没有连接，反向用例会恒绿（§7.2 的桩病）。
      writeFileSync(join(s.root, "src", "a.ts"), "export const a = 3\n")
      const addCmd = stagingCommands(["src/a.ts"])[0]
      expect(addCmd.startsWith("git add")).toBe(true)
      git(s.root, addCmd.replace(/^git\s+/, "").split(/\s+/))
      git(s.root, ["commit", "-q", "-m", "fix: a 的解析（只带自己的文件）"])

      // ① **别人的新文件不得进提交**（这是本条纪律的硬核，不靠人肉核对）
      const files = commitFiles(s.root)
      expect(files).toContain("src/a.ts")
      expect(files).not.toContain("src/other-worker.ts")
      // 它还躺在工作树里（谁也没碰它）
      const st = git(s.root, ["status", "--porcelain"]).stdout ?? ""
      expect(st).toContain("?? src/other-worker.ts")

      // ② review 不误报，且**明说已核过**（「0 命中」与「读不到」必须能分辨）
      const rv = await tool("collect").execute({ action: "review", alias: "a-1" }, s.ctx)
      expect(rv.output).not.toContain("疑似宽范围暂存")
      expect(rv.output).toContain("均在")
      // ③ 但 `??` 必须被提醒（下一次宽范围暂存会带走它）
      expect(rv.output).toContain("未跟踪新文件")
      expect(rv.output).toContain("src/other-worker.ts")
      cleanup(s)
    })

    test("拿不到 git 时如实报「本项未核」——不得当成「没有宽范围暂存」", async () => {
      // 「0 命中」与「读不到」必须是两个结论（PITFALLS）。把读不到报成通过，
      // 等于在最需要警报的环境里安静（比如沙箱里没有 git、或仓还没 init）。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "改一行", domain: "x" }] }, s.ctx)
      s.running.clear()
      s.commandHook = async () => ({ stdout: "", stderr: "git: command not found", code: 127 })
      const rv = await tool("collect").execute({ action: "review", alias: "x-1" }, s.ctx)
      expect(rv.output).toContain("提交范围本项未核")
      expect(rv.output).toContain("只是无从核")
      expect(rv.output).not.toContain("均在")
      expect(rv.output).not.toContain("疑似宽范围暂存")
      cleanup(s)
    })

    test("纯函数：判定「哪些文件不在工单里」+ 追加式暂存命令 + 纪律文本同源", () => {
      // ① git 输出解析（两种行交织：raw 块 + numstat 块，按路径合并）
      const head = parseLastCommit(
        [
          "856ba9bbdeadbeefdeadbeefdeadbeefdeadbeef\x1f2026-10-17 11:00:00 +0800\x1ffeat(shengsheng): 仓级资源租约",
          ":000000 100644 0000000 1111111 A\tpackages/agents/src/agents/shengsheng/proc-tree.ts",
          ":000000 100644 0000000 2222222 A\tpackages/agents/src/agents/shengsheng/shengsheng-retire-tree.test.ts",
          ":100644 100644 3333333 4444444 M\tpackages/agents/src/agents/shengsheng/shengsheng.ts",
          ":000000 100644 0000000 5555555 A\tassets/logo.bin",
          "596\t0\tpackages/agents/src/agents/shengsheng/proc-tree.ts",
          "573\t0\tpackages/agents/src/agents/shengsheng/shengsheng-retire-tree.test.ts",
          "12\t3\tpackages/agents/src/agents/shengsheng/shengsheng.ts",
          "-\t-\tassets/logo.bin",
        ].join("\n"),
      )!
      expect(head.sha).toBe("856ba9bbdeadbeefdeadbeefdeadbeefdeadbeef")
      expect(head.files.map((f) => f.path)).toEqual([
        "packages/agents/src/agents/shengsheng/proc-tree.ts",
        "packages/agents/src/agents/shengsheng/shengsheng-retire-tree.test.ts",
        "packages/agents/src/agents/shengsheng/shengsheng.ts",
        "assets/logo.bin",
      ])
      expect(head.files[0]).toMatchObject({ status: "A", added: 596, deleted: 0 })
      expect(head.files[3].added).toBe(null)                 // 二进制：不编造 0 行
      expect(parseLastCommit("not a commit")).toBe(null)     // 读不到 ⇒ null，不是空提交

      // ② 工单自由文本 → 路径候选 → 覆盖判定（三档：同路径 / 目录 / 文件名）
      const mentions = mentionPaths("改 packages/agents/src/agents/shengsheng/shengsheng.ts 与 docs/PITFALLS.md，另看 proc-tree.ts")
      expect(mentions).toContain("packages/agents/src/agents/shengsheng/shengsheng.ts")
      expect(mentions).toContain("docs/PITFALLS.md")
      expect(mentions).toContain("proc-tree.ts")
      expect(mentionCovers("docs/PITFALLS.md", "docs/PITFALLS.md")).toBe(true)
      expect(mentionCovers("packages/agents/src/agents/shengsheng", "packages/agents/src/agents/shengsheng/x.ts")).toBe(true)
      expect(mentionCovers("proc-tree.ts", "packages/agents/src/agents/shengsheng/proc-tree.ts")).toBe(true)
      expect(mentionCovers("docs/PITFALLS.md", "docs/BACKLOG.md")).toBe(false)

      // ③ 把事故那次提交**原样**喂进去：两份不在工单里的文件必须被单列出来
      const audit = scopeAudit(head.files, mentionPaths("仓级资源租约：改 shengsheng.ts 与 control-format.ts"))
      expect(audit.inScope.map((f) => f.path)).toEqual(["packages/agents/src/agents/shengsheng/shengsheng.ts"])
      expect(audit.outOfScope.map((f) => f.path)).toEqual([
        "packages/agents/src/agents/shengsheng/proc-tree.ts",
        "packages/agents/src/agents/shengsheng/shengsheng-retire-tree.test.ts",
        "assets/logo.bin",
      ])

      // ④ 追加式暂存命令：逐个列路径 + `--`（路径以 `-` 开头时没有它会被当成选项）
      expect(stagingCommands(["src/a.ts", "docs/b.md"])[0]).toBe("git add -- src/a.ts docs/b.md")
      expect(stagingCommands([])[0]).toContain("逐个列出")
      expect(stagingCommands(["src/a.ts"])[1]).toBe("git diff --cached --name-status")

      // ⑤ 纪律文本与命令同源：示例路径取工单里的、禁用形态逐条点名
      const line = commitProtocolLine(["src/a.ts"])
      expect(line).toContain("git add -- src/a.ts")
      expect(line).toContain("git diff --cached --name-status")
      for (const w of WIDE_STAGING_FORMS) expect(line).toContain(w.form)
      expect(line).toContain("??")

      // ⑥ 工作树解析：`??` 正是「别人的新文件」那一类
      const tree = parseStatusPorcelain("?? src/other.ts\n M src/a.ts\nR  old.ts -> new.ts\n")
      expect(tree.map((e) => e.code)).toEqual(["??", " M", "R "])
      expect(tree[2].path).toBe("new.ts")
      expect(untrackedOf(tree).map((e) => e.path)).toEqual(["src/other.ts"])
    })

    test("派工消息必带提交纪律（禁宽范围暂存 + 逐个列路径 + 提交前核对 + `??` 先问是谁的）", async () => {
      // 派工模板是 worker 唯一的世界：纪律不写进消息，worker 只会用它自己的习惯（`git add -A` 最常见）。
      // 断言用**字面文本**而非 `commitProtocolLine(...)` 生成值：两者同源时用它做断言会把差异抹平
      // （CONVENTIONS §7.2——回退纪律文本后测试仍绿）。
      const s = stub()
      await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
      await tool("dispatch").execute({ orders: [{ task: "改 src/a.ts", domain: "a", deliver: "src/a.ts 的补丁" }] }, s.ctx)
      const msg = s.sent[0].message
      expect(msg).toContain("提交纪律")
      expect(msg).toContain("git add -- src/a.ts")            // 示例路径取自本单（照抄即合规）
      expect(msg).toContain("git add -A")
      expect(msg).toContain("git commit -a")
      expect(msg).toContain("git diff --cached --name-status")
      expect(msg).toContain("这是谁的新文件")
      // 既有的「纪律」字段仍在（本条是**追加**，不是替换）
      expect(msg).toContain("纪律：")
      cleanup(s)
    })
  })
})


// ==================================================================
// `pgrep -f` 自匹配活锁：review 必须能自动认出来
//
// 现场两笔（都是实测，不是推理）：
//  · 会话 a8319257（别名 ctx-units-1）：等待循环里写 `pgrep -f "reverse_verify.py"`，而它自己的
//    `bash -c` 命令行里就含着这串 ⇒ pgrep 每次命中**这条 shell 自己** ⇒ 条件恒真 ⇒
//    11:34:51 → 11:46:19 空转 12 分钟、159 条消息、106K ctx、**一行实现都没写**。
//    它当时以为自己在「守规矩地等别人释放文件」。
//  · 同一窗口生生自己的命令行：`pgrep -c -f 'python3.*reverse_verify'` 也匹配到自己；
//    改成 `'python3.*reverse_verify[.]py'` 后正确返回 0——**破字串能自救**。
//
// 为什么必须自动判（而不是只写纪律）：两次都是「worker 以为自己在守规矩」，自律完全不管用；
// 而「它反复等同一个 gate、那 gate 现在没有进程」这件事只有**外部观测**看得见。
// 纪律与检测**同源**（`waitProtocolLine`）：派工消息教的是哪三条写法，检测就按哪三条判。
// ==================================================================
describe("pgrep 自匹配活锁（review 的自动检测）", () => {
  test("派工消息必带等待纪律（禁自匹配 + 三条正确写法 + 循环上限）", async () => {
    // 派工模板是 worker 唯一看得见的世界：纪律不写进消息，它只会用自己习惯的写法。
    // 断言用**字面文本**而非 `waitProtocolLine()` 生成值：两者同源时用它做断言会把差异抹平（§7.2）。
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "改 src/a.ts", domain: "a", deliver: "src/a.ts 的补丁" }] }, s.ctx)
    const msg = s.sent[0].message
    expect(msg).toContain("等待纪律")
    expect(msg).toContain("pgrep -f")
    expect(msg).toContain("条件恒真")
    expect(msg).toContain("kill -0")          // 写法①（有 pid 就别用 pgrep）
    expect(msg).toContain("pgrep -x")         // 写法②（按进程名）
    expect(msg).toContain("[.]")              // 写法③（破字串）
    expect(msg).toContain("$$")
    expect(msg).toContain("seq 1 60")         // 循环上限（否则错一次就是无限期空转）
    // 既有的「提交纪律」仍在（本条是**追加**，不是替换）
    expect(msg).toContain("提交纪律")
    cleanup(s)
  })
  /** 把一条「等待类命令」作为 sh 工具调用塞进 worker 会话的消息流（形态与真会话一致）。 */
  const withWaitCommand = (s: Stub, sid: string, commands: string[]) => {
    const sess = s.sessions.get(sid)
    if (!sess) throw new Error(`没有会话 ${sid}`)
    for (const command of commands)
      sess.messages.push({ role: "assistant", content: "", toolCalls: [{ id: "c1", name: "sh", arguments: { command } }] })
  }

  test("纯函数：自匹配判据 = pgrep 的正则语义（破字串能自救，正确写法不假报）", () => {
    // ① 错法：pattern 原样出现在自己的命令行里 ⇒ 正则能匹配到它自己
    const bad = `for i in $(seq 1 40); do if ! pgrep -f "reverse_verify.py" >/dev/null 2>&1; then echo done; break; fi; sleep 5; done`
    expect(isSelfMatch(bad, "reverse_verify.py")).toBe(true)
    expect(extractPatterns(bad)).toEqual(["reverse_verify.py"])
    // ② 对法（破字串）：命令行里是字面 `[.]`，正则要的是 `.` ⇒ 匹配不到自己
    const good = `for i in $(seq 1 40); do if ! pgrep -f "reverse_verify[.]py" >/dev/null 2>&1; then break; fi; sleep 5; done`
    expect(isSelfMatch(good, "reverse_verify[.]py")).toBe(false)
    // ③ 对法（有 pid）：根本不扫命令行
    expect(extractPatterns("while kill -0 12345 2>/dev/null; do sleep 1; done")).toEqual([])
    // ④ `-x` 不扫命令行 ⇒ 不当证据（也不该被抽成 pattern）
    expect(extractPatterns("for i in 1 2; do pgrep -x python3; done")).toEqual([])
    // ⑤ 聚合选项形态 `-c -f` / `-af` 都能抽出 pattern
    expect(extractPatterns("while pgrep -c -f 'python3.*reverse_verify' >/dev/null; do sleep 1; done")).toEqual(["python3.*reverse_verify"])
    expect(extractPatterns("until pgrep -af gate_marker; do sleep 1; done")).toEqual(["gate_marker"])
    // ⑥ 循环形态判据：单次 `pgrep x && echo` 不是等待，不构成活锁（不稀释警报）
    expect(isWaitCommand("pgrep -f x >/dev/null && echo ok")).toBe(false)
    expect(isWaitCommand(bad)).toBe(true)
  })

  test("进程表判据：破字串断开自身 ⇒ 判据不会自己复刻同一个坑", () => {
    // 反例现场（本仓 verify_guard.py 已记着这一型的另一面）：朴素的 `pgrep -af x.py` 会命中
    // **发起它的那条命令行**，于是「永远有人在跑」。这里用传入的进程表 + 破字串 tail 判据。
    const rows = [
      { pid: 100, ppid: 1, pgid: 100, startTime: 1, cmd: `bash -c for i in 1 2; do pgrep -f reverse_verify.py; done` },
      { pid: 200, ppid: 1, pgid: 200, startTime: 2, cmd: `python3 packages/agents/src/agents/shengsheng/reverse_verify.py` },
    ]
    // 真进程（200）在 → 不算「已无进程」
    expect(aliveMatching(rows, "reverse_verify.py").map((r) => r.pid)).toEqual([100, 200])
    // 排除自身后只剩真进程
    expect(aliveMatching(rows, "reverse_verify.py", [100]).map((r) => r.pid)).toEqual([200])
    // 破字串：正则语义不变（照样命中真进程），但命令行里的字面 pattern 不再精确重合
    expect(disjoinTail("reverse_verify.py")).toBe("reverse_verify.p[y]")
    expect(aliveMatching([rows[1]], "reverse_verify.py").length).toBe(1)
    // 元字符结尾不改语义：`x.*` 包成 `x.[*]` 就会变，故退回原文
    expect(disjoinTail("x.*")).toBe("x.*")
    expect(aliveMatching([], "anything")).toEqual([])
  })

  test("活锁惯性那一路单独可报：gate 无进程 + 反复等 ⇒ 报（不靠自匹配）", () => {
    // 为何单列一条：端到端那条里命令**自己**含 pattern，自匹配那一路就会命——
    // 「活锁惯性」那一路因此可以被完全回退而不变红（本单实测踩到：把 staleGates 退回永不产生，
    // 全套用例仍 96 pass）。两条判据必须各有一条**只靠它自己**会红的用例。
    //
    // 这里用**不自匹配**的形态：pattern 用变量间接给出（命令行里没有 pattern 原文），
    // 但它确实被反复等待——于是文本那一路沉默，只剩进程表那一路。
    // （变量形态下进程表判据看不到真进程：`$gate` 不是合法正则意义上的名字——因此另用一条
    //   字面 pattern 的命令验「gate 还活着不报」那一半。）
    const loop = `gate=sgs_stale_gate_2718.py; for i in 1 2 3 4; do if ! pgrep -f "$gate" >/dev/null 2>&1; then break; fi; sleep 1; done`
    const messages = Array.from({ length: 4 }, () => ({ role: "assistant", toolCalls: [{ name: "sh", arguments: { command: loop } }] }))
    const v = detectPgrepLock(messages, [])          // 空表 = 那个 gate 现在没有进程
    expect(v.block.selfMatches).toHaveLength(0)       // 确实不是自匹配（否则这条用例测的是另一路）
    expect(v.block.staleGates.map((g) => g.pattern)).toEqual(["$gate"])
    expect(v.locked).toBe(true)
    expect(v.advice).toContain("连续等待 4 次")
    expect(v.advice).toContain("没有任何进程匹配它")
    // 反向：只等 2 次（未达阈值 3）⇒ 不报（宁缺勿滥）
    const few = detectPgrepLock(messages.slice(0, 2), [])
    expect(few.block.staleGates).toHaveLength(0)
    // 反向：gate 还活着（进程表里有匹配进程）⇒ 不报。
    // pattern 写**破字串**（`direc[t]`）：命令行里只有字面 `[t]` ⇒ 不自匹配，
    // 而进程表里那条真命令行是真 `direct` ⇒ 正则命中 ⇒ 判「还没结束」。两边同时成立才算对。
    const aliveDirect = detectPgrepLock(
      Array.from({ length: 4 }, () => ({ role: "assistant", toolCalls: [{ name: "sh", arguments: { command: `for i in 1 2 3; do pgrep -f "sgs_alive_direc[t]_2718" >/dev/null; sleep 1; done` } }] })),
      [{ pid: 4243, ppid: 1, pgid: 4243, startTime: 1, cmd: "python3 sgs_alive_direct_2718" }],
    )
    expect(aliveDirect.block.selfMatches).toHaveLength(0)
    expect(aliveDirect.block.alive["sgs_alive_direc[t]_2718"]).toBe(1)
    expect(aliveDirect.block.staleGates).toHaveLength(0)
  })

  test("端到端：worker 连续 5 次等同一个 gate、而该 gate 现已无进程 ⇒ review 报「疑似 pgrep 自匹配活锁」+ 正确写法", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "改 src/a.ts", domain: "a" }] }, s.ctx)
    const sid = s.created[0].id
    s.running.clear()
    // 现场原样：同一条等待循环连做 5 次（gate 名是本次专用的，机器上不会有真进程）
    const loop = `for i in $(seq 1 40); do if ! pgrep -f "sgs_lock_gate_2718.py" >/dev/null 2>&1; then echo "gate finished"; break; fi; sleep 5; done`
    withWaitCommand(s, sid, Array.from({ length: 5 }, () => loop))

    const rv = await tool("collect").execute({ action: "review", alias: "a-1" }, s.ctx)
    expect(rv.output).toContain("疑似 `pgrep` 自匹配活锁")
    // 必须给出**正确写法**（只说「你错了」而不给替代，下一息还会撞）
    expect(rv.output).toContain("kill -0")
    expect(rv.output).toContain("pgrep -x")
    expect(rv.output).toContain("[.]")           // 破字串
    expect(rv.output).toContain("$$")             // 排除自身
    expect(rv.output).toContain("上限")           // 等待循环必须有上限
    // 结构化字段（控制面/前端按字段用，不靠抠文本）
    const data = rv.data as { pgrepLock?: { selfMatches: Array<{ pattern: string }>; probes: number } }
    expect(data.pgrepLock?.probes).toBe(5)
    expect(data.pgrepLock?.selfMatches[0]?.pattern).toBe("sgs_lock_gate_2718.py")
    cleanup(s)
  })

  test("反向用例：正确等待写法与「gate 还活着」都不得误报（否则警报疲劳淹没真信号）", async () => {
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "改 src/a.ts", domain: "a" }] }, s.ctx)
    const sid = s.created[0].id
    s.running.clear()

    // ① 正确写法（有 pid / 破字串 / -x）：连续等 5 次也不报
    withWaitCommand(s, sid, [
      `while kill -0 999999 2>/dev/null; do sleep 1; done`,
      `for i in 1 2 3; do if ! pgrep -f "sgs_gate_ok_2718[.]py" >/dev/null 2>&1; then break; fi; sleep 1; done`,
      `for i in 1 2 3; do pgrep -x python3 >/dev/null; sleep 1; done`,
    ])
    const good = await tool("collect").execute({ action: "review", alias: "a-1" }, s.ctx)
    expect(good.output).not.toContain("疑似 `pgrep` 自匹配活锁")
    // 但**要明说已核过**（「0 命中」与「没核」必须能分辨）
    expect(good.output).toContain("自匹配活锁：已核 1 条等待类命令")   // 只有 `-f` 那条算证据（`kill -0`/`-x` 不扫命令行）

    // ② gate 还活着：「gate 现已无进程」那一路不得出现。
    //
    // 用具名 gate 进程（命令行里**真带**这个串：`sh -c 'sleep 30 # sgs_alive_gate_2718'`——
    // 这正是现场的形状：gate 是某条含该名字的命令）。
    // ⚠ 用 `spawn` 而不是 `spawnSync`：`spawnSync` 会把后台子进程的 stdout 管道抱到它退出，
    //    那就变成了「测试真的等 30 秒」——与「自匹配活锁」恰好同一个形状（你等的那个东西是被你自己拖着的）。
    const gate = spawn("/bin/sh", ["-c", "sleep 30 # sgs_alive_gate_2718"], { stdio: "ignore" })
    const gatePid = gate.pid!
    expect(gatePid).toBeGreaterThan(0)
    withWaitCommand(s, sid, Array.from({ length: 4 }, () =>
      `for i in 1 2 3; do if ! pgrep -f "sgs_alive_gate_2718" >/dev/null 2>&1; then break; fi; sleep 1; done`))
    const busy = await tool("collect").execute({ action: "review", alias: "a-1" }, s.ctx)
    // 这条命令**自己**含该串 ⇒ 自匹配那一条会报（真的错法，应当报）；
    // 但「gate 现已无进程」那一路不得出现（进程表里确实有含该串的进程）
    const busyData = busy.data as { pgrepLock?: { staleGates: Array<{ pattern: string }>; alive: Record<string, number> } }
    expect(busyData.pgrepLock?.staleGates ?? []).toHaveLength(0)
    try {
      process.kill(gatePid, "SIGKILL")
    } catch {
      /* 已退出 */
    }
    cleanup(s)
  })

  test("拿不到进程表时如实报「本项未核」——不得当成「没有活锁」", async () => {
    // 「0 命中」与「读不到」必须是两个结论：把读不到报成通过，等于在最需要警报的环境里安静。
    const s = stub()
    await tool("state").execute({ action: "init", root: s.root, verify: "bun test" }, s.ctx)
    await tool("dispatch").execute({ orders: [{ task: "改 src/a.ts", domain: "a" }] }, s.ctx)
    const sid = s.created[0].id
    s.running.clear()
    // 只给一条「正确写法」的等待：自匹配那一路判不出来，于是只能靠进程表 ⇒ 进程表不可用时必须说未核
    withWaitCommand(s, sid, [`for i in $(seq 1 9); do pgrep -f "sgs_unverified_gate_2718[.]py" >/dev/null; sleep 1; done`])
    const rv = await tool("collect").execute({ action: "review", alias: "a-1" }, s.ctx)
    // 本机有 /proc，所以这里应当是「已核」；断言它对**两种**环境都给出可分辨的结论：
    const out = rv.output
    const checked = out.includes("自匹配活锁：已核") || out.includes("自匹配活锁本项未核")
    expect(checked).toBe(true)
    expect(out).not.toContain("疑似 `pgrep` 自匹配活锁")
    cleanup(s)
  })

  test("进程表不可读：自匹配仍能纯文本判出，但「gate 现已无进程」那一路必须说「本项未核」", async () => {
    // “活锁惯性”那一路的判据在**进程表**上；拿不到表就不能当它不存在。
    // 纯函数据此分两半：`locked`（文本）与 `checked`（能否核到「现有无进程」）——
    // 把 checked 写成恒真，就会在无 /proc 的机器上安安静静地一声不响。
    const loop = `for i in 1 2 3 4; do if ! pgrep -f "sgs_noproc_gate_2718.py" >/dev/null 2>&1; then break; fi; sleep 1; done`
    const messages = [{ role: "assistant", toolCalls: [{ name: "sh", arguments: { command: loop } }] }]
    // ① 自匹配那一路：纯文本即可判（与进程表无关）——真的错法，**即便没有进程表也要报**
    const withTable = detectPgrepLock(messages, [])
    expect(withTable.locked).toBe(true)          // 自匹配 = 恒真条件；空表也能报
    expect(withTable.checked).toBe(true)         // 两路都核了（活锁惯性：空表 ⇒ gate 已无进程）
    expect(withTable.block.selfMatches).toHaveLength(1)
    expect(withTable.block.probes).toBe(1)
    // ② 显式传 tableError：自匹配仍报（纯文本），但必须明说「活锁惯性那一路未核」
    const err = detectPgrepLock(messages, [], { tableError: "进程表不可读（/proc）：无此目录" })
    expect(err.locked).toBe(true)
    expect(err.checked).toBe(false)              // ← 不得假称“核过”
    expect(err.advice).toContain("本项未核")
    expect(err.advice).toContain("进程表不可读")
  })
})
