/**
 * 生生（shengsheng）——项目驻留自主演进的调度中枢工具集。
 *
 * 三个工具按一息的相位分件，互不重叠、不重复既有能力：
 * - `state`：复命与收息（读态 / 固化坐标 / 择种入队 / 落断点 / 心跳任务管理）；
 * - `dispatch`：布雨（复用 idle worker 或新建会话，按模板装配自包含派工消息并注入触发）；
 * - `collect`：收藏与退役（深观消息尾 / 入台账幂等 / 退役会话）。
 *
 * 状态固定落在 `<项目根>/.evolve/`：`control.md`（调度状态）与 `ledger.md`（演进台账）由本模块
 * 结构化读写——字段结构、幂等去重、池与在途对账由代码保证，模型不再手写 markdown。
 *
 * 能力来源一律经契约上下文：会话控制面（`ctx.sessions`）与任务面（`ctx.tasks`）由引擎注入，
 * 路径读写经 `ctx.readFile`/`ctx.writeFile`（服务模式下受沙箱约束，越界如实报错，不另开通道）。
 */
import type { SubAgentDef, Tool, ToolContext } from "@gebai/sdk"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./shengsheng.md" with { type: "text" }

function schema(properties: Record<string, unknown>, required: string[] = []) {
  return { type: "object", properties, required } as Tool["parameters"]
}

// ---------------------------------------------------------------------------
// 契约内的服务面（引擎注入，子Agent 侧按最小可用面收窄，不 import 引擎模块）
// ---------------------------------------------------------------------------

interface SessionsLike {
  store: {
    createSession(userId: string, name?: string): Promise<{ id: string; name?: string }>
    load(id: string, user?: string): Promise<{ id: string; name?: string; messages?: unknown[]; ctxTokens?: number } | null>
    delete(id: string, userId: string): Promise<void>
    setEnv(sessionId: string, userId: string, vars: Record<string, string | null>): Promise<Record<string, string>>
  }
  engine: {
    isRunning(sessionId: string): boolean
    cancel(sessionId: string): void
    runtimeOf(user: string): Promise<Record<string, { startedAt: number; pending: unknown[]; toolCalls?: number }>>
  }
  run: (sessionId: string, user: string, prompt: string, opts?: Record<string, unknown>) => Promise<void>
}

interface TasksLike {
  add(input: Record<string, unknown>, originSessionId?: string): Promise<Record<string, unknown>>
  list(): Promise<Array<Record<string, unknown>>>
  update(id: string, patch: Record<string, unknown>): Promise<Record<string, unknown> | null>
  notify(input: { text: string; title?: string }, id?: string): Promise<{ taskId: string; delivered: number; errors: string[] }>
}

const sessionsOf = (ctx: ToolContext): SessionsLike | undefined => (ctx as unknown as { sessions?: SessionsLike }).sessions
const tasksOf = (ctx: ToolContext): TasksLike | undefined => (ctx as unknown as { tasks?: TasksLike }).tasks

// ---------------------------------------------------------------------------
// 状态模型与 `.evolve` 结构化读写
// ---------------------------------------------------------------------------

/** 项目坐标（control.md 首部，一次写入终身受用）。 */
export interface Coord {
  root?: string
  verify?: string
  seedDoc?: string
  rules?: string
  preload?: string
}

/** worker 池条目（复用单位：同域下一单直接续派，记忆延续）。 */
export interface Worker {
  alias: string
  sessionId: string
  domain: string
  /** 最近一次派工的任务摘要。 */
  task: string
  /** 首次派发时间（复用者保留：体现“这一位工人的资历”）。 */
  dispatchedAt: number
  /** 最近一次派工的轮次号（台账幂等键的稳定来源）。 */
  round: number
  status: "running" | "idle"
}

/** 在途（已布未收）。 */
export interface Inflight {
  alias: string
  direction: string
  deliverable: string
  dispatchedAt: number
  /** 本次派工的轮次号（与 Worker.round 同源写入；collect 据此去重）。 */
  round: number
}

/** 种子（值得生的方向）。 */
export interface Seed {
  priority: string
  direction: string
  source: string
  constraint: string
}

/** 收藏待办（worker 已完成但台账未更新）。 */
export interface Pending {
  alias: string
  summary: string
  at: number
}

export interface EvolveState {
  coord: Coord
  breath: number
  /** 派工轮次计数器（单调递增，跨息持续）：每派一单 +1，作为台账幂等键的稳定来源。
   *  不用时间戳的理由：复用时 worker 与 inflight 的时间戳不同源，键会漂移。 */
  seq: number
  workers: Worker[]
  inflight: Inflight[]
  seeds: Seed[]
  pending: Pending[]
  note?: string
}

const COORD_LABELS: Array<[keyof Coord, string]> = [
  ["root", "项目根"],
  ["verify", "验证命令"],
  ["seedDoc", "种子库"],
  ["rules", "硬约束文档"],
  ["preload", "worker 预载提示"],
]

const EMPTY: EvolveState = { coord: {}, breath: 0, seq: 0, workers: [], inflight: [], seeds: [], pending: [] }

/** `.evolve` 目录与两个状态文件的位置（给定项目根）。 */
function evolvePaths(root: string): { dir: string; control: string; ledger: string } {
  const dir = `${root.replace(/[\\/]+$/, "")}/.evolve`
  return { dir, control: `${dir}/control.md`, ledger: `${dir}/ledger.md` }
}

/** 表格行拆分（`| a | b |` → ["a","b"]）。 */
function cells(line: string): string[] {
  return line
    .trim()
    .replace(/^\|/, "")
    .replace(/\|$/, "")
    .split("|")
    .map((s) => s.trim())
}

const isSeparator = (line: string): boolean => /^\|[\s:|-]+\|$/.test(line.trim())

