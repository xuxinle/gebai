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
// control.md 两张登记表的**格式版本与版本化解析**（在途/池的历代列、证据判代、升级函数）
// 拆在独立模块：加列只需在那里加一支版本分支，本模块只认现行形态（下游零改动）。
import {
  tableBlock,
  tableRows,
  parseInflightRow,
  migrateInflight,
  inflightHeader,
  inflightCells,
  parsePoolRow,
  migratePool,
  poolHeader,
  poolCells,
} from "./control-format"
// 卦象数据与起卦内核（纯数据 + 纯函数）：三张表（八卦/64 卦名/爻位语义）与「由输入算卦」的
// 实现都在那里，本模块只负责「感天时」（读环境）与「落卦」（记档、闸门）。
import {
  buildHexagram,
  hourIndexOf,
  pickCandidate,
  trigramOf,
  type DivineInputs,
} from "./hexagrams"

// 解析面的公共出口（**兼容面**：形状判据与表格读取历来由此模块导出，其它模块/测试按其引用；
// 实现在 `control-format.ts`，此处只做转出——单一实现，不会两处各写一份）。
export { cells, isSeparator, tableBlock, tableRows, isSessionIdShape } from "./control-format"

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

/** worker 池条目。**主键 = `sessionId`**：同一会话在池中只保留**最新一轮**登记（续派/复用取代旧轮）。
 *  别名是「工人」的显示名、会话是「工人」的本体——同一会话换轮次不得换别名（换了旧别名立即失效，
 *  台账与派工消息再无同一工人可回溯），也不得并列两条（并列即「同一会话两个身份」的登记裂缝）。 */
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

/** 在途（已布未收）。**主键 = 会话 id**：同一会话只保留最新一轮在途（写侧 upsert + 读侧归一）。
 *  `sessionId` 是登记里**必须自带**的一列：别名只是池里的影子，别名漂移/池条目被清后就只剩会话一个实体
 *  线索——不带着它，「在途」就无从“先探会话再判生死”（只能按别名猜，而猜会把可达会话误判为死单）。
 *  旧格式（无此列）仍可解析：会话经池的同名条目解析，解析不到即无从核实。
 *  引擎不变量撑着去重——同一会话同一时刻只有一次运行，故「同一会话两条在途」必然是陈旧残留。 */