/** 取 `## <标题>` 段落内的表格数据行（跳过表头与分隔行；`（空）` 视为零条目）。 */
function tableRows(md: string, heading: string): string[][] {
  const lines = md.split(/\r?\n/)
  const start = lines.findIndex((l) => l.trim() === `## ${heading}`)
  if (start < 0) return []
  const rows: string[][] = []
  for (let i = start + 1; i < lines.length; i++) {
    const line = lines[i]
    if (/^##\s/.test(line.trim())) break
    if (!line.trim().startsWith("|")) continue
    if (isSeparator(line)) continue
    const c = cells(line)
    if (c[0] === "别名" || c[0] === "优先级" || c[0] === "键") continue // 表头
    if (c[0] === "（空）") continue
    rows.push(c)
  }
  return rows
}

/** 取 `## <标题>` 段落下的自由文本（非表格行）。 */
function sectionText(md: string, heading: string): string {
  const lines = md.split(/\r?\n/)
  const start = lines.findIndex((l) => l.trim() === `## ${heading}`)
  if (start < 0) return ""
  const out: string[] = []
  for (let i = start + 1; i < lines.length; i++) {
    if (/^##\s/.test(lines[i].trim())) break
    out.push(lines[i])
  }
  return out.join("\n").trim()
}

/** 解析 control.md（缺失/损坏时返回空态——损坏不阻断，下一次写入按解析结果重建结构）。 */
export function parseControl(md: string | null): EvolveState {
  if (!md) return { ...EMPTY, coord: {}, workers: [], inflight: [], seeds: [], pending: [] }
  const coord: Coord = {}
  const coordText = sectionText(md, "项目坐标")
  for (const line of coordText.split(/\r?\n/)) {
    const m = /^-\s*([^：:]+)[：:]\s*(.*)$/.exec(line.trim())
    if (!m) continue
    const hit = COORD_LABELS.find(([, label]) => label === m[1].trim())
    if (hit && m[2].trim()) coord[hit[0]] = m[2].trim()
  }
  const breathMatch = /-\s*呼吸[：:]\s*(\d+)/.exec(sectionText(md, "计数"))
  const seqMatch = /-\s*派工序号[：:]\s*(\d+)/.exec(sectionText(md, "计数"))
  const state: EvolveState = {
    coord,
    breath: breathMatch ? Number(breathMatch[1]) : 0,
    seq: seqMatch ? Number(seqMatch[1]) : 0,
    workers: tableRows(md, "worker 池")
      .map((c): Worker => ({
        alias: c[0] ?? "",
        sessionId: c[1] ?? "",
        domain: c[2] ?? "",
        task: c[3] ?? "",
        dispatchedAt: Number(c[4]) || 0,
        round: Number(c[5]) || 0,
        status: c[6] === "idle" ? "idle" : "running",
      }))
      .filter((w) => w.alias),
    inflight: tableRows(md, "在途")
      .map((c) => ({ alias: c[0] ?? "", direction: c[1] ?? "", deliverable: c[2] ?? "", dispatchedAt: Number(c[3]) || 0, round: Number(c[4]) || 0 }))
      .filter((i) => i.alias),
    seeds: tableRows(md, "种子队列").map((c) => ({ priority: c[0] ?? "", direction: c[1] ?? "", source: c[2] ?? "", constraint: c[3] ?? "" })).filter((s) => s.direction),
    pending: tableRows(md, "收藏待办").map((c) => ({ alias: c[0] ?? "", summary: c[1] ?? "", at: Number(c[2]) || 0 })).filter((p) => p.alias),
  }
  const note = sectionText(md, "断点")
  if (note) state.note = note
  return state
}

/** 表格渲染（空集合写 `（空）` 占位，保留节结构便于人工查看）。 */
function renderTable(header: string[], rows: string[][]): string {
  if (!rows.length) return "（空）"
  const head = `| ${header.join(" | ")} |`
  const sep = `| ${header.map(() => "---").join(" | ")} |`
  return [head, sep, ...rows.map((r) => `| ${r.map((c) => c.replace(/\|/g, "\\|").replace(/\r?\n/g, " ")).join(" | ")} |`)].join("\n")
}

/** 序列化 control.md（节结构确定性重写，内容全部来自解析后的状态）。 */
export function renderControl(state: EvolveState): string {
  const coordLines = COORD_LABELS.filter(([k]) => state.coord[k]).map(([k, label]) => `- ${label}：${state.coord[k]}`)
  const parts = [
    "> 生生调度状态（`shengsheng_state` 托管，勿手改）。",
    "",
    "## 项目坐标",
    coordLines.length ? coordLines.join("\n") : "（未固化）",
    "",
    "## 计数",
    `- 呼吸：${state.breath}`,
    `- 派工序号：${state.seq}`,
    "",
    "## worker 池",
    renderTable(["别名", "会话", "领域", "任务", "派发时间", "轮次", "状态"], state.workers.map((w) => [w.alias, w.sessionId, w.domain, w.task, String(w.dispatchedAt), String(w.round), w.status])),
    "",
    "## 在途",
    renderTable(["别名", "方向", "预期产出", "派发时间", "轮次"], state.inflight.map((i) => [i.alias, i.direction, i.deliverable, String(i.dispatchedAt), String(i.round)])),
    "",
    "## 种子队列",
    renderTable(["优先级", "方向", "来源", "约束"], state.seeds.map((s) => [s.priority, s.direction, s.source, s.constraint])),
    "",
    "## 收藏待办",
    renderTable(["别名", "摘要", "时间"], state.pending.map((p) => [p.alias, p.summary, String(p.at)])),
    "",
    "## 断点",
    state.note?.trim() ? state.note.trim() : "（无）",
    "",
  ]
  return parts.join("\n")
}
// ---------------------------------------------------------------------------
// 项目根解析
// ---------------------------------------------------------------------------

/** 进程内会话级项目根（`state action=init` 写入）：会话 env 由引擎在 run 开始时解析，
 *  同一息内 init 之后写的 env 本息读不到——布雨须紧跟 init，故另置此进程内记忆（会话级，
 *  服务重启即空；环境变量 `SHENGSHENG_ROOT` 是跨重启的那条路）。 */
const rootMemo = new Map<string, string>()

/** 项目根解析：显式参数 > 会话 env / 本进程 init 记忆 > 工作目录向上探测既有 `.evolve/control.md`。 */
async function resolveRoot(ctx: ToolContext, argRoot?: string): Promise<{ root?: string; via: string; error?: string }> {
  const clean = (p: string): string => p.trim().replace(/[\\/]+$/, "")
  const explicit = (argRoot ?? "").trim()
  if (explicit) return { root: clean(explicit), via: "参数" }
  const fromEnv = (ctx.env?.SHENGSHENG_ROOT ?? "").trim()
  if (fromEnv) return { root: clean(fromEnv), via: "环境变量 SHENGSHENG_ROOT" }
  const memo = rootMemo.get(ctx.sessionId)
  if (memo) return { root: memo, via: "本会话 init 记忆" }
  let dir = (ctx.sessionWorkdir ?? ctx.workdir ?? "").replace(/[\\/]+$/, "")
  for (let i = 0; i < 4 && dir; i++) {
    const r = await readAbs(ctx, `${dir}/.evolve/control.md`)
    if (r.error) return { via: "工作目录探测", error: r.error }
    if (r.text) return { root: dir, via: "工作目录探测" }
    const parent = dir.replace(/[\\/][^\\/]+$/, "")
    if (parent === dir) break
    dir = parent
  }
  return { via: "未解析" }
}

/** 路径闸门：绝对路径经 `ctx.resolvePath` 解析（服务模式下越界抛错即拒绝）。 */
function gate(ctx: ToolContext, abs: string): { path?: string; error?: string } {
  try {
    return { path: ctx.resolvePath(abs) }
  } catch (err) {
    return { error: `路径不可达（沙箱约束）：${abs}——${describeError(err)}` }
  }
}

export function describeError(err: unknown): string {
  const e = err as { message?: string; code?: string }
  return String(e?.message || e?.code || err)
}

/** 读文件：`{ text: null }` = 不存在；`{ error }` = 读失败（权限/约束等，非缺失）。 */
async function readAbs(ctx: ToolContext, abs: string): Promise<{ text?: string | null; error?: string }> {
  const g = gate(ctx, abs)
  if (g.error) return { error: g.error }
  try {
    return { text: await ctx.readFile(g.path!) }
  } catch (err) {
    const code = (err as { code?: string })?.code
    if (code === "ENOENT" || code === "ENOTDIR") return { text: null }
    return { error: `读取失败：${g.path}——${describeError(err)}` }
  }
}

async function writeAbs(ctx: ToolContext, abs: string, content: string): Promise<{ error?: string }> {
  const g = gate(ctx, abs)
  if (g.error) return { error: g.error }
  try {
    await ctx.writeFile(g.path!, content)
    return {}
  } catch (err) {
    return { error: `写入失败：${g.path}——${describeError(err)}` }
  }
}

// ---------------------------------------------------------------------------
// 状态装载（gated）
// ---------------------------------------------------------------------------

interface Loaded {
  root?: string
  via: string
  state: EvolveState
  initialized: boolean
  error?: string
}

async function loadState(ctx: ToolContext, argRoot?: string): Promise<Loaded> {
  const { root, via, error } = await resolveRoot(ctx, argRoot)
  if (error) return { via, state: { ...EMPTY, coord: {}, workers: [], inflight: [], seeds: [], pending: [] }, initialized: false, error }
  if (!root) return { via, state: { ...EMPTY, coord: {}, workers: [], inflight: [], seeds: [], pending: [] }, initialized: false }
  const r = await readAbs(ctx, evolvePaths(root).control)
  if (r.error) return { root, via, state: parseControl(null), initialized: false, error: r.error }
  return { root, via, state: parseControl(r.text ?? null), initialized: r.text !== null }
}

const freshState = (): EvolveState => ({ coord: {}, breath: 0, seq: 0, workers: [], inflight: [], seeds: [], pending: [] })

async function saveState(ctx: ToolContext, root: string, state: EvolveState): Promise<{ error?: string }> {
  return writeAbs(ctx, evolvePaths(root).control, renderControl(state))
}

/**
 * 台账追加（含记录键：同一轮只入一次，重放幂等）。
 *
 * 读改写序列在本进程内按路径串行（同一次心跳里 collect 与 state.note 并发调用时不互相踩），
 * 且**用文件本身做幂等源**（每次读到的都是最新内容）——避免「先判定未入账、后双写」。
 * 台账是控制平面唯一的成果记忆，宁可慢一点也不能写坏。
 */
const ledgerLocks = new Map<string, Promise<unknown>>()

function withLedgerLock<T>(path: string, fn: () => Promise<T>): Promise<T> {
  const prev = ledgerLocks.get(path) ?? Promise.resolve()
  const next = prev.then(fn, fn)
  ledgerLocks.set(path, next.catch(() => {}))
  return next
}

async function appendLedger(ctx: ToolContext, root: string, key: string, block: string): Promise<{ path: string; appended: boolean; error?: string }> {
  const path = evolvePaths(root).ledger
  return withLedgerLock(path, async () => {
    const r = await readAbs(ctx, path)
    if (r.error) return { path, appended: false, error: r.error }
    const md = r.text ?? "# 演进台账\n\n（生生逐轮追加——每轮一次 worker 交付，记录键保证幂等。）\n"
    if (md.includes(key)) return { path, appended: false }
    const w = await writeAbs(ctx, path, `${md.replace(/\s*$/, "")}\n\n${block}\n`)
    if (w.error) return { path, appended: false, error: w.error }
    return { path, appended: true }
  })
}

// ---------------------------------------------------------------------------
// 会话观察（worker 的实时态与消息尾）
// ---------------------------------------------------------------------------

interface Live {
  exists: boolean
  running: boolean
  startedAt?: number
  minutes?: number
  pending?: string[]
  toolCalls?: number
  messages?: number
  ctxTokens?: number
  last?: string
}

/** 消息文本抽取（内容块取 text 段，压平空白）。 */
function messageText(m: unknown, max = 400): string {
  const content = (m as { content?: unknown })?.content
  let text = ""
  if (typeof content === "string") text = content
  else if (Array.isArray(content)) text = content.map((b) => (b && typeof b === "object" && "text" in b ? String((b as { text?: unknown }).text ?? "") : "")).filter(Boolean).join(" ")
  text = text.replace(/\s+/g, " ").trim()
  return text.length > max ? `${text.slice(0, max)}…` : text
}

/** 会话观察（一次 runtimeOf 摊到全部 worker + 逐会话读尾部）。 */
async function observe(ctx: ToolContext, svc: SessionsLike, ids: string[]): Promise<Record<string, Live>> {
  const out: Record<string, Live> = {}
  let runtime: Record<string, { startedAt: number; pending: unknown[]; toolCalls?: number }> = {}
  try {
    runtime = await svc.engine.runtimeOf(ctx.user)
  } catch {
    runtime = {}
  }
  const now = Date.now()
  for (const id of new Set(ids.filter(Boolean))) {
    const live: Live = { exists: false, running: svc.engine.isRunning(id) }
    const rt = runtime[id]
    if (rt) {
      live.startedAt = rt.startedAt
      live.minutes = Math.max(0, Math.round((now - rt.startedAt) / 60000))
      live.toolCalls = rt.toolCalls
      live.pending = (rt.pending ?? []).map((p) => {
        const o = (p ?? {}) as Record<string, unknown>
        const type = String(o.type ?? "?")
        if (type === "approval") return `审批 ${String(o.tool ?? "")}`
        if (type === "choice") return `选择 ${String(o.prompt ?? "").slice(0, 40)}`
        if (type === "env") return `填值 ${String(o.name ?? "")}`
        return type
      })
    }
    try {
      const s = await svc.store.load(id, ctx.user)
      if (s) {
        live.exists = true
        const msgs = (s.messages ?? []).filter((m) => (m as { role?: string })?.role !== "system")
        live.messages = msgs.length
        live.ctxTokens = s.ctxTokens
        const lastText = messageText(msgs[msgs.length - 1])
        if (lastText) live.last = lastText
      }
    } catch {
      live.exists = false
    }
    out[id] = live
  }
  return out
}

function liveLine(alias: string, w: Worker | undefined, live: Live | undefined): string {
  const w2 = w
  const bits: string[] = [alias]
  if (w2) bits.push(`${w2.domain}｜${w2.task}`)
  if (!live || !live.exists) bits.push("**会话不存在**（外部删除/服务重启——清出池，在途回种子队列）")
  else if (live.running) bits.push(`运行中 ${live.minutes ?? "?"} 分钟` + (live.toolCalls ? `｜工具 ${live.toolCalls}` : "") + (live.pending?.length ? `｜待决：${live.pending.join("、")}` : ""))
  else bits.push("空闲" + (live.ctxTokens ? `｜ctx ${live.ctxTokens}` : ""))
  return `- ${bits.join("｜")}`
}

/** 派工/继承：装配自包含派工消息（任务/上下文/交付/验证/纪律/预载——模板由代码保证不漏项）。 */
function renderOrderPrompt(order: Order, coord: Coord, injectedEnv: string[]): string {
  const contextLine = (order.context ?? coord.seedDoc ?? "（无——自行摸底定位）").trim()
  const lines = [
    `任务：${order.task.trim()}`,
    `上下文：${contextLine}`,
    `交付：${(order.deliver ?? "具体产出物与落盘路径（代码+回归测试 / 分析方案 / 基线数据）").trim()}`,
    `验证：${(order.verify ?? coord.verify ?? "（未提供验证命令）——无法运行时说明原因并给出可复现的验证步骤").trim()}`,
    `纪律：${(order.rules ?? coord.rules ?? "原子提交不 push；设计层/公共接口变更只分析不落地").trim()}`,
  ]
  const preload = (order.preload ?? coord.preload ?? "").trim()
  if (preload) lines.push(`预载：${preload}`)
  if (preload && preload === contextLine) {
    // 同一路径既是「上下文」又是「预载」：前者是「去读它」，后者是「把它当子Agent 装载」——语义不同，
    // 不点名会派出去一个语义冲突的消息（worker 无法判断到底该读还是该装）。
    lines.push(`（注：「预载」与「上下文」指向同一路径——前者是子Agent 名清单（按需 agent_load），后者是资料文件（直接 read）；本条请分别理解。）`)
  }
  if (injectedEnv.length) lines.push(`环境：本会话已注入 ${injectedEnv.join("、")}（项目坐标）`)
  lines.push("本次为无人值守执行（无交互通道）：不要依赖询问用户与前端渲染（ask、show 的页面预览、页面捕获不可用）；需审批工具已由本次派工授权自动通过——不需要为工具审批等待或重试。")
  lines.push("单任务完成即止——输出最终报告后结束，不要循环找新事。报告须含：交付／验证证据／未竟事项／建议下一条。")
  return lines.join("\n")
}

/** 派工预算：worker 会话专属 env（服务重启即空，可重复注入）。 */
function workerEnv(root: string, coord: Coord): Record<string, string> {
  const vars: Record<string, string> = { SHENGSHENG_ROOT: root }
  if (coord.verify) vars.SHENGSHENG_VERIFY = coord.verify
  if (coord.seedDoc) vars.SHENGSHENG_SEED_DOC = coord.seedDoc
  if (coord.rules) vars.SHENGSHENG_RULES = coord.rules
  return vars
}

// ---------------------------------------------------------------------------
// 工单与派工消息
// ---------------------------------------------------------------------------

export interface Order {
  alias?: string
  session_id?: string
  /** 强制新建会话（不复用池中 idle worker）。 */
  fresh?: boolean
  domain?: string
  task: string
  context?: string
  deliver?: string
  verify?: string
  rules?: string
  preload?: string
}

/** worker 会话上下文水位上限（超此值的 idle worker 不再复用——上下文过肥，续派质量下降）。 */
export const WORKER_CTX_LIMIT = 150_000
/** 活动 worker 上限（running + idle）。 */
export const ACTIVE_WORKER_LIMIT = 3
/** 单批派工上限。 */
export const DISPATCH_LIMIT = 8
/** 同一 worker 会话承接单数达此值即提示换新（上下文渐胖，继续复用质量下降）。 */
export const REUSE_GENERATION_HINT = 8

const sanitizeAlias = (s: string): string =>
  s
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_-]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 24)

/** 未命名工单的别名派生：`{领域}-{序号}`（与既有池不冲突）。 */
function deriveAlias(domain: string | undefined, taken: Set<string>): string {
  const base = sanitizeAlias(domain ?? "") || "w"
  for (let i = 1; i < 100; i++) {
    const alias = `${base}-${i}`
    if (!taken.has(alias)) return alias
  }
  return `w-${Date.now().toString(36)}`
}

function normalizeOrders(raw: unknown): Order[] {
  const list = Array.isArray(raw) ? raw : raw && typeof raw === "object" ? [raw] : []
  const out: Order[] = []
  for (const item of list) {
    if (!item || typeof item !== "object") continue
    const o = item as Record<string, unknown>
    const task = String(o.task ?? "").trim()
    if (!task) continue
    out.push({
      task,
      ...(o.alias != null ? { alias: String(o.alias).trim() } : {}),
      ...(o.session_id != null ? { session_id: String(o.session_id).trim() } : {}),
      ...(o.fresh === true ? { fresh: true } : {}),
      ...(o.domain != null ? { domain: String(o.domain).trim() } : {}),
      ...(o.context != null ? { context: String(o.context) } : {}),
      ...(o.deliver != null ? { deliver: String(o.deliver) } : {}),
      ...(o.verify != null ? { verify: String(o.verify) } : {}),
      ...(o.rules != null ? { rules: String(o.rules) } : {}),
      ...(o.preload != null ? { preload: String(o.preload) } : {}),
    })
  }
  return out
}

// ---------------------------------------------------------------------------
// 工具一：state（复命 / 择种 / 收息 / 心跳任务管理）
// ---------------------------------------------------------------------------