export interface Inflight {
  alias: string
  /** 本轮的 worker 会话 id（与池同名条目同源写入；旧格式缺失时经池解析）。 */
  sessionId?: string
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

// ---------------------------------------------------------------------------
// 登记去重（主键 = 会话 id）：同一会话只保留最新一轮
// ---------------------------------------------------------------------------

/**
 * 登记**就地归一**：同一会话只保留**最新一轮**（round 最大；并列时取靠后一条，视作同轮重复登记）。
 *
 * 为什么读侧也要归一，而不只在写侧 upsert：`control.md` 是磁盘上的共享态——上一版工具写下的
 * 「同一会话并列多条」还留在盘上，人工查看也可能误改。读侧不归一，裂缝就会一直活着：按别名寻址
 * 撞上旧条目、按会话 id 寻址命中当前条目而**清不掉**陈旧的另一条（已实测两次的缺陷形态）。
 *
 * 归一保持条目**原相对次序**（幸存者留在它原本的位置），故渲染结果确定、人工可读、往返幂等。
 */
function dedupeBySession<T>(items: T[], sessionOf: (item: T) => string, roundOf: (item: T) => number): T[] {
  const winners = new Map<string, T>()
  const superseded = new Set<T>()
  for (const item of items) {
    const key = sessionOf(item)
    // 无会话 id 的条目**无从判定同源**：一律保留——宁可多留一条登记，不可错杀一条真实在途。
    if (!key) continue
    const current = winners.get(key)
    if (!current) {
      winners.set(key, item)
      continue
    }
    if (roundOf(item) >= roundOf(current)) {
      superseded.add(current)
      winners.set(key, item)
    } else superseded.add(item)
  }
  return items.filter((i) => !superseded.has(i))
}

/** 别名 → 会话 id（池是别名的权威；旧格式在途缺会话列时靠它解析）。 */
function aliasToSession(workers: Worker[]): Map<string, string> {
  const out = new Map<string, string>()
  for (const w of workers) if (w.alias) out.set(w.alias, w.sessionId)
  return out
}

/**
 * 登记**身份键**：在途条目**优先取会话 id**（自带列 → 经池解析别名，即 {@link inflightSession}），
 * 只有「会话线索全缺」时才退化为别名（此时它确实是「无从核实」那一类）；池条目的身份键就是它的会话 id
 * （{@link dedupeWorkers} 直接用 `w.sessionId`）。
 *
 * 为什么在途不能只用别名当键：别名是**显示名**、会话才是本体——旧版工具写下的同一会话两个别名
 * （别名漂移）会让「按别名过滤/寻址」两边都漏：写侧 `filter((x) => x.alias !== plan.alias)` 过滤不掉
 * 旧名那条，`collect`/`retire` 按 worker 别名也删不掉它，于是**同一会话在途并列两条**（已实测：
 * 池 1 条、在途 2 条，且旧名那条因轮次更大而在读侧归一里胜出，顶掉本轮新登记）。
 * 会话判定直接复用 `inflightSession`——读态「这条在途是谁的」与写侧「该清哪条」必须是**同一个判据**，
 * 各自实现一分就会出现「读态认得出、收藏清不掉」这类裂缝。
 */
const inflightIdentity = (i: Inflight, workers: Worker[]): string => inflightSession(i, workers) ?? i.alias

/** 在途的会话线索：自带列优先，旧格式回落到池的同名条目解析（仍解析不到 = 无从核实）。 */
export function inflightSession(i: Inflight, workers: Worker[]): string | undefined {
  return i.sessionId || aliasToSession(workers).get(i.alias) || undefined
}

/** 会话 id 短形（回显用：完整 id 不丢，短形只为对齐与可读）。 */
function idShort(id: string | undefined): string {
  if (!id) return "（未登记）"
  return id.length > 12 ? `${id.slice(0, 8)}…` : id
}

/** 池归一（纯函数，返回新数组——不就地改调用方持有的 state）。 */
function dedupeWorkers(workers: Worker[]): Worker[] {
  return dedupeBySession(workers, (w) => w.sessionId, (w) => w.round || 0)
}

/** 在途归一：同一会话只留最新一轮（身份键经 {@link inflightIdentity} 解析——会话优先、无会话
 *  线索者才用别名）；身份键为空（无别名、也不属任何会话）的条目无从判定同源，一律保留。 */
function dedupeInflight(inflight: Inflight[], workers: Worker[]): Inflight[] {
  return dedupeBySession(inflight, (i) => inflightIdentity(i, workers), (i) => i.round || 0)
}

/** 该 worker 的在途登记（们）：**会话 id 为主键**、兼按别名——读态、深观、收藏、退役共用同一判据，
 *  故「池里一个工人 ↔ 在途一条登记」在四处一致；旧版遗留的**别名漂移残留**也归到同一个工人名下。 */
function inflightOf(state: EvolveState, worker: Worker): Inflight[] {
  return state.inflight.filter((i) => i.alias === worker.alias || inflightIdentity(i, state.workers) === worker.sessionId)
}

/** 取走该 worker 的全部在途登记（会话 id 为主键、兼按别名；含别名漂移的陈旧残留）并就地移除。 */
function takeInflight(state: EvolveState, worker: Worker): Inflight[] {
  const hit = inflightOf(state, worker)
  if (hit.length) state.inflight = state.inflight.filter((i) => !hit.includes(i))
  return hit
}

/** 在途方向退回种子队列（按方向去重）——退役/清理路径统一走它，别名漂移的残留也不丢方向。 */
function returnToSeeds(state: EvolveState, rows: Inflight[], prefix: string, reason: string, constraint: string): boolean {
  let pushed = false
  for (const i of rows) {
    if (state.seeds.some((s) => s.direction === i.direction)) continue
    state.seeds.push({ priority: "待定", direction: i.direction, source: `${prefix} ${i.alias}${reason ? `（${reason}）` : ""}`, constraint })
    pushed = true
  }
  return pushed
}

/** 渲染器写出的**全部规范节标题**（按写出次序）——`renderControl` 只写这些节。
 *
 *  为什么需要显式清单：断点是**自由文本节**（正文由模型写，可能是多段 markdown），若把任何
 *  `^##\s` 行都当节边界，正文里一行手写的小标题（`## 下一步`、粘贴进来的 md 片段）就会**静默截断**
 *  断点——读进来只剩前半段，下一次 `note` 写回时后半段永久丢失。改为只认规范节标题：非规范
 *  `## ` 行是内容而不是结构。手写文件里多出的非规范标题也归到所在节正文（**不静默丢**）。 */
export const SECTION_HEADINGS = ["项目坐标", "计数", "worker 池", "在途", "种子队列", "收藏待办", "断点"] as const

/** 这一行是不是**规范节标题**（`## <清单里的标题>`）。 */
const isSectionHeading = (line: string): boolean => {
  const t = line.trim()
  return t.startsWith("## ") && (SECTION_HEADINGS as readonly string[]).includes(t.slice(3).trim())
}

/** 节标题行：入参限定为语料里的标题——拼错或漏登一个节，编译期即报（写出侧与解析侧同源）。 */
const H = (title: (typeof SECTION_HEADINGS)[number]): string => `## ${title}`

/** 取 `## <标题>` 段落下的自由文本（非表格行）。
 *
 *  边界只认**规范节标题**（见 {@link SECTION_HEADINGS}）：断点正文里的 `## ` 行是内容，不是结构——
 *  否则一段以 `## ` 开头的手写小标题会被当成节边界，断点被静默截断（写回时后半段丢失）。 */
function sectionText(md: string, heading: string): string {
  const lines = md.split(/\r?\n/)
  const start = lines.findIndex((l) => l.trim() === `## ${heading}`)
  if (start < 0) return ""
  const out: string[] = []
  for (let i = start + 1; i < lines.length; i++) {
    if (isSectionHeading(lines[i])) break
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
  // 池先归一，在途再按归一的池解析别名 → 会话：**读进来就是干净的**，下游（读态/收藏/布雨）
  // 一律以「同一会话至多一条」为前提，不必各自再防一遍。
  const workBlock = tableBlock(md, "worker 池")
  // 池工表：历代读法（版本声明、尾列锚点、溢格并回）全在 `control-format.ts` 里；
  // 本处只做「取格 → 按版本升级为现行形态」。
  const workRows = workBlock.rows.map((c): Worker => migratePool(parsePoolRow(c, { declared: workBlock.declared })))
  const flightBlock = tableBlock(md, "在途")
  // 池表先出“同文件证据”：在途会话列的辨识除了形状，还认「这一格就是池里某个 worker 的会话」——
  // 池是在途的登记源（派工写两侧），两侧同源、同文件，比形状更贴身份。
  const poolSessions = new Set(workRows.map((w) => w.sessionId).filter(Boolean))
  // 在途行同理：**判代（按证据，不看格数）+ 升级为现行形态**都在 `control-format.ts`——
  // 旧代（无会话列）在此补 `sessionId: ""` 占位，对象形状与新版一致，渲染→解析→渲染往返恒等。
  const flightRows = flightBlock.rows.map((c): Inflight => migrateInflight(parseInflightRow(c, { declared: flightBlock.declared, poolSessions })))
  // 池/在途**先清无会话 id 的坏行**再归一：去重的主键是会话 id，主键为空即不构成「同一会话」的并列
  // （无会话列的表头行、半截写入的残行）——不清会把它们当成合法登记留在盘上，而它们永远核不出会话。
  const workers = dedupeWorkers(workRows.filter((w) => w.alias && w.sessionId))
  const state: EvolveState = {
    coord,
    breath: breathMatch ? Number(breathMatch[1]) : 0,
    seq: seqMatch ? Number(seqMatch[1]) : 0,
    workers,
    inflight: dedupeInflight(flightRows.filter((i) => i.alias), workers),
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

/** 登记归一入口（导出以便单测直接锁契约）：池/在途各返回去重后的副本 + 被取代的条目数。
 *  写侧（dispatch）也走同一对纯函数——「读进来归一」与「写回去归一」共用一套判据。 */
export function dedupeState(state: EvolveState): { workers: Worker[]; inflight: Inflight[]; dropped: number } {
  const workers = dedupeWorkers(state.workers)
  const inflight = dedupeInflight(state.inflight, workers)
  return { workers, inflight, dropped: state.workers.length - workers.length + (state.inflight.length - inflight.length) }
}

/** 序列化 control.md（节结构确定性重写，内容全部来自解析后的状态）。
 *
 *  节标题一律经 {@link H} 取自语料（{@link SECTION_HEADINGS}）——写错或漏登一个节，编译期即报。 */
export function renderControl(state: EvolveState): string {
  const coordLines = COORD_LABELS.filter(([k]) => state.coord[k]).map(([k, label]) => `- ${label}：${state.coord[k]}`)
  const parts = [
    "> 生生调度状态（`shengsheng_state` 托管，勿手改）。",
    "",
    H("项目坐标"),
    coordLines.length ? coordLines.join("\n") : "（未固化）",
    "",
    H("计数"),
    `- 呼吸：${state.breath}`,
    `- 派工序号：${state.seq}`,
    "",
    H("worker 池"),
    renderTable(poolHeader(), state.workers.map((w) => poolCells(w))),
    "",
    H("在途"),
    renderTable(inflightHeader(), state.inflight.map((i) => inflightCells(i))),
    "",
    H("种子队列"),
    renderTable(["优先级", "方向", "来源", "约束"], state.seeds.map((s) => [s.priority, s.direction, s.source, s.constraint])),
    "",
    H("收藏待办"),
    renderTable(["别名", "摘要", "时间"], state.pending.map((p) => [p.alias, p.summary, String(p.at)])),
    "",
    H("断点"),
    state.note?.trim() ? state.note.trim() : "（无）",
    "",
  ]
  return parts.join("\n")
}
// ---------------------------------------------------------------------------
// 项目根解析
// ---------------------------------------------------------------------------

/** 进程内会话级项目根记忆（仅 state action=init 写入）。
 *  以会话**工作目录**为键：工作目录是「会话身份」的稳定投影——同一目录下的会话共享同一份既存
 *  .evolve/control.md 记忆（重启后新会话沿用同一目录，init 记忆因此在进程内存活）；按会话 ID
 *  作键时「本会话未 init」与「会话换了 ID」无法区分，一旦记忆被当成解析来源就会把**其他根**
 *  当成会话基线（陈旧未初始化）。读/记两处一律按同一归一化键。
 *
 *  **只装既有 .evolve/control.md 的根**——没有该文件的目录不得作为解析结果参与回落，
 *  否则会凭空造出既非本会话记忆、也非探测目标的路径。 */
const rootMemo = new Map<string, string>()

/** 记忆键：会话工作目录（相对路径不得作键——按目录归一化后即会话身份）。 */
const memoKey = (ctx: ToolContext): string => String(ctx.sessionWorkdir ?? ctx.workdir ?? "").replace(/[\\/]+$/, "")

/** 读本会话记忆：键不中即无——绝不回落到其他根。 */
function memoizedRoot(ctx: ToolContext): string | undefined {
  const key = memoKey(ctx)
  if (!key) return undefined
  return rootMemo.get(key)
}

const cleanRoot = (p: string): string => p.trim().replace(/[\\/]+$/, "")
const isAbsRoot = (p: string): boolean => /^[\\/]/.test(p) || /^[A-Za-z]:[\\/]/.test(p)

/** 记本会话记忆（state action=init 用）：只记绝对路径——相对根已由参数按会话工作目录解释，
 *  写进记忆只会留下脏条目供其他动作误用。 */
function rememberRoot(ctx: ToolContext, root: string): void {
  const key = memoKey(ctx)
  if (!key) return
  const abs = cleanRoot(root)
  if (!isAbsRoot(abs)) return
  rootMemo.set(key, abs)
}

/** 未初始化时的统一引导：区分「根已解析但没有既有 control.md」与「本会话无从解析项目根」。
 *  两种情形都不得凭空造根——写分支（seed_add/seed_done/note/task）与布雨/收藏都以既有 control.md 为基线。 */
function notInitialized(root: string | undefined, via: string, control: string): string {
  if (root) return "项目根已解析（" + via + "）：" + control + " 不存在——尚未初始化。请用 action=init 固化坐标（root=" + root + "），否则写分支与布雨/收藏无基线可依。"
  return "本会话无从解析项目根（显式 root、会话 env SHENGSHENG_ROOT、本会话 init 记忆、工作目录向上 4 层的 .evolve/control.md 皆无）：请传 root=<项目根绝对路径> 并用 action=init 固化。"
}

/** 项目根解析：显式参数 > 会话 env > 本会话 init 记忆 > 工作目录向上探测（候选须含既有 .evolve/control.md）。
 *
 *  **候选一律核实**：参数与 env 都可能带进陈旧路径（项目改名/搬迁/删除、env 未随会话清理），故核到
 *  .evolve/control.md 才采纳；核不中即回落下一级，而不是拿这个候选去判「初生」（那会让写分支无从落地，
 *  也把不存在或别处的根误当本会话基线）。工作目录探测本身以 control.md 为硬判据，无需再核；
 *  **记忆不外泄**：记忆键为会话工作目录且只在 init 时写入——任何其他根（参数/env）的解析结果都不落记忆。 */
async function resolveRoot(ctx: ToolContext, argRoot?: string): Promise<{ root?: string; via: string; error?: string }> {
  const explicit = (argRoot ?? "").trim()
  if (explicit) {
    const cand = cleanRoot(explicit)
    const r = await readAbs(ctx, evolvePaths(cand).control)
    if (r.error) return { via: "参数", error: r.error }
    if (r.text != null) return { root: cand, via: "参数" }
    return { root: cand, via: "参数（无既有 control.md）" }
  }
  const fromEnv = (ctx.env?.SHENGSHENG_ROOT ?? "").trim()
  if (fromEnv) {
    const cand = cleanRoot(fromEnv)
    const r = await readAbs(ctx, evolvePaths(cand).control)
    if (r.error) return { via: "环境变量 SHENGSHENG_ROOT", error: r.error }
    if (r.text != null) return { root: cand, via: "环境变量 SHENGSHENG_ROOT" }
    return { root: cand, via: "环境变量 SHENGSHENG_ROOT（无既有 control.md）" }
  }
  const memo = memoizedRoot(ctx)
  if (memo) {
    const r = await readAbs(ctx, evolvePaths(memo).control)
    if (r.error) return { via: "本会话 init 记忆", error: r.error }
    if (r.text != null) return { root: memo, via: "本会话 init 记忆" }
  }
  let dir = memoKey(ctx)
  for (let i = 0; i < 4 && dir; i++) {
    const r = await readAbs(ctx, dir + "/.evolve/control.md")
    if (r.error) return { via: "工作目录探测", error: r.error }
    if (r.text != null) return { root: dir, via: "工作目录探测" }
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
  const resolved = await resolveRoot(ctx, argRoot)
  if (resolved.error) return { via: resolved.via, state: parseControl(null), initialized: false, error: resolved.error }
  // 回落链：解析结果 → 本会话 init 记忆 → 会话工作目录。任一层核到既有 control.md 即采纳——
  // 根不存在 / control.md 不存在（陈旧参数或 env）时不得直接判「初生」而让写分支无从落地。
  const candidates: Array<[string, string]> = []
  if (resolved.root) candidates.push([resolved.root, resolved.via])
  const memo = memoizedRoot(ctx)
  if (memo && !candidates.some((c) => c[0] === memo)) candidates.push([memo, "本会话 init 记忆"])
  const here = memoKey(ctx)
  if (here && !candidates.some((c) => c[0] === here)) candidates.push([here, "会话工作目录"])
  for (const [dir, via] of candidates) {
    const r = await readAbs(ctx, evolvePaths(dir).control)
    if (r.error) return { root: dir, via, state: parseControl(null), initialized: false, error: r.error }
    if (r.text != null) return { root: dir, via, state: parseControl(r.text), initialized: true }
  }
  return { root: resolved.root, via: resolved.via, state: parseControl(null), initialized: false }
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

/** 派工/继承/续派：装配自包含派工消息（任务/上下文/交付/验证/纪律/预载——模板由代码保证不漏项）。 */
function renderOrderPrompt(order: Order, coord: Coord, injectedEnv: string[], opts: { resume?: boolean } = {}): string {
  const contextLine = (order.context ?? coord.seedDoc ?? "（无——自行摸底定位）").trim()
  const lines = [
    `任务：${order.task.trim()}`,
    `上下文：${contextLine}`,
    `交付：${(order.deliver ?? "具体产出物与落盘路径（代码+回归测试 / 分析方案 / 基线数据）").trim()}`,
    `验证：${(order.verify ?? coord.verify ?? "（未提供验证命令）——无法运行时说明原因并给出可复现的验证步骤").trim()}`,
    `纪律：${(order.rules ?? coord.rules ?? "原子提交不 push；设计层/公共接口变更只分析不落地").trim()}`,
  ]
  if (opts.resume) {
    // 续派消息必须点名「这是接着干」：同一模板另起一单会让 worker 从头重做或绕开被中断的步骤。
    lines.splice(1, 0, "续派：本会话此前已承接同一演进线的任务（你的上下文里有那段进度）——从中断处继续，把被中断的步骤补齐，不要从头重做、也不要绕开它。")
  }
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
    "生生的读态与收息工具（一息入口）。无参=复命：返回项目坐标、呼吸计数、worker 池、在途、种子队列，以及各 worker 的实时态（运行中/空闲/起始时间/待决交互）与**三类回收线索**（判据都是逐个会话的实测结果，不拿登记猜真值）：待收藏线索（在途且会话可达已空闲）；中道而废（在途且会话**确已不存在**——退回种子）；无从核实（在途无会话线索、别名也不属本会话——不可当可达、也不可当已死，须先核实）。池与在途均以**会话 id 为主键**：同一会话只保留最新一轮（旧轮已被新轮取代，不并列、不标脏）。action=init 从唤醒消息固化项目坐标（初生第一动作）；seed_add/seed_done 择种入队与销账；note 落断点并推进呼吸（每息收尾）；task 管理心跳驱动任务（op=ensure 幂等确保定时任务存在并启用；op=pause 记暂停+通知用户+停用任务；op=resume 重新启用；op=status 只看状态）。control.md（调度状态）由本工具托管——勿用 read/write 手工编辑。",
  parameters: schema(
    {
      action: { enum: ["read", "init", "seed_add", "seed_done", "note", "task"], description: "动作（缺省 read=读态）" },
      root: { type: "string", description: "项目根绝对路径（解析次序：显式参数 > 会话 env SHENGSHENG_ROOT > 本会话 init 记忆 > 工作目录向上探测既有 .evolve/control.md；候选一律核到既有 control.md 才采纳，核不中即往下回落）" },
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
      rememberRoot(ctx, coord.root!)
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
        output: notInitialized(loaded.root, loaded.via, loaded.root ? evolvePaths(loaded.root).control : "")
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
      // 呼吸计数**在此推进**（每息收尾一次）：它是「这条河跑了多久」的唯一计数，
      // 也是断点文本之外唯一能自查的机器可读量——不推进则永远停在初值、与断点叙述相左（登记与真值裂缝）。
      state.breath += 1
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
          "一息流程：shengsheng_state（读态）→ 观复（看 awaiting 与各 worker 实时态）→ shengsheng_collect（收藏/退役）→ 择种布雨（shengsheng_dispatch；迷茫时先 shengsheng_divine）→ shengsheng_state action=note（收息）。",
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
    // 回收线索一律**先探会话、再判生死**：会话是实体，别名只是它在池中的影子——池条目被清、别名漂移
    // 都不等于会话消失。旧判据「别名不在池中 ⇒ 中道而废」会把**会话仍活着并在跑**的在途误判为收不回的
    // 死单（已实测两次的假阳性），也会把这种误判当作「可回收产能」的反面嘴给模型。
    interface Trail {
      i: Inflight
      /** 会话线索（自带列优先，旧格式经池解析）。 */
      sid?: string
      raw?: Live
    }
    const trails: Trail[] = []
    // 控制面不可用时**不探也不判**：那种情况下所有在途都「核不到」，硬判就是把整张表诬告
    // （本行末尾已显式报「仅读磁盘状态」）。
    if (svc) for (const i of state.inflight) {
      // 会话线索自带优先（别名漂移出池也能核实）；旧格式回落到池解析；都没有即无从核实。
      const sid = inflightSession(i, state.workers)
      if (!sid) {
        // 无从解析 = 查证不了 ⇒ **不得当作可达**（谎报「在途且空闲」会嗂给模型一个收不回的实物）
        trails.push({ i })
        continue
      }
      // 顺带核到的会话并入 live：别名不在池中的陈旧在途也因此能在盘上验明生死
      if (!live[sid]) live[sid] = (await observe(ctx, svc, [sid]))[sid] ?? { exists: false, running: false }
      trails.push({ i, sid, raw: live[sid] })
    }
    // 待收藏线索：在途、**会话确实可达且已空闲**（有消息尾可 review、有交付物可入账）。
    // 不另加「别名仍须在池中」：可达会话就是真在途（派工授权随会话走，不在池也收得回来），
    // 多一道影子条件只会把真在途误判成「中道而废」（反方向的假阳性）。
    const awaiting = trails.filter((t) => t.raw?.exists && !t.raw.running).map((t) => t.i)
    // 不可达（会话确实不存在）——退回种子后退役。
    const gone = trails.filter((t) => t.sid && t.raw?.exists === false).map((t) => t.i)
    // **无从核实**（在途无会话线索，旧格式里别名又不属本会话）——单独一栏，不算可达、也不算已死（第三类）：
    // 旧格式在途没会话列、且别名漂移出了池（正是同会话并列条目的历史形态）——此时**无从探会话**。
    // 不归入「中道而废」也不归入「待收藏」：把没探过的东西当已死，与把没探过的东西当可达一样，都是
    // 在登记上猜真值（会话可能还活着、也可能早没了）——归入哪一边都是假信号。
    const unresolved = trails.filter((t) => !t.sid).map((t) => t.i)
    // 虚假产能：池中会话**全部核实为不存在**——复用价值已随会话消失。判据是会话本身的实测结果，
    // 未核实（会话控制面不可用/池空）不得诬告。
    const allGone = !!(svc && active > 0 && state.workers.every((w) => live[w.sessionId]?.exists === false))
    const lines = [
      `坐标（${loaded.via}）：${root}`,
      `- 验证命令：${state.coord.verify ?? "（未固化）"}`,
      state.coord.seedDoc ? `- 种子库：${state.coord.seedDoc}` : "",
      state.coord.rules ? `- 硬约束：${state.coord.rules}` : "",
      `呼吸：${state.breath}｜池 ${active}（${idle} idle，运行中 ${runningNow}）｜在途 ${state.inflight.length}｜种子 ${state.seeds.length}｜收藏待办 ${state.pending.length}`,
      state.note ? `断点：${state.note}` : "",
      active && idle === active && allGone ? "池内 worker 的会话全部不可达（已逐会话核实）——**池中 idle 是虚假产能**：复用价值已随会话消失，布雨须 fresh=true。" : "",
      state.inflight.length ? `在途：\n${state.inflight.map((i) => `- ${i.alias}｜会话 ${idShort(inflightSession(i, state.workers))}｜${i.direction}${i.deliverable ? `｜交付 ${i.deliverable}` : ""}`).join("\n")}` : "在途：（空）",
      state.workers.length ? `池：\n${state.workers.map((w) => liveLine(w.alias, w, live[w.sessionId])).join("\n")}` : "池：（空）",
      awaiting.length
        ? `待收藏线索（在途但已空闲——用 shengsheng_collect review/collect 处理）：\n${awaiting.map((i) => `- ${i.alias}｜${i.direction}`).join("\n")}`
        : "",
      gone.length
        ? [
            "中道而废（在途但会话**确已不存在**——review 取不到消息尾、无交付物可入账：应 retire mode=idle 清池并把方向退回种子队列，**不要**按「待收藏」处理）：",
            ...gone.map((i) => `- ${i.alias}｜${i.direction}`),
          ].join("\n")
        : "",
      unresolved.length
        ? [
            "无从核实（在途无会话线索、别名也不属本会话——登记里查不到它是谁：**既不可当可达、也不可当已死**）：",
            ...unresolved.map((i) => `- ${i.alias}｜${i.direction}`),
            "（处置：先核实（台账/会话列表）再决定——能核到会话则按待收藏收，核不到才 retire mode=idle 退回种子；定向清理用 `action=retire alias=<该条别名>`）",
          ].join("\n")
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
        gone,
        unresolved,
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
  /** 续派：`orders[].session_id` 指向池中既有 worker 会话（不新建、不占新建额度；同样无人值守授权）。 */
  resume?: boolean
  /** 复用/续派命中时的原池条目（沿用其别名与首派时间）。 */
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
    "布雨：把一批工单派给 worker 会话（一息一次调用，1~8 单）。同领域 idle worker 直接复用（摸底归零、记忆延续），异领域或 fresh=true 时新建会话；**续派**——orders[].session_id 指向池中既有 worker 会话（只认本演进线派生的 worker），用于中断后接着干或人工抢跑后续跑（详见 description 末）。派工消息按模板装配（任务/上下文/交付/验证命令/纪律/预载/无人值守提示——代码保证不漏项），注入后立即返回**不等 worker 完成**，并自动登记池与在途。**派工即授权**：本工具的审批就是该 worker 全部动作的授权（新建/复用/续派三条路一视同仁）——worker 以无人值守姿态运行（需审批工具自动通过、无交互通道），不会为单个工具等待确认。活动 worker（运行中+空闲）上限 3：**只挡新建**——复用与续派不改池容量，不受限。派工消息自包含——worker 是普通会话，只凭这一条消息独立干活。不传 orders 或 env SHENGSHENG_DISPATCH=preview 时只出计划不派工（预检）。",
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
            session_id: { type: "string", description: "续派：指定池中既有 worker 会话 id（从中断处接着干、或人工抢跑后续跑——不得用 session send，那条路丢无人值守授权）。只认池中登记的 worker（本演进线派生）；不新建、不占新建额度" },
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
      let resume = false
      let sessionId = order.session_id ?? ""
      let revived: Worker | undefined
      if (sessionId) {
        // 续派：只认**池中登记的 worker 会话**——dispatch 的审批只覆盖本演进线自己派生的会话；
        // 放任意 session id 以 autoApprove 跑，等于把「派工即授权」扩大到用户会话（授权面放大）。
        const known = state.workers.find((w) => w.sessionId === sessionId)
        if (!known) {
          return {
            output: `orders[].session_id=${sessionId} 不是池中登记的 worker 会话——续派只允许本演进线派生的 worker（授权面不外扩）。池中 worker：${state.workers.length ? state.workers.map((w) => `${w.alias}=${w.sessionId}`).join("、") : "（空）"}。接手别的会话请按别名/领域复用或 fresh=true 新建。`,
            data: { blocked: true, unknownSession: sessionId },
          }
        }
        revived = known
        resume = true
      } else if (!order.fresh) {
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
      plans.push({ alias, domain, task: order.task, sessionId, reuse, created: !reuse && !sessionId, revived, resume })
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
        output: [`布雨预检（未派工${svc ? "" : "；会话控制面不可用"}）：`, ...plans.map((p) => `- ${p.alias}｜${p.created ? "新建" : p.resume ? `续派会话 ${p.sessionId}` : "复用（同域 idle）"}｜${p.domain}｜${p.task}`)].join("\n"),
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
        } else {
          // 复用/续派：会话 env 是内存态（服务重启即空）——重注入项目坐标，使派工消息里
          //「环境：本会话已注入…」为真（续跑多发生在中断/重启之后，此时 env 往往已丢）。
          const kind = plan.resume ? "续派" : "复用"
          try {
            await svc!.store.setEnv(plan.sessionId, ctx.user, envPatch)
            plan.note = `${kind}会话 ${plan.sessionId}（项目坐标 env 已重注入）`
          } catch (err) {
            plan.note = `${kind}会话 ${plan.sessionId}（env 重注入失败：${describeError(err)}）`
          }
        }
        if (!plan.sessionId) {
          results.push(`- ${plan.alias}：**未派**（无可用会话且新建失败）`)
          continue
        }
        const live = (await observe(ctx, svc!, [plan.sessionId]))[plan.sessionId]
        if (live && live.exists && live.running) {
          results.push(`- ${plan.alias}：**未派**（会话 ${plan.sessionId} 正在运行——同会话并发注入会被拒；若它卡在待决审批（无人应答约 5 分钟超时），等本轮结束再用 session_id 续派）`)
          continue
        }
        if (live && !live.exists) {
          results.push(`- ${plan.alias}：**未派**（会话 ${plan.sessionId} 不存在——用 collect action=retire 清出池后重派）`)
          continue
        }
        const message = renderOrderPrompt(order, state.coord, Object.keys(envPatch), { resume: plan.resume })
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
        // 池 upsert（**主键 = 会话 id**，复用/续派者保留原别名、领域与首派时间）：round 为本次派工轮次号。
        // 同一会话只留一条——旧轮次被本次取代（round 递增即“已被新轮次取代”的机器可读证据），
        // 不并列、不标脏（陈旧并列条目正是「同一会话两个身份」的登记裂缝来源）。
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
        state.workers = dedupeWorkers([...state.workers.filter((w) => w.sessionId !== plan.sessionId && w.alias !== plan.alias), worker])
        const inflight: Inflight = { alias: plan.alias, sessionId: plan.sessionId, direction: order.task, deliverable: order.deliver ?? "", dispatchedAt: now, round: worker.round }
        // 在途 upsert **按会话 id 取代**——仅按别名过滤会漏掉旧名残留（别名漂移），而它随后可能因轮次
        // 更大而在归一里胜出、顶掉本轮新登记。写侧与读侧（parseControl 的归一）因此共用同一套身份键。
        state.inflight = dedupeInflight([...state.inflight.filter((x) => x.alias !== plan.alias && inflightIdentity(x, state.workers) !== plan.sessionId), inflight], state.workers)
        // 复用代数提示：同一会话连续承接的单数（首派 1）——接近上限时建议收藏后退役换新上下文
        const generation = prev ? prev.round + 1 : 1
        const genNote = generation >= REUSE_GENERATION_HINT ? `｜**已承接 ${generation} 单**（上下文渐胖，宜收藏后退役换新）` : ""
        const kind = plan.created ? "新建" : plan.resume ? "续派" : "复用"
        results.push(`- ${plan.alias}｜${kind}会话 ${plan.sessionId}｜${plan.domain}｜轮次 ${worker.round}｜${plan.task.slice(0, 60)}${genNote}`)
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
    "收藏与退役（回收阶段）。action=review：深观一个 worker（运行态/起始时间/消息数/上下文水位/待决交互/最后一条回复）并给出处置建议（只读）。action=collect：把报告要点追加进演进台账（幂等——同一 worker 只入一次）、worker 标 idle 留池、在途清除、建议下一条自动入种子队列。action=retire：退役 worker——mode=idle 清出池并把在途方向退回种子队列（会话不存在/领域已终）、mode=stop 终止超时运行（先 review 抢救结论）、mode=delete 删除会话（运行中会被拒绝）。**寻址语义**：同一会话在池/在途只保留**当前轮**（会话 id 为主键，旧轮已被新轮取代）——`session_id` 命中的就是该会话的当前轮条目（返回里回工命中的别名与轮次），别名与 session_id 指向同一会话时可只传其一（两者矛盾时拒绝不猜）；别名只在在途、池中无对应 worker 的**陈旧残留**用 `alias=` 定向清理（session_id 寻不到残留）。",
  parameters: schema(
    {
      action: { enum: ["review", "collect", "retire"], description: "动作" },
      alias: { type: "string", description: "worker 别名（池登记用）；**别名优先**——也是清理「池中无对应 worker 的陈旧在途残留」的唯一寻址手段" },
      session_id: { type: "string", description: "worker 会话 id（缺省按别名解析）：按**会话当前轮**命中池条目与其在途（旧轮已归一，不存在「命中较新、陈旧寻不到」）；返回里回工命中的别名与轮次" },
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
    // **寻址语义（session_id 指向会话，不指向「某条登记」）**：
    // - 池：会话 id 为主键，读侧已归一 → 按 session_id 命中的就是该会话的**当前轮**条目；
    // - 在途：同一会话至多一条（写侧 upsert + 读侧归一），故 `alias` 与 `session_id` 指向同一条，
    //   不存在「按 session_id 命中较新、陈旧条目寻不到」的空间（那是「同会话并列两条」时代的病）；
    // - 别名两侧都查：池先命中、再落到在途——别名已漂移出池的**陈旧在途残留**因此仍可定向（见下方 retire 分支）。
    const worker = state.workers.find((w) => (sidArg && w.sessionId === sidArg) || (alias && w.alias === alias))
    const findInflight = (a: string): Inflight | undefined => (a ? state.inflight.find((i) => i.alias === a) : undefined)
    // 别名命中的在途（陈旧残留的别名不属于该会话，只此一路可寻）
    const inflightByAlias = findInflight(alias)
    // 会话 id 命中的在途（在途自带会话列时可用）：池条目已被清、只在途还留着的那条——
    // 按 session_id 也能定向到它（否则一条「会话可达但不在池」的在途既收不回也清不掉）。
    // 同传别名与会话 id 时**先按会话 id 取**（会话是主键）：两者因别名漂移而不一致时，
    // 取别名会把「同一个工人的旧名登记」当成另一个工人——定向清理就清到了错的那条。
    const inflightBySession = sidArg ? state.inflight.find((i) => inflightIdentity(i, state.workers) === sidArg) : undefined
    const stale = inflightBySession ?? inflightByAlias
    const staleKey = inflightBySession ? `session_id=${sidArg}` : `alias=${alias}`
    const candidates = () => [
      state.workers.length ? `池中 worker：\n${state.workers.map((w) => `- ${w.alias}｜${w.domain}｜${w.task}｜${w.status}`).join("\n")}` : "池中无 worker。",
      state.inflight.length
        ? `在途中：\n${state.inflight.map((i) => `- ${i.alias}｜会话 ${idShort(inflightSession(i, state.workers))}｜轮次 ${i.round}｜${i.direction.slice(0, 40)}`).join("\n")}`
        : "在途：（空）",
    ].join("\n")
    if (!worker) {
      // 池中无此 worker，但别名/会话命中一条在途：池条目已被清、登记却还在的**陈旧在途残留**
      // （历史缺陷或人工误改）。它不进「中道而废」列表的寻址范围——清理须定向。
      if (stale) {
        const staleSid = inflightSession(stale, state.workers)
        const where = staleSid ? `会话 ${staleSid}` : "无从核实的会话"
        if (action !== "retire") {
          return {
            output: `按 ${staleKey} 命中的是**只在「在途」里的陈旧登记**（别名 ${stale.alias}｜${where}），池中无对应 worker——${action === "collect" ? "没有池条目可收藏（collect 需要 worker 与其会话）" : "无池条目可深观"}。\n- 在途方向：${stale.direction}（轮次 ${stale.round}）\n- 清掉它用 \`action=retire ${staleKey} mode=idle\`（方向退回种子队列）。`,
            data: { inflight: stale, stale: true },
          }
        }
        const reason = args.reason != null ? String(args.reason).trim() : ""
        state.inflight = state.inflight.filter((i) => i !== stale)
        let back = ""
        if (!state.seeds.some((s) => s.direction === stale.direction)) {
          state.seeds.push({ priority: "待定", direction: stale.direction, source: `在途回退 ${stale.alias}${reason ? `（${reason}）` : ""}`, constraint: "worker 会话不可用，重开时从种子库/台账重新摸底" })
          back = "；方向已退回种子队列"
        } else back = "；方向已在种子队列（未重复）"
        const sw = await saveState(ctx, root, state)
        if (sw.error) return { output: sw.error }
        return {
          output: `已清掉陈旧在途登记：${stale.alias}（轮次 ${stale.round}｜${where}）${back}。池中无对应 worker——无会话可终止/删除（销个会话请走 session 工具或先重新派工）。${reason ? `原因：${reason}` : ""}`,
          data: { inflight: stale, seeds: state.seeds },
        }
      }
      return { output: `未在池中找到该 worker（alias=${alias || "（未传）"}${sidArg ? ` session_id=${sidArg}` : ""}）。${candidates()}` }
    }

    const res: Record<string, unknown> = {}
    // 两参数指向不同条目时的显式提示（读只提示；collect 则拒绝——误寻址不静默取一）
    const addrNote =
      !alias || alias === worker?.alias
        ? ""
        : `alias=${alias} ${state.workers.some((w) => w.alias === alias) ? "与 session_id 指向不同 worker" : "不在池中"}；session_id=${sidArg} 命中的是 ${worker?.alias ?? "（无）"}——请只传一个（工具不猜）`
    const svc = sessionsOf(ctx)
    if (!svc) return { output: "会话控制面不可用（ctx.sessions 未注入）——无法观察/操作 worker 会话。" }
    const live = (await observe(ctx, svc, [worker.sessionId]))[worker.sessionId] ?? { exists: false, running: false }
    // 寻址回工（命中的池条目 + 会话经池解析出的本轮在途）：凡用 session_id 寻址（或委托寻址）都回工，
    if (sidArg) {
      res.alias = worker.alias
      res.inflight = inflightOf(state, worker)[0] ?? null
    } else if (alias && alias !== worker.alias) {
      res.aliasBySession = worker.alias
    }

    // ---- review：深观（只读）----
    if (action === "review") {
      const inflight = inflightOf(state, worker)[0]
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
          sidArg ? `- 寻址：按 session_id=${sidArg} 命中本条——别名 ${worker.alias}｜轮次 ${worker.round}｜在途 ${inflight ? `有（轮次 ${inflight.round}）` : "无"}（同一会话只保留当前轮，无旧轮并列可寻）` : "",
          alias && alias !== worker.alias ? `- 寻址：alias=${alias} 不在池中；仅 session_id 解析到 ${worker.alias}（两者不同——请只传一个）` : "",
          `- 任务：${worker.task}｜派发 ${new Date(worker.dispatchedAt).toLocaleString("zh-CN")}｜池状态 ${worker.status}`,
          `- 现有态：${!live.exists ? "会话不存在" : live.running ? `运行中 ${live.minutes ?? "?"} 分钟` : "空闲"}${live.messages != null ? `｜消息 ${live.messages}` : ""}${live.ctxTokens != null ? `｜ctx ${live.ctxTokens}` : ""}${live.toolCalls ? `｜在途工具 ${live.toolCalls}` : ""}`,
          live.pending?.length ? `- 待决：${live.pending.join("、")}` : "",
          inflight ? `- 在途登记：${inflight.direction}${inflight.deliverable ? `｜交付 ${inflight.deliverable}` : ""}` : "- 在途登记：（无——可能已收藏）",
          live.last ? `- 最后一条回复（截断 ${COLLECT_SNIPPET} 字）：\n${live.last.slice(0, COLLECT_SNIPPET)}` : "- 最后一条回复：**（无文本/未产出）**",
          `处置建议：\n${advice.map((a) => `- ${a}`).join("\n")}`,
        ].filter(Boolean).join("\n"),
        data: { worker, live, inflight: inflight ?? null, advice, ...res },
      }
    }

    // ---- collect：入台账（幂等）+ 标 idle + 在途清除 + 建议下一条入种子 ----
    if (action === "collect") {
      const summary = String(args.summary ?? "").trim()
      if (!summary) return { output: "collect 需要 summary（报告要点：交付/验证证据/未竟事项）——原样入台账。" }
      // 误寻址护栏：显式别名与 session_id 指向**不同** worker 时不得静默取其一。
      // 会话 id 是主键、别名是显示名，正常二者同指；报错不猜——猜错会拿别的会话的报告入账。
      if (addrNote) {
        return {
          output: `寻址冲突：${addrNote}。${candidates()}`,
          data: { conflict: { alias, session_id: sidArg, resolved: worker.alias } },
        }
      }
      const next = args.next != null ? String(args.next).trim() : ""
      // 本轮在途（会话 id 优先）：台账的行数与幂等键都取自它，不能用别名找——别名漂移时会漏掉本轮那条，
      // 幂等键回落到池的轮次上（与派工时写入的键不一致即挡不住真重复）。
      const current = inflightOf(state, worker)[0]
      const stamp = new Date().toISOString()
      // 幂等键：worker 会话 + **轮到哪一轮**（dispatch 写入的单调递增代号）。
      // 不用时间戳：复用时 worker 与 inflight 的时间戳不同源，键会漂移（既挡不住真重复、也可能误伤）。
      // 取值次序为 `max(在途, 池)` 而非 `在途 ?? 池`：在途清掉后再收（或旧版残留的轮次低于池时）
      // 回落到池轮次会造出一个**更小的新键**——同一轮于是被当成新轮重复入账（幂等键一旦回退就不再幂等）。
      const roundKey = Math.max(current?.round ?? 0, worker.round ?? 0)
      const ledgerKey = `collect:${worker.sessionId}:r${roundKey}`
      const block = [
        `<!-- ${ledgerKey} -->`,
        `## ${stamp}｜${worker.alias}｜${worker.domain}`,
        "",
        `- 方向：${current?.direction ?? worker.task}`,
        current?.deliverable ? `- 预期产出：${current.deliverable}` : "",
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
      // 在途清除**按会话 id**（主键）兼按别名：只按别名清不掉「别名已漂移」的那条残留，
      // 而它随后会在读态里重新被报成「待收藏/无从核实」——收藏过的东西又冒出来，是同一裂缝的另一面。
      const cleared = takeInflight(state, worker)
      state.pending = state.pending.filter((p) => p.alias !== worker.alias)
      returnToSeeds(state, cleared.filter((i) => i.alias !== worker.alias), "在途回退", "（别名漂移的陈旧登记）", "worker 会话不可用，重开时从种子库/台账重新摸底")
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
        data: { ledger: led.path, worker, seeds: state.seeds, inflight: state.inflight, ...res },
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
        // 在途取出**按会话 id**（主键）兼按别名：别名漂移的残留也一并带走（否则它永远卡在盘上）
        const inflight = takeInflight(state, worker)
        returnToSeeds(state, inflight, "中道而废", reason, "重开时先读台账上一轮的中断点")
        state.workers = state.workers.filter((w) => w.alias !== worker.alias)
        const sw = await saveState(ctx, root, state)
        if (sw.error) return { output: sw.error }
        return { output: `已终止并退役：${worker.alias}｜会话 ${worker.sessionId}（保留可查）${sidArg ? `｜寻址：按 session_id 命中本条（别名 ${worker.alias}｜轮次 ${worker.round}）` : ""}（在途方向已退回种子队列）。${reason ? `原因：${reason}` : ""}`, data: { worker, seeds: state.seeds, ...res } }
      }
      if (mode === "delete") {
        if (live.running) return { output: `会话 ${worker.sessionId} 正在运行——先 \`action=retire mode=stop\` 终止再删除（删除运行中会话会留下孤儿任务态）。` }
        await svc.store.delete(worker.sessionId, ctx.user)
        const inflight = takeInflight(state, worker)
        returnToSeeds(state, inflight, "在途回退", reason, "")
        state.workers = state.workers.filter((w) => w.alias !== worker.alias)
        state.pending = state.pending.filter((p) => p.alias !== worker.alias)
        const sw = await saveState(ctx, root, state)
        if (sw.error) return { output: sw.error }
        return { output: `已删除会话并退役：${worker.alias}（${worker.sessionId} 进归档，7 天内可由管理员恢复）${sidArg ? `｜寻址：按 session_id 命中本条（别名 ${worker.alias}｜轮次 ${worker.round}）` : ""}。${reason ? `原因：${reason}` : ""}`, data: { worker, seeds: state.seeds, ...res } }
      }
      // mode=idle：仅清出池（在途退回种子）
      const inflight = takeInflight(state, worker)
      let back = ""
      if (inflight.length) {
        const pushed = returnToSeeds(state, inflight, "在途回退", reason, "worker 会话不可用，重开时从种子库/台账重新摸底")
        back = pushed ? `；在途方向已退回种子队列` : `；在途方向已在种子队列（未重复）`
      }
      state.workers = state.workers.filter((w) => w.alias !== worker.alias)
      state.pending = state.pending.filter((p) => p.alias !== worker.alias)
      const sw = await saveState(ctx, root, state)
      if (sw.error) return { output: sw.error }
      return {
        output: `已退役（仅清出池）：${worker.alias}${live.exists ? `——会话 ${worker.sessionId} 保留在磁盘，需彻底清理由 action=retire mode=delete` : "（会话已不存在）"}${sidArg ? `｜寻址：按 session_id 命中本条（别名 ${worker.alias}｜轮次 ${worker.round}）` : ""}${back}${reason ? `。原因：${reason}` : ""}`,
        data: { worker, seeds: state.seeds, ...res },
      }
    }

    return { output: `未知 action: ${action}（可用：review / collect / retire）` }
  },
}

// ---------------------------------------------------------------------------
// divine：问卦（择的探索哨兵）
// ---------------------------------------------------------------------------

/** 台账里卦史的记录键前缀（与 `collect:` / `pause:` 并列，各自独立不吞噬）。 */
const DIVINE_KEY = "divine"
/** 问句哈希长度（**定长** hex）：`appendLedger` 的幂等判据是 `md.includes(key)` 的**子串**匹配，
 *  变长键会让短的前缀被长的吞掉（`divine:3:ab` 命中 `divine:3:abc` 的记录）——定长即无此患。 */
export const DIVINE_HASH_LEN = 8
/** 卦史警示窗口：近 N 息内已问 ≥ K 卦时给模型一句提醒（**不拒**——要证据在场与迷茫，最终仍由它断）。 */
export const DIVINE_WARN_WINDOW = 5
export const DIVINE_WARN_COUNT = 3

/** 问句哈希（定长 hex，FNV-1a 双流）：仅作**记录与回看**的标识，不参与幂等——
 *  幂等键若掺问句，换个说法就能重掷，「初筮告再三渎」的闸等于没有。 */
export function questionHash(question: string): string {
  const norm = question.replace(/\s+/g, " ").trim()
  let h1 = 0x811c9dc5
  let h2 = 0x01000193
  for (let i = 0; i < norm.length; i++) {
    const c = norm.charCodeAt(i)
    h1 = Math.imul(h1 ^ c, 0x01000193) >>> 0
    h2 = Math.imul(h2 ^ (c + i), 0x85ebca6b) >>> 0
  }
  return (h1.toString(16).padStart(8, "0") + h2.toString(16).padStart(8, "0")).slice(0, DIVINE_HASH_LEN)
}

/** 掷值（0..2^32-1）：取**密码学随机源**，不用 `Date.now()`——
 *  后者的分布由「模型何时决定调用」决定，越犹豫越集中于常用区间，等于让被随机化者提供随机性。 */
function drawValue(): number {
  try {
    return crypto.getRandomValues(new Uint32Array(1))[0]
  } catch {
    return Math.floor(Math.random() * 0x100000000)
  }
}

/** 时辰名（子一 … 亥十二），与 {@link hourIndexOf} 同序。 */
const HOUR_NAMES = ["子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"] as const
const WEEKDAYS = ["日", "一", "二", "三", "四", "五", "六"] as const

/** 地气＝当前分支名（只读 `.git/HEAD` 一行）。
 *  **不解析 HEAD hash、不跟随 gitdir（worktree）、不回退 packed-refs**：它只用于断卦合参、不入公式，
 *  而那三种布局（worktree 的 `.git` 是文件、gc 后的 packed-refs、bare）各有代价；读不到一律写「未取」。 */
async function readGitBranch(ctx: ToolContext, root: string): Promise<string> {
  const r = await readAbs(ctx, `${root.replace(/[\\/]+$/, "")}/.git/HEAD`)
  if (r.error || r.text == null) return "未取"
  const m = /^ref:\s*refs\/heads\/(.+)$/m.exec(r.text.trim())
  return m ? m[1].trim() : "未取（游离 HEAD 或非常规布局）"
}

/** 台账尾部的最近几条记录标题（不入公式，仅供断卦合参）。 */
function ledgerTail(md: string, n: number): string[] {
  return md
    .split(/\r?\n/)
    .filter((l) => /^##\s/.test(l.trim()))
    .map((l) => l.trim().slice(2).trim())
    .slice(-n)
}

/** 卦史：台账里带 `<!-- divine:{breath}:{hash} -->` 的已问息数。 */
function divineBreaths(md: string): number[] {
  const out: number[] = []
  const re = /<!--\s*divine:(\d+):/g
  let m: RegExpExecArray | null
  while ((m = re.exec(md))) out.push(Number(m[1]))
  return out
}

const divineTool: Tool = {
  name: "divine",
  description:
    "问卦（择的探索哨兵）——前途迷茫时起一卦：环境数据入卦（呼吸/在途/池/种子/时辰/分支）、卦象入台账、卦指即落向。**只在迷茫时用**：岔口（≥2 候选证据平衡，传 candidates 定卦）、方向枯（种子空而河将断未断，不传 candidates 开卦）、势乱（大裂痕刚清完）。**证据明确、有待收成果、裂缝在场时不问**（问卦是渎）；无向可择的枯才问，需人拍板/环境受限的有向枯是缺门、不问。**一息一卦**（初筮告，再三渎，渎则不告）：同息再问不掷新卦，返回前卦原文——没有这道闸，模型会重掷到自己喜欢的答案。卦只给方向域，具体落向由你断（象/境/合参/落向）；定卦模式下「卦指即落」，但**卦不得覆盖硬约束**（违约束则弃卦并记一行理由），且**卦决定方向不等于保证可派**（池满派不出就把方向落种子、记「卦落未派」，不重掷）。卦落之向本息内不翻悔，新证据到下一息可改道（台账记「卦改判」）。不派 worker、不执行命令、不写 control.md。",
  parameters: schema(
    {
      question: { type: "string", description: "迷茫之问（一句话，入台账）——须是真的迷茫：证据平衡的岔口、方向枯、势乱" },
      candidates: { type: "array", items: { type: "string" }, description: "定卦模式：候选清单（通常为种子队列同优先级条目）。传即定卦（卦指即落），不传即开卦（卦只给方向域）" },
      root: { type: "string", description: "项目根绝对路径（解析次序与 state 一致：显式参数 > 会话 env SHENGSHENG_ROOT > 本会话 init 记忆 > 工作目录向上探测）" },
    },
    ["question"],
  ),
  // 与 collect 同级：只读 + 台账追加（不派会话、不执行命令、不写 control.md）
  requiresApproval: false,
  outputSchema: schema(
    {
      mode: { title: "模式", type: "string", enum: ["定卦", "开卦"] },
      question: { title: "问", type: "string" },
      hexagram: {
        title: "卦卡",
        type: "object",
        properties: {
          name: { title: "本卦", type: "string" },
          upperName: { title: "上卦", type: "string" },
          lowerName: { title: "下卦", type: "string" },
          moving: { title: "动爻", type: "number" },
          movingLabel: { title: "爻位", type: "string" },
          changed: { title: "变卦", type: "string" },
          yong: { title: "用卦", type: "string" },
          saying: { title: "生生断", type: "string" },
          lineSemantics: { title: "爻位语义", type: "string" },
        },
        required: ["name", "moving", "changed"],
      },
      pick: { title: "卦指", type: "object", description: "定卦模式才有：指中的候选序号（自 1 起）与候选原文", properties: { index: { type: "number" }, candidate: { type: "string" } } },
      env: { title: "环境快照", type: "object", description: "感天时得的实测值（卦卡同源）" },
      inputs: { title: "起卦输入", type: "object", description: "全部起卦输入（含掷值）——卦可据此复算回放" },
      replayKey: { title: "回放键", type: "string" },
      replayed: { title: "是否回放", type: "boolean", description: "true = 同息再问，未掷新卦，返回前卦原文" },
      ledgerKey: { title: "记录键", type: "string" },
      accounted: { title: "是否已记档", type: "boolean" },
      note: { title: "附注", type: "string" },
    },
    ["mode", "hexagram", "env", "inputs", "replayed"],
  ),
  async execute(args, ctx) {
    const question = String(args.question ?? "").trim()
    if (!question) return { output: "divine 需要 question（迷茫之问，一句话）——空问不成卦。" }
    const rootArg = args.root != null ? String(args.root) : undefined
    const loaded = await loadState(ctx, rootArg)
    if (loaded.error) return { output: loaded.error }
    if (!loaded.initialized || !loaded.root) {
      return { output: `未初始化：内势无据（呼吸/池/在途都读不到），先 \`shengsheng_state action=init\` 固化坐标。${notInitialized(loaded.root, loaded.via, "")}` }
    }
    const root = loaded.root
    const state = loaded.state

    // 台账全文取一次：**一息一卦的闸与卦史都从它读**（台账是卦的唯一持久记忆）。
    const led = await readAbs(ctx, evolvePaths(root).ledger)
    if (led.error) return { output: `台账不可读（${led.error}）——卦可掷但无处记档，且无法判定同息是否已问：先修台账可达性。` }
    const ledgerMd = led.text ?? ""
    const prior = divineBreaths(ledgerMd)

    // ---- 一息一卦（初筮告，再三渎，渎则不告）----
    // 键只含呼吸、**不含问句哈希**：换一个说法就能重掷的闸等于没有。
    // 定卦与开卦共用这一把锁（同息先开卦再定卦同样被挡）。
    const already = prior.includes(state.breath)
    if (already) {
      const at = ledgerMd.indexOf(`<!-- ${DIVINE_KEY}:${state.breath}:`)
      const rest = at >= 0 ? ledgerMd.slice(at) : ""
      const next = rest.indexOf("<!--", 5)
      const block = (next > 0 ? rest.slice(0, next) : rest.slice(0, 2000)).trim()
      return {
        output: [
          `再三渎，渎则不告——第 ${state.breath} 息已问过一卦，本息不再掷。前卦原文：`,
          "",
          block || "（台账里该息的卦块读不出原文）",
          "",
          "卦落之向本息内不翻悔；新证据（worker 回报 / 深观发现）到**下一息**可改道（台账记「卦改判」及理由）。",
        ].join("\n"),
        data: { mode: "（未掷）", answer: "渎则不告", breath: state.breath, replayed: true, replayKey: `${DIVINE_KEY}:${state.breath}`, archived: block, accounted: true },
      }
    }

    // ---- 感天时：全部实测，不造数 ----
    const now = new Date()
    const month = now.getMonth() + 1
    const day = now.getDate()
    const hourIndex = hourIndexOf(now.getHours())
    const running = state.workers.filter((w) => w.status !== "idle").length
    const draw = drawValue()
    const branch = await readGitBranch(ctx, root)

    const inputs: DivineInputs = {
      breath: state.breath,
      inflight: state.inflight.length,
      running,
      seeds: state.seeds.length,
      seq: state.seq,
      month,
      day,
      hourIndex,
      draw,
    }
    const hex = buildHexagram(inputs)
    const replayKey = [DIVINE_KEY, state.breath, month, day, hourIndex, state.seq, state.seeds.length, state.inflight.length, running, draw].join(":")

    // ---- 定卦：候选入卦，卦指即落 ----
    const cands = Array.isArray(args.candidates) ? (args.candidates as unknown[]).map((c) => String(c ?? "").trim()).filter(Boolean) : []
    const mode = cands.length ? "定卦" : "开卦"
    let pick: { index: number; candidate: string } | undefined
    let pickLine = ""
    if (cands.length) {
      const index = pickCandidate(hex, cands.length, draw)
      pick = { index, candidate: cands[index - 1] }
      pickLine = `卦指：第 ${index} 候选（共 ${cands.length}）——**${pick.candidate}**`
    }

    // ---- 环境快照（原文与数值都呈给模型；工具只呈事实、不解读）----
    const prio = state.seeds.reduce<Record<string, number>>((acc, s) => {
      const k = s.priority || "待定"
      acc[k] = (acc[k] ?? 0) + 1
      return acc
    }, {})
    const prioText = Object.entries(prio).map(([k, v]) => `${k}×${v}`).join(" ") || "（无）"
    const idle = state.workers.length - running
    const recent = ledgerTail(ledgerMd, 4)
    const near = prior.filter((b) => b > state.breath - DIVINE_WARN_WINDOW && b < state.breath)

    const envLines = [
      `- 天时：${HOUR_NAMES[hourIndex - 1]}时（${hourIndex}）｜星期${WEEKDAYS[now.getDay()]}｜${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}`,
      `- 内势：呼吸 ${state.breath}｜池 ${state.workers.length}（${running} 跑 / ${idle} 闲）｜在途 ${state.inflight.length}`,
      `- 粮草：种子 ${state.seeds.length}（${prioText}）`,
      `- 地气：分支 ${branch}`,
      `- 近收：${recent.length ? recent.join("；") : "（台账无记录）"}`,
    ]

    const card = [
      `# 卦·第 ${state.breath} 息｜${hex.name}`,
      "",
      `- 卦：**${hex.name}**（上${hex.upperName}${trigramOf(hex.upper).symbol} 下${hex.lowerName}${trigramOf(hex.lower).symbol}）动**${hex.movingLabel}**爻（${hex.moving}）→ 变**${hex.changed}**`,
      `- 体用：用 ${hex.yong.name}（${hex.yong.side}——事在${hex.yong.scope}）｜体 ${hex.ti.name}（${hex.ti.side}）`,
      `- 生生断：${hex.saying}`,
      `- 爻位：${hex.movingLabel}＝${hex.lineSemantics}`,
      pickLine ? `- ${pickLine}` : `- 开卦：卦只给方向域（爻位 + 生生断 + 变卦之势），据此择一条新方向入种子再布雨`,
    ]
      .filter(Boolean)
      .join("\n")

    const envBlock = ["## 环境快照（实测）", "", ...envLines].join("\n")
    const frame = [
      "## 断卦框架（你来填，四行）",
      "",
      "1. **象**：本卦→变卦，是什么势；",
      "2. **境**：环境快照里最硬的一个事实；",
      "3. **合参**：卦象呼应了环境里哪件事；",
      "4. **落向**：具体动作（seed_add / dispatch / 修门 / 停）。",
      "",
      mode === "定卦"
        ? "定卦模式：**卦指即落**——迷茫即证据平衡，卦的全部价值就是终结选择。布雨照常走 dispatch 纪律（同域复用、池限额一概不变）；**卦不得覆盖硬约束**（候选违约束则弃卦并记一行理由），**派不出不算渎**（落种子记「卦落未派」，不重掷）。"
        : "开卦模式：卦只给方向域，据爻位 + 生生断 + 变卦之势择一条新方向 seed_add（可回读台账找与卦象呼应的旧线索），再布雨。",
    ].join("\n")

    // ---- 落卦：记档（幂等）+ 承诺 ----
    const stamp = now.toISOString()
    const hash = questionHash(question)
    const ledgerKey = `${DIVINE_KEY}:${state.breath}:${hash}`
    const block = [
      `<!-- ${ledgerKey} -->`,
      `## 卦·${stamp}｜第 ${state.breath} 息`,
      "",
      `- 问：${question}`,
      `- 卦：${hex.name}（上${hex.upperName}下${hex.lowerName}）动${hex.movingLabel}爻 → 变${hex.changed}｜用${hex.yong.name}（${hex.yong.side}·事在${hex.yong.scope}）`,
      `- 天时：${HOUR_NAMES[hourIndex - 1]}时｜星期${WEEKDAYS[now.getDay()]}｜${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}`,
      `- 内势：呼吸 ${state.breath}｜池 ${running} 跑 ${idle} 闲｜在途 ${state.inflight.length}`,
      `- 粮草：种子 ${state.seeds.length}（${prioText}）`,
      `- 地气：分支 ${branch}`,
      `- 近收：${recent.join("；") || "（无）"}`,
      pick ? `- 卦指：第 ${pick.index} 候选／${cands.length}——${pick.candidate}` : `- 开卦：${hex.saying}`,
      `- 起卦输入（可复算）：${replayKey}`,
    ].join("\n")
    const appended = await appendLedger(ctx, root, `<!-- ${ledgerKey} -->`, block)
    let note = ""
    if (appended.error) note = `台账不可写（${appended.error}）——**未记档的卦不算掷过**，下息可重问。`
    else if (!appended.appended) note = "台账已有该息的卦（并发写入被另一路先落）——以台账上那一卦为准。"

    // 滥用提示（不拒）：问卦比硬想便宜，纪律只写在提示词里易被自己绕过。
    const warn = near.length >= DIVINE_WARN_COUNT ? `\n\n⚠ 近 ${DIVINE_WARN_WINDOW} 息已问 ${near.length} 卦（本息为第 ${near.length + 1}）——**证据在场时问卦是渎**；裂缝在场先修门、有待收先收，比问卦大。` : ""

    return {
      output: [card, "", envBlock, "", frame, note ? `\n（落卦：${note}）` : `\n（落卦：已记档 ${appended.path}｜${ledgerKey}）`].join("\n") + warn,
      data: {
        mode,
        question,
        hexagram: {
          name: hex.name,
          upper: hex.upper,
          lower: hex.lower,
          upperName: hex.upperName,
          lowerName: hex.lowerName,
          moving: hex.moving,
          movingLabel: hex.movingLabel,
          changed: hex.changed,
          yong: `${hex.yong.name}（${hex.yong.side}·事在${hex.yong.scope}）`,
          saying: hex.saying,
          lineSemantics: hex.lineSemantics,
        },
        pick: pick ?? null,
        env: { month, day, hourIndex, hourName: HOUR_NAMES[hourIndex - 1], weekday: WEEKDAYS[now.getDay()], branch, breath: state.breath, pool: state.workers.length, running, idle, inflight: state.inflight.length, seeds: state.seeds.length, priority: prio, recent },
        inputs,
        replayKey,
        replayed: false,
        ledgerKey,
        accounted: !appended.error && appended.appended,
        note: note || null,
      },
    }
  },
}

// ---------------------------------------------------------------------------
// 定义
// ---------------------------------------------------------------------------

export const name = "shengsheng"
export const description =
  "生生（shengsheng）——《周易》「生生之谓易」。项目驻留自主演进的调度中枢：由定时任务周期唤醒（sticky 会话），每息择种下一个最值得做的方向、派 worker 会话并行干落地（编码/验证/分析）、回收成果入台账——控制平面自己永不干活，重活全在 worker，死一跳不丢全局。自带四工具（state=读态/坐标/择种/断点/心跳任务；dispatch=布雨派工；collect=深观/收藏/退役；divine=问卦——迷茫时起一卦，环境数据入卦、卦象入台账、卦指即落向，一息一卦），`.evolve/control.md` 与台账由工具托管。适合给任意 git 项目搭「持续变好、无人值守」的演进流水线。当用户说「演进」「驻留开发」「持续改进这个项目」「算卦/问卦/迷茫问向」或要为项目创建自主演进任务时装载。输入：驱动任务的唤醒消息（含项目根/验证命令/种子库等项目坐标）。"
export const systemPrompt = systemPromptBase.endsWith("\n") ? systemPromptBase : `${systemPromptBase}\n`
export const tools: Record<string, Tool> = { state: stateTool, dispatch: dispatchTool, collect: collectTool, divine: divineTool }
/** 审批：布雨会派生会改仓库的会话（派生前确认）；state 的原子动作按分支动态门控（见工具定义）；收藏/退役是回收动作、问卦是只读+台账追加，皆免审批。 */
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