const stateTool: Tool = {
  name: "state",
  description:
    "生生的读态与收息工具（一息入口）。无参=复命：返回项目坐标、呼吸计数、worker 池、在途、种子队列，以及各 worker 的实时态（运行中/空闲/起始时间/待决交互）与待收藏线索。action=init 从唤醒消息固化项目坐标（初生第一动作）；seed_add/seed_done 择种入队与销账；note 落断点并推进呼吸（每息收尾）；task 管理心跳驱动任务（op=ensure 幂等确保定时任务存在并启用；op=pause 记暂停+通知用户+停用任务；op=resume 重新启用；op=status 只看状态）。control.md（调度状态）由本工具托管——勿用 read/write 手工编辑。",
  parameters: schema(
    {
      action: { enum: ["read", "init", "seed_add", "seed_done", "note", "task"], description: "动作（缺省 read=读态）" },
      root: { type: "string", description: "项目根绝对路径（缺省：会话 env SHENGSHENG_ROOT > 工作目录向上探测既有 .evolve/control.md）" },
      verify: { type: "string", description: "init：worker 三重门第一门——验证命令原样（派工消息原样传递，你永不执行）" },
      seed_doc: { type: "string", description: "init：种子库文档路径（待做方向清单，可选）" },
      rules: { type: "string", description: "init：硬约束文档路径（worker 必守的项目规矩，可选）" },
      preload: { type: "string", description: "init：派工消息末尾附上的工具装载提示（可选）" },
      direction: { type: "string", description: "seed_add：方向描述（必填）" },
      priority: { type: "string", description: "seed_add：优先级（如 P0/P1/P2，缺省「待定」）" },
      source: { type: "string", description: "seed_add：来源（回收报告 / 台账 / 种子库 / 自主探索）" },
      constraint: { type: "string", description: "seed_add：约束（环境受限 / 只分析 / 可动手 / 需预载什么工具）" },
      match: { type: "string", description: "seed_done：方向匹配串（唯一包含即销账；不唯一时返回候选）" },
      note: { type: "string", description: "note：断点内容（观复到哪 / 收藏剩谁 / 布雨缺口），写「（无）」清除" },
      op: { enum: ["ensure", "pause", "resume", "status"], description: "task：心跳任务动作（ensure=幂等确保存在且启用；pause=暂停；resume=恢复；status=只看状态）" },
      schedule: { type: "string", description: "task op=ensure：心跳表达式（缺省 @every 3m）" },
      reason: { type: "string", description: "task op=pause：暂停原因（写入台账并通知用户）" },
      task_id: { type: "string", description: "task：直接指定心跳任务 ID（缺省按任务名 SHENGSHENG_TASK_NAME 反查）" },
    },
    [],
  ),
  requiresApproval: (args) => {
    const action = String(args.action ?? "read")
    if (action !== "task") return false
    const op = String(args.op ?? "status")
    return op === "ensure" || op === "pause" || op === "resume"
  },
  async execute(args, ctx) {
    const action = String(args.action ?? "read").trim() || "read"
    const rootArg = args.root != null ? String(args.root) : undefined
    // 会话控制面（init 固坐标、观复、派工、退役共用）
    const svc = sessionsOf(ctx)

    // ---- init：固化项目坐标（control.md 首部，一次写入终身受用）----
    if (action === "init") {
      if (!rootArg?.trim()) return { output: "action=init 需要 root（项目根绝对路径——从唤醒消息提取）。" }
      const loaded = await loadState(ctx, rootArg)
      if (loaded.error) return { output: loaded.error }
      const state = loaded.initialized ? loaded.state : freshState()
      const coord: Coord = { ...state.coord, root: rootArg.trim().replace(/[\\/]+$/, "") }
      for (const [k] of COORD_LABELS) {
        if (k === "root") continue
        const v = args[k === "seedDoc" ? "seed_doc" : k]
        if (v != null && String(v).trim()) coord[k] = String(v).trim()
      }
      state.coord = coord
      const w = await saveState(ctx, coord.root!, state)
      if (w.error) return { output: w.error }
      // 本进程记忆（同一息后续调用立即可解析）+ 会话 env（跨轮次/跨息，粘性会话复用）
      rootMemo.set(ctx.sessionId, coord.root!)
      let envNote = ""
      if (svc) {
        try {
          await svc.store.setEnv(ctx.sessionId, ctx.user, { SHENGSHENG_ROOT: coord.root! })
          envNote = `，已写入会话环境变量 SHENGSHENG_ROOT（本息起立即可解析）`
        } catch (err) {
          envNote = `（会话环境变量写入失败：${describeError(err)}——本进程内仍可解析）`
        }
      } else envNote = "（会话控制面不可用——本进程内仍可解析，会话重建后需重传 root）"
      const lines = COORD_LABELS.filter(([k]) => coord[k]).map(([k, label]) => `- ${label}：${coord[k]}`)
      return {
        output: [
          `项目坐标已固化（${loaded.via}）${envNote}：${evolvePaths(coord.root!).control}`,
          ...lines,
          loaded.initialized
            ? `沿用既有状态：池 ${state.workers.length}、在途 ${state.inflight.length}、种子 ${state.seeds.length}。`
            : "初生：池/在途/种子均空——接着用 action=seed_add 播下第一批种子。",
        ].join("\n"),
        data: { root: coord.root, coord, initial: !loaded.initialized },
      }
    }

    // ---- 其余动作都需要既有状态 ----
    // 项目根解析次序：显式 root 参数 > 会话 env（init 已固化）> 工作目录向上探测。
    const loaded = await loadState(ctx, rootArg)
    if (loaded.error) return { output: loaded.error }
    if (!loaded.root || !loaded.initialized) {
      return {
        output: "未解析出项目根：请在首次调用时传 root=<项目根绝对路径>（并把坐标以 action=init 固化），或设置会话环境变量 SHENGSHENG_ROOT。"
      }
    }
    const { root, state } = loaded

    // ---- seed_add / seed_done：择种 ----
    if (action === "seed_add") {
      const direction = String(args.direction ?? "").trim()
      if (!direction) return { output: "seed_add 需要 direction（方向描述）。" }
      const seed: Seed = {
        priority: String(args.priority ?? "待定").trim() || "待定",
        direction,
        source: String(args.source ?? "自主探索").trim() || "自主探索",
        constraint: String(args.constraint ?? "").trim(),
      }
      const dup = state.seeds.find((s) => s.direction === seed.direction)
      if (dup) return { output: `种子已存在（未重复入队）：[${dup.priority}] ${dup.direction}` }
      state.seeds.push(seed)
      const w = await saveState(ctx, root, state)
      if (w.error) return { output: w.error }
      return { output: `种子入队：[${seed.priority}] ${seed.direction}（来源 ${seed.source}${seed.constraint ? `；约束 ${seed.constraint}` : ""}）——队列现 ${state.seeds.length} 条。`, data: { seed, seeds: state.seeds } }
    }

    if (action === "seed_done") {
      const match = String(args.match ?? "").trim()
      if (!match) return { output: "seed_done 需要 match（方向匹配串）。" }
      const hits = state.seeds.filter((s) => s.direction.includes(match))
      if (!hits.length) return { output: `种子队列无匹配「${match}」（现 ${state.seeds.length} 条）。` }
      if (hits.length > 1) return { output: `匹配不唯一（${hits.length} 条）——请用更具体的 match：\n${hits.map((s) => `- [${s.priority}] ${s.direction}`).join("\n")}` }
      state.seeds = state.seeds.filter((s) => s !== hits[0])
      const w = await saveState(ctx, root, state)
      if (w.error) return { output: w.error }
      return { output: `种子销账（已完成）：[${hits[0].priority}] ${hits[0].direction}——队列剩 ${state.seeds.length} 条。`, data: { removed: hits[0], seeds: state.seeds } }
    }

    // ---- note：落断点并推进呼吸（每息收尾）----
    if (action === "note") {
      const note = args.note != null ? String(args.note) : ""
      const clearing = /^（?无）?$/.test(note.trim())
      state.note = note.trim() && !clearing ? note.trim() : undefined
      const w = await saveState(ctx, root, state)
      if (w.error) return { output: w.error }
      return {
        output: `断点已落盘（呼吸 ${state.breath}）：在途 ${state.inflight.length}、种子 ${state.seeds.length}、池 ${state.workers.length}（${state.workers.filter((x) => x.status === "idle").length} idle）${state.note ? `——${state.note}` : "——（无断点）"}。`,
        data: { note: state.note ?? null, breath: state.breath },
      }
    }

    // ---- task：心跳驱动任务管理 ----
    if (action === "task") {
      const op = String(args.op ?? "status").trim() || "status"
      const tasks = tasksOf(ctx)
      if (!tasks) return { output: "任务能力未启用（服务端未挂调度器）——生生无法管理心跳任务。" }
      const taskName = (ctx.env?.SHENGSHENG_TASK_NAME ?? "SHENGSHENG_EVOLVE").trim() || "SHENGSHENG_EVOLVE"
      const all = await tasks.list()
      const explicitId = args.task_id != null ? String(args.task_id).trim() : ""
      let task = explicitId ? all.find((t) => String(t.id) === explicitId) : all.find((t) => String(t.name ?? "") === taskName)

      if (op === "status") {
        if (!task) return { output: `未找到心跳任务（名 ${taskName}${explicitId ? ` / id ${explicitId}` : ""}）——用 action=task op=ensure 创建。`, data: { found: false } }
        return {
          output: [
            `心跳任务：${task.id}「${task.name ?? ""}」`,
            `- 状态：${task.enabled ? "启用" : "停用"}｜运行态 ${task.state ?? "idle"}｜已运行 ${task.runCount ?? 0} 次`,
            task.schedule ? `- 周期：${task.schedule}` : "- 周期：（未设置）",
            task.nextRunAt ? `- 下次：${new Date(Number(task.nextRunAt)).toLocaleString("zh-CN")}` : "",
            task.lastStatus ? `- 上次：${task.lastStatus}${task.lastError ? `（${String(task.lastError).slice(0, 200)}）` : ""}` : "",
          ].filter(Boolean).join("\n"),
          data: { task },
        }
      }

      if (op === "resume") {
        if (!task) return { output: `未找到心跳任务（名 ${taskName}）——用 op=ensure 创建。` }
        const updated = await tasks.update(String(task.id), { enabled: true })
        return { output: `心跳任务已启用：${updated?.id}${updated?.schedule ? `（${updated.schedule}）` : ""}——演进恢复，下一息由它送达。`, data: { task: updated } }
      }

      if (op === "pause") {
        const reason = String(args.reason ?? "").trim() || "无种可播：待人工补充方向"
        const stamp = new Date().toISOString()
        const led = await appendLedger(ctx, root, `<!-- pause:${stamp} -->`, `## 演进暂停（${stamp}）\n\n${reason}\n\n（恢复：用户启用心跳任务，或 shengsheng_state action=task op=resume。）`)
        if (led.error) return { output: led.error }
        let disabled = ""
        if (task) {
          const updated = await tasks.update(String(task.id), { enabled: false })
          disabled = updated ? `心跳任务已停用：${updated.id}` : "心跳任务停用失败"
        } else disabled = `未找到心跳任务（名 ${taskName}）——无需停用`
        let notifyNote = "未配置通知通道（暂停信息已写入台账）"
        if (task) {
          try {
            const r = await tasks.notify({ text: `**演进暂停**：${reason}\n\n台账：${led.path}`, title: "生生·演进暂停" }, String(task.id))
            notifyNote = r.delivered ? `已通知 ${r.delivered} 个通道` : `通知未投递（${r.errors.join("；") || "无可用通道"}）`
          } catch (err) {
            notifyNote = `通知失败：${describeError(err)}`
          }
        }
        state.note = `演进暂停：${reason}`
        const w = await saveState(ctx, root, state)
        if (w.error) return { output: w.error }
        return { output: `演进已暂停（藏器于身，待时而动）：\n- ${disabled}\n- ${notifyNote}\n- 台账：${led.path}`, data: { paused: true, reason } }
      }

      // op=ensure：幂等确保心跳任务存在且启用
      const schedule = String(args.schedule ?? "@every 3m").trim() || "@every 3m"
      if (task) {
        const patch: Record<string, unknown> = {}
        if (!task.enabled) patch.enabled = true
        if (task.kind === "scheduled" && String(task.schedule ?? "") !== schedule) patch.schedule = schedule
        const updated = Object.keys(patch).length ? await tasks.update(String(task.id), patch) : task
        task = updated ?? task
        const kindNote = task.kind === "scheduled" ? "" : `\n- 注意：该任务类别为 ${task.kind}（非定时）——心跳不会自动送达，请改用 scheduled 任务或手动 task_run。`
        return {
          output: `心跳任务已存在（幂等，未新建）：${task.id}「${task.name ?? ""}」——${task.enabled ? "启用" : "停用"}｜周期 ${task.schedule ?? "（未设置）"}${task.nextRunAt ? `｜下次 ${new Date(Number(task.nextRunAt)).toLocaleString("zh-CN")}` : ""}${kindNote}`,
          data: { task, created: false },
        }
      }
      const created = await tasks.add({
        kind: "scheduled",
        runner: "prompt",
        name: taskName,
        schedule,
        target: "sticky",
        agents: ["shengsheng"],
        prompt: [
          `${taskName} 心跳：生生，执行一息。`,
          `项目根：${root}`,
          state.coord.verify ? `验证命令（只转发给 worker，你永不执行）：${state.coord.verify}` : "",
          state.coord.seedDoc ? `种子库：${state.coord.seedDoc}` : "",
          state.coord.rules ? `硬约束文档：${state.coord.rules}` : "",
          "一息流程：shengsheng_state（读态）→ 观复（看 awaiting 与各 worker 实时态）→ shengsheng_collect（收藏/退役）→ 择种布雨（shengsheng_dispatch）→ shengsheng_state action=note（收息）。",
        ].filter(Boolean).join("\n"),
        env: workerEnv(root, state.coord),
        maxConsecutiveErrors: 5,
        notifyOn: "model",
      })
      return {
        output: `心跳任务已创建：${created.id}「${created.name ?? taskName}」｜周期 ${schedule}｜执行目标 sticky（下次触发时自动新建专用会话并预载 shengsheng）｜连续失败 5 次自动停用。\n第一息可立即验证：用 task_run 手动跑一次（或等下一定时触发）。`,
        data: { task: created, created: true },
      }
    }

    // ---- read：复命读态 ----
    const ids = state.workers.map((w) => w.sessionId)
    const live = svc ? await observe(ctx, svc, ids) : {}
    const active = state.workers.length
    const idle = state.workers.filter((w) => w.status === "idle").length
    const runningNow = state.workers.filter((w) => live[w.sessionId]?.running).length
    // 待收藏线索：在途但已空闲（worker 跑完了等回收）。**仅限会话仍存活者**——会话不存在的
    // 在途不是「可收藏」（无消息尾可 review、无交付物可入账），而是「中道而废」：应 retire 回退种子。
    // 不区分这两种，会把死会话当成可收藏产能嗂给模型。
    const awaiting = state.inflight.filter((i) => {
      const w = state.workers.find((x) => x.alias === i.alias)
      if (!w) return false
      const l = live[w.sessionId]
      return !!l?.exists && !l.running
    })
    // 中道而废：在途且会话不可恢复（不存在）——回退种子队列后退役
    const lost = state.inflight.filter((i) => {
      const w = state.workers.find((x) => x.alias === i.alias)
      return !w || !live[w.sessionId]?.exists
    })
    const lines = [
      `坐标（${loaded.via}）：${root}`,
      `- 验证命令：${state.coord.verify ?? "（未固化）"}`,
      state.coord.seedDoc ? `- 种子库：${state.coord.seedDoc}` : "",
      state.coord.rules ? `- 硬约束：${state.coord.rules}` : "",
      `呼吸：${state.breath}｜池 ${active}（${idle} idle，运行中 ${runningNow}）｜在途 ${state.inflight.length}｜种子 ${state.seeds.length}｜收藏待办 ${state.pending.length}`,
      state.note ? `断点：${state.note}` : "",
      active && active === idle && lost.length ? "池内 worker 的会话全部不可达——**池中 idle 是虚假产能**：复用价值已随会话消失，布雨须 fresh=true。" : "",
      state.inflight.length ? `在途：\n${state.inflight.map((i) => `- ${i.alias}｜${i.direction}${i.deliverable ? `｜交付 ${i.deliverable}` : ""}`).join("\n")}` : "在途：（空）",
      state.workers.length ? `池：\n${state.workers.map((w) => liveLine(w.alias, w, live[w.sessionId])).join("\n")}` : "池：（空）",
      awaiting.length
        ? `待收藏线索（在途但已空闲——用 shengsheng_collect review/collect 处理）：\n${awaiting.map((i) => `- ${i.alias}｜${i.direction}`).join("\n")}`
        : "",
      lost.length
        ? `中道而废（在途但会话不可达——review 取不到消息尾、无交付物可入账：应 retire mode=idle 清池并把方向退回种子队列，**不要**按「待收藏」处理）：\n${lost.map((i) => `- ${i.alias}｜${i.direction}`).join("\n")}`
        : "",
      state.pending.length ? `收藏待办：\n${state.pending.map((p) => `- ${p.alias}｜${p.summary}`).join("\n")}` : "",
      state.seeds.length
        ? `种子队列：\n${state.seeds.map((s) => `- [${s.priority}] ${s.direction}（${s.source}）${s.constraint ? `｜${s.constraint}` : ""}`).join("\n")}`
        : "种子队列：（空）",
      active >= ACTIVE_WORKER_LIMIT ? `活动 worker 已达上限 ${ACTIVE_WORKER_LIMIT}——本息只回收不布雨。` : "",
      !svc ? "会话控制面不可用（ctx.sessions 未注入）——仅读磁盘状态。" : "",
    ].filter(Boolean)
    return {
      output: lines.join("\n"),
      data: {
        root,
        coord: state.coord,
        breath: state.breath,
        workers: state.workers.map((w) => ({ ...w, ...(live[w.sessionId] ?? {}) })),
        inflight: state.inflight,
        seeds: state.seeds,
        pending: state.pending,
        awaiting,
        lost,
        note: state.note ?? null,
      },
    }
  },
}

// ---------------------------------------------------------------------------
// 工具二：dispatch（布雨）
// ---------------------------------------------------------------------------

interface PlanItem {
  alias: string
  domain: string
  task: string
  sessionId: string
  reuse: boolean
  created: boolean
  /** 复用命中时的原池条目（沿用其别名与首派时间）。 */
  revived?: Worker
  note?: string
}

function ensureUniqueAlias(state: EvolveState, order: Order, taken: Set<string>): string {
  const requested = order.alias ? sanitizeAlias(order.alias) : ""
  if (requested && !taken.has(requested) && !state.workers.some((w) => w.alias === requested)) return requested
  return deriveAlias(order.domain, new Set([...taken, ...state.workers.map((w) => w.alias)]))
}

const dispatchTool: Tool = {
  name: "dispatch",
  description:
    "布雨：把一批工单派给 worker 会话（一息一次调用，1~8 单）。同领域 idle worker 直接复用（摸底归零、记忆延续），异领域或 fresh=true 时新建会话；派工消息按模板装配（任务/上下文/交付/验证命令/纪律/预载/无人值守提示——代码保证不漏项），注入后立即返回**不等 worker 完成**，并自动登记池与在途。**派工即授权**：本工具的审批就是该 worker 全部动作的授权——worker 以无人值守姿态运行（需审批工具自动通过、无交互通道），不会为单个工具等待确认。活动 worker（运行中+空闲）上限 3：已达上限时先收藏/退役再布雨。派工消息自包含——worker 是普通会话，只凭这一条消息独立干活。不传 orders 或 env SHENGSHENG_DISPATCH=preview 时只出计划不派工（预检）。",
  parameters: schema(
    {
      orders: {
        type: "array",
        description: "工单数组（1~8 单）。task 必填；domain 用于复用同域 idle worker；fresh=true 强制新建（任务与 worker 历史领域无关或历史结论有误导风险时用）",
        items: {
          type: "object",
          properties: {
            task: { type: "string", description: "任务描述（含背景与验收标准，必填）" },
            alias: { type: "string", description: "worker 别名（缺省按领域自动派生，如 refactor-1）" },
            session_id: { type: "string", description: "直接指定 worker 会话 id（缺省按别名/领域解析）" },
            fresh: { type: "boolean", description: "强制新建会话（不复用池中 idle worker）" },
            domain: { type: "string", description: "领域标签（复用匹配键，如 lint / docs / perf）" },
            context: { type: "string", description: "上下文（种子库条目原文 / 台账相关轮次 / 关键文件:行号）" },
            deliver: { type: "string", description: "交付物与落盘路径（代码+回归测试 / 分析方案 / 基线数据）" },
            verify: { type: "string", description: "验证命令（缺省用项目坐标的验证命令）" },
            rules: { type: "string", description: "硬约束（缺省用项目坐标的硬约束文档）" },
            preload: { type: "string", description: "worker 预载子Agent 提示（缺省用项目坐标的 worker 预载提示）" },
          },
          required: ["task"],
        },
      },
      root: { type: "string", description: "项目根（缺省按会话 env / 工作目录探测）" },
    },
    [],
  ),
  requiresApproval: true,
  async execute(args, ctx) {
    const loaded = await loadState(ctx, args.root != null ? String(args.root) : undefined)
    if (loaded.error) return { output: loaded.error }
    if (!loaded.root || !loaded.initialized) {
      return { output: `control.md 未就绪（${loaded.root ? evolvePaths(loaded.root).control : "项目根未解析"}）——先 shengsheng_state action=init …。` }
    }
    const { root, state } = loaded
    const orders = normalizeOrders(args.orders)
    if (!orders.length) return { output: "dispatch 需要 orders（每单 task 必填）——空工单不派。" }
    if (orders.length > DISPATCH_LIMIT) return { output: `单批派工上限 ${DISPATCH_LIMIT} 单（收到 ${orders.length}）——拆成多批。` }
    const preview = (ctx.env?.SHENGSHENG_DISPATCH ?? "").trim() === "preview"
    const svc = sessionsOf(ctx)
    if (!svc && !preview) return { output: "会话控制面不可用（ctx.sessions 未注入）——无法派工。" }

    const taken = new Set<string>()
    const plans: PlanItem[] = []
    for (const order of orders) {
      // 复用匹配**先于**别名派生：命中的 worker 必须沿用它的既有别名——
      // 否则同一个会话会被改名（旧别名立即失效，台账/派工消息也无法回溯同一工人）。
      let reuse = false
      let sessionId = order.session_id ?? ""
      let revived: Worker | undefined
      if (!sessionId && !order.fresh) {
        // 复用匹配：显式领域优先（在 `taken` 之外的 idle worker 中取首个）；
        // 未给领域且池中恰有一个 idle worker 时复用它
        const idleWorkers = state.workers.filter((w) => w.status === "idle" && !taken.has(w.alias))
        const hit = order.domain ? idleWorkers.find((w) => w.domain === order.domain) : idleWorkers.length === 1 ? idleWorkers[0] : undefined
        if (hit) {
          sessionId = hit.sessionId
          reuse = true
          revived = hit
        }
      }
      const alias = revived ? revived.alias : ensureUniqueAlias(state, order, taken)
      taken.add(alias)
      const domain = order.domain?.trim() || revived?.domain || alias.replace(/-\d+$/, "")
      plans.push({ alias, domain, task: order.task, sessionId, reuse, created: !reuse && !sessionId, revived })
    }

    // 池容量：活动 worker（running + idle）上限——只挡住新增，不阻断回收
    const newCount = plans.filter((p) => p.created).length
    if (!preview && state.workers.length + newCount > ACTIVE_WORKER_LIMIT) {
      return {
        output: `活动 worker 已 ${state.workers.length}，本次还需新建 ${newCount} → 超过上限 ${ACTIVE_WORKER_LIMIT}。先回收：读态（shengsheng_state）看哪些 idle/超时，然后 shengsheng_collect（collect 入账 / retire 退役），再布雨。`,
        data: { blocked: true, workers: state.workers.length, needNew: newCount, limit: ACTIVE_WORKER_LIMIT },
      }
    }

    if (preview) {
      return {
        output: [`布雨预检（未派工${svc ? "" : "；会话控制面不可用"}）：`, ...plans.map((p) => `- ${p.alias}｜${p.created ? "新建" : "复用"}${p.reuse ? "（同域 idle）" : ""}｜${p.domain}｜${p.task}`)].join("\n"),
        data: { preview: true, plan: plans },
      }
    }

    const envPatch = workerEnv(root, state.coord)
    const now = Date.now()
    const kicked: Promise<void>[] = []
    const results: string[] = []
    // 派工轮次号逐单递增（幂等键来源）；只有**真正派出**的单占号，被挡下的不占
    let nextSeq = state.seq
    for (let i = 0; i < plans.length; i++) {
      const plan = plans[i]
      const order = orders[i]
      try {
        if (plan.created) {
          const created = await svc!.store.createSession(ctx.user, `evolve-w-${plan.alias}`)
          plan.sessionId = created.id
          plan.note = `新建会话（名 evolve-w-${plan.alias}）`
          try {
            await svc!.store.setEnv(created.id, ctx.user, envPatch)
            plan.note += `，注入项目坐标 env ${Object.keys(envPatch).length} 项`
          } catch (err) {
            plan.note += `，env 注入失败（${describeError(err)}）`
          }
        }
        if (!plan.sessionId) {
          results.push(`- ${plan.alias}：**未派**（无可用会话且新建失败）`)
          continue
        }
        const live = (await observe(ctx, svc!, [plan.sessionId]))[plan.sessionId]
        if (live && live.exists && live.running) {
          results.push(`- ${plan.alias}：**未派**（会话 ${plan.sessionId} 正在运行——同会话并发注入会被拒，下息再派或换 worker）`)
          continue
        }
        if (live && !live.exists) {
          results.push(`- ${plan.alias}：**未派**（会话 ${plan.sessionId} 不存在——用 collect action=retire 清出池后重派）`)
          continue
        }
        const message = renderOrderPrompt(order, state.coord, Object.keys(envPatch))
        // worker 继承本次派工的授权：派工本身经用户审批（dispatch 是需审批工具），
        // 派出去的就是这一次已授权的一次性执行——若仍按实时审批逐工具等确认，无人值守下
        // 会挂到审批超时才继续（每个重活 worker 都中道而废）。故以 autoApprove 运行，
        // 与 task 型无人值守执行同一授权面（tasks.ts 的 autoApprove 语义）。
        // interactionMode: none —— worker 无交互通道（ask/show 预览/页面捕获不可用）。
        const started = svc!.run(plan.sessionId, ctx.user, message, { autoApprove: true, interactionMode: "none" }).catch((err) => {
          results.push(`- ${plan.alias}：注入后运行报错——${describeError(err)}`)
        })
        kicked.push(started)
        nextSeq += 1
        // 池 upsert（复用者保留原别名与首派时间，新建者记当前）：round 为本次派工轮次号
        const prev = state.workers.find((w) => w.sessionId === plan.sessionId)
        const worker: Worker = {
          alias: plan.alias,
          sessionId: plan.sessionId,
          domain: plan.domain,
          task: plan.task.slice(0, 80),
          dispatchedAt: prev?.dispatchedAt ?? now,
          round: nextSeq,
          status: "running",
        }
        state.workers = [...state.workers.filter((w) => w.sessionId !== plan.sessionId && w.alias !== plan.alias), worker]
        const inflight: Inflight = { alias: plan.alias, direction: order.task, deliverable: order.deliver ?? "", dispatchedAt: now, round: worker.round }
        state.inflight = [...state.inflight.filter((x) => x.alias !== plan.alias), inflight]
        // 复用代数提示：同一会话连续承接的单数（首派 1）——接近上限时建议收藏后退役换新上下文
        const generation = prev ? prev.round + 1 : 1
        const genNote = generation >= REUSE_GENERATION_HINT ? `｜**已承接 ${generation} 单**（上下文渐胖，宜收藏后退役换新）` : ""
        results.push(`- ${plan.alias}｜${plan.reuse ? "复用" : "新建"}会话 ${plan.sessionId}｜${plan.domain}｜轮次 ${worker.round}｜${plan.task.slice(0, 60)}${genNote}`)
      } catch (err) {
        results.push(`- ${plan.alias}：**派工失败**——${describeError(err)}`)
      }
    }
    state.seq = nextSeq
    // 初期错误窗口（会话缺失/存储异常等同步段失败）：不等待执行完成，只捞早期异常
    await Promise.race([Promise.allSettled(kicked), new Promise((r) => setTimeout(r, 1500))])
    const w = await saveState(ctx, root, state)
    if (w.error) return { output: `派工已发出但状态落盘失败：${w.error}\n${results.join("\n")}` }
    return {
      output: [
        `布雨完成（在途 ${state.inflight.length}｜池 ${state.workers.length}）：`,
        ...results,
        "worker 在后台自行运行——**不要等**，本息到此收息（shengsheng_state action=note），下息由 shengsheng_state 的 awaiting 提示回收。",
      ].join("\n"),
      data: { root, plan: plans, workers: state.workers, inflight: state.inflight },
    }
  },
}

// ---------------------------------------------------------------------------
// 工具三：collect（收藏与退役）
// ---------------------------------------------------------------------------

const COLLECT_SNIPPET = 600

const collectTool: Tool = {
  name: "collect",
  description:
    "收藏与退役（回收阶段）。action=review：深观一个 worker（运行态/起始时间/消息数/上下文水位/待决交互/最后一条回复）并给出处置建议（只读）。action=collect：把报告要点追加进演进台账（幂等——同一 worker 只入一次）、worker 标 idle 留池、在途清除、建议下一条自动入种子队列。action=retire：退役 worker——mode=idle 清出池并把在途方向退回种子队列（会话不存在/领域已终）、mode=stop 终止超时运行（先 review 抢救结论）、mode=delete 删除会话（运行中会被拒绝）。",
  parameters: schema(
    {
      action: { enum: ["review", "collect", "retire"], description: "动作" },
      alias: { type: "string", description: "worker 别名（池登记用；也可传 session_id）" },
      session_id: { type: "string", description: "worker 会话 id（缺省按别名解析）" },
      summary: { type: "string", description: "collect：报告要点（markdown，原样入台账——交付/验证证据/未竟事项）" },
      next: { type: "string", description: "collect：报告「建议下一条」（自动入种子队列）" },
      mode: { enum: ["idle", "stop", "delete"], description: "retire：退役方式（缺省 idle=仅清出池）" },
      reason: { type: "string", description: "retire：退役原因（记入种子来源与回显）" },
      root: { type: "string", description: "项目根（缺省按会话 env / 工作目录探测）" },
    },
    ["action"],
  ),
  requiresApproval: false,
  async execute(args, ctx) {
    const action = String(args.action ?? "").trim()
    const loaded = await loadState(ctx, args.root != null ? String(args.root) : undefined)
    if (loaded.error) return { output: loaded.error }
    if (!loaded.root || !loaded.initialized) {
      return { output: `control.md 未就绪（${loaded.root ? evolvePaths(loaded.root).control : "项目根未解析"}）——先 shengsheng_state action=init …。` }
    }
    const { root, state } = loaded
    const alias = args.alias != null ? String(args.alias).trim() : ""
    const sidArg = args.session_id != null ? String(args.session_id).trim() : ""
    const worker = state.workers.find((w) => (alias && w.alias === alias) || (sidArg && w.sessionId === sidArg))
    const candidates = () => state.workers.length
      ? `池中 worker：\n${state.workers.map((w) => `- ${w.alias}｜${w.domain}｜${w.task}｜${w.status}`).join("\n")}`
      : "池中无 worker。"
    if (!worker) return { output: `未在池中找到该 worker（alias=${alias || "（未传）"}${sidArg ? ` session_id=${sidArg}` : ""}）。${candidates()}` }

    const svc = sessionsOf(ctx)
    if (!svc) return { output: "会话控制面不可用（ctx.sessions 未注入）——无法观察/操作 worker 会话。" }
    const live = (await observe(ctx, svc, [worker.sessionId]))[worker.sessionId] ?? { exists: false, running: false }

    // ---- review：深观（只读）----
    if (action === "review") {
      const inflight = state.inflight.find((i) => i.alias === worker.alias)
      const advice: string[] = []
      if (!live.exists) advice.push("会话不存在（外部删除/服务重启）→ `shengsheng_collect action=retire mode=idle`（在途退回种子队列）")
      else if (live.running) {
        if ((live.minutes ?? 0) > 40) advice.push(`运行已 ${live.minutes} 分钟（超 40）→ 先本 review 已抢救消息尾，再 \`action=retire mode=stop\` 记「中道而废」`)
        else advice.push(`运行中（${live.minutes ?? "?"} 分钟，未超 40）→ 万物自化，勿扰，下息再看`)
        if (live.pending?.length) advice.push(`有待决交互（${live.pending.join("、")}）——无人值守会话会挂起至超时`)
      } else {
        const short = (live.last?.length ?? 0) < 200
        if (short) advice.push("空闲但末条疑似半截 → 记中断后 `action=retire`（该领域下次重开）")
        else advice.push("空闲且末条像完整报告 → `action=collect alias=… summary=… [next=…]` 入台账")
        if ((live.ctxTokens ?? 0) > WORKER_CTX_LIMIT) advice.push(`上下文体量 ${live.ctxTokens}（超 ${WORKER_CTX_LIMIT}）→ 收藏后退役，下次同域新建`)
      }
      return {
        output: [
          `worker ${worker.alias}（${worker.domain}）｜会话 ${worker.sessionId}`,
          `- 任务：${worker.task}｜派发 ${new Date(worker.dispatchedAt).toLocaleString("zh-CN")}｜池状态 ${worker.status}`,
          `- 现有态：${!live.exists ? "会话不存在" : live.running ? `运行中 ${live.minutes ?? "?"} 分钟` : "空闲"}${live.messages != null ? `｜消息 ${live.messages}` : ""}${live.ctxTokens != null ? `｜ctx ${live.ctxTokens}` : ""}${live.toolCalls ? `｜在途工具 ${live.toolCalls}` : ""}`,
          live.pending?.length ? `- 待决：${live.pending.join("、")}` : "",
          inflight ? `- 在途登记：${inflight.direction}${inflight.deliverable ? `｜交付 ${inflight.deliverable}` : ""}` : "- 在途登记：（无——可能已收藏）",
          live.last ? `- 最后一条回复（截断 ${COLLECT_SNIPPET} 字）：\n${live.last.slice(0, COLLECT_SNIPPET)}` : "- 最后一条回复：**（无文本/未产出）**",
          `处置建议：\n${advice.map((a) => `- ${a}`).join("\n")}`,
        ].filter(Boolean).join("\n"),
        data: { worker, live, inflight: inflight ?? null, advice },
      }
    }

    // ---- collect：入台账（幂等）+ 标 idle + 在途清除 + 建议下一条入种子 ----
    if (action === "collect") {
      const summary = String(args.summary ?? "").trim()
      if (!summary) return { output: "collect 需要 summary（报告要点：交付/验证证据/未竟事项）——原样入台账。" }
      const next = args.next != null ? String(args.next).trim() : ""
      const inflight = state.inflight.find((i) => i.alias === worker.alias)
      const stamp = new Date().toISOString()
      // 幂等键：worker 会话 + **本轮轮次号**（dispatch 写入的单调递增代号）。
      // 不用时间戳：复用时 worker 与 inflight 的时间戳不同源，键会漂移（既挡不住真重复、也可能误伤）。
      // 轮次号在会话内单调递增，语义就是「第几轮交付」——同一轮重复 collect 必被拦，新轮必可入账。
      const roundKey = inflight?.round ?? worker.round ?? 0
      const ledgerKey = `collect:${worker.sessionId}:r${roundKey}`
      const block = [
        `<!-- ${ledgerKey} -->`,
        `## ${stamp}｜${worker.alias}｜${worker.domain}`,
        "",
        `- 方向：${inflight?.direction ?? worker.task}`,
        inflight?.deliverable ? `- 预期产出：${inflight.deliverable}` : "",
        `- 会话：${worker.sessionId}｜轮次 ${roundKey}`,
        "",
        summary,
        next ? `\n建议下一条：${next}` : "",
      ].filter(Boolean).join("\n")
      const led = await appendLedger(ctx, root, ledgerKey, block)
      if (led.error) return { output: led.error }
      if (!led.appended) return { output: `台账已有该 worker 的记录（幂等跳过，未重复追加）：${worker.alias}｜${led.path}\n若要点有更新，请先 \`action=retire\` 让该 worker 退出池，下轮以新记录入账。` }

      const w = state.workers.find((x) => x.alias === worker.alias)
      if (w) w.status = "idle"
      state.inflight = state.inflight.filter((i) => i.alias !== worker.alias)
      state.pending = state.pending.filter((p) => p.alias !== worker.alias)
      let seedNote = ""
      if (next) {
        if (state.seeds.some((s) => s.direction === next)) seedNote = `「建议下一条」已在种子队列（未重复入队）`
        else {
          state.seeds.push({ priority: "待定", direction: next, source: `回收报告 ${worker.alias}`, constraint: "" })
          seedNote = `「建议下一条」已入种子队列`
        }
      }
      const sw = await saveState(ctx, root, state)
      if (sw.error) return { output: `台账已追加但状态落盘失败：${sw.error}` }
      return {
        output: [
          `收藏完成：${worker.alias}｜${worker.domain}`,
          `- 台账：${led.path}（已追加）`,
          `- worker 留池标 idle（上下文里的领域结论可复用；退役用 action=retire）`,
          seedNote ? `- ${seedNote}（现 ${state.seeds.length} 条）` : "- 未给 next（报告里的建议下一条没进队列）",
          `- 在途剩 ${state.inflight.length}｜池 ${state.workers.length}（${state.workers.filter((x) => x.status === "idle").length} idle）`,
        ].join("\n"),
        data: { ledger: led.path, worker, seeds: state.seeds, inflight: state.inflight },
      }
    }

    // ---- retire：退役 ----
    if (action === "retire") {
      const mode = String(args.mode ?? "idle").trim() || "idle"
      const reason = args.reason != null ? String(args.reason).trim() : ""
      if (mode === "stop") {
        if (live.running) {
          svc.engine.cancel(worker.sessionId)
        }
        const anchor = state.workers.find((w) => w.alias === worker.alias)
        if (anchor) {
          anchor.status = "idle"
          anchor.domain = `${anchor.domain}（已退役）`
        }
        const inflight = state.inflight.find((i) => i.alias === worker.alias)
        if (inflight) {
          state.inflight = state.inflight.filter((i) => i.alias !== worker.alias)
          if (!state.seeds.some((s) => s.direction === inflight.direction)) {
            state.seeds.push({ priority: "待定", direction: inflight.direction, source: `中道而废 ${worker.alias}${reason ? `（${reason}）` : ""}`, constraint: "重开时先读台账上一轮的中断点" })
          }
        }
        state.workers = state.workers.filter((w) => w.alias !== worker.alias)
        const sw = await saveState(ctx, root, state)
        if (sw.error) return { output: sw.error }
        return { output: `已终止并退役：${worker.alias}（会话 ${worker.sessionId} 保留可查；在途方向已退回种子队列）。${reason ? `原因：${reason}` : ""}`, data: { worker, seeds: state.seeds } }
      }
      if (mode === "delete") {
        if (live.running) return { output: `会话 ${worker.sessionId} 正在运行——先 \`action=retire mode=stop\` 终止再删除（删除运行中会话会留下孤儿任务态）。` }
        await svc.store.delete(worker.sessionId, ctx.user)
        const inflight = state.inflight.find((i) => i.alias === worker.alias)
        if (inflight) {
          state.inflight = state.inflight.filter((i) => i.alias !== worker.alias)
          if (!state.seeds.some((s) => s.direction === inflight.direction)) {
            state.seeds.push({ priority: "待定", direction: inflight.direction, source: `在途回退 ${worker.alias}${reason ? `（${reason}）` : ""}`, constraint: "" })
          }
        }
        state.workers = state.workers.filter((w) => w.alias !== worker.alias)
        state.pending = state.pending.filter((p) => p.alias !== worker.alias)
        const sw = await saveState(ctx, root, state)
        if (sw.error) return { output: sw.error }
        return { output: `已删除会话并退役：${worker.alias}（${worker.sessionId} 进归档，7 天内可由管理员恢复）。${reason ? `原因：${reason}` : ""}`, data: { worker, seeds: state.seeds } }
      }
      // mode=idle：仅清出池（在途退回种子）
      const inflight = state.inflight.find((i) => i.alias === worker.alias)
      let back = ""
      if (inflight) {
        state.inflight = state.inflight.filter((i) => i.alias !== worker.alias)
        if (!state.seeds.some((s) => s.direction === inflight.direction)) {
          state.seeds.push({ priority: "待定", direction: inflight.direction, source: `在途回退 ${worker.alias}${reason ? `（${reason}）` : ""}`, constraint: "worker 会话不可用，重开时从种子库/台账重新摸底" })
          back = `；在途方向已退回种子队列`
        } else back = `；在途方向已在种子队列（未重复）`
      }
      state.workers = state.workers.filter((w) => w.alias !== worker.alias)
      state.pending = state.pending.filter((p) => p.alias !== worker.alias)
      const sw = await saveState(ctx, root, state)
      if (sw.error) return { output: sw.error }
      return {
        output: `已退役（仅清出池）：${worker.alias}${live.exists ? `——会话 ${worker.sessionId} 保留在磁盘，需彻底清理由 action=retire mode=delete` : "（会话已不存在）"}${back}${reason ? `。原因：${reason}` : ""}`,
        data: { worker, seeds: state.seeds },
      }
    }

    return { output: `未知 action: ${action}（可用：review / collect / retire）` }
  },
}

// ---------------------------------------------------------------------------
// 定义
// ---------------------------------------------------------------------------

export const name = "shengsheng"
export const description =
  "生生（shengsheng）——《周易》「生生之谓易」。项目驻留自主演进的调度中枢：由定时任务周期唤醒（sticky 会话），每息择种下一个最值得做的方向、派 worker 会话并行干落地（编码/验证/分析）、回收成果入台账——控制平面自己永不干活，重活全在 worker，死一跳不丢全局。自带三工具（state=读态/坐标/择种/断点/心跳任务；dispatch=布雨派工；collect=深观/收藏/退役），`.evolve/control.md` 与台账由工具托管。适合给任意 git 项目搭「持续变好、无人值守」的演进流水线。当用户说「演进」「驻留开发」「持续改进这个项目」或要为项目创建自主演进任务时装载。输入：驱动任务的唤醒消息（含项目根/验证命令/种子库等项目坐标）。"
export const systemPrompt = systemPromptBase.endsWith("\n") ? systemPromptBase : `${systemPromptBase}\n`
export const tools: Record<string, Tool> = { state: stateTool, dispatch: dispatchTool, collect: collectTool }
/** 审批：布雨会派生会改仓库的会话（派生前确认）；state 的原子动作按分支动态门控（见工具定义）；收藏/退役是回收动作，免审批。 */
export const requiresApproval: Record<string, boolean> = { dispatch: true, collect: false }
export const preload = false
export const envVars = [
  { name: "SHENGSHENG_ROOT", description: "项目根绝对路径（control.md 首部固化的项目坐标；唤醒消息未携带时的解析来源）" },
  { name: "SHENGSHENG_TASK_NAME", description: "心跳任务名（state action=task 按它反查驱动任务，缺省 SHENGSHENG_EVOLVE）" },
  { name: "SHENGSHENG_VERIFY", description: "默认验证命令（派工消息原样传递，生生永不执行）" },
  { name: "SHENGSHENG_SEED_DOC", description: "种子库文档路径（待做方向清单）" },
  { name: "SHENGSHENG_RULES", description: "硬约束文档路径（worker 必守的项目规矩）" },
  { name: "SHENGSHENG_DISPATCH", description: "设为 preview 时 dispatch 只出计划不派工（预检）" },
]

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars }
