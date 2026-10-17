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
// 会话派生的**子孙进程树**（回收 mode=stop 的伴随动作）与工作树回退态告警：判据（进程表/环境标记/
// pid 复用）与调度逻辑无关，独立成模块——干跑与真杀因此走**同一份**计划代码（见 proc-tree.ts 头部）。
import {
  PROC_MARKER_ENV,
  detectRollbackResidue,
  planReap,
  readProcTable,
  readSessionMarkers,
  reapProcesses,
  renderPlan,
  renderReapResult,
  renderResidue,
  protectLineage,
  type ProcRow,
  type ReapPlan,
  type ReapResult,
} from "./proc-tree"
// 提交范围（宽范围暂存的纪律与判据）：`review` 时核「最近一次提交 vs 该 worker 工单提到的路径」，
// 派工模板里注入同源的追加式暂存命令。判据与调度逻辑无关，独立成模块（见 commit-scope.ts 头部）。
import {
  commitProtocolLine,
  describeFile,
  mentionPaths,
  readLastCommit,
  readWorkTree,
  scopeAudit,
  untrackedOf,
  type CommitFile,
} from "./commit-scope"
// `pgrep -f` 自匹配活锁的识别：`review` 的处置建议里报「疑似 pgrep 自匹配活锁」+ 正确等待写法；
// 派工模板里注入**同源的**等待纪律（`waitProtocolLine`）——纪律与检测不会各说一套。
// 判据（命令文本 + 进程表）与调度逻辑无关，独立成模块（见 pgrep-lock.ts 头部两次实测现场）。
import { REPEAT_THRESHOLD, detectPgrepLock, waitProtocolLine, type PgrepLockVerdict } from "./pgrep-lock"

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
    /** **模型上下文窗口**（token，0/缺失 = 未知）。可选：老引擎/夹具不提供时回落 env → 缺省值，
     *  但回显必须报出数源（见 {@link ctxScale}）——硬编码“512K”在 `.env` 改小时会静默说错话。 */
    contextWindow?(): number
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

/**
 * **独占资源租约**（control.md 「资源租约」表的行）。
 *
 * ## 为什么需要它（两度实证，都是本仓真发生的事故）
 *
 * 生生一息里能同时派多个 worker，它们共享**同一台机器**的 CPU／磁盘／构建缓存，而**其中有些结论只在
 * 「机器干净」时成立**——负载敏感的性能测量与最终门禁。已实测两次：
 *
 * ① x11-2buf 末条「**现在等机器空闲后开始测量**」与 red-case「**首先要等负载降下来**」在等**同一种稀缺
 *    资源**：谁先看到 load 降谁先跑，另一个随后也跑——两套测量互相污染，而两个结论都依赖「负载干净」。
 * ② red-case 为复现 pty 竞态**故意**起 10 个并发 `st build`（load 峰值 **55**），而 x11-2buf 正在同机跑最终
 *    门禁 `st test`。那次 `184 passed / 0 failed` **只因时间错开**（出结果时 load 已降到 1.05）才没被污染
 *    ——**纯偶然，不是设计**；若错开失败，它就是一条假绿或假红。
 *
 * 所以「谁在独占这台机器」必须是**登记**（盘上可见、可顺延、可回收），而不是两个 worker 各自的自觉。
 *
 * ## 语义（与「活动 worker 上限」同型：只挡**申请同一资源的人**，不挡无关工单）
 *
 * - **资源名**是自由串（缺省 {@link DEFAULT_EXCLUSIVE_RESOURCE}）。同一资源名**同时至多一位持有者**；
 *   不同资源名互不干涉（日后「独占某端口」「独占某基准数据文件」可各自成行，不必改结构）。
 * - **占用时顺延**：申请时该资源已被**别人**持着 → **不派**、把方向退回种子、下息再派（不硬来、不静默丢）。
 * - **续期而非重占**：同一持有者（同一会话）再申请同一资源 = 续期（`start` 保留、`expireAt` 前推），
 *   **不会**被自己挡住（与「复用与续派不占新建额度」同型）。
 * - **释放**：`collect` / `retire` 自动释放该 worker 的租约；`state` 读态做**超期回收**（持有者已转 idle／
 *   会话已不存在／已到期）——否则一条死租约会**永久堵住院子**，而堵住的恰好是「要独占才敢做」的那类活。
 * - **到期时刻只是兜底**：`expireAt = 派发时刻 + 租期`（env `SHENGSHENG_LEASE_MINUTES`，缺省
 *   {@link DEFAULT_LEASE_MINUTES}）。它防的是「持有者悄悄死掉而登记没被碰」（服务重启、会话被外部删掉）；
 *   不防「活着的 worker 超时」——那种情形由 `collect`/`retire` 与前面两条真值判据兜。
 */
export interface Lease {
  /** 资源名（同一名字同时至多一位持有者）。 */
  resource: string
  /** 持有者别名（回显用：要能说出「被谁占着」）。 */
  alias: string
  /** 持有者会话 id（**主键**：别名会漂移，会话才是本体——与池/在途同一套身份口径）。 */
  sessionId: string
  /** 起租时刻（ms；续期不改它）。 */
  start: number
  /** 到期时刻（ms；兜底回收判据，见类型注释）。 */
  expireAt: number
  /** 用途（人读：哪一条测量/门禁、为什么必须独占）。 */
  purpose: string
}

/**
 * **独占资源租约**（control.md 「资源租约」表的行）。
 *
 * ## 为什么需要它（两度实证，都是本仓真发生的事故）
 *
 * 生生一息里能同时派多个 worker，它们共享**同一台机器**的 CPU/磁盘/构建缓存，而**其中有些结论只在
 * 「机器干净」时成立**——负载敏感的性能测量与最终门禁。已实测两次：
 *
 * ① x11-2buf 末条「**现在等机器空闲后开始测量**」与 red-case「**首先要等负载降下来**」在等**同一种
 *    稀缺资源**：谁先看到 load 降谁先跑，另一个随后也跑——两套测量互相污染，而两个结论都依赖「负载干净」。
 * ② red-case 为复现 pty 竞态**故意**起 10 个并发 `st build`（load 峰值 **55**），而 x11-2buf 正在同一台机跑
 *    最终门禁 `st test`。那次 `184 passed / 0 failed` **只因时间错开**（出结果时 load 已降到 1.05）才没被污染
 *    ——**纯偶然，不是设计**；若错开失败，它就是一条假绿或假红。
 *
 * 所以「谁在独占这台机器」必须是**登记**（盘上可见、可顺延、可回收），而不是两个 worker 各自的自觉。
 *
 * ## 语义（与「活动 worker 上限」同型：只挡**申请方**，不档无关工单）
 *
 * - **资源名**是一个自由串（缺省 {@link DEFAULT_EXCLUSIVE_RESOURCE}）。同一资源名**同时至多一位持有者**；
 *   不同资源名互不干涉（日后「独占某端口」「独占某基准数据文件」可各自成行，不必改结构）。
 * - **占用时顺延**：申请时该资源已被**别人**持着 → **不派**、把方向退回种子、下息再派（不硬来、不静默丢）。
 * - **续期而非重占**：同一持有者（同一会话）再申请同一资源 = 续期（start 保留、expireAt 前推），
 *    **不会**被自己挡住（与「复用与续派不占新建额度」同型）。
 * - **释放**：`collect` / `retire` 自动释放该 worker 的租约；`state` 读态做**超期回收**（持有者已转 idle /
 *    会话已不存在 / 已到期）——否则一条死租约会**永久堵住院子**，而堵住的恰好是「要独占才敢做」的那类活。
 * - **到期时刻**只是兜底：`expireAt = 派发时刻 + 租期`（`SHENGSHENG_LEASE_MINUTES`，缺省 {@link DEFAULT_LEASE_MINUTES}）。
 *    它防的是「持有者悄悄死掉而登记没被碰」（服务重启、dump 里的会话消失）——不防「活着的 worker 超时」。
 */
export interface Lease {
  /** 资源名（同一名字同时至多一位持有者）。 */
  resource: string
  /** 持有者别名（回显用：要能说出「被谁占着」）。 */
  alias: string
  /** 持有者会话 id（**主键**：别名会漂移，会话才是本体——与池/在途同一套身份口径）。 */
  sessionId: string
  /** 起租时刻（ms；续期不改）。 */
  start: number
  /** 到期时刻（ms；兜底回收判据，见类型注释）。 */
  expireAt: number
  /** 用途（人读：哪一条测量/门禁、为什么必须独占）。 */
  purpose: string
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
  /** 独占资源租约（见 {@link Lease}）：**同一资源名同时至多一条**（读侧归一，写侧 upsert）。 */
  leases: Lease[]
  note?: string
}

const COORD_LABELS: Array<[keyof Coord, string]> = [
  ["root", "项目根"],
  ["verify", "验证命令"],
  ["seedDoc", "种子库"],
  ["rules", "硬约束文档"],
  ["preload", "worker 预载提示"],
]

const EMPTY: EvolveState = { coord: {}, breath: 0, seq: 0, workers: [], inflight: [], seeds: [], pending: [], leases: [] }

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

// ---------------------------------------------------------------------------
// 独占资源租约（见 `Lease`）
// ---------------------------------------------------------------------------

/** 缺省资源名：**同一台机器的 CPU/磁盘/构建缓存**（负载敏感的测量与门禁都申请它）。
 *
 * 为何把缺省名定为它、而不把“独占”做成布尔的 `exclusive: true`：两者**同时支持**——`exclusive: true`
 * 就是 `resource = 本常量` 的简写（派工消息与注释里说清），而 `resource: "<名>"` 留给日后更细的抢占
 * （如 `port:9222`、`bench:baseline.json`）。语义只一条：**同名同时至多一位持有者**。 */
export const DEFAULT_EXCLUSIVE_RESOURCE = "cpu-exclusive"

/** 租期缺省值（分钟）：到期是**兜底**回收判据（防“持有者悄悄死掉而登记没被碰”），不是正常释放路径。
 *  正常释放在 `collect`/`retire`；超期回收在 `state` 读态。取 90 分钟（一条测量/门禁通常远短于它;
 *  而 worker 真在跑长任务时，`collect` 前租约仍由会话存活与持有登记守着）。 */
export const DEFAULT_LEASE_MINUTES = 90

/** 租期硬上界（分钟）：防 env 误设（多写一个零）把一条租约变成永久拘禁。 */
export const MAX_LEASE_MINUTES = 24 * 60

/** “刚领的租约不能因瞬时无运行态就回收”的宽限（ms）。
 *
 * 为何必需（实测风险）：派工返回后 worker 的会话由引擎管启动，**开跑到 isRunning 为真是有延迟的**；
 * 而 `dispatch` 末尾会立刻 `loadState` 回显，下一息开头也会马上调 `state`。若“未运行”就回收，
 * **自己刚领的租约会在同一息内被自己撕掉**——紧接着的第二单就进来了（又变成两套测量互污染）。
 * 取 3 分钟：远大于启动延迟，但远小于一条测量/门禁的实际时长；**超过宽限还 idle 就是真 idle**。
 * 这条宽限**只管“空闲”一档**：会话已不存在、已到期两档不受它约束。 */
export const LEASE_START_GRACE_MS = 3 * 60_000

/** 租期解析（每次调用现算）：env `SHENGSHENG_LEASE_MINUTES` 形如 `90`。 */
export function leaseMinutes(env?: Record<string, string | undefined>): number {
  const raw = (env?.SHENGSHENG_LEASE_MINUTES ?? "").trim()
  if (!/^\d+$/.test(raw)) return DEFAULT_LEASE_MINUTES
  return Math.max(1, Math.min(Number(raw), MAX_LEASE_MINUTES))
}

/** 资源名归一（trim + 小写）：`cpu-exclusive` 与 ` CPU-Exclusive ` 是同一个资源——否则一个空格就能绕开租约。 */
export const resourceKey = (s: string): string => s.trim().toLowerCase()

/**
 * 租约表**就地归一**：同一资源名只保留一条。
 *
 * 为何读侧也要归一（不光写侧 upsert）：`control.md` 是磁盘上的共享态——手改、旧版并行写、或「一边回收
 * 一边写回」都可能留下「同一资源两个持者」。不归一的话，一个持者释放后另一条死租约还在，**永久堵院**
 * （本单要消灭的正是这种“登记与真值裂缝”）。胜者取**到期最晚**那条（并列时取靠后一条，视作同轮重复登记）；
 * 与 {@link dedupeState} 的池/在途归一同一形状：保持幸存的条目**原相对次序**，渲染确定、往返幂等。
 */
function dedupeLeases<T extends Lease>(leases: T[]): T[] {
  const winners = new Map<string, T>()
  const superseded = new Set<T>()
  for (const l of leases) {
    const key = resourceKey(l.resource)
    // 无资源名的条目**无从判定同源**：一律保留（宁可多留一条登记，不可错杀一个真持者）
    if (!key) continue
    const cur = winners.get(key)
    if (!cur) {
      winners.set(key, l)
      continue
    }
    if ((l.expireAt || 0) >= (cur.expireAt || 0)) {
      superseded.add(cur)
      winners.set(key, l)
    } else superseded.add(l)
  }
  return leases.filter((l) => !superseded.has(l))
}

/** 该资源名当前的持者（归一后同一名字至多一条）。 */
function leaseOf<T extends Lease>(state: { leases: T[] }, resource: string): T | undefined {
  const key = resourceKey(resource)
  return dedupeLeases(state.leases).find((l) => resourceKey(l.resource) === key)
}

/** 持者是否是**这个** worker（会话为主键、兼按别名——与池/在途同源口径）。 */
const leaseHeldBy = (l: Lease, sessionId: string, alias?: string): boolean =>
  (!!sessionId && l.sessionId === sessionId) || (!!alias && l.alias === alias)

/** 释放该 worker 的全部租约（就地改），返回被释放的条目（供回显）。 */
function releaseLeases(state: EvolveState, sessionId: string, alias?: string): Lease[] {
  const hit = state.leases.filter((l) => leaseHeldBy(l, sessionId, alias))
  if (hit.length) state.leases = state.leases.filter((l) => !hit.includes(l))
  return hit
}

/** 租约到期时刻的**可读形**（回显用：要能说清“何时到期”）——带绝对时刻与剩余分钟两个口径：
 *  只说「下息」不够（模型会算错呼吸节奏），只说绝对时刻也不好算「还能不能等」。 */
function leaseExpiryText(l: Lease, now = Date.now()): string {
  const abs = new Date(l.expireAt).toLocaleString("zh-CN")
  const left = Math.round((l.expireAt - now) / 60000)
  return left > 0 ? `${abs}（还有约 ${left} 分钟）` : `${abs}（已到期）`
}

/** 在**锁内**释放某 worker 的租约（返回被释放的条目）。
 *  `collect`/`retire` 与“派工中途失败”共用它：释放也必须走 `mutateControl`——
 *  另开裸读改写会与并行派工/回收互相覆盖（本模块已实测过的丢写形态）。
 *
 *  **两个域都释放**（本地 control.md + 仓级共享表）：只放一个就会留下“半个租约”——
 *  本地看着空着、另一条线仍挡着，而这种裂缝恰好只在跨线的时侯才显形（最难查）。 */
async function releaseWorkerLeases(
  ctx: ToolContext,
  root: string,
  sessionId: string,
  alias?: string,
): Promise<{ released: AnyLease[]; error?: string }> {
  const m = await mutateControl<Lease[]>(ctx, root, (cur) => {
    const hit = releaseLeases(cur, sessionId, alias)
    return hit.length ? { next: cur, result: hit } : { result: [] }
  })
  const repo = await releaseRepoLeases(ctx, root, sessionId, alias)
  const released: AnyLease[] = [...(m.result ?? []), ...repo.released]
  if (m.error) return { released, error: m.error }
  if (repo.error) return { released, error: repo.error }
  return { released }
}

/** 释放本线在**仓级共享表**上持有的租约（只动 `root` 属于本线的条目）。 */
async function releaseRepoLeases(
  ctx: ToolContext,
  root: string,
  sessionId: string,
  alias?: string,
): Promise<{ released: RepoLease[]; error?: string }> {
  const probe = await repoTopOf(ctx, root)
  if (!probe.top) return { released: [] }
  const mine = rootKey(root)
  const m = await mutateSharedLeases<RepoLease[]>(ctx, probe.top, (cur) => {
    const hit = sharedLeasesOf(cur, sessionId, alias).filter((l) => rootKey(l.root) === mine)
    if (!hit.length) return { result: [] }
    return { next: cur.filter((l) => !hit.includes(l)), result: hit }
  })
  return m.error ? { released: [], error: m.error } : { released: m.result! }
}

/** 释放结果的**回显句**（刻意不说“无租约”：释放只在**真有时**才提一句，避免每条回显都添噪声）。
 *
 * 同一资源会在两个域各有一条（本地镜像 + 仓级共享表）——它们的资源名与持者**完全一致**，
 * 对读者是同一件事。只按资源名展示时合并成一条，否则回显会出现“同一个资源释放了两遍”的噪声
 * （已实测：collect 的回显里同一条 cpu-exclusive 列了两次，看不出是镜像照实还是真两份）。 */
export function leaseReleaseText(released: AnyLease[]): string {
  if (!released.length) return ""
  // 两个域各一条、资源名相同时**只展示一次**（那是对同一件事的两份登记，不是两个资源），
  // 但若资源名确实不同（同时释放多条），逐条列出。
  const names = Array.from(new Set(released.map((l) => l.resource)))
  const shown = names.length === 1 ? [leaseDisplayName(released.find((l) => l.resource === names[0])!)] : names
  const holder = released[0].alias
  return `租约已释放：${shown.join("、")}（原持者 ${holder}）——资源已交回，其他 worker 可申请。`
}

// ---------------------------------------------------------------------------
// 仓级租约：同一个 git 仓上的多条演进线共享同一棵树、同一台机器
// ---------------------------------------------------------------------------

/** 仓级租约（`.evolve-shared/leases.json` 里的一条）：与本地租约**同形**，多一个 `root`。
 *
 *  为何单独一份文件而不是给 control.md 的租约表加一列：control.md 是**每条演进线各一份**
 *  （`<项目根>/.evolve/control.md`），而资源是**机器**的——两条线各有各的 control.md，
 *  写在哪一份里另一条都看不见。加列还会动到黄金语料（`fixtures/control-format-corpus/golden.json`）。
 *  所以：本地域（control.md）与仓域（共享文件）**各写各的**，读态时合并展示。 */
export interface RepoLease extends Lease {
  /** 持有它的那条演进线的**项目根**（跨线回显要能说出“被哪条线占着”）。 */
  root: string
}

/** 两种租约的并集（回显、释放、合并展示都按它处理）。 */
export type AnyLease = Lease | RepoLease

/** 是不是仓级租约（`root` 非空即仓域条目）。 */
const isRepoLease = (l: AnyLease): l is RepoLease => !!(l as RepoLease).root

/** 租约的**可读名**（回显用）：仓级租约带根名，本地租约就是资源名。 */
export function leaseDisplayName(l: AnyLease): string {
  return isRepoLease(l) ? `${l.resource}｜根 ${l.root}` : l.resource
}

/** 根名（回显用的人读短名；路径为空时退回整串）。 */
export function rootLabel(root: string | undefined): string {
  const p = (root ?? "").replace(/[\\/]+$/, "")
  return p.split(/[\\/]/).pop() || p || "（未知根）"
}

/** 共享租约目录名（落在 **git 顶层**下，而不是各项目根的 `.evolve/`）。 */
export const SHARED_DIR_NAME = ".evolve-shared"

/** 仓级租约文件名。 */
export const SHARED_LEASE_FILE = "leases.json"

/** git 顶层探测结果的缓存时长：太短则每条回显都 spawn 一次 git，太长则 `git init` 后要等到过期。 */
export const REPO_TOP_TTL_MS = 60_000

/** 共享租约文件的版本号（结构升级时靠它判别；读到未知版本按空表处理并报错）。 */
export const SHARED_LEASE_VERSION = 1

/** 仓级共享租约的位置（给定 git 顶层）。 */
export function repoSharedPaths(top: string): { dir: string; file: string } {
  const dir = `${top.replace(/[\\/]+$/, "")}/${SHARED_DIR_NAME}`
  return { dir, file: `${dir}/${SHARED_LEASE_FILE}` }
}

/**
 * 仓级资源名：`repo:<git 顶层>:<资源名>`（例：`repo:/home/x/code/gebai:cpu-exclusive`）。
 *
 * ## 为什么必须带 git 顶层
 *
 * 资源（CPU/磁盘/构建缓存）是**机器**的，而一台机器上**同一个 git 仓**可能同时跑着多条演进线：
 * 各自有 `<项目根>/.evolve/control.md`，却共享**同一棵树、同一批文件、同一台机器**。
 * 实测结构（本仓真值）：`/home/x/code/gebai/.evolve`（歌白线）与
 * `/home/x/code/gebai/shuangtian/.evolve`（霜天线）——两者的 `git rev-parse --show-toplevel`
 * 都等于 `/home/x/code/gebai`。不带顶层时，两条线的租约表**互相不可见**：一条在跑 `tsc --noEmit`，
 * 另一条正在等 `load<2` 做性能测量，两边都以为自己独占。
 *
 * 而**不同 git 仓的两棵树**必须互不干涉（否则就变成把所有人锁死）——顶层进名字正是那条界线。
 *
 * 已知边界：`resourceKey` 归一含小写化，故仅大小写不同的两个顶层路径会被当作同一仓
 * （Linux 上这是两个目录）。方向是**过度互斥**（宁可多等），不是漏放。
 */
export function repoResourceName(top: string, resource: string): string {
  return `repo:${rootKey(top)}:${resourceKey(resource)}`
}

/** 一次 `git rev-parse --show-toplevel` 的结论。 */
export interface RepoTopProbe {
  /** git 顶层绝对路径；`null` = 拿不到（退化为「只按项目根」的既有行为）。 */
  top: string | null
  /** 判定依据（人读：拿到了就写命令，拿不到就写为什么）。 */
  reason: string
}

/** 探测结果的进程内缓存（按归一化根）：`state`/`dispatch` 一息里会问好几次。 */
const repoTopMemo = new Map<string, { probe: RepoTopProbe; at: number }>()

/** 只清缓存（测试与 `init` 用：`git init` 之后不必等 TTL 过期）。 */
export function forgetRepoTop(root?: string): void {
  if (root === undefined) repoTopMemo.clear()
  else repoTopMemo.delete(rootKey(root))
}

/**
 * 探测项目根所在的 **git 顶层**（`git rev-parse --show-toplevel`）。
 *
 * **拿不到就安静退化**（`top: null`）：不在 git 仓内、没装 git、宿主没提供 `runCommand`、
 * 命令超时——一律退回「只按项目根」的既有行为，**绝不让 dispatch 因此起不来**。
 * 反过来（把「探测失败」当成错误报出去）会让整条演进线在一个没有 git 的环境里彻底停摆。
 *
 * 结果按 60s 缓存：`state` 读态与 `dispatch` 一息里会问多次，每次 spawn 一个 git 进程不值得。
 */
async function repoTopOf(ctx: ToolContext, root: string): Promise<RepoTopProbe> {
  const key = rootKey(root)
  const hit = repoTopMemo.get(key)
  if (hit && Date.now() - hit.at < REPO_TOP_TTL_MS) return hit.probe
  const probe = await probeRepoTop(ctx, root)
  repoTopMemo.set(key, { probe, at: Date.now() })
  return probe
}

/** 真正问 git 的那一步（`repoTopOf` 的缓存外壳之下）。 */
async function probeRepoTop(ctx: ToolContext, root: string): Promise<RepoTopProbe> {
  if (typeof ctx.runCommand !== "function") return { top: null, reason: "宿主未提供 runCommand（无从探测 git 顶层）" }
  if (root.includes('"')) return { top: null, reason: "项目根含双引号，跳过 git 探测（不拼命令）" }
  try {
    const r = await ctx.runCommand(`git -C "${root}" rev-parse --show-toplevel`, { timeoutMs: 5000 })
    if (r.code !== 0) return { top: null, reason: `不在 git 仓内（git rev-parse 退出码 ${r.code}）` }
    const top = (r.stdout ?? "").split(/\r?\n/).map((s) => s.trim()).find(Boolean) ?? ""
    if (!top) return { top: null, reason: "git rev-parse 无输出" }
    if (!isAbsRoot(top)) return { top: null, reason: `git 顶层不是绝对路径：${top}` }
    return { top: cleanRoot(top), reason: "git rev-parse --show-toplevel" }
  } catch (err) {
    return { top: null, reason: `git 探测失败：${describeError(err)}` }
  }
}

/** 解析共享租约文件（容错：结构不认识/字段缺失的条目一律丢弃并计入 dropped）。 */
export function parseSharedLeases(text: string | null): { leases: RepoLease[]; error?: string } {
  if (!text || !text.trim()) return { leases: [] }
  let doc: unknown
  try {
    doc = JSON.parse(text)
  } catch (err) {
    return { leases: [], error: `共享租约文件不是合法 JSON：${describeError(err)}` }
  }
  const o = (doc ?? {}) as { version?: unknown; leases?: unknown }
  const ver = Number(o.version ?? 0) || 0
  if (ver !== SHARED_LEASE_VERSION) return { leases: [], error: `共享租约文件版本 ${ver} 不认识（本版只认 ${SHARED_LEASE_VERSION}）` }
  const rows = Array.isArray(o.leases) ? o.leases : []
  const out: RepoLease[] = []
  for (const r of rows) {
    if (!r || typeof r !== "object") continue
    const x = r as Record<string, unknown>
    const resource = String(x.resource ?? "").trim()
    const alias = String(x.alias ?? "").trim()
    const sessionId = String(x.sessionId ?? "").trim()
    // 无资源名或无持者的残行挡不住任何人，留着只干扰读态（与本地表的同一判据）
    if (!resource || (!alias && !sessionId)) continue
    out.push({
      resource,
      alias,
      sessionId,
      start: Number(x.start) || 0,
      expireAt: Number(x.expireAt) || 0,
      purpose: String(x.purpose ?? ""),
      root: String(x.root ?? "").trim(),
    })
  }
  return { leases: dedupeLeases(out) }
}

/** 序列化共享租约文件（确定性：字段次序固定、缩进 2——人可读、可 diff）。 */
export function renderSharedLeases(leases: RepoLease[]): string {
  const rows = dedupeLeases(leases).map((l) => ({
    resource: l.resource,
    alias: l.alias,
    sessionId: l.sessionId,
    start: l.start,
    expireAt: l.expireAt,
    purpose: l.purpose,
    root: l.root ?? "",
  }))
  return `${JSON.stringify({ version: SHARED_LEASE_VERSION, leases: rows }, null, 2)}\n`
}

/** 读共享租约表（缺失 = 空表；读失败/结构损坏 = 带 error，调用方据它决定是否拒绝授予）。 */
async function readSharedLeases(ctx: ToolContext, top: string): Promise<{ leases: RepoLease[]; error?: string }> {
  const r = await readAbs(ctx, repoSharedPaths(top).file)
  if (r.error) return { leases: [], error: r.error }
  return parseSharedLeases(r.text ?? null)
}

/**
 * 原子落盘共享租约表：写同目录临时文件 → `rename` 覆盖。
 *
 * 为什么不能直接 `writeFile`：共享表是**多条演进线**读写的文件，直写有「长度为 0 / 半个 JSON」的
 * 窗口——另一条线此刻读到它就是「租约表损坏」，按本模块的判据会**拒绝授予**（宁可多等），
 * 于是两条线一起停摆。`rename` 之后读者要么看到旧内容、要么看到新内容。
 * `rename` 不可用（宿主没提供 `moveFile` / 跨设备）时**退化为直写**——非原子，但比“租约压根写不进去”好。
 */
async function writeSharedAtomic(ctx: ToolContext, file: string, content: string): Promise<{ error?: string }> {
  const tmp = `${file}.tmp-${process.pid.toString(36)}-${Date.now().toString(36)}`
  const w = await writeAbs(ctx, tmp, content)
  if (w.error) return w
  if (typeof ctx.moveFile === "function") {
    try {
      await ctx.moveFile(tmp, file)
      return {}
    } catch (err) {
      const direct = await writeAbs(ctx, file, content)
      if (!direct.error) {
        try { await ctx.deleteFile?.(tmp) } catch { /* 临时文件残留不影响判据 */ }
        return {}
      }
      return { error: `原子替换失败（${describeError(err)}），回退直写亦失败（${direct.error}）` }
    }
  }
  const direct = await writeAbs(ctx, file, content)
  if (direct.error) return direct
  try { await ctx.deleteFile?.(tmp) } catch { /* 同上 */ }
  return {}
}

/** 共享租约的读-改-写（**同一把路径锁内**完成，与 `mutateControl` 同型）。
 *  `mutate` 允许异步：仓级回收要读**别的线自己的登记**（`<它的根>/.evolve/control.md`）才敢判“它已下班”。 */
async function mutateSharedLeases<T>(
  ctx: ToolContext,
  top: string,
  mutate: (cur: RepoLease[]) => { next?: RepoLease[]; result: T } | Promise<{ next?: RepoLease[]; result: T }>,
): Promise<{ result?: T; error?: string }> {
  const { file } = repoSharedPaths(top)
  return withPathLock(file, async () => {
    const r = await readSharedLeases(ctx, top)
    // 结构损坏时**不覆盖**（覆盖就把别的线的租约抹掉了）：如实报错，让调用方拒绝授予。
    if (r.error) return { error: r.error }
    const { next, result } = await mutate(r.leases)
    if (next !== undefined) {
      const w = await writeSharedAtomic(ctx, file, renderSharedLeases(next))
      if (w.error) return { error: w.error }
    }
    return { result }
  })
}

/** 仓级租约的冲突判定（跨线：**所有线**的持者都算，不只本线）。 */
function repoLeaseOf(leases: RepoLease[], resource: string): RepoLease | undefined {
  const key = resourceKey(resource)
  return dedupeLeases(leases).find((l) => resourceKey(l.resource) === key)
}

/** 共享表里某 worker（会话/别名）持有的全部租约——释放与回收共用它。 */
function sharedLeasesOf(leases: RepoLease[], sessionId: string, alias?: string): RepoLease[] {
  return leases.filter((l) => leaseHeldBy(l, sessionId, alias))
}

/**
 * 仓级租约的回收判定（**跨线**：一条仓级租约可能属于任何一条线，判定要看**它的线自己的登记**）。
 *
 * ## 为何跨线回收要“两重参证”（而本地域只需一重）
 *
 * 本地域的“持有者会话已不存在”可以直接信本会话的控制面——那条线的 worker 就是本服务建的。
 * 但仓级租约的持者可能在**另一条线**上，而那条线未必在同一个服务进程里（两个 gebai 实例共用
 * 一棵 git 树是可能的部署）：**我看不到它的会话 ≠ 它的会话不存在**。已实测踩到：
 * 用“本控制面查不到”去判异线租约 → 把另一条线**正在跑**的门禁租约抢了——正好是本单要消灭的
 * “两套测量互相污染”的另一种发生方式（这次是“以为它死了”）。
 *
 * 所以异线租约的“已下班”判据必须**两边一致**：
 *   · 它自己那条线的 `control.md` 里**不**再说它在跑（池里找不到，或已标 idle）；且
 *   · 或到期时刻已过（盘上写明的硬边界，不依赖任何一侧的观察能力）。
 * 反过来：**它自己那条线还说它在跑 ⇒ 一律不回收**（即使我这边查不到它）。
 *
 * 三条判据逐条：
 *   ① 到期（与本地域同源，不依赖观察能力）；
 *   ② 异线自己的池里已标 idle（真值：它已交付）；
 *   ③ 异线自己的池里已无此 worker **且** 本控制面确认会话不存在（两重印证才判“已不存在”）。
 */
async function repoRecycleReason(
  ctx: ToolContext,
  svc: SessionsLike | undefined,
  l: RepoLease,
  nowMs: number,
  liveOf: (sid: string) => Live | undefined,
  selfRoot: string,
): Promise<string> {
  if (l.expireAt && l.expireAt <= nowMs) return `已到期（${new Date(l.expireAt).toLocaleString("zh-CN")}）`
  // 没有会话控制面 ⇒ 观测不到任何会话 ⇒ 只能靠到期（上一条）——不拿“没观测”当“已死”。
  if (!svc) return ""
  const sid = l.sessionId
  const foreign = !!l.root && rootKey(l.root) !== rootKey(selfRoot)
  // 它自己那条线的登记（读不到就当“无从核实”——与在途的“无从核实”同型：不拿没读过的东西当已死）
  let peer: EvolveState | null = null
  if (l.root) {
    const r = await readAbs(ctx, evolvePaths(l.root).control)
    if (!r.error && r.text != null) peer = parseControl(r.text)
  }
  const peerWorker = peer?.workers.find((x) => (!!sid && x.sessionId === sid) || (!!l.alias && x.alias === l.alias))
  // ② 它已交付（它自己标了 idle）——与本地域同一判据，当即可信。
  if (peerWorker?.status === "idle") return `持有者已空闲（${rootLabel(l.root)} 的池中登记为 idle）`
  const observed = sid ? liveOf(sid) : undefined
  const peerSaysRunning = peerWorker?.status === "running"
  // ③ 会话不存在：异线要**两重印证**（本控制面看不到 **且** 它自己那条线也不再登记它）；
  //    同一条线（同服务）时本控制面的结论已是真值，一重就够。
  if (sid && observed && observed.exists === false) {
    if (!foreign) return "持有者会话已不存在"
    if (!peerWorker) return `持有者会话已不存在（本控制面查不到，且 ${rootLabel(l.root)} 的池中已无此 worker）`
  }
  // ④ 会话可达但不在跑且过启动宽限（与本地域同源）——但异线自己还说在跑时不采信。
  if (sid && observed && !observed.running && !peerSaysRunning && nowMs - (l.start || 0) > LEASE_START_GRACE_MS) {
    return "持有者已空闲（会话可达且不在运行，已过启动宽限）"
  }
  return ""
}

/** 仓级租约的回显行（与本地租约同一形状，多一列“根”以区分哪条线）。 */
export function repoLeaseLine(l: RepoLease, now = Date.now()): string {
  return `- ${l.resource}｜持者 ${rootLabel(l.root)} 的 ${l.alias}（会话 ${idShort(l.sessionId)}）｜起租 ${new Date(l.start).toLocaleString("zh-CN")}｜到期 ${leaseExpiryText(l, now)}｜${l.purpose || "（未登记用途）"}`
}

/** 渲染器写出的**全部规范节标题**（按写出次序）——`renderControl` 只写这些节。
 *
 *  为什么需要显式清单：断点是**自由文本节**（正文由模型写，可能是多段 markdown），若把任何
 *  `^##\s` 行都当节边界，正文里一行手写的小标题（`## 下一步`、粘贴进来的 md 片段）就会**静默截断**
 *  断点——读进来只剩前半段，下一次 `note` 写回时后半段永久丢失。改为只认规范节标题：非规范
 *  `## ` 行是内容而不是结构。手写文件里多出的非规范标题也归到所在节正文（**不静默丢**）。 */
export const SECTION_HEADINGS = ["项目坐标", "计数", "worker 池", "在途", "资源租约", "种子队列", "收藏待办", "断点"] as const

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
    // 租约表：列固定（资源名|别名|会话|起租|到期|用途），**用途是自由文本列**（会含竖线，如
    // “st test 门禁｜前后 uptime”）——渲染器已转义，切格器已解转义，故直接取格即可。
    // 读侧归一（同资源只留一条）与写侧 upsert 同源：两者共用 {@link dedupeLeases}。
    leases: dedupeLeases(
      tableRows(md, "资源租约")
        .map((c): Lease => ({
          resource: c[0] ?? "",
          alias: c[1] ?? "",
          sessionId: c[2] ?? "",
          start: Number(c[3]) || 0,
          expireAt: Number(c[4]) || 0,
          purpose: c[5] ?? "",
        }))
        // 无资源名或无持者的残行不当租约（它们不能挡任何人，留着只会干扰读态）
        .filter((l) => l.resource && (l.sessionId || l.alias)),
    ),
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
export function dedupeState(state: EvolveState): { workers: Worker[]; inflight: Inflight[]; leases: Lease[]; dropped: number } {
  const workers = dedupeWorkers(state.workers)
  const inflight = dedupeInflight(state.inflight, workers)
  const leases = dedupeLeases(state.leases ?? [])
  const dropped =
    state.workers.length - workers.length + (state.inflight.length - inflight.length) + ((state.leases?.length ?? 0) - leases.length)
  return { workers, inflight, leases, dropped }
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
    H("资源租约"),
    renderTable(
      ["资源名", "持有者别名", "会话", "起租时刻", "到期时刻", "用途"],
      (state.leases ?? []).map((l) => [l.resource, l.alias, l.sessionId, String(l.start), String(l.expireAt), l.purpose]),
    ),
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

// ---------------------------------------------------------------------------
// 心跳任务的项目归属（任务面按**名字**反查，名字因此必须承载项目身份）
// ---------------------------------------------------------------------------

/** 历史缺省任务名：早于按项目派生的版本用它建任务，故仍须认得（升级不丢既有项目的驱动任务）。 */
const LEGACY_TASK_NAME = "SHENGSHENG_EVOLVE"

/** 根归一化键：统一分隔符、去尾分隔符、Windows 盘符与大小写归一——仅比字符串会把同一项目判成两个。 */
function rootKey(p: string): string {
  const s = cleanRoot(p).replace(/\\/g, "/")
  return /^[A-Za-z]:/.test(s) ? s.toLowerCase() : s
}

/** 两个项目根是否同一项目（任一为空即否：无从归属的登记不得据以复用或停用）。 */
function sameRoot(a: string | undefined, b: string | undefined): boolean {
  return !!a && !!b && rootKey(a) === rootKey(b)
}

/** 任务登记上的项目根（任务级 env 的 `SHENGSHENG_ROOT`；兼容从唤醒 prompt 的「项目根：」行回读）。
 *  这是任务与项目之间**唯一**的绑定证据——`ensure`/`pause`/`resume` 的归属判据全部落在它上面。 */
function taskRootOf(task: Record<string, unknown> | undefined): string | undefined {
  const envRoot = taskEnvRoot(task)
  if (envRoot) return envRoot
  const prompt = typeof task?.prompt === "string" ? task.prompt : ""
  const m = prompt.match(/^\s*项目根[:：]\s*(.+?)\s*$/m)
  return m ? cleanRoot(m[1]) : undefined
}

/** 任务级 env 上的项目根（**只读 env**，不做 prompt 回读——用于判断「env 绑定是否已写」）。 */
function taskEnvRoot(task: Record<string, unknown> | undefined): string | undefined {
  const env = task?.env as Record<string, unknown> | undefined
  const v = typeof env?.SHENGSHENG_ROOT === "string" ? env.SHENGSHENG_ROOT.trim() : ""
  return v ? cleanRoot(v) : undefined
}

/** 按项目派生的心跳任务名：`SHENGSHENG_EVOLVE::<根名>-<根哈希>`。
 *
 *  **为什么不能是常量名**：任务面按名字反查（`tasks.list()`），多个演进项目共存时同名心跳会彼此
 *  命中对方的任务——`ensure` 会把别人的驱动任务当成自己的（复用它＝唤醒别人的项目），
 *  `pause` 会把别人的演进停掉。根名只为可读，**根哈希**才是判据（跨机、同名目录不撞）；
 *  同根多次调用派生同一名字，故幂等。 */
function evolveTaskName(root: string): string {
  const key = rootKey(root)
  const base = (key.replace(/\/+$/, "").split("/").pop() || "root").replace(/[^a-zA-Z0-9._-]+/g, "-").slice(0, 32).replace(/^-+|-+$/g, "") || "root"
  let h = 0x811c9dc5
  for (let i = 0; i < key.length; i++) {
    h ^= key.charCodeAt(i)
    h = Math.imul(h, 0x01000193) >>> 0
  }
  return `${LEGACY_TASK_NAME}::${base}-${h.toString(16).padStart(8, "0")}`
}

/** 心跳任务名解析（task 分支专用）。归属判据一律是任务上的项目根，**名字只作候选**：
 *  - 本项目**已绑定**的任务（旧常量名 / 派生名 / 显式名依次探测）沿用其名——一条演进线只有一条心跳，
 *    升级也不强制改名（唤醒消息自带项目坐标，改名只为人眼可读）；
 *  - 尚无绑定任务时：显式名（会话 env，人写的，未被他项占用）优先于派生名；
 *  - 被其他项目占用的同名任务一并回报（供调用方如实提示，不得静默复用）。 */
function evolveTaskNames(
  root: string,
  all: Array<Record<string, unknown>>,
  legacy: string,
  explicit: string,
): { name: string; conflicts: Array<Record<string, unknown>> } {
  const byName = (n: string) => all.find((t) => String(t.name ?? "") === n)
  const mine = (t: Record<string, unknown> | undefined) => !!t && sameRoot(taskRootOf(t), root)
  const explicitClean = explicit.trim()
  const derived = evolveTaskName(root)
  for (const n of [legacy, derived, explicitClean]) {
    if (n && mine(byName(n))) return { name: n, conflicts: [] }
  }
  const conflicts = [legacy, explicitClean, derived]
    .filter((n, i, a) => n && a.indexOf(n) === i)
    .map(byName)
    .filter((t): t is Record<string, unknown> => !!t)
  const name = explicitClean && explicitClean !== legacy && !byName(explicitClean) ? explicitClean : derived
  return { name, conflicts }
}

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
  }  const fromEnv = (ctx.env?.SHENGSHENG_ROOT ?? "").trim()
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

// ---------------------------------------------------------------------------
// 子孙进程树：定位（retire mode=stop 的伴随动作）
// ---------------------------------------------------------------------------

/**
 * 定位一个 worker 会话派生的子孙进程（**只读**，不发信号）。
 *
 * 归因靠 {@link PROC_MARKER_ENV} 环境标记（派工时注入、子进程继承）+ 子孙递归，
 * 不用命令行/名字猜——后者会把别的线的同名进程一起杀了。拿不到进程表时返回 `error`，
 * 由调用方**如实报告**（静默当成「已清理」是这条最坏的失败方式）。
 */
async function locateSessionTree(sessionId: string): Promise<{ plan?: ReapPlan; error?: string }> {
  const table = await readProcTable()
  if (table.error) return { error: table.error }
  const markers = await readSessionMarkers(table.rows.map((r) => r.pid))
  const plan = planReap({ rows: table.rows, markers, sessionId, protect: await protectLineage(), source: table.source })
  return { plan }
}

/** 保护集：自身 pid 与自身祖先链（**永不发信号**——杀了宿主就等于把整条演进线一起干掉）。
 *  实现在 `proc-tree.protectLineage`（与平面判定同处，只一份）。 */

/** 工作树回退态告警（**只告警、不自动恢复**：自动恢复可能覆盖别人正在验的回退态）。 */
async function rollbackResidueText(ctx: ToolContext, root: string): Promise<string> {
  const scan = await detectRollbackResidue(root, {
    io: {
      list: async (dir) => {
        const entries = await ctx.listDir(dir)
        return entries.map((e) => ({ name: e.path, isDir: e.isDir }))
      },
      read: async (f) => ctx.readBinaryFile(f),
    },
  })
  return renderResidue(scan)
}

/**
 * `review` 的**提交范围核对**：最近一次提交里的文件 vs 该 worker 工单提到的路径。
 *
 * 三种结论分开（这是本条判据最容易被做成假绿的地方）：
 * - `advice` = **`git` 不可用 / 不在仓内** ⇒ 明说「本项未核」（不是「没问题」）；
 * - `advice` = 提交全在工单范围内 ⇒ 给一句已核过的肯定（让人知道判据真跑了）；
 * - `advice` = 有文件不在工单里 ⇒ **疑似宽范围暂存带走了别人的改动**，逐个列出 `状态 路径（+N −M）`。
 *
 * 同时看一眼工作树里的 `??`：那是**别的 worker 的新文件**，也是下一次宽范围暂存会带走的东西
 * ——新增文件不受 `edit`/`write` 的防陈旧覆盖守卫保护（守卫只管已跟踪文件）。
 *
 * 只读：不碰工作树、不碰 index。读不到就如实说读不到，不当成通过。
 */
async function auditCommitScope(
  ctx: ToolContext,
  root: string,
  worker: Worker,
  inflight: Inflight | undefined,
): Promise<{ advice?: string; block?: Record<string, unknown> }> {
  const probe = await repoTopOf(ctx, root)
  if (!probe.top) return { advice: `提交范围本项未核：拿不到 git 顶层（${probe.reason}）——这不等于「没有宽范围暂存」，只是无从核` }
  const head = await readLastCommit(ctx, probe.top)
  if (!head) return { advice: `提交范围本项未核：读不到最近一次提交（git log 失败或仓内无提交）——不等于无异常` }
  if (!head.files.length) return { advice: `提交范围：最近一次提交 ${head.sha.slice(0, 8)}「${head.subject}」无文件改动（合并/空提交？）——本项无可核` }

  // 工单的「这一单该动哪些文件」全在自由文本里：方向（task）+ 预期产出（deliver）+ 本单任务摘要。
  // 三条都抽（任一条提到即算覆盖）——只用工单原文会漏掉「worker 交付时写的具体路径」。
  const mentions = mentionPaths([worker.task, inflight?.direction ?? "", inflight?.deliverable ?? ""].join("\n"))
  const audit = scopeAudit(head.files, mentions)
  const head8 = head.sha.slice(0, 8)
  const list = (fs: readonly CommitFile[]): string => fs.map((f) => describeFile(f)).join("；")

  // `??` 行：别人的新文件（下一次宽范围暂存会带走它们；本条纪律最想让人看见的那一类）
  const tree = await readWorkTree(ctx, probe.top)
  const untracked = tree ? untrackedOf(tree) : []
  const untrackedNote = tree
    ? untracked.length
      ? `工作树里另有 ${untracked.length} 个未跟踪新文件（\`??\`）——提交前先问「这是谁的新文件」，它们不受 edit 守卫保护：${untracked.slice(0, 5).map((e) => `\`${e.path}\``).join("；")}${untracked.length > 5 ? " …" : ""}`
      : "工作树无未跟踪新文件（没有可被宽范围暂存带走的新增物）"
    : "工作树未核（`git status` 读不到）"

  if (!audit.outOfScope.length) {
    return {
      advice: `提交范围：最近一次提交 ${head8}「${head.subject}」${head.files.length} 个文件**均在**工单提到的路径内（已核：${audit.inScope.length}/${head.files.length}）✅；${untrackedNote}`,
      block: { head: head8, inScope: audit.inScope.length, outOfScope: [] as string[], untracked: untracked.map((e) => e.path) },
    }
  }

  // 命中：报出来但**不自动处置**（回退已落的提交是危险动作，归属仲裁是人/控制面的决定）
  const names = audit.outOfScope.map((f) => f.path)
  const looksWide = audit.outOfScope.length >= 2 || audit.outOfScope.some((f) => f.status === "A")
  const hint = looksWide
    ? "形态像**宽范围暂存**（`git add -A` / `.` / `-u` / `commit -a`）带走了别人的改动——核一下这些文件是不是别的 worker 的交付物"
    : "这些文件不在本工单提到的路径里（可能是你自己拆出的新文件；若不是，那就是别人的活被一起提交了）"
  return {
    advice: [
      `⚠ **疑似宽范围暂存带走了别人的改动**：最近一次提交 ${head8}「${head.subject}」有 ${audit.outOfScope.length} 个文件不在本工单提到的路径内——${list(audit.outOfScope)}`,
      `  ${hint}。核对办法：\`git show --stat ${head8}\` 逐行看，再问每份文件「它在我的工单清单里吗」；若确实带走了，**不要 revert**（那会把已落盘的成果一起抹掉），先在台账记清归属再续派给真正的作者。`,
      `  工单覆盖的（${audit.inScope.length} 个）：${audit.inScope.length ? list(audit.inScope) : "（无）"}`,
      `  ${untrackedNote}`,
    ].join("\n"),
    block: { head: head8, inScope: audit.inScope.length, outOfScope: names, untracked: untracked.map((e) => e.path), mentions: audit.mentions },
  }
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

/**
 * ⚠ **不存在「裸写的 saveState」**（2026-10-17 移除）。
 *
 * 它曾在这里，被 10 处「加载→改→保存」的分支调用——而并行调用时后写者会以陈旧基线
 * 覆盖前者的成果（实测：并行两次 seed_add 只留一条；note 与 seed_add 并行就丢呼吸）。
 * 现在所有写都走 `mutateControl` / `evolveLock`（读-改-写在**回一把锁**内完成）。
 * **不要为“只是想快速写一下”而重新引入它**——它就是缺陷的入口。
 */

/**
 * 路径级串行锁（同一进程内按绝对路径排队）。
 *
 * ## 为什么必须存在（实测代价：静默丢数据）
 *
 * `.evolve/` 下的两份文件都是**读-改-写**（`loadState` → 改内存 → `saveState`），
 * 而 agent 的**同一次回复里多个工具调用是并行执行的**（系统提示的明确语义，不是边缘情况）：
 * 「择种 + 布雨」「多个 seed_add」都是自然写法。并行时后写者带**陈旧基线**覆盖前者。
 *
 * 实测（真往返撞出，当时 52 个既有用例全绿）：
 *   · 并行两次 `seed_add` → 只留一条（另一条无声消失）；
 *   · 并行 `note` + `seed_add` → **呼吸仍为 0、断点为空**（note 整次写入被吞）。
 * 呼吸计数被吞即「这条河跑了多久」的历史断档，比丢一条种子更严重。
 *
 * ## 锁的单位是 `.evolve` **目录**，不是单个文件
 *
 * 因为两者之间存在**跨文件的一致要求**：`collect` 的台账幂等键取自 control.md 里的轮次
 * （`collect:<会话>:r<轮次>`）——若台账与状态分属两把锁，并行两个 collect 会走出
 * 「两边都认为自己该入账」或「一边先改状态使另一边的键算错」的窗口。
 * 锁目录 = 声明「`.evolve/` 下的读写是一个事务域」。
 *
 * 不做的事：不跨进程（服务重启即空，与 `.evolve` 的单写者假设一致）、不做超时（
 * 被串行的都是本地文件读写，量级毫秒；引入超时反而可能让两个写者同时进临界区）。
 */
const pathLocks = new Map<string, Promise<unknown>>()

function withPathLock<T>(path: string, fn: () => Promise<T>): Promise<T> {
  const prev = pathLocks.get(path) ?? Promise.resolve()
  const next = prev.then(fn, fn)
  // 存一只**不会 reject** 的尾：前一位失败不得阻断后续排队者（否则一次写失败会锁死整条路径）。
  pathLocks.set(path, next.then(
    () => undefined,
    () => undefined,
  ))
  return next
}

/** 按项目根的 `.evolve` 目录取锁（读-改-写的**事务域**；见 `withPathLock` 的说明）。 */
function evolveLock<T>(root: string, fn: () => Promise<T>): Promise<T> {
  return withPathLock(evolvePaths(root).dir, fn)
}

/**
 * 在**路径锁内**完成一次 control.md 的读-改-写。
 *
 * 调用方传的 `mutate` 拿到的 state 一定是**刚读到的、未被并行者覆盖的**那份——
 * 这就是它与「先 `loadState`、再 `saveState`」的本质区别：后者之间有一个可被插入的窗口。
 * `mutate` 返回 `next: undefined` = 放弃写入（例如去重命中、参数非法），此时不落盘也不报错。
 *
 * ⚠ 泛型参数用**具名 alias**（`ControlMutation<T>`）而不是内联对象字面量：内联写法下
 * TypeScript 会把 `{result: {dup}}` 与 `{next, result: {added, total}}` 推成互斥的 union，
 * 调用方 `"dup" in r` 的窄化全部失效（实测 20 条 TS2339/TS2638）。具名 alias 让两者
 * 按同一结构比较，判别收窄才成立。
 */
type ControlMutation<T> = (state: EvolveState) =>
  | { next?: EvolveState; result: T }
  | Promise<{ next?: EvolveState; result: T }>

async function mutateControl<T>(
  ctx: ToolContext,
  root: string,
  mutate: ControlMutation<T>,
): Promise<{ result?: T; error?: string }> {
  const path = evolvePaths(root).control
  return evolveLock(root, async () => {
    const r = await readAbs(ctx, path)
    if (r.error) return { error: r.error }
    const state = parseControl(r.text ?? null)
    const { next, result } = await mutate(state)
    if (next !== undefined) {
      const w = await writeAbs(ctx, path, renderControl(next))
      if (w.error) return { error: w.error }
    }
    return { result }
  })
}

/**
 * 台账追加（含记录键：同一轮只入一次，重放幂等）。
 *
 * 读改写序列在本进程内按路径串行（同一次心跳里 collect 与 state.note 并发调用时不互相踩），
 * 且**用文件本身做幂等源**（每次读到的都是最新内容）——避免「先判定未入账、后双写」。
 * 台账是控制平面唯一的成果记忆，宁可慢一点也不能写坏。
 *
 * 锁与 control.md **共用同一套**（`evolveLock` 按 `.evolve` 目录）——两份文件的
 * 串行语义必须同源，否则又会出现「一边护着、一边裸着」的同型缺陷。
 */
async function appendLedger(ctx: ToolContext, root: string, key: string, block: string): Promise<{ path: string; appended: boolean; error?: string }> {
  return evolveLock(root, () => appendLedgerLocked(ctx, root, key, block))
}

/** `appendLedger` 的**已持锁**版本：供需要「台账 + 状态」原子完成的调用方（如 collect）在
 *  同一把锁内使用——再调 `appendLedger` 会自锁（同一路径的锁不可重入）。 */
async function appendLedgerLocked(ctx: ToolContext, root: string, key: string, block: string): Promise<{ path: string; appended: boolean; error?: string }> {
  const path = evolvePaths(root).ledger
  const r = await readAbs(ctx, path)
  if (r.error) return { path, appended: false, error: r.error }
  const md = r.text ?? "# 演进台账\n\n（生生逐轮追加——每轮一次 worker 交付，记录键保证幂等。）\n"
  if (md.includes(key)) return { path, appended: false }
  const w = await writeAbs(ctx, path, `${md.replace(/\s*$/, "")}\n\n${block}\n`)
  if (w.error) return { path, appended: false, error: w.error }
  return { path, appended: true }
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
  /** 消息尾部原文（`review` 的自匹配活锁判据要从中筛「等待类命令」）。
   *  只留尾部一批：判据看的是「最近在等什么」，不需要全量会话。 */
  tail?: unknown[]
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
        // 尾部只留 N 条：自匹配活锁的判据只看「最近在等什么」（更早的等待对现在无意义）
        live.tail = msgs.slice(-PGREP_SCAN_LIMIT)
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

function liveLine(alias: string, w: Worker | undefined, live: Live | undefined, scale: CtxScale): string {
  const w2 = w
  const bits: string[] = [alias]
  if (w2) bits.push(`${w2.domain}｜${w2.task}`)
  if (!live || !live.exists) bits.push("**会话不存在**（外部删除/服务重启——清出池，在途回种子队列）")
  else if (live.running) bits.push(`运行中 ${live.minutes ?? "?"} 分钟` + (live.toolCalls ? `｜工具 ${live.toolCalls}` : "") + (live.pending?.length ? `｜待决：${live.pending.join("、")}` : ""))
  // ctx 口径**两个量都报**（过建议线≠过模型窗口，只写一个数会被读成另一个量）。
  else bits.push("空闲" + (live.ctxTokens != null ? `｜${ctxLine(live.ctxTokens, scale)}` : ""))
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
  // 提交纪律**每单必带**（不是可覆盖的 `rules` 的一部分）：宽范围暂存会把同仓另一个 worker 的未完成
  // 成果一起提交（本仓真事故 856ba9bb 带走了 retire-tree 的 +596 / +573 两份交付物），而
  // edit/write 的防陈旧覆盖守卫只管已跟踪文件、**管不了未跟踪的新文件**。示例路径取自本单的 `deliver`，
  // 让 worker 照抄即合规（纪律文本与示例路径同源，不各写一份）。
  lines.push(`提交纪律：${commitProtocolLine(mentionPaths(order.deliver ?? ""))}`)
  // 等待纪律**每单必带**（同提交纪律的理由：worker 只看得见这条消息）：`pgrep -f` 自匹配是本仓
  // 实测两次的活锁，代价可量化（一次 12 分钟零产出 / 159 条消息），而它**看起来完全像在守规矩**。
  lines.push(`等待纪律：${waitProtocolLine()}`)
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
function workerEnv(root: string, coord: Coord, sessionId?: string): Record<string, string> {
  const vars: Record<string, string> = { SHENGSHENG_ROOT: root }
  if (coord.verify) vars.SHENGSHENG_VERIFY = coord.verify
  if (coord.seedDoc) vars.SHENGSHENG_SEED_DOC = coord.seedDoc
  if (coord.rules) vars.SHENGSHENG_RULES = coord.rules
  // 会话归属标记（{@link PROC_MARKER_ENV}）：`retire mode=stop` 靠它**唯一地**认出「这是该会话派生的子进程」。
  // 不可省：子进程会继承它，而它比命令行匹配可靠得多——按名字猜正是「把全机同名的一起杀掉」的来源。
  // 只能在**会话已存在**时写入（新建分支拿到 id 后再补一次，见派工分支）。
  if (sessionId) vars[PROC_MARKER_ENV] = sessionId
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
  /** 申请**独占资源**（自由串，见 {@link Lease}）：本单开工期间该资源不得被其他 worker 持着。
   *  `exclusive: true` 是 `resource: DEFAULT_EXCLUSIVE_RESOURCE` 的简写。 */
  resource?: string
  /** `resource` 的简写：`true` ⇒ 申请缺省独占资源（{@link DEFAULT_EXCLUSIVE_RESOURCE}）。 */
  exclusive?: boolean
}

/** 工单申请的资源名（归一口径）：显式 `resource` 优先，否则 `exclusive: true` ⇒ 缺省资源名，都没有 ⇒ ""。 */
export function orderResource(o: Order): string {
  const explicit = (o.resource ?? "").trim()
  if (explicit) return resourceKey(explicit)
  return o.exclusive === true ? DEFAULT_EXCLUSIVE_RESOURCE : ""
}

/** 真实**模型上下文窗口**的缺省值（token）：provider 不报、env 也没说时的最后兜底。
 *  可用会话 env `SHENGSHENG_MODEL_CTX_WINDOW` 覆盖；**引擎自报优先**（见 {@link ctxScale}）。 */
export const DEFAULT_MODEL_CONTEXT_WINDOW = 512_000

/** **复用质量建议线**（token）：超过它的 worker 不再值得复用（上下文过肥、续派质量下降、每次请求更贵）。
 *  可用会话 env `SHENGSHENG_REUSE_ADVISORY_CTX` 覆盖。
 *
 *  ⚠ **它不是“模型窗口上限”。** 这是「一个数字两个量」的第三例，代价是真误报：
 *  旧代码只有一个 `WORKER_CTX_LIMIT = 150_000`，既当「肥到不值得复用」的建议线、又被回显写成
 *  **「超上限」**。于是 ctx 224K 的 worker 被 review 判成「上下文已过肥（149%），超上限」——
 *  而真实模型窗口是 512K，224K 只占 **44%**，它**没有超任何硬限**。更坏的是这个错理由会直接生出
 *  错的处置决策（下息的人照着「超上限」去退役，并把错理由写进台账）。
 *  本仓同型已有两例：`round`/`seq`（一个数字既当全局派工序号又当会话轮次）、`ctxRatio` 退回裸数字
 *  （分子标了、分母没说是什么量）。通则：**凡是“上限/比例”类数字，必须同时说清分子与分母各自是什么量**
 *  ——「149% of 复用建议线」与「44% of 模型窗口」是两个量，永远不可互换、不可互相冒充。 */
export const DEFAULT_REUSE_ADVISORY_LIMIT = 150_000

const kilo = (v: number): string => (v >= 1000 ? `${(v / 1000).toFixed(1)}K` : String(v))

/** 数值型 env 解析（每次调用现算）：空/非法 ⇒ `undefined`（调用方自己回落）；
 *  `0`/`off`/`none`/`unknown`/`?`/`null` ⇒ `0`（显式「这个量不知道」或「关掉这条判据」，由调用方解释）。 */
function countEnv(raw: string | undefined): number | undefined {
  const v = (raw ?? "").trim().toLowerCase()
  if (!v) return undefined
  if (["0", "off", "none", "unknown", "?", "null"].includes(v)) return 0
  if (!/^\d+$/.test(v)) return undefined
  return Number(v)
}

/** 上下文口径的**两个量**，以及它们各自从哪来（回显必须说清数源：缺省值 ≠ 引擎真值）。 */
export interface CtxScale {
  /** 真实模型上下文窗口（token）；**0 = 未知**（不得凭空编造成 512K 之类）。 */
  window: number
  /** 窗口这个数从哪来：`引擎自报` / `env SHENGSHENG_MODEL_CTX_WINDOW` / `缺省值` / `env 显式声明未知`。 */
  windowSource: string
  /** 复用质量建议线（token）；**0 = 未设**（该判据关闭，不编造百分比）。 */
  advisory: number
  advisorySource: string
}

/** 解析两个量（每次调用现算）。
 *  **窗口优先取引擎自报**（`ctx.sessions.engine.contextWindow?.()`）：硬编码的「512K」在 `.env`
 *  改成 `GEBAI_LLM_MAX_CONTEXT=128000` 时会**静默说错话**——那就把「一个数字两个量」换成了
 *  「一个数字两个源」的同型错。引擎不报时才按 env、再按缺省，并把数源一并带回去。 */
export function ctxScale(env?: Record<string, string | undefined>, engine?: { contextWindow?: () => number }): CtxScale {
  let probed = 0
  try {
    probed = Math.max(0, Math.floor(engine?.contextWindow?.() ?? 0))
  } catch {
    probed = 0 // 引擎探测抛错 = 不知道，不是 0 窗口
  }
  if (probed > 0) return { window: probed, windowSource: "引擎自报", ...advisoryOf(env) }
  const fromEnv = countEnv(env?.SHENGSHENG_MODEL_CTX_WINDOW)
  const window = fromEnv ?? DEFAULT_MODEL_CONTEXT_WINDOW
  const windowSource = fromEnv == null ? "缺省值" : fromEnv > 0 ? "env SHENGSHENG_MODEL_CTX_WINDOW" : "env 显式声明未知"
  return { window, windowSource, ...advisoryOf(env) }
}

function advisoryOf(env?: Record<string, string | undefined>): Pick<CtxScale, "advisory" | "advisorySource"> {
  const adv = countEnv(env?.SHENGSHENG_REUSE_ADVISORY_CTX)
  return {
    advisory: adv ?? DEFAULT_REUSE_ADVISORY_LIMIT,
    advisorySource: adv == null ? "缺省值" : adv > 0 ? "env SHENGSHENG_REUSE_ADVISORY_CTX" : "env 显式关闭",
  }
}

/** 上下文水位的**判定档**——四档语义互不相同，不得混用一句话说：
 *  - `over-window`：过了**真实模型窗口** ⇒ **必须退役**（再续派会截断/报错，不是省钱问题）；
 *  - `over-advisory`：过了**复用建议线**但仍在窗口内 ⇒ **可复用，但收益递减**（建议换新，不强制）；
 *  - `near-advisory`：接近建议线（80%↑）⇒ 本轮收完宜换新；
 *  - `ok`：远低于建议线。 */
export type CtxLevel = "ok" | "near-advisory" | "over-advisory" | "over-window"

export interface CtxVerdict {
  tokens: number
  scale: CtxScale
  /** 占**复用建议线**的百分比（未设 ⇒ 0，不代表 0%）。 */
  advisoryPct: number
  /** 占**真实模型窗口**的百分比（窗口未知 ⇒ 0，不代表 0%）。 */
  windowPct: number
  level: CtxLevel
  /** 冷事实的一行：**两个量都报、各自点名**（`ctx 224.0K｜复用建议线 150.0K（149%）｜模型窗口 512.0K（44%）`）。 */
  line: string
  /** 按两个量**分别**给的处置建议（过建议线 ≠ 过窗口，措辞不得混）。 */
  action: string
}

/** 把一个 ctx 读成「两个量 + 一档处置」。`tokens == null` ⇒ `null`（读不到就什么都不报，不编造 0%）。 */
export function ctxVerdict(tokens: number | undefined, scale: CtxScale): CtxVerdict | null {
  if (tokens == null) return null
  const t = Math.max(0, Math.round(tokens))
  const { window, advisory } = scale
  const advisoryPct = advisory > 0 ? Math.round((t / advisory) * 100) : 0
  const windowPct = window > 0 ? Math.round((t / window) * 100) : 0
  // ⚠ 两档判据各用各的分母：**过建议线 ⇒ 复用不划算；过真实窗口 ⇒ 复用根本干不成**。
  // 把两者合并成一个阈，就是把「149% of 建议线」当成「超了模型窗口」——本单要消灭的正是这一句。
  const level: CtxLevel =
    window > 0 && t > window
      ? "over-window"
      : advisory > 0 && t > advisory
        ? "over-advisory"
        : advisory > 0 && t > advisory * 0.8
          ? "near-advisory"
          : "ok"
  const advPart = advisory > 0 ? `复用建议线 ${kilo(advisory)}（${advisoryPct}%）` : `复用建议线：未设（${scale.advisorySource}）`
  const winPart = window > 0 ? `模型窗口 ${kilo(window)}（${windowPct}%）` : `模型窗口：未知（${scale.windowSource}）`
  const line = `ctx ${kilo(t)}｜${advPart}｜${winPart}`
  const action =
    level === "over-window"
      ? `**必须退役**：ctx ${kilo(t)} 已过**模型窗口** ${kilo(window)}——再续派一步就会截断/报错，先 review 抢救结论，再 \`action=retire\`（该领域下次 fresh=true 重开）`
      : level === "over-advisory"
        ? `**可复用，但收益递减**：ctx 是**复用建议线** ${kilo(advisory)} 的 ${advisoryPct}%，${window > 0 ? `而只占**模型窗口** ${kilo(window)} 的 ${windowPct}%——技术上进得去，只是“省钱的复用”到此为止` : `而模型窗口未知（${scale.windowSource}）——离截断还有多远无从判断`}；建议本单收完就换新、别再续派`
        : level === "near-advisory"
          ? `接近**复用建议线**（${advisoryPct}% of ${kilo(advisory)}）——本轮收完宜退役换新（留出空间给下一单）${window > 0 ? `；离模型窗口尚远（${windowPct}%）` : ""}`
          : `远低于**复用建议线**（${advisoryPct}% of ${kilo(advisory)}），可放心复用${window > 0 ? `（模型窗口占 ${windowPct}%）` : ""}`
  return { tokens: t, scale, advisoryPct, windowPct, level, line, action }
}

/** ctx 的可读口径（一行，两个量都报）。读不到就什么都不报。 */
export function ctxLine(tokens: number | undefined, scale: CtxScale): string {
  return ctxVerdict(tokens, scale)?.line ?? ""
}

/** 两个量各自是什么（数源 + env 名）：加在 `state` 读态头部，让池行里的两个百分比不用猜分母。 */
export function ctxScaleLine(scale: CtxScale): string {
  const adv =
    scale.advisory > 0
      ? `复用建议线 ${kilo(scale.advisory)}（${scale.advisorySource}；env SHENGSHENG_REUSE_ADVISORY_CTX）——过线 ⇒ **可复用但收益递减**，建议换新`
      : `复用建议线：未知/关闭（${scale.advisorySource}；env SHENGSHENG_REUSE_ADVISORY_CTX）`
  const win =
    scale.window > 0
      ? `模型窗口 ${kilo(scale.window)}（${scale.windowSource}；env SHENGSHENG_MODEL_CTX_WINDOW）——过窗 ⇒ **必须退役**（否则下次截断/报错）`
      : `模型窗口：未知（${scale.windowSource}；env SHENGSHENG_MODEL_CTX_WINDOW）——过不过窗本息无从判，别把建议线当上限说`
  return `${adv}｜${win}`
}
/** 活动 worker 上限（running + idle）的**缺省值**；实际值由会话 env `SHENGSHENG_MAX_WORKERS` 决定。 */
export const DEFAULT_ACTIVE_WORKER_LIMIT = 3
/** 活动 worker 上限的**从宽上界**：防止环境变量误设（多写一个零）把控制面拖垮。 */
export const MAX_ACTIVE_WORKER_LIMIT = 64
/** 活动 worker 上限解析（每次调用现算）：env 形如 `6`、`1x`、`2|6`（默认|硬顶）。
 *  默认值仍是 3（产品决策不变），要跑得更宽由会话 env 放开——演进会话里做一次性拓宽不必改源码。 */
export function activeWorkerLimit(env?: Record<string, string | undefined>): number {
  const raw = (env?.SHENGSHENG_MAX_WORKERS ?? "").trim()
  if (!raw) return DEFAULT_ACTIVE_WORKER_LIMIT
  const [d, c] = raw.split(/[|/,]/).map((x) => x.trim())
  const cap = /^\d+$/.test(c ?? "") ? Math.max(1, Math.min(Number(c), MAX_ACTIVE_WORKER_LIMIT)) : MAX_ACTIVE_WORKER_LIMIT
  if (!/^\d+$/.test(d)) return cap === MAX_ACTIVE_WORKER_LIMIT ? DEFAULT_ACTIVE_WORKER_LIMIT : cap
  return Math.max(1, Math.min(Number(d), cap))
}
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
      ...(o.resource != null && String(o.resource).trim() ? { resource: String(o.resource).trim() } : {}),
      ...(o.exclusive === true ? { exclusive: true } : {}),
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
    "生生的读态与收息工具（一息入口）。无参=复命：返回项目坐标、呼吸计数、worker 池、在途、**独占资源租约**、种子队列，以及各 worker 的实时态（运行中/空闲/起始时间/待决交互）与**三类回收线索**（判据都是逐个会话的实测结果，不拿登记猜真值）：待收藏线索（在途且会话可达已空闲）；中道而废（在途且会话**确已不存在**——退回种子）；无从核实（在途无会话线索、别名也不属本会话——不可当可达、也不可当已死，须先核实）。池与在途均以**会话 id 为主键**：同一会话只保留最新一轮（旧轮已被新轮取代，不并列、不标脏）。**租约超期回收在读态顺手完成**：持有者会话已不存在 / 已转 idle / 已到期 → 自动释放并显式报出原因（否则一条死租约永久堵院）。action=init 从唤醒消息固化项目坐标（初生第一动作）；seed_add/seed_done 择种入队与销账；note 落断点并推进呼吸（每息收尾）；task 管理心跳驱动任务（op=ensure 幂等确保定时任务存在并启用；op=pause 记暂停+通知用户+停用任务；op=resume 重新启用；op=status 只看状态）。control.md（调度状态）由本工具托管——勿用 read/write 手工编辑。",
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
      const targetRoot = rootArg.trim().replace(/[\\/]+$/, "")
      // ⚠ **只以本次 init 的目标根为准，不得走 `loadState` 的回落链**。
      //
      // 为何（实测事故）：`loadState` 在显式根没有 control.md 时会回落——会话 env
      // `SHENGSHENG_ROOT`（上一条演进线留下的）→ 本会话 init 记忆 → 工作目录向上探测。
      // 于是 `init root=B` 时会出现「读 A 的状态（回显“沿用既有状态：池 2…”）、
      // 写 B 的坐标」——**把另一条演进线的 worker/在途/种子整体带到新根**（跨线污染）。
      // 更阴的是回显看起来正常（“沿用既有状态”），下息才发现 worker 不是自己的。
      // 坐标是**每个根自己的一件事**：目标根没有 control.md 就是初生，**不间任何回落。
      const probe = await readAbs(ctx, evolvePaths(targetRoot).control)
      if (probe.error) return { output: probe.error }
      const initialized = probe.text != null
      const via = initialized ? "参数" : "参数（无既有 control.md）"
      // 坐标覆盖在**锁内**基于**目标根本身**的内容做。
      const m = await mutateControl(ctx, targetRoot, (cur) => {
        const coord: Coord = { ...cur.coord, root: targetRoot }
        for (const [k] of COORD_LABELS) {
          if (k === "root") continue
          const v = args[k === "seedDoc" ? "seed_doc" : k]
          if (v != null && String(v).trim()) coord[k] = String(v).trim()
        }
        cur.coord = coord
        return { next: cur, result: { coord, seeds: cur.seeds.length, workers: cur.workers.length, inflight: cur.inflight.length } }
      })
      if (m.error) return { output: m.error }
      const coord = m.result!.coord
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
          `项目坐标已固化（${via}）${envNote}：${evolvePaths(coord.root!).control}`,
          ...lines,
          initialized
            ? `沿用既有状态：池 ${m.result!.workers}、在途 ${m.result!.inflight}、种子 ${m.result!.seeds}。`
            : "初生：池/在途/种子均空——接着用 action=seed_add 播下第一批种子。",
        ].join("\n"),
        data: { root: coord.root, coord, initial: !initialized },
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
      // 读-改-写**整体**在路径锁内（不是“先 load 后 save”——两者之间有可被并行者插入的窗口）。
      // 判别用显式字段（`dup: null` vs `dup: Seed`）而不是 `in`：TS 对字面量 union 的
      // `in` 窄化会推成 `{}`（实测 TS2638/TS18046），显式 null 两侧都干净。
      type AddResult = { dup: Seed | null; total: number; seeds?: Seed[] }
      const m = await mutateControl<AddResult>(ctx, root, (cur) => {
        const dup = cur.seeds.find((s) => s.direction === seed.direction)
        if (dup) return { result: { dup, total: cur.seeds.length } }
        cur.seeds.push(seed)
        return { next: cur, result: { dup: null, total: cur.seeds.length, seeds: [...cur.seeds] } }
      })
      if (m.error) return { output: m.error }
      const r = m.result!
      if (r.dup !== null) return { output: `种子已存在（未重复入队）：[${r.dup.priority}] ${r.dup.direction}` }
      return { output: `种子入队：[${seed.priority}] ${seed.direction}（来源 ${seed.source}${seed.constraint ? `；约束 ${seed.constraint}` : ""}）——队列现 ${r.total} 条。`, data: { seed, seeds: r.seeds } }
    }

    if (action === "seed_done") {
      const match = String(args.match ?? "").trim()
      if (!match) return { output: "seed_done 需要 match（方向匹配串）。" }
      type DoneResult =
        | { kind: "none"; total: number }
        | { kind: "ambiguous"; hits: Seed[] }
        | { kind: "removed"; removed: Seed; total: number; seeds: Seed[] }
      const m = await mutateControl<DoneResult>(ctx, root, (cur) => {
        const hits = cur.seeds.filter((s) => s.direction.includes(match))
        if (!hits.length) return { result: { kind: "none", total: cur.seeds.length } }
        if (hits.length > 1) return { result: { kind: "ambiguous", hits } }
        cur.seeds = cur.seeds.filter((s) => s !== hits[0])
        return { next: cur, result: { kind: "removed", removed: hits[0], total: cur.seeds.length, seeds: [...cur.seeds] } }
      })
      if (m.error) return { output: m.error }
      const r = m.result!
      if (r.kind === "none") return { output: `种子队列无匹配「${match}」（现 ${r.total} 条）。` }
      if (r.kind === "ambiguous") {
        return { output: `匹配不唯一（${r.hits.length} 条）——请用更具体的 match：\n${r.hits.map((s) => `- [${s.priority}] ${s.direction}`).join("\n")}` }
      }
      return { output: `种子销账（已完成）：[${r.removed.priority}] ${r.removed.direction}——队列剩 ${r.total} 条。`, data: { removed: r.removed, seeds: r.seeds } }
    }

    // ---- note：落断点并推进呼吸（每息收尾）----
    if (action === "note") {
      const note = args.note != null ? String(args.note) : ""
      const clearing = /^（?无）?$/.test(note.trim())
      // 呼吸计数**在此推进**（每息收尾一次）：它是「这条河跑了多久」的唯一计数，
      // 也是断点文本之外唯一能自查的机器可读量——不推进则永远停在初值、与断点叙述相左（登记与真值裂缝）。
      // ⚠ 它必须在**锁内**读-改-写：实测过「note 与 seed_add 并行 ⇒ 呼吸仍为 0、断点为空」
      // （后写者带陈旧基线把整次收息覆盖）——那是历史断档，比丢一条种子更严重。
      const m = await mutateControl(ctx, root, (cur) => {
        cur.note = note.trim() && !clearing ? note.trim() : undefined
        cur.breath += 1
        return { next: cur, result: { breath: cur.breath, note: cur.note, inflight: cur.inflight.length, seeds: cur.seeds.length, workers: cur.workers.length, idle: cur.workers.filter((x) => x.status === "idle").length } }
      })
      if (m.error) return { output: m.error }
      const r = m.result!
      return {
        output: `断点已落盘（呼吸 ${r.breath}）：在途 ${r.inflight}、种子 ${r.seeds}、池 ${r.workers}（${r.idle} idle）${r.note ? `——${r.note}` : "——（无断点）"}。`,
        data: { note: r.note ?? null, breath: r.breath },
      }
    }

    // ---- task：心跳驱动任务管理 ----
    if (action === "task") {
      const op = String(args.op ?? "status").trim() || "status"
      const tasks = tasksOf(ctx)
      if (!tasks) return { output: "任务能力未启用（服务端未挂调度器）——生生无法管理心跳任务。" }
      const all = await tasks.list()
      const explicitId = args.task_id != null ? String(args.task_id).trim() : ""
      // 归属解析：名字候选**一律按任务上的项目根核实**——同名但属他项的任务绝不当成本项目的（见 evolveTaskNames）。
      const explicitName = (ctx.env?.SHENGSHENG_TASK_NAME ?? "").trim()
      const resolved = evolveTaskNames(root, all, LEGACY_TASK_NAME, explicitName)
      const taskName = resolved.name
      const byId = explicitId ? all.find((t) => String(t.id) === explicitId) : undefined
      // 显式点名 id：仍须核归属（id 可能来自别的演进线），核不中即拒——不采信、不改动。
      const byIdForeign = !!explicitId && !!byId && !sameRoot(taskRootOf(byId), root)
      let task = explicitId ? (byIdForeign ? undefined : byId) : all.find((t) => String(t.name ?? "") === taskName)
      /** 同名但属他项的任务（含显式 id 撞他项）：停下来如实报，不复用。 */
      const foreign = byIdForeign
        ? [{ task: byId!, name: String(byId!.name ?? "") }]
        : resolved.conflicts.map((t) => ({ task: t, name: String(t.name ?? "") }))
      const foreignLine = (indent: string) =>
        foreign.length
          ? [
              `${indent}提示：以下同名心跳任务属于**其他项目**，不是本项目的——不得复用、不得停用（要管它们，去那个项目的会话里按 id 操作）：`,
              ...foreign.map((f) => `${indent}  - 「${f.name}」id=${f.task.id}｜项目根 ${taskRootOf(f.task) ?? "（登记无项目根，无从归属）"}`),
            ].join("\n")
          : ""
      /** 「本项目没有心跳任务」的统一表述（status / resume 共用）：一并道出同名但属他项的任务。 */
      const missing = () =>
        [
          explicitId
            ? `未找到心跳任务 id=${explicitId}${byId ? "（该任务属于其他项目，已按归属拒绝）" : ""}。`
            : `本项目（${root}）尚无心跳任务（探测名：${LEGACY_TASK_NAME} / ${evolveTaskName(root)}${explicitName ? ` / ${explicitName}` : ""}）。`,
          foreignLine("-"),
        ].filter(Boolean).join("\n")

      // 显式点名了他项任务的 id：**直接拒绝**（既不采信也不改动，也不顺手另建一份）——
      // 复用/停用别人的演进任务正是本归属校验要杜绝的错。
      if (byIdForeign) {
        return {
          output: [`拒绝：任务 id=${explicitId} 不属于本项目（${root}）——不得复用、不得停用，也不代为新建。`, foreignLine("-")].join("\n"),
          data: { blocked: true, task: byId, foreign: [byId], root },
        }
      }

      if (op === "status") {
        if (!task) return { output: `${missing()}\n——用 action=task op=ensure 创建。`, data: { found: false, foreign: foreign.map((f) => f.task), root } }
        return {
          output: [
            `心跳任务：${task.id}「${task.name ?? ""}」`,
            `- 归属：项目根 ${taskRootOf(task) ?? "（登记无项目根——旧版创建，归属无从核实）"}`,
            `- 状态：${task.enabled ? "启用" : "停用"}｜运行态 ${task.state ?? "idle"}｜已运行 ${task.runCount ?? 0} 次`,
            task.schedule ? `- 周期：${task.schedule}` : "- 周期：（未设置）",
            task.nextRunAt ? `- 下次：${new Date(Number(task.nextRunAt)).toLocaleString("zh-CN")}` : "",
            task.lastStatus ? `- 上次：${task.lastStatus}${task.lastError ? `（${String(task.lastError).slice(0, 200)}）` : ""}` : "",
            foreignLine("-"),
          ].filter(Boolean).join("\n"),
          data: { task, foreign: foreign.map((f) => f.task), root },
        }
      }

      if (op === "resume") {
        if (!task) return { output: `${missing()}——用 op=ensure 创建。`, data: { found: false, foreign: foreign.map((f) => f.task) } }
        const updated = await tasks.update(String(task.id), { enabled: true })
        return { output: `心跳任务已启用：${updated?.id}${updated?.schedule ? `（${updated.schedule}）` : ""}（项目根 ${taskRootOf(task) ?? "未知"}）——演进恢复，下一息由它送达。`, data: { task: updated } }
      }

      if (op === "pause") {
        const reason = String(args.reason ?? "").trim() || "无种可播：待人工补充方向"
        const stamp = new Date().toISOString()
        const led = await appendLedger(ctx, root, `<!-- pause:${stamp} -->`, `## 演进暂停（${stamp}）\n\n${reason}\n\n（恢复：用户启用心跳任务，或 shengsheng_state action=task op=resume。）`)
        if (led.error) return { output: led.error }
        let disabled = ""
        // 停用是有破坏性的写：任务存在但归属核不中时**只按 id 且已核归属**才动（此处 task 已核过归属）
        if (task) {
          const updated = await tasks.update(String(task.id), { enabled: false })
          disabled = updated ? `心跳任务已停用：${updated.id}（项目根 ${taskRootOf(task) ?? "未知"}）` : "心跳任务停用失败"
        } else disabled = `未找到本项目的心跳任务（名 ${taskName}）——无需停用`
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
        // 锁内写（与 note/seed_add 共用同一把锁——并行时不得以陈旧基线覆盖）
        const m = await mutateControl(ctx, root, (cur) => {
          cur.note = `演进暂停：${reason}`
          return { next: cur, result: { note: cur.note } }
        })
        if (m.error) return { output: m.error }
        return { output: [`演进已暂停（藏器于身，待时而动）：`, `- ${disabled}`, `- ${notifyNote}`, `- 台账：${led.path}`, foreignLine("-")].filter(Boolean).join("\n"), data: { paused: true, reason, foreign: foreign.map((f) => f.task) } }
      }

      // op=ensure：幂等确保本项目的心跳任务存在且启用
      const schedule = String(args.schedule ?? "@every 3m").trim() || "@every 3m"
      if (task) {
        const patch: Record<string, unknown> = {}
        if (!task.enabled) patch.enabled = true
        if (task.kind === "scheduled" && String(task.schedule ?? "") !== schedule) patch.schedule = schedule
        // 旧版建的任务若任务级 env 缺项目根（唤醒 prompt 里带也可能被人改），补写一次——
        // 归属从此有机器可读的绑定（幂等：env 已一致时不写）。
        if (!sameRoot(taskEnvRoot(task), root)) patch.env = { ...((task.env as Record<string, string>) ?? {}), SHENGSHENG_ROOT: root }
        const updated = Object.keys(patch).length ? await tasks.update(String(task.id), patch) : task
        task = updated ?? task
        const kindNote = task.kind === "scheduled" ? "" : `\n- 注意：该任务类别为 ${task.kind}（非定时）——心跳不会自动送达，请改用 scheduled 任务或手动 task_run。`
        return {
          output: [
            `心跳任务已存在（幂等，未新建）：${task.id}「${task.name ?? ""}」`,
            `- 归属：项目根 ${taskRootOf(task) ?? "未知"}${sameRoot(taskRootOf(task), root) ? "（与本项目一致）" : ""}`,
            `- 状态：${task.enabled ? "启用" : "停用"}｜周期 ${task.schedule ?? "（未设置）"}${task.nextRunAt ? `｜下次 ${new Date(Number(task.nextRunAt)).toLocaleString("zh-CN")}` : ""}${kindNote}`,
            foreignLine("-"),
          ].filter(Boolean).join("\n"),
          data: { task, created: false, root, foreign: foreign.map((f) => f.task) },
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
        output: [
          `心跳任务已创建：${created.id}「${created.name ?? taskName}」｜项目根 ${root}｜周期 ${schedule}｜执行目标 sticky（下次触发时自动新建专用会话并预载 shengsheng）｜连续失败 5 次自动停用。`,
          `第一息可立即验证：用 task_run 手动跑一次（或等下一定时触发）。`,
          foreignLine("-"),
        ].filter(Boolean).join("\n"),
        data: { task: created, created: true, root, foreign: foreign.map((f) => f.task) },
      }
    }

    // ---- read：复命读态 ----
    const ids = state.workers.map((w) => w.sessionId)
    // 租约持有者也要探：它们可能**不在池**（worker 已退役而租约残留）——不探就无法判“会话已不存在”，
    // 而这条判据恰好是死锁的出口（本单要堵的正是“一条死租约永久堵院”）。
    const leaseIds = state.leases.map((l) => l.sessionId).filter(Boolean)
    const live = svc ? await observe(ctx, svc, [...ids, ...leaseIds]) : {}
    // 上下文口径的两个量（本息现算一次，整段回显共用同一个分母——不得到处各自硬编码）
    const scale = ctxScale(ctx.env, svc?.engine)
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
    const awaiting = trails.filter((t) => t.raw?.exists && !t.raw.running).map((t) => t.i)    // 不可达（会话确实不存在）——退回种子后退役。
    const gone = trails.filter((t) => t.sid && t.raw?.exists === false).map((t) => t.i)
    // **无从核实**（在途无会话线索，旧格式里别名又不属本会话）——单独一栏，不算可达、也不算已死（第三类）：
    // 旧格式在途没会话列、且别名漂移出了池（正是同会话并列条目的历史形态）——此时**无从探会话**。
    // 不归入「中道而废」也不归入「待收藏」：把没探过的东西当已死，与把没探过的东西当可达一样，都是
    // 在登记上猜真值（会话可能还活着、也可能早没了）——归入哪一边都是假信号。
    const unresolved = trails.filter((t) => !t.sid).map((t) => t.i)
    // 虚假产能：池中会话**全部核实为不存在**——复用价值已随会话消失。判据是会话本身的实测结果，
    // 未核实（会话控制面不可用/池空）不得诬告。
    const allGone = !!(svc && active > 0 && state.workers.every((w) => live[w.sessionId]?.exists === false))

    // ---- 租约超期回收（读态顺手做：**不回收就是堵院**）----
    //
    // 为何回收必须在这里：`collect`/`retire` 只在“有人亲手收”时才跑，而持有者掉队的方式远多于此——
    // 会话被外部删除、worker 早已转 idle 而收藏路径没走（或旧版工具没有租约释放）、服务重启后租约还在盘上。
    // 不回收就会死锁：而锁住的恰好是“要独占才敢做”的那类活（负载敏感测量与最终门禁）。
    // 三条判据全部基于**真值**（会话存活/运行态）或**盘上已写明的时间**，不靠推断：
    //   ① 持有者会话已不存在（外部删除/服务重启）；② 持有者已不在跑（池里标 idle，或会话可达但空闲）；
    //   ③ 到期时刻已过（兜底：防“持有者悄悄死掉而登记没被碰”）。
    // 回收在锁内重做（上面算出的只是线索）——否则会以陈旧基线把并行 dispatch 刚写的租约盖回去。
    let leaseRecycled: Array<{ lease: AnyLease; reason: string }> = []
    let leaseRecycleError = ""
    if (state.leases.length) {
      const workerOf = (l: Lease): Worker | undefined =>
        state.workers.find((w) => (!!l.sessionId && w.sessionId === l.sessionId) || (!!l.alias && w.alias === l.alias))
      /** 一条租约的回收理由（空串 = 不回收）。**线索函数**：锁内要对刚读到的租约再算一次。 */
      const recycleReasonOf = (l: Lease, liveIdx: Record<string, Live>, nowMs = Date.now()): string => {
        const w = workerOf(l)
        const sid = l.sessionId || w?.sessionId || ""
        const raw = sid ? liveIdx[sid] : undefined
        if (l.expireAt && l.expireAt <= nowMs) return `已到期（${new Date(l.expireAt).toLocaleString("zh-CN")}）`
        if (!sid) {
          // 无会话线索：只能按池里的别名判；两者都没有即**无从核实**（不回收——与在途的“无从核实”同型：
          // 把没探过的东西当已死，与把没探过的东西当可达一样，都是在登记上猜真值）。
          // 例外：别名也变了（续期换了持者）→ 老线索失效，同样不回收。
          if (w && w.status === "idle" && (!l.alias || w.alias === l.alias)) return "持有者已空闲（池中登记为 idle，无会话可核）"
          return ""
        }
        if (!!ctx.sessionId && sid === ctx.sessionId) return ""   // 本会话自己的——见下
        if (svc && raw && raw.exists === false) return "持有者会话已不存在"
        // 空闲一档必须两重确认：
        //   ① **池登记标 idle**（worker 已交付/已被收）—— 这条当即可信；
        //   ② 或**观测到会话可达但不在运行**且租约已过启动宽限（见 {@link LEASE_START_GRACE_MS}）
        //      ——为何要宽限：`dispatch` 返回后引擎把会话拉起来有延迟，窄窗口内“未运行”不等于“已下班”。
        if (w?.status === "idle" && (!sid || !raw || raw.exists)) return "持有者已空闲（已交付/已被标 idle，租约不再有意义）"
        const isIdle = svc && raw ? !raw.running : false
        if (isIdle && nowMs - (l.start || 0) > LEASE_START_GRACE_MS) {
          return "持有者已空闲（会话可达且不在运行，已过启动宽限）"
        }
        return ""
      }
      const reasons = new Map<string, string>()
      for (const l of state.leases) {
        const r = recycleReasonOf(l, live)
        if (r) reasons.set(resourceKey(l.resource), r)
      }
      if (reasons.size) {
        const m = await mutateControl<Array<{ lease: Lease; reason: string }>>(ctx, root, (cur) => {
          const killed: Array<{ lease: Lease; reason: string }> = []
          // 锁内**重算**（不只重看 expireAt）：并行 dispatch 可能刚把这条租约续给别人（别名/会话都变了），
          // 拿本次调用开头算出的线索去删它，就会删掉一张**新的**租约——那是用陈旧基线做破坏的另一种形态。
          cur.leases = cur.leases.filter((l) => {
            const reason = recycleReasonOf(l, live)
            if (!reason) return true
            killed.push({ lease: l, reason })
            return false
          })
          return killed.length ? { next: cur, result: killed } : { result: [] }
        })
        if (m.error) {
          // 不静默：回收失败就是“院子可能仍被堵着”，必须让模型看到
          leaseRecycleError = `租约回收失败（${m.error}）——资源可能仍被占着，下面显示的租约未过期。`
        } else leaseRecycled = m.result!
      }
      // 回显以**回收后**的租约为准：从盘上重读一遍（不凭本次推导）——租约是共享态，
      // 并行 dispatch 可能刚领了一条，拿旧那份会把新持者隐藏。
      if (leaseRecycled.length) {
        const fresh = await loadState(ctx, root)
        if (fresh.initialized) state.leases = fresh.state.leases
      }
    }
    // ---- 仓级（跨线）租约：读态合并展示 + 超期回收 ----
    //
    // 为何要在**本线读态**里做：仓级租约可能属于另一条演进线，它自己的 `state` 才能把它收回去；
    // 但如果它已经彻底停了（心跳被停用、会话被删），就没人会去收。本线读态是唯一的观察窗口，
    // 所以连带把**能看到且能判死的**仓级租约一并回收（与本地域同一套判据，另加“去看它的线自己的登记”）。
    let repoLeases: RepoLease[] = []
    let repoRecycled: Array<{ lease: RepoLease; reason: string }> = []
    let repoLeaseError = ""
    let repoTop: string | null = null
    let repoTopReason = ""
    {
      const probe = await repoTopOf(ctx, root)
      repoTop = probe.top
      repoTopReason = probe.reason
      if (probe.top) {
        const rd = await readSharedLeases(ctx, probe.top)
        if (rd.error) repoLeaseError = `仓级租约表读不出来（${rd.error}）——跨线互斥可能未生效`
        else {
          repoLeases = rd.leases
          // 先把可能被回收的会话一根探齐（仓级持者可能不在本线池中）
          const extra = repoLeases.map((l) => l.sessionId).filter((s) => s && !live[s])
          if (svc && extra.length) Object.assign(live, await observe(ctx, svc, extra))
          type RepoKilled = Array<{ lease: RepoLease; reason: string }>
          const m = await mutateSharedLeases<RepoKilled>(ctx, probe.top, async (cur) => {
            const killed: RepoKilled = []
            const keep: RepoLease[] = []
            for (const l of cur) {
              // 锁内**重算**（与本地域同一理由：并行续期换了持者，不得拿陈旧线索删新租约）
              const reason = await repoRecycleReason(ctx, svc, l, Date.now(), (sid) => live[sid], root)
              if (reason) killed.push({ lease: l, reason })
              else keep.push(l)
            }
            return killed.length ? { next: keep, result: killed } : { result: killed }
          })
          if (m.error) repoLeaseError = `仓级租约回收失败（${m.error}）——跨线资源可能仍被占着`
          else if (m.result!.length) {
            repoRecycled = m.result!
            repoLeases = repoLeases.filter((l) => !repoRecycled.some((r) => r.lease === l || (r.lease.resource === l.resource && r.lease.sessionId === l.sessionId)))
          }
        }
      }
    }
    // 合并展示：**本地表的行不再重复展示仓级同名资源**（仓级行自带根名，信息更全）。
    const repoNames = new Set(repoLeases.map((l) => resourceKey(l.resource)))
    const localOnly = state.leases.filter((l) => !repoNames.has(resourceKey(l.resource)))
    const showLeases = state.leases.length + repoLeases.length
    const lines = [
      `坐标（${loaded.via}）：${root}`,
      `- 验证命令：${state.coord.verify ?? "（未固化）"}`,
      state.coord.seedDoc ? `- 种子库：${state.coord.seedDoc}` : "",
      state.coord.rules ? `- 硬约束：${state.coord.rules}` : "",
      `呼吸：${state.breath}｜池 ${active}（${idle} idle，运行中 ${runningNow}）｜在途 ${state.inflight.length}｜租约 ${showLeases}${repoLeases.length ? `（含仓级 ${repoLeases.length}）` : ""}｜种子 ${state.seeds.length}｜收藏待办 ${state.pending.length}`,
      // 上下文口径的**两个量各自是什么**（数源一并报）：不写这行时，下面池行里的两个百分比
      // 只能靠猜分母——而「一个数字两个量」正是本仓反复撞的坡。
      `上下文口径（本息判定阈值，env 可改）：${ctxScaleLine(scale)}`,
      state.note ? `断点：${state.note}` : "",
      active && idle === active && allGone ? "池内 worker 的会话全部不可达（已逐会话核实）——**池中 idle 是虚假产能**：复用价值已随会话消失，布雨须 fresh=true。" : "",
      // 租约回收：**必须显式报出**（回收与原因两件事）——否则模型会以为院子空着、或以为还占着。
      // 不报的代价是双向的：
      //   ① 报“空着”而其实已回收 → 下息白等一个不存在的持者；
      //   ② 不报“回收”而其实释放了 → 它会以为资源已被自己占着，继续派负载敏感的活。
      leaseRecycled.length
        ? `租约回收（${leaseRecycled.length} 条，已释放）：\n${leaseRecycled.map((r) => `- ${leaseDisplayName(r.lease)}｜原持者 ${r.lease.alias}（会话 ${idShort(r.lease.sessionId)}）｜原因：${r.reason}｜用途：${r.lease.purpose || "（未登记）"}`).join("\n")}\n（已写入 control.md；下息此类活可重新申请）`
        : "",
      leaseRecycleError,
      // 仓级（跨线）租约：**必须显式报出“同仓还有另一条线”**——否则模型看到本线表空着，
      // 会以为院子空着，而它其实被另一条线（另一份 control.md）占着。这正是本单要消灭的裂缝。
      repoRecycled.length
        ? `仓级租约回收（${repoRecycled.length} 条，已释放——跨线）：\n${repoRecycled.map((r) => `- ${r.lease.resource}｜原持者 ${rootLabel(r.lease.root)} 的 ${r.lease.alias}（会话 ${idShort(r.lease.sessionId)}）｜原因：${r.reason}｜用途：${r.lease.purpose || "（未登记）"}`).join("\n")}`
        : "",
      repoLeaseError,
      showLeases
        ? `独占资源租约（同一资源同时至多一位持者；派工声明 resource/exclusive 时自动领用）：\n${[
            ...repoLeases.map((l) => repoLeaseLine(l)),
            ...localOnly.map((l) => `- ${l.resource}｜持者 ${l.alias}（会话 ${idShort(l.sessionId)}）｜起租 ${new Date(l.start).toLocaleString("zh-CN")}｜到期 ${leaseExpiryText(l)}｜${l.purpose || "（未登记用途）"}`),
          ].join("\n")}`
        : "独占资源租约：（空——无人独占，负载敏感的测量与门禁可申请）",
      repoTop
        ? `仓级租约域：${repoSharedPaths(repoTop).file}（同一 git 仓上的**所有**演进线共享；每条线另有自己的 .evolve/control.md）`
        : `仓级租约域：**未启用**（${repoTopReason}）——只有本线 .evolve 的租约在生效，同仓其他线不可见`,
      // 反向提示：本线没有仓级租约但同仓**别人有**时，也要说清（否则它以为“没人占”）。
      !repoLeases.length && state.leases.length ? "（本线租约均在本地域：同仓暂无别的线持着仓级资源）" : "",
      state.inflight.length ? `在途：\n${state.inflight.map((i) => `- ${i.alias}｜会话 ${idShort(inflightSession(i, state.workers))}｜${i.direction}${i.deliverable ? `｜交付 ${i.deliverable}` : ""}`).join("\n")}` : "在途：（空）",
      state.workers.length ? `池：\n${state.workers.map((w) => liveLine(w.alias, w, live[w.sessionId], scale)).join("\n")}` : "池：（空）",
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
              active >= activeWorkerLimit(ctx.env) ? `活动 worker 已达上限 ${activeWorkerLimit(ctx.env)}（可用 env SHENGSHENG_MAX_WORKERS 放宽）——本息只回收不布雨。` : "",
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
        leases: state.leases,
        repoLeases,
        repoShared: repoTop ? repoSharedPaths(repoTop).file : null,
        repoTop,
        leaseRecycled,
        repoRecycled,
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
  /** 本单申请的独占资源名（归一后；空串 = 不申请）。见 {@link Lease}。 */
  resource?: string
  /** 执行阶段被租约挡下时的持者（不派、方向退回种子）——预检时也可能带出。 */
  leaseBlocked?: AnyLease
  /** 本单实际获得的租约（派遣时回显用）。 */
  lease?: AnyLease
}

/** 租约申请结果：`granted` = 已持有（`renewed` 表续期），`blocked` = 被他人持着（未写盘，方向已退回种子）。
 *  `scope` 说明挡在哪个域：`repo` = **仓级**（可能来自另一条演进线），`local` = 本线 `.evolve`。 */
type LeaseClaim =
  | { kind: "granted"; lease: AnyLease; renewed: boolean; seeded: false; scope: LeaseScope; note?: string }
  | { kind: "blocked"; holder: AnyLease; seeded: boolean; scope: LeaseScope }

type LeaseScope = "repo" | "local"

/**
 * 在**控制面锁内**申请/续期一条独占资源租约。
 *
 * ## 为何必须“先领租约、后开跑”（而不是“跑完再登记”）
 *
 * 两度实证的事故都是**时间窗口**问题：两个 worker 各自看到“负载降了”就开跑（第二次甚至是一个 worker
 * 故意压满 CPU、另一个正在跑门禁）。“先跑后登记”只是把事故记下来，不阻止它——租约的全部价值就在**先**。
 * 所以本函数在 `svc.run` **之前**调用，且整个“查冲突 → 写租约”在 `mutateControl` 的**同一把锁内**完成：
 * 同一批多单、同一息多个 dispatch 调用、两个演进会话同时布雨，都只能有一位持者。
 *
 * ## 续期语义
 *
 * 同一会话/别名再申请同名资源 = **续期**（`start` 保留、`expireAt` 前推、`purpose` 更新），不是重占：
 * 1) 不会被**自己**挡住（否则续派自己的长测量会永远轮不上）；
 * 2) 不并列两条（同一资源同时至多一位持者，读侧归一与写侧 upsert 同源）。
 */
async function claimLease(
  ctx: ToolContext,
  root: string,
  req: { resource: string; alias: string; sessionId: string; purpose: string; minutes: number; now: number; seed?: Seed },
): Promise<{ claim?: LeaseClaim; error?: string }> {
  const key = resourceKey(req.resource)
  // ---- ①仓级（共享表）先领：“同一棵树”的互斥必须跨线生效 ----
  //
  // 为何仓域在前：本地域只看得见本线的租约表。若先写本地再去碰共享表，就会出现
  // 「本地已写、仓级被别人挡着」的半个状态——那个窗口里本线会以为自己持着资源而开跑。
  // 仓域先成事，本地只作镜像与降级兼容。
  const probe = await repoTopOf(ctx, root)
  const repoName = probe.top ? repoResourceName(probe.top, key) : ""
  let degradedNote = ""
  if (probe.top && repoName) {
    type RepoOutcome =
      | { kind: "granted"; lease: RepoLease; renewed: boolean }
      | { kind: "blocked"; holder: RepoLease }
    const m = await mutateSharedLeases<RepoOutcome>(ctx, probe.top, (cur) => {
      const holder = repoLeaseOf(cur, repoName)
      if (holder && !leaseHeldBy(holder, req.sessionId, req.alias)) return { result: { kind: "blocked", holder } }
      const lease: RepoLease = holder
        ? { ...holder, alias: req.alias, sessionId: req.sessionId || holder.sessionId, expireAt: req.now + req.minutes * 60_000, purpose: req.purpose, root }
        : { resource: repoName, alias: req.alias, sessionId: req.sessionId, start: req.now, expireAt: req.now + req.minutes * 60_000, purpose: req.purpose, root }
      return {
        next: [...cur.filter((l) => resourceKey(l.resource) !== resourceKey(repoName)), lease],
        result: { kind: "granted", lease, renewed: !!holder },
      }
    })
    if (m.error) {
      // **安静降级**（不能因此让 dispatch 起不来）：拿不到共享表就按“只有本地域”跑。
      // 但必须回显说出来——静默降级会让模型以为跨线互斥生效了，而它其实只在本地域生效。
      degradedNote = `仓级租约表不可用（${m.error}）——本次只在本地域互斥，跨线互斥**未生效**`
    } else {
      const o = m.result!
      if (o.kind === "blocked") {
        // 被挡：方向退回种子（与本地域的 blocked 同一口径：不派、不丢方向）
        let seeded = false
        if (req.seed) {
          const seed = req.seed
          const pushed = await mutateControl<boolean>(ctx, root, (cur) => {
            if (cur.seeds.some((x) => x.direction === seed.direction)) return { result: false }
            cur.seeds.push(seed)
            return { next: cur, result: true }
          })
          if (!pushed.error) seeded = !!pushed.result
        }
        return { claim: { kind: "blocked", holder: o.holder, seeded, scope: "repo" } }
      }
      // 本地镜像：同一资源名（仓级全名）写进 control.md，读态合并时才不会重复展示。
      const mirror = await mutateControl<Lease>(ctx, root, (cur) => {
        const prev = cur.leases.find((l) => resourceKey(l.resource) === resourceKey(repoName))
        const lease: Lease = {
          resource: repoName,
          alias: req.alias,
          sessionId: req.sessionId || prev?.sessionId || "",
          start: prev?.start ?? req.now,
          expireAt: o.lease.expireAt,
          purpose: req.purpose,
        }
        cur.leases = dedupeLeases([...cur.leases.filter((l) => resourceKey(l.resource) !== resourceKey(repoName)), lease])
        return { next: cur, result: lease }
      })
      if (mirror.error) return { error: `仓级租约已领但本地镜像落盘失败——${mirror.error}` }
      return { claim: { kind: "granted", lease: o.lease, renewed: o.renewed, seeded: false, scope: "repo" } }
    }
  }

  // ---- ②本地域（control.md）：无 git 顶层（或仓级表不可用）时的唯一防线 ----
  const m = await mutateControl<LeaseClaim>(ctx, root, (cur) => {
    const holder = leaseOf(cur, key)
    if (holder && !leaseHeldBy(holder, req.sessionId, req.alias)) {
      // 被他人持着：**不派**，并把方向退回种子（与“额度只挡新建、不静默丢”同型）。
      // ret:seeded 只在真入队时为 true（同方向已在队列就不重复）。
      let seeded = false
      if (req.seed && !cur.seeds.some((s) => s.direction === req.seed!.direction)) {
        cur.seeds.push(req.seed)
        seeded = true
      }
      return { next: cur, result: { kind: "blocked", holder, seeded, scope: "local" } }
    }
    const lease: Lease = holder
      ? { ...holder, alias: req.alias, sessionId: req.sessionId || holder.sessionId, expireAt: req.now + req.minutes * 60_000, purpose: req.purpose }
      : { resource: key, alias: req.alias, sessionId: req.sessionId, start: req.now, expireAt: req.now + req.minutes * 60_000, purpose: req.purpose }
    cur.leases = dedupeLeases([...cur.leases.filter((l) => resourceKey(l.resource) !== key), lease])
    return { next: cur, result: { kind: "granted", lease, renewed: !!holder, seeded: false, scope: "local", ...(degradedNote ? { note: degradedNote } : {}) } }
  })
  return m.error ? { error: m.error } : { claim: m.result! }
}

/** 租约的**短名**（回显用）：仓级租约的完整名是 `repo:<顶层>:<资源>`，人读太长；
 *  只取末段并标一句“仓级”——模型与人看“独占 cpu-exclusive（仓级）”就够，
 *  完整顶层路径在 `state` 的仓级租约域行与租约表里（可查）。 */
export function leaseShortName(l: AnyLease): string {
  if (!isRepoLease(l)) return l.resource
  const tail = l.resource.split(":").pop() || l.resource
  return `${tail}（仓级）`
}

/** 租约冲突的**回显句**（四件事缺一不可：被谁占着、**哪条线**占着、何时到期、下息再试）。
 *  头三件少一件都会让模型卡住（实测：只说“被占用”时它会原地等或换人硬派）；
 *  “哪条线”只在**仓级**冲突时出现——它就是“同一棵树上的另一条演进线”存在的证据（模型据此知道
 *  该去找哪条线请它释放，而不是在本线里换人硬派）。 */
function leaseBlockedText(holder: AnyLease, now = Date.now()): string {
  const who = isRepoLease(holder)
    ? `被 ${rootLabel(holder.root)} 的 ${holder.alias} 占着（会话 ${idShort(holder.sessionId)}｜同一 git 仓的仓级租约）`
    : `被 ${holder.alias} 占着（会话 ${idShort(holder.sessionId)}）`
  return `${who}｜到期 ${leaseExpiryText(holder, now)}｜用途：${holder.purpose || "（未登记）"}｜**下息再试**`
}

/** 租约用途串（写进租约表与回显）：哪一单、为什么必须独占（可读、能回溯到工单文本）。 */
const leasePurposeOf = (plan: PlanItem): string => `dispatch:${plan.alias}｜${plan.task.slice(0, 80)}`

function ensureUniqueAlias(state: EvolveState, order: Order, taken: Set<string>): string {
  const requested = order.alias ? sanitizeAlias(order.alias) : ""
  if (requested && !taken.has(requested) && !state.workers.some((w) => w.alias === requested)) return requested
  return deriveAlias(order.domain, new Set([...taken, ...state.workers.map((w) => w.alias)]))
}

const dispatchTool: Tool = {
  name: "dispatch",
  description:
    "布雨：把一批工单派给 worker 会话（一息一次调用，1~8 单）。同领域 idle worker 直接复用（摸底归零、记忆延续），异领域或 fresh=true 时新建会话；**续派**——orders[].session_id 指向池中既有 worker 会话（只认本演进线派生的 worker），用于中断后接着干或人工抢跑后续跑（详见 description 末）。派工消息按模板装配（任务/上下文/交付/验证命令/纪律/预载/无人值守提示——代码保证不漏项），注入后立即返回**不等 worker 完成**，并自动登记池与在途。**派工即授权**：本工具的审批就是该 worker 全部动作的授权（新建/复用/续派三条路一视同仁）——worker 以无人值守姿态运行（需审批工具自动通过、无交互通道），不会为单个工具等待确认。活动 worker（运行中+空闲）上限 3：**只挡新建**——复用与续派不改池容量，不受限。**独占资源**：`orders[].resource=\"cpu-exclusive\"`（或简写 `exclusive: true`）声明本单需独占整台机器（负载敏感的性能测量/最终门禁）；该资源已被**别人**持着时不派、把方向退回种子、下息再派（回显说清「被谁占着、何时到期」）——同一持有者再申请 = 续期，不挡自己。派工消息自包含——worker 是普通会话，只凭这一条消息独立干活。不传 orders 或 env SHENGSHENG_DISPATCH=preview 时只出计划不派工（预检）。",
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
            resource: { type: "string", description: "申请独占资源（自由串，缺省名 cpu-exclusive）——负载敏感的性能测量/最终门禁/计时实验必须声明：本单开工期间同名资源不得被其他 worker 持着。已被别人持着时不派、方向退回种子队列、下息再试（回显说清持者与到期时刻）。不声明 = 不申请，不受任何租约限制" },
            exclusive: { type: "boolean", description: "resource=\"cpu-exclusive\" 的简写：true 即申请独占整台机器（负载敏感测量与门禁用）" },
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
      plans.push({ alias, domain, task: order.task, sessionId, reuse, created: !reuse && !sessionId, revived, resume, resource: orderResource(order) || undefined })
    }

    // 池容量：活动 worker（running + idle）上限——**只挡新增**（新建），不阻断复用/续派（它们不增活动数）。
    // 超出部分**本批不新建**（不整批拒绝：可复用的单照派、把新建的单如实回给模型下息补）——
    // 整批原子拒绝会让一息凭空白费，是本仓实测的产能浪费形态。
    const limit = activeWorkerLimit(ctx.env)
    const room = Math.max(0, limit - state.workers.length)
    const plannedNew = plans.filter((p) => p.created)
    const over = new Set(plannedNew.slice(room))
    const overflowNote = over.size
      ? `容量提示：活动 worker 当前 ${state.workers.length}、上限 ${limit}——本批 ${over.size} 单新建已顺延到下息（回收后再派）；复用/续派不受限。`
      : ""

    if (preview) {
      // 预检**也算租约**（只读）：只说“会派”而隐去“会被占着”是骗人的预检——
      // 预检的价值就在“先看排布”，而租约冲突恰好是排布里最需要看见的一项。
      // 仓级租约也要看：只看本地表时，预检会说“可领”而实际会被另一条线挡下。
      const previewProbe = await repoTopOf(ctx, root)
      const previewRepo = previewProbe.top ? (await readSharedLeases(ctx, previewProbe.top)).leases : []
      const previewLease = (p: PlanItem): string => {
        if (!p.resource) return ""
        const repoName = previewProbe.top ? repoResourceName(previewProbe.top, p.resource) : ""
        const rh = repoName ? repoLeaseOf(previewRepo, repoName) : undefined
        if (rh && !leaseHeldBy(rh, p.sessionId, p.alias)) return `｜独占 ${p.resource}：**未派（${leaseBlockedText(rh)}）**`
        const h = leaseOf(state, p.resource)
        if (!h) return `｜独占 ${p.resource}（可领${repoName ? "·仓级" : ""}）`
        if (leaseHeldBy(h, p.sessionId, p.alias)) return `｜独占 ${p.resource}（自己持着，续期）`
        return `｜独占 ${p.resource}：**未派（${leaseBlockedText(h)}）**`
      }
      return {
        output: [
          `布雨预检（未派工${svc ? "" : "；会话控制面不可用"}）：`,
          ...plans.map((p) => `- ${p.alias}｜${over.has(p) ? "新建（超出上限，待下息）" : p.created ? "新建" : p.resume ? `续派会话 ${p.sessionId}` : "复用（同域 idle）"}｜${p.domain}｜${p.task}${previewLease(p)}`),
          overflowNote,
          previewProbe.top ? `仓级租约域：${repoSharedPaths(previewProbe.top).file}（同仓 ${previewRepo.length} 条）` : `仓级租约域未启用（${previewProbe.reason}）`,
        ].filter(Boolean).join("\n"),
        data: { preview: true, plan: plans, limit, over: [...over].length, leases: state.leases, repoLeases: previewRepo },
      }
    }

    const envPatch = workerEnv(root, state.coord, ctx.sessionId)
    const now = Date.now()
    const leaseMin = leaseMinutes(ctx.env)
    const kicked: Promise<void>[] = []
    const results: string[] = []
    /** 被顺延时的种子（优先沿用队列里那条的 priority 等字段）——方向不得因租约冲突而丢。 */
    const seedForBlocked = (cur: EvolveState, task: string, alias: string, resource: string): Seed =>
      cur.seeds.find((x) => x.direction === task) ?? {
        priority: "待定",
        direction: task,
        source: `独占顺延 ${alias}@${resource}`,
        constraint: "资源被他人持着，下息重派（方向未丢）",
      }
    // 派工轮次号逐单递增（幂等键来源）；只有**真正派出**的单占号，被挡下的不占
    let nextSeq = state.seq
    for (let i = 0; i < plans.length; i++) {
      const plan = plans[i]
      const order = orders[i]
      try {
        // 超出活动上限的新建单：本批顺延（不占额度、不占轮次号）
        if (over.has(plan)) {
          results.push(`- ${plan.alias}：**未派**（活动 worker 达上限 ${limit}，当前 ${state.workers.length}——下息回收后再派）`)
          continue
        }
        // ① 独占资源：**先领租约，后开跑**。
        //
        // 位置很关键——放在**会话新建与 svc.run 之前**：两度实证的事故都是“两个 worker 都以为自己
        // 是唯一在跑的”（一次是两边都看到 load 降就一起跑，一次是一个故意压满 CPU、另一个跑门禁）。
        // 跑完再登记只是把事故记下来；租约的**全部价值**是先于开跑。
        // 被占时不派、方向退回种子、回显说清三件事（见 leaseBlockedText）。
        if (plan.resource) {
          const claim = await claimLease(ctx, root, {
            resource: plan.resource,
            alias: plan.alias,
            sessionId: plan.sessionId,
            purpose: leasePurposeOf(plan),
            minutes: leaseMin,
            now,
            seed: seedForBlocked(state, order.task, plan.alias, plan.resource),
          })
          if (claim.error) {
            results.push(`- ${plan.alias}：**未派**（独占资源租约落盘失败——${claim.error}）`)
            continue
          }
          const c = claim.claim!
          if (c.kind === "blocked") {
            plan.leaseBlocked = c.holder
            const scopeNote = c.scope === "repo" ? "（**跨线**：同一 git 仓上的另一条演进线持着它）" : ""
            results.push(
              `- ${plan.alias}：**未派**（独占资源 \`${plan.resource}\` ${leaseBlockedText(c.holder)}${scopeNote}）${c.seeded ? "；方向已退回种子队列" : "；方向仍在种子队列（未重复）"}`,
            )
            continue
          }
          plan.lease = c.lease
          // 降级必须说出来（仓级表不可用 → 只在本地域互斥，跨线互斥未生效）；
          // 不说就会让模型以为跨线互斥生效了——而那正是本单要堵的裂缝。
          if (c.kind === "granted" && c.note) results.push(`- ${plan.alias}：⚠ ${c.note}`)
        }
        if (plan.created) {
          const created = await svc!.store.createSession(ctx.user, `evolve-w-${plan.alias}`)
          plan.sessionId = created.id
          plan.note = `新建会话（名 evolve-w-${plan.alias}）`
          try {
            // 标记要写**本 worker 的会话 id**（不是派发者会话）：新建会话的 id 此时才存在，
            // 故基础 envPatch（带派发者 id）之外再盖一层——否则子进程会带着**派发者**的标记，
            // retire 那个 worker 时反而碰不到它自己的子进程（而 retired 派发者会误伤）。
            const scoped = { ...envPatch, [PROC_MARKER_ENV]: created.id }
            await svc!.store.setEnv(created.id, ctx.user, scoped)
            plan.note += `，注入项目坐标 env ${Object.keys(scoped).length} 项`
          } catch (err) {
            plan.note += `，env 注入失败（${describeError(err)}）`
          }
          // 会话 id 在**领租约之后**才存在：先按**别名**占住资源（别人已进不来），
          // 这里把会话列补上（续期语义：start 保留、不并列两条）。补不上也不算灾难——
          // 空会话列的租约在 `state` 读态按「持有者已不存」回收，不会永久堵院。
          if (plan.resource && plan.lease && !plan.lease.sessionId) {
            const bound = await claimLease(ctx, root, {
              resource: plan.resource,
              alias: plan.alias,
              sessionId: plan.sessionId,
              purpose: leasePurposeOf(plan),
              minutes: leaseMin,
              now,
            })
            if (bound.claim?.kind === "granted") plan.lease = bound.claim.lease
          }
        } else {
          // 复用/续派：会话 env 是内存态（服务重启即空）——重注入项目坐标，使派工消息里
          //「环境：本会话已注入…」为真（续跑多发生在中断/重启之后，此时 env 往往已丢）。
          const kind = plan.resume ? "续派" : "复用"
          try {
            // 标记写**被复用会话自己的 id**（复用者与被复用者是同一会话，id 不变）
            await svc!.store.setEnv(plan.sessionId, ctx.user, { ...envPatch, [PROC_MARKER_ENV]: plan.sessionId })
            plan.note = `${kind}会话 ${plan.sessionId}（项目坐标 env 已重注入）`
          } catch (err) {
            plan.note = `${kind}会话 ${plan.sessionId}（env 重注入失败：${describeError(err)}）`
          }
        }
        if (!plan.sessionId) {
          await releaseWorkerLeases(ctx, root, plan.sessionId, plan.alias)
          results.push(`- ${plan.alias}：**未派**（无可用会话且新建失败）`)
          continue
        }
        const live = (await observe(ctx, svc!, [plan.sessionId]))[plan.sessionId]
        if (live && live.exists && live.running) {
          // 未开跑就不该持租约（否则一个没跑的单会替别人堵住资源）
          await releaseWorkerLeases(ctx, root, plan.sessionId, plan.alias)
          results.push(`- ${plan.alias}：**未派**（会话 ${plan.sessionId} 正在运行——同会话并发注入会被拒；若它卡在待决审批（无人应答约 5 分钟超时），等本轮结束再用 session_id 续派）`)
          continue
        }
        if (live && !live.exists) {
          await releaseWorkerLeases(ctx, root, plan.sessionId, plan.alias)
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
        // 登记更新（轮次 + 池 upsert + 在途 upsert）在**路径锁内基于刚读到的状态**做——
        // 否则并行调用（同一息里模型并发发单）会以陈旧基线互相覆盖。
        const reg = await mutateControl(ctx, root, (cur) => {
          // 全局派工序号：只增不改其它字段，是台账幂等键的**跨会话唯一源**。
          cur.seq += 1
          const seq = cur.seq
          const prevW = cur.workers.find((w) => w.sessionId === plan.sessionId)
          // 《轮次》= **该会话承接的第几轮**（首派 1）——**不是**全局派工序号。
          // 为何必须分开：round 同时是 ①读侧归一的胜负判据（dedupeBySession 取最大轮）
          // ②复用代数 `prev.round + 1` ③收藏幂等键 `collect:<sid>:r<round>`——用全局序号
          // 会让「同会话轮次」与「全局序号」的大小关系相反时选错条目、幂等键漂移。
          // （实测：连派两个全新会话 → 第二个首派却写成轮次 2。）
          const round = (prevW?.round ?? 0) + 1
          const worker: Worker = {
            alias: plan.alias,
            sessionId: plan.sessionId,
            domain: plan.domain,
            task: plan.task.slice(0, 80),
            dispatchedAt: prevW?.dispatchedAt ?? now,
            round,
            status: "running",
          }
          cur.workers = dedupeWorkers([...cur.workers.filter((w) => w.sessionId !== plan.sessionId && w.alias !== plan.alias), worker])
          const inflight: Inflight = { alias: plan.alias, sessionId: plan.sessionId, direction: order.task, deliverable: order.deliver ?? "", dispatchedAt: now, round }
          // 在途 upsert **按会话 id 取代**——仅按别名过滤会漏掉旧名残留（别名漂移），而它随后可能因轮次
          // 更大而在归一里胜出、顶掉本轮新登记。写侧与读侧（parseControl 的归一）因此共用同一套身份键。
          cur.inflight = dedupeInflight([...cur.inflight.filter((x) => x.alias !== plan.alias && inflightIdentity(x, cur.workers) !== plan.sessionId), inflight], cur.workers)
          // **派出去即销账**：销掉与本次方向对应的种子。
          //
          // 为何必须现做（不是“等收藏时再说”）：章程规定「择定的方向先 `seed_add` 落笔，再布雨」——
          // 派完后种子若留在队列，下一息读态会把它当“待派方向”**再派一次**（真重复烧一条 worker 命）。
          // 实测（本轮）：种子 1 条 + 派它后 → 种子仍在、在途也有一份，同一方向两处并存（“脱节的清单”同型）。
          // 只销**同一方向**的条目（不是全部种子）；带 priority 等其它字段不动——这里只做“这条已经派出去了”这一件事。
          const seeded = cur.seeds.findIndex((s) => s.direction === order.task)
          const consumed = seeded >= 0 ? cur.seeds[seeded] : null
          if (seeded >= 0) cur.seeds.splice(seeded, 1)
          return { next: cur, result: { seq, round, generation: round, workers: cur.workers.length, inflight: cur.inflight.length, consumed } }
        })
        if (reg.error) {
          results.push(`- ${plan.alias}：已注入但登记落盘失败——${reg.error}`)
          continue
        }
        const { seq, generation, round, consumed } = reg.result!
        nextSeq = seq
        state.seq = seq
        // 复用代数提示：同一会话连续承接的单数（首派 1）——达此值建议收藏后退役换新上下文。
        // 它不是“上限”：它是「同一会话承接数」这个量的经验阀（与 ctx 的两个量无关，也不是窗口）。
        const genNote = generation >= REUSE_GENERATION_HINT ? `｜**已承接 ${generation} 单**（上下文渐胖，宜收藏后退役换新）` : ""
        const kind = plan.created ? "新建" : plan.resume ? "续派" : "复用"
        // 种子销账要回工（否则生生无法确认“这条不会再被派一次”）：只在真有对应种子时提一句。
        const seedNote = consumed ? `｜种子已销账（[${consumed.priority}]）` : ""
        // 独占租约也要回工：它是本单**与别人共存**的条件，不回工则模型不知道院子已被自己占住、
        // 下息可能又派一单负载敏感的活出去（两边互污染）。
        // 仓级租约要显式标“仓级”：本线以外的读者看不到 repo 前缀，只说资源名会误以为它只在本地域。
        const leaseNote = plan.lease ? `｜独占 ${leaseShortName(plan.lease)}（到期 ${leaseExpiryText(plan.lease, now)}）` : ""
        results.push(`- ${plan.alias}｜${kind}会话 ${plan.sessionId}｜${plan.domain}｜轮次 ${round}｜${plan.task.slice(0, 60)}${genNote}${seedNote}${leaseNote}`)
      } catch (err) {
        // 中途失败也不得留租约：租约的存在就是「有人在用这台机器」的声明，
        // 一个连开跑都没跑成的单留着它，就是把院子交给一个不会干活的人。
        if (plan.lease) await releaseWorkerLeases(ctx, root, plan.sessionId, plan.alias).catch(() => undefined)
        results.push(`- ${plan.alias}：**派工失败**——${describeError(err)}`)
      }
    }
    // ⚠ 这里**不再**落盘 state：登记已在上面的锁内逐单提交（mutateControl）。
    // 末尾再 saveState 会把「读到这份陈旧 state 的其它调用」的成果整个盖回去——
    // 这正是「并行 seed_add 丢一条」的同型缺陷，只是换了个位置。
    void nextSeq
    // 初期错误窗口（会话缺失/存储异常等同步段失败）：不等待执行完成，只捞早期异常
    await Promise.race([Promise.allSettled(kicked), new Promise((r) => setTimeout(r, 1500))])
    // 回显用最新落盘值（而不是本次调用开始时读到的那份）
    const after = await loadState(ctx, root)
    const shown = after.initialized ? after.state : state
    // 仓级租约也回显：只报本地表时，跨线那一条会“看不见”——而它恰好是另一条线被挡/挡人的依据。
    const afterProbe = await repoTopOf(ctx, root)
    const afterRepo = afterProbe.top ? (await readSharedLeases(ctx, afterProbe.top)).leases : []
    const afterRepoNote = afterProbe.top
      ? afterRepo.length
        ? `｜仓级租约 ${afterRepo.length}（${rootLabel(afterProbe.top)}：${afterRepo.map((l) => `${l.resource.split(":").pop()}/${rootLabel(l.root)}`).join("、")}）`
        : "｜仓级租约 0"
      : `｜仓级租约域未启用（${afterProbe.reason}）`
    return {
      output: [
        // **回显派到了哪条线**：多条演进线共存时（自举闭环的常态），
        // “本次落在哪个根”必须明说——否则忘记传 root 时会静默派到会话 env 指的那条线，
        // 而派工本身不报错（实测：先在 biz2 派、后 init plat2，再次派工却落到 plat2）。
        `布雨完成（${root}）：在途 ${shown.inflight.length}｜池 ${shown.workers.length}${shown.leases.length ? `｜租约 ${shown.leases.map((l) => l.resource).join("、")}` : ""}${afterRepoNote}`,
        ...results,
        overflowNote,
        "worker 在后台自行运行——**不要等**，本息到此收息（shengsheng_state action=note），下息由 shengsheng_state 的 awaiting 提示回收。",
      ].filter(Boolean).join("\n"),
      data: { root, plan: plans, workers: shown.workers, inflight: shown.inflight, leases: shown.leases, repoLeases: afterRepo, repoTop: afterProbe.top, limit },
    }
  },
}

// ---------------------------------------------------------------------------
// 工具三：collect（收藏与退役）
// ---------------------------------------------------------------------------

const COLLECT_SNIPPET = 600

/** `review` 的自匹配活锁判据回看的消息尾条数（与 `pgrep-lock.SCAN_MESSAGE_LIMIT` 同口径）。 */
const PGREP_SCAN_LIMIT = 60

/**
 * `review` 的**自匹配活锁核对**：从 worker 会话最近的命令里筛出「等待类命令」，判它是不是
 * 在等一个早已结束的东西（`pgrep -f` 命中自己）。
 *
 * 为什么必须自动判、不能只靠纪律：现场两次都是「worker 以为自己在守规矩地等别人」，
 * 自律完全不管用——只有**外部观测**才能看见「它已经连续等 N 次、而那个 gate 早就没了」。
 *
 * 三种结论严格分开（「0 命中」与「没核」不得互相冒充）：
 * · 没扫到等待类命令 ⇒ `checked=false`，不出这一行（不掣告）；
 * · 破了但拿不到进程表 ⇒ 如实报「本项未核」（自匹配那条仍可纯文本判定）；
 * · 核过无异常 ⇒ 给一句已核过的肯定（让人知道判据真跑了）。
 */
async function auditPgrepLock(live: Live): Promise<{ advice?: string; block?: PgrepLockVerdict["block"] }> {
  const messages = live.tail ?? []
  let rows: ProcRow[] = []
  let tableError: string | undefined
  let exclude: number[] = []
  try {
    const table = await readProcTable()
    rows = table.rows
    tableError = table.error
    // 排除自身与祖先链：我们自己的命令行里也可能带着这些 pattern（不排除就会“永远有人在跑”）
    exclude = await protectLineage()
  } catch (err) {
    tableError = `读进程表失败：${describeError(err)}`
  }
  const verdict = detectPgrepLock(messages, rows, { exclude, tableError })
  if (verdict.locked) return { advice: verdict.advice, block: verdict.block }
  if (!verdict.checked) {
    // 有等待类命令但无从判定：要么没扫到（本项不适用），要么拿不到进程表（必须说“未核”）
    if (!verdict.block.probes) return {}
    return { advice: `自匹配活锁本项未核：${tableError ?? "进程表不可用"}——不等于「没有活锁」（判据靠进程表看「那个 gate 现在还有没有进程」）`, block: verdict.block }
  }
  return {
    advice: `自匹配活锁：已核 ${verdict.block.probes} 条等待类命令，无自匹配、也无「反复等一个已无进程的 gate」（阈值 ${REPEAT_THRESHOLD} 次）`,
    block: verdict.block,
  }
}

const collectTool: Tool = {
  name: "collect",
  description:
    "收藏与退役（回收阶段）。action=review：深观一个 worker（运行态/起始时间/消息数/上下文水位/待决交互/最后一条回复）并给出处置建议（只读）。**上下文水位报两个量**：`ctx 224K｜复用建议线 150K（149%）｜模型窗口 512K（44%）`——过**复用建议线** ⇒「可复用，但收益递减」（建议换新，不强制）；过**真实模型窗口** ⇒「必须退役」（否则下次截断/报错）。两个字不可互换，回显也不得把建议线叫“上限”。action=collect：把报告要点追加进演进台账（幂等——同一 worker 只入一次）、worker 标 idle 留池、在途清除、**自动释放该 worker 的独占资源租约**、建议下一条自动入种子队列。action=retire：退役 worker——mode=idle 清出池并把在途方向退回种子队列（会话不存在/领域已终）、mode=stop 终止超时运行（先 review 抢救结论）、mode=delete 删除会话（运行中会被拒绝）；**三种模式都自动释放租约**（worker 走了，院子必须交回）。**寻址语义**：同一会话在池/在途只保留**当前轮**（会话 id 为主键，旧轮已被新轮取代）——`session_id` 命中的就是该会话的当前轮条目（返回里回工命中的别名与轮次），别名与 session_id 指向同一会话时可只传其一（两者矛盾时拒绝不猜）；别名只在在途、池中无对应 worker 的**陈旧残留**用 `alias=` 定向清理（session_id 寻不到残留）。",
  parameters: schema(
    {
      action: { enum: ["review", "collect", "retire"], description: "动作" },
      alias: { type: "string", description: "worker 别名（池登记用）；**别名优先**——也是清理「池中无对应 worker 的陈旧在途残留」的唯一寻址手段" },
      session_id: { type: "string", description: "worker 会话 id（缺省按别名解析）：按**会话当前轮**命中池条目与其在途（旧轮已归一，不存在「命中较新、陈旧寻不到」）；返回里回工命中的别名与轮次" },
      summary: { type: "string", description: "collect：报告要点（markdown，原样入台账——交付/验证证据/未竟事项）" },
      next: { type: "string", description: "collect：报告「建议下一条」（自动入**本项目**种子队列）" },
      feed_to: { type: "string", description: "collect：跨层反哺——把这个 worker 撞出的**调度层/生命周期层**问题送回指定演进线的种子队列（项目根绝对路径，通常是生生自己那条线）。**只在问题出在调度层时用**：业务层问题归本项目（用 `next`），调度层问题（派工丢写 / 轮次错 / 种子泄漏 / 状态污染 / 会话不可达……）才回流给平台自身——这就是自举闭环。条目来源标为「反哺 <别名>@<本项目根>」以便溯源" },
      mode: { enum: ["idle", "stop", "delete"], description: "retire：退役方式（缺省 idle=仅清出池）" },
      dry_run: { type: "boolean", description: "retire mode=stop：干跑——只列出「要杀的 pid + 命令行」与工作树回退态，不终止会话、不发任何信号、不改 control.md" },
      reason: { type: "string", description: "retire：退役原因（记入种子来源与回显）" },
      root: { type: "string", description: "项目根（缺省按会话 env / 工作目录探测）" },
    },
    ["action"],
  ),  requiresApproval: false,
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
        // ⚠ 不再依赖在调用开始时 `loadState` 读到的那份（陈旧基线）；在**锁内**基于最新内容做变更。
        const m = await mutateControl(ctx, root, (cur) => {
          // 身份键与寻址同源：别名 + 方向两侧都清（陈旧残留可能只见于其中一侧）。
          const sid = inflightSession(stale, cur.workers)
          cur.inflight = cur.inflight.filter(
            (i) => !(i.alias === stale.alias && i.direction === stale.direction && inflightSession(i, cur.workers) === sid),
          )
          let back = ""
          if (!cur.seeds.some((s) => s.direction === stale.direction)) {
            cur.seeds.push({ priority: "待定", direction: stale.direction, source: `在途回退 ${stale.alias}${reason ? `（${reason}）` : ""}`, constraint: "worker 会话不可用，重开时从种子库/台账重新摸底" })
            back = "；方向已退回种子队列"
          } else back = "；方向已在种子队列（未重复）"
          return { next: cur, result: { back, seeds: cur.seeds } }
        })
        if (m.error) return { output: m.error }
        return {
          output: `已清掉陈旧在途登记：${stale.alias}（轮次 ${stale.round}｜${where}）${m.result!.back}。池中无对应 worker——无会话可终止/删除（销个会话请走 session 工具或先重新派工）。${reason ? `原因：${reason}` : ""}`,
          data: { inflight: stale, seeds: m.result!.seeds },
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
    // 本 worker 的上下文口径（两个量）：review / collect 共用，避免一处报一个数、另一处报两个数。
    const ctxV = ctxVerdict(live.ctxTokens, ctxScale(ctx.env, svc.engine))
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
      // 上下文水位**不分运行/空闲**：它是唯一一项「两种状态下都要看一眼」的量。
      // 实测踩到：worker 已跑到 ctx 171K（过复用建议线 150K）时，review 只说了
      // 「运行中 18 分钟，未超 40 → 勿扰」——因为 ctx 提醒当时只写在「非运行中」分支里。
      // 而**运行中恰恰最该看到它**：它还在消费，上下文只会继续长。
      //
      // ⚠ 但“该看到”不等于“说它超上限”：旧版把建议线（150K）当成模型窗口，于是 ctx 224K
      // 被报成「已过肥，超上限（149%）」——假的（它只占真窗口 512K 的 44%，**根本不算超限**）。
      // 现在按两个量分别判：过**建议线** ⇒ 可复用但收益递减；过**真实窗口** ⇒ 必须退役。
      const overWindow = ctxV?.level === "over-window"
      if (!live.exists) advice.push("会话不存在（外部删除/服务重启）→ `shengsheng_collect action=retire mode=idle`（在途退回种子队列）")
      else if (live.running) {
        if ((live.minutes ?? 0) > 40) advice.push(`运行已 ${live.minutes} 分钟（超 40）→ 先本 review 已抢救消息尾，再 \`action=retire mode=stop\` 记「中道而废」`)
        else if (overWindow) advice.push(`运行中（${live.minutes ?? "?"} 分钟）但 **ctx 已过真实模型窗口**（${ctxV!.line}）——不要“勿扰”着等：先 review 抢救已产出的结论，再 \`action=retire mode=stop\``)
        else if (ctxV?.level === "over-advisory")
          // 过了**建议线**但还在窗口内：不必现在打断它（活还在干），但**不要“勿扰”着无限等**：
          // 它的上下文只会更长，续派就亏了——本轮收口即换新，收不了口再 stop。
          advice.push(`运行中（${live.minutes ?? "?"} 分钟，未超 40）但 ${ctxV.line}——不必现在打断；本轮收完即换新、别再续派；若久不收口，先 review 抢救结论再 \`action=retire mode=stop\``)
        else advice.push(`运行中（${live.minutes ?? "?"} 分钟，未超 40）→ 万物自化，勿扰，下息再看`)
        if (live.pending?.length) advice.push(`有待决交互（${live.pending.join("、")}）——无人值守会话会挂起至超时`)
      } else {
        const short = (live.last?.length ?? 0) < 200
        if (short) advice.push("空闲但末条疑似半截 → 记中断后 `action=retire`（该领域下次重开）")
        else advice.push("空闲且末条像完整报告 → `action=collect alias=… summary=… [next=…]` 入台账")
      }
      // 处置按**两个量分别给**：过建议线 ≠ 过窗口（一句“超上限”把两者混了，就是本单要消灭的误报）
      if (ctxV) advice.push(ctxV.action)

      // ---- 提交范围核对（宽范围暂存的痕迹）----
      // 判据：**最近一次提交里的文件** vs **该 worker 工单提到的路径**。不在后者里的文件，正是
      // `856ba9bb` 那次事故的形状——另一个 worker 的 `proc-tree.ts`（+596）与
      // `shengsheng-retire-tree.test.ts`（+573）被 `git add -A` 一起带了进去，而提交说明只字未提。
      // 三种结论必须分开报（「0 命中」与「读不到」是两回事）：读不到 git / 核过无异常 / 有疑——
      // 把「读不到」也报成「没有宽范围暂存」是 PITFALLS 里的经典假绿。
      const commitNote = await auditCommitScope(ctx, root, worker, inflight)
      if (commitNote.advice) advice.push(commitNote.advice)
      if (commitNote.block) res.commitScope = commitNote.block
      // ---- 自匹配活锁核对（`pgrep -f` 命中等它的那条 shell 自己）----
      // 现场两次：worker 以为自己在「守规矩地等别人释放文件」，实际条件恒为真——12 分钟、159 条消息、
      // 一行实现都没写。它不是懒，它是被执行环境骗了；而骗局只能从**外面**看见（「它反复等同一个 gate，
      // 而那 gate 现在没有进程」），自省看不见。
      const pgrepNote = await auditPgrepLock(live)
      if (pgrepNote.advice) advice.push(pgrepNote.advice)
      if (pgrepNote.block) res.pgrepLock = pgrepNote.block
      return {
        output: [
          `worker ${worker.alias}（${worker.domain}）｜会话 ${worker.sessionId}`,
          sidArg ? `- 寻址：按 session_id=${sidArg} 命中本条——别名 ${worker.alias}｜轮次 ${worker.round}｜在途 ${inflight ? `有（轮次 ${inflight.round}）` : "无"}（同一会话只保留当前轮，无旧轮并列可寻）` : "",
          alias && alias !== worker.alias ? `- 寻址：alias=${alias} 不在池中；仅 session_id 解析到 ${worker.alias}（两者不同——请只传一个）` : "",
          `- 任务：${worker.task}｜派发 ${new Date(worker.dispatchedAt).toLocaleString("zh-CN")}｜池状态 ${worker.status}`,
          `- 现有态：${!live.exists ? "会话不存在" : live.running ? `运行中 ${live.minutes ?? "?"} 分钟` : "空闲"}${live.messages != null ? `｜消息 ${live.messages}` : ""}${ctxV ? `｜${ctxV.line}` : ""}${live.toolCalls ? `｜在途工具 ${live.toolCalls}` : ""}`,
          live.pending?.length ? `- 待决：${live.pending.join("、")}` : "",
          inflight ? `- 在途登记：${inflight.direction}${inflight.deliverable ? `｜交付 ${inflight.deliverable}` : ""}` : "- 在途登记：（无——可能已收藏）",
          live.last ? `- 最后一条回复（截断 ${COLLECT_SNIPPET} 字）：\n${live.last.slice(0, COLLECT_SNIPPET)}` : "- 最后一条回复：**（无文本/未产出）**",
          `处置建议：\n${advice.map((a) => `- ${a}`).join("\n")}`,
        ].filter(Boolean).join("\n"),
        // data 里也分开给：调用方（监督/界面）不必再去解析文案才能知道“过的是哪条线”
        data: { worker, live, inflight: inflight ?? null, advice, ctx: ctxV ? { tokens: ctxV.tokens, level: ctxV.level, advisoryPct: ctxV.advisoryPct, windowPct: ctxV.windowPct, scale: ctxV.scale, line: ctxV.line } : null, ...res },
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
      // 幂等键：worker 会话 + **轮到哪一轮**（dispatch 写入的「该会话第几单」，首派 1）。
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
      // 《台账 + 状态》在**同一把锁内**完成：幂等键取自 control.md 的轮次，两文件分属两把锁会出现
      // 「两边都认为自己该入账」/「一边先改状态使另一边算错键」的窗口。
      const done = await evolveLock(root, async () => {
        const led = await appendLedgerLocked(ctx, root, ledgerKey, block)
        if (led.error) return { led }
        if (!led.appended) return { led }
        const r = await readAbs(ctx, evolvePaths(root).control)
        if (r.error) return { led, error: r.error }
        const cur = parseControl(r.text ?? null)
        const cw = cur.workers.find((x) => x.sessionId === worker.sessionId || x.alias === worker.alias)
        if (cw) cw.status = "idle"
        // 在途清除**按会话 id**（主键）兼按别名：只按别名清不掉「别名已漂移」的那条残留，
        // 而它随后会在读态里重新被报成「待收藏/无从核实」——收藏过的东西又冒出来，是同一裂缝的另一面。
        const cleared = takeInflight(cur, cw ?? worker)
        cur.pending = cur.pending.filter((p) => p.alias !== worker.alias)
        returnToSeeds(cur, cleared.filter((i) => i.alias !== worker.alias), "在途回退", "（别名漂移的陈旧登记）", "worker 会话不可用，重开时从种子库/台账重新摸底")
        let seedNote = ""
        if (next) {
          if (cur.seeds.some((s) => s.direction === next)) seedNote = `「建议下一条」已在种子队列（未重复入队）`
          else {
            cur.seeds.push({ priority: "待定", direction: next, source: `回收报告 ${worker.alias}`, constraint: "" })
            seedNote = `「建议下一条」已入种子队列`
          }
        }
        const released = releaseLeases(cur, worker.sessionId, worker.alias)
        const w2 = await writeAbs(ctx, evolvePaths(root).control, renderControl(cur))
        if (w2.error) return { led, error: w2.error }
        return { led, seedNote, seeds: cur.seeds, inflight: cur.inflight, workers: cur.workers, released }
      })
      // 仓级租约在**锁外**释放（不是在 evolveLock 里面）：两个域是两把锁，若在持 evolve 锁时去等共享表
      // 的锁，而另一条线恰好持着共享锁去等自己那条线的 evolve 锁，就成一个环（死锁）。
      // 故锁只包一个域：先在锁内改本地，再在锁外改共享表。两个都完成才叫释放完毕——
      // 只放一个会留下“本地看着空着、另一条线仍挡着”的半个租约，而那正是本单要消灭的裂缝形态。
      const repoRel = await releaseRepoLeases(ctx, root, worker.sessionId, worker.alias)
      done.released = [...(done.released ?? []), ...repoRel.released]
      const repoError = repoRel.error ? `仓级租约释放失败（${repoRel.error}）——跨线资源可能仍被占着` : ""
      if (done.led.error) return { output: done.led.error }
      if (!done.led.appended) {
        // 幂等重放（台账已有该轮记录）也**必须释放租约**：重放往往发生在“上一轮已收、但租约因故没释放”之后
        // （旧版工具/中途崩溃/手改控制面）。不在这里释放，就只剩超期那条兜底——而兜底期间院子被堵着。
        const again = await releaseWorkerLeases(ctx, root, worker.sessionId, worker.alias)
        const rel = leaseReleaseText(again.released)
        return {
          output: `台账已有该 worker 的记录（幂等跳过，未重复追加）：${worker.alias}｜${done.led.path}${rel ? `\n${rel}` : ""}\n若要点有更新，请先 \`action=retire\` 让该 worker 退出池，下轮以新记录入账。`,
          data: { ledger: done.led.path, worker, released: again.released, ...res },
        }
      }
      if (done.error) return { output: `台账已追加但状态落盘失败：${done.error}` }

      // ---- 跨层反哺：把调度层/生命周期层的问题送回平台自身的种子队列 ----
      //
      // 为何需要它（自举闭环缺的那一环）：worker 撞出的问题若属于**调度层**
      // （派工丢写 / 轮次错 / 种子泄漏 / 跨线污染 / 会话不可达…），它不属于被演进的那个项目——
      // 写进该项目的 BACKLOG 永远没人会去看（那里只管那个项目自己的事）。
      // 而没有回流，“用生生干活”就无法让生生自己变好——三层链条（息→平台→业务→反哺平台）就断了。
      //
      // 口径：`next` 是**本项目**的下一条（业务层）；`feed_to` 是**另一个演进线**的下一条（调度层）。
      // 两者可同时给：一份报告可能既有业务发现、也撞出了平台问题。
      let feedNote = ""
      const feedTo = args.feed_to != null ? String(args.feed_to).trim().replace(/[\\/]+$/, "") : ""
      if (feedTo) {
        const feedText = String(args.next ?? "").trim()
        if (!feedText) {
          feedNote = `反哺未落盘：feed_to 需配合 next（那条要回流的方向）——只给目标根等于凭空造一条种子`
        } else if (feedTo === root) {
          feedNote = `反哺未落盘：feed_to 与 root 相同（${root}）——同一条线用 next 即可，不需跨层`
        } else {
          const source = `反哺 ${worker.alias}@${root}`
          type FeedResult = { added: boolean; total: number; dup: Seed | null }
          const fed = await mutateControl<FeedResult>(ctx, feedTo, (dst) => {
            const dup = dst.seeds.find((s) => s.direction === feedText)
            if (dup) return { result: { added: false, total: dst.seeds.length, dup } }
            dst.seeds.push({ priority: "待定", direction: feedText, source, constraint: "" })
            return { next: dst, result: { added: true, total: dst.seeds.length, dup: null } }
          })
          if (fed.error) {
            // **不静默失败**：反哺不成功必须说出来（否则“以为送回去了”比没送更坏）
            feedNote = `反哺失败（${feedTo}）：${fed.error}`
          } else if (!fed.result!.added) {
            feedNote = `反哺已在目标队列（未重复入队）：${feedTo}——现 ${fed.result!.total} 条`
          } else {
            feedNote = `已反哺 ${feedTo}（源 ${source}）——现 ${fed.result!.total} 条`
          }
        }
      }

      return {
        output: [
          `收藏完成：${worker.alias}｜${worker.domain}`,
          `- 台账：${done.led.path}（已追加）`,
          `- worker 留池标 idle（上下文里的领域结论可复用；退役用 action=retire）`,
          // 收藏时的 ctx 口径也要两个量都报：这一行是“留池还是退役”的直接依据
          // （旧版这里只说“留池可复用”，与 review 的「超上限」互相矛盾——两边在说同一件事却用两把尺）。
          ctxV ? `- 上下文：${ctxV.line}——${ctxV.action}` : "",
          leaseReleaseText(done.released ?? []),
          done.seedNote ? `- ${done.seedNote}（现 ${done.seeds!.length} 条）` : "- 未给 next（报告里的建议下一条没进队列）",
          feedNote ? `- ${feedNote}` : "",
          repoError ? `- ⚠ ${repoError}` : "",
          `- 在途剩 ${done.inflight!.length}｜池 ${done.workers!.length}（${done.workers!.filter((x) => x.status === "idle").length} idle）`,
        ].filter(Boolean).join("\n"),
        data: { ledger: done.led.path, worker, seeds: done.seeds, inflight: done.inflight, released: done.released, feed: feedNote || null, ...res },
      }
    }

    // ---- retire：退役 ----
    if (action === "retire") {
      const mode = String(args.mode ?? "idle").trim() || "idle"
      const reason = args.reason != null ? String(args.reason).trim() : ""
      // 三个分支共用同一套**锁内**变更：它们都是「取出在途 → 退回种子 → 清出池 → 写回」。
      // 差别只在退回种子的来源标签与是否额外清 pending；故把差异收敛成一个 pushLabel 与一条 filter。
      // （原先四个分支各自 `saveState`，是四个“陈旧基线覆盖”的入口。）
      // 仓域释放放在**锁外**（与 collect 同一理由：两个域两把锁，交叉持锁会成环）。
      const apply = async (pushLabel: string, constraint: string, dropPending: boolean) => {
        type RetireResult = { inflightRows: number; pushed: boolean; seeds: Seed[]; workers: Worker[]; inflight: Inflight[]; released: AnyLease[]; repoError?: string }
        const m = await mutateControl<RetireResult>(ctx, root, (cur) => {
          const workerNow = cur.workers.find((w) => w.sessionId === worker.sessionId || w.alias === worker.alias) ?? worker
          const inflightRows = takeInflight(cur, workerNow)
          const pushed = returnToSeeds(cur, inflightRows, pushLabel, reason, constraint)
          cur.workers = cur.workers.filter((w) => w.sessionId !== worker.sessionId && w.alias !== worker.alias)
          if (dropPending) cur.pending = cur.pending.filter((p) => p.alias !== worker.alias)
          // 退役即释放租约：worker 走了，院子必须交回（否则一条死租约永久堵住“只能独占才做”的那类活）。
          const released: AnyLease[] = releaseLeases(cur, worker.sessionId, worker.alias)
          return { next: cur, result: { inflightRows: inflightRows.length, pushed, seeds: cur.seeds, workers: cur.workers, inflight: cur.inflight, released } }
        })
        // 本地改成功才去碰仓域：本地失败时租约本就不在（或无从确认），去删共享表只会删错。
        if (m.error) return m
        const repoRel = await releaseRepoLeases(ctx, root, worker.sessionId, worker.alias)
        m.result!.released = [...(m.result!.released ?? []), ...repoRel.released]
        if (repoRel.error) m.result!.repoError = `仓级租约释放失败（${repoRel.error}）——跨线资源可能仍被占着`
        return m
      }

      if (mode === "stop") {
        // **终止会话 ≠ 停止该会话干的事**：`engine.cancel` 只打断会话的任务循环，而 worker 经
        // `sh`/`py`/`js` 派出去的子进程（并发的构建、长跑脚本、验证门禁）不是会话的一部分——
        // 它们会**在会话死后继续改同一棵树**（两例实测，见 proc-tree.ts 头部）。故 stop 的完整语义是
        // 「停会话 + 回收它派生的子孙进程树 + 检查工作树是否被留在回退态」。
        const dryRun = args.dry_run === true
        // ① 先定位并列清单（**杀之前**写进回显：可审计，也不给“悄悄杀了什么”留空间）
        const located = await locateSessionTree(worker.sessionId)
        const treeText = located.error
          ? `子孙进程树：**无法定位**（${located.error}）——只终止了会话本身，它的子进程可能仍在改同一棵树`
          : renderPlan(located.plan!, { dryRun })
        if (dryRun) {
          return {
            output: `[干跑] 未终止会话、未杀任何进程、未改 control.md：\n${treeText}\n（确认清单后可去掉 dry_run 重跑）`,
            data: { dryRun: true, plan: located.plan ?? null, treeError: located.error ?? null, ...res },
          }
        }
        // ② 终止会话（先于杀子进程：否则它可能在两者之间再派生新的）
        if (live.running) {
          svc.engine.cancel(worker.sessionId)
        }
        // ③ 回收子孙进程树（SIGTERM → 等 → 仍在者 SIGKILL；无法定位时如实报告，不静默当“已清理”）
        const reap: ReapResult | undefined = located.plan ? await reapProcesses(located.plan) : undefined
        const reapText = located.error ? "" : renderReapResult(reap!)
        // ④ 工作树可疑态（`*.revbak` 与源文件不一致 ⇒ 源码可能停在回退态）：**只告警、不自动恢复**
        //    ——自动恢复可能覆盖别人正在验的那份回退态（`reverse_verify.py` 就在回退态下跑测）。
        const residueText = await rollbackResidueText(ctx, root)
        const m = await apply("中道而废", "重开时先读台账上一轮的中断点", false)
        if (m.error) return { output: m.error }
        const rel = leaseReleaseText(m.result!.released)
        const repoErr = m.result!.repoError ? `⚠ ${m.result!.repoError} ` : ""
        const body = [`已终止并退役：${worker.alias}｜会话 ${worker.sessionId}（保留可查）${sidArg ? `｜寻址：按 session_id 命中本条（别名 ${worker.alias}｜轮次 ${worker.round}）` : ""}（在途方向已退回种子队列）。${repoErr}${rel ? `${rel} ` : ""}${reason ? `原因：${reason}` : ""}`, treeText, reapText, residueText]
          .filter(Boolean)
          .join("\n")
        return {
          output: body,
          data: {
            worker,
            seeds: m.result!.seeds,
            released: m.result!.released,
            reap: reap ?? null,
            tree: located.plan ?? null,
            treeError: located.error ?? null,
            ...res,
          },
        }
      }
      if (mode === "delete") {
        if (live.running) return { output: `会话 ${worker.sessionId} 正在运行——先 \`action=retire mode=stop\` 终止再删除（删除运行中会话会留下孤儿任务态）。` }
        await svc.store.delete(worker.sessionId, ctx.user)
        const m = await apply("在途回退", "", true)
        if (m.error) return { output: m.error }
        const rel = leaseReleaseText(m.result!.released)
        const repoErr = m.result!.repoError ? `⚠ ${m.result!.repoError} ` : ""
        return { output: `已删除会话并退役：${worker.alias}（${worker.sessionId} 进归档，7 天内可由管理员恢复）${sidArg ? `｜寻址：按 session_id 命中本条（别名 ${worker.alias}｜轮次 ${worker.round}）` : ""}。${repoErr}${rel ? `${rel} ` : ""}${reason ? `原因：${reason}` : ""}`, data: { worker, seeds: m.result!.seeds, released: m.result!.released, ...res } }
      }
      // mode=idle：仅清出池（在途退回种子）
      const m = await apply("在途回退", "worker 会话不可用，重开时从种子库/台账重新摸底", true)
      if (m.error) return { output: m.error }
      const back = m.result!.inflightRows
        ? m.result!.pushed
          ? "；在途方向已退回种子队列"
          : "；在途方向已在种子队列（未重复）"
        : ""
      const rel = leaseReleaseText(m.result!.released)
      const repoErr = m.result!.repoError ? `⚠ ${m.result!.repoError}` : ""
      return {
        output: `已退役（仅清出池）：${worker.alias}${live.exists ? `——会话 ${worker.sessionId} 保留在磁盘，需彻底清理由 action=retire mode=delete` : "（会话已不存在）"}${sidArg ? `｜寻址：按 session_id 命中本条（别名 ${worker.alias}｜轮次 ${worker.round}）` : ""}${back}${repoErr}${rel ? `；${rel}` : ""}${reason ? `。原因：${reason}` : ""}`,
        data: { worker, seeds: m.result!.seeds, released: m.result!.released, ...res },
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
  { name: "SHENGSHENG_MAX_WORKERS", description: "活动 worker 上限（running+idle，缺省 3）——形如 `6` 或 `默认|硬顶`（如 `3|16`），用于放宽并发" },
        { name: "SHENGSHENG_DISPATCH", description: "设为 preview 时 dispatch 只出计划不派工（预检）" },
  { name: "SHENGSHENG_LEASE_MINUTES", description: "独占资源租约的缺省租期（分钟，缺省 90）——超期是兜底回收判据（防“持有者悄悄死掉而登记没被碰”），正常释放在 collect/retire" },
  { name: "SHENGSHENG_MODEL_CTX_WINDOW", description: "**真实模型上下文窗口**（token，缺省 512000）——引擎 `contextWindow()` 自报优先；`0`/`off` = 未知（回显会明说未知，不编造）。退役硬判据：过此值=必须退役" },
  { name: "SHENGSHENG_REUSE_ADVISORY_CTX", description: "**复用质量建议线**（token，缺省 150000）——超过它的 idle worker 不再值得复用（上下文肥、续派质量降）；`0`/`off` = 关掉此判据。**它不是模型窗口上限**：过线只算“收益递减”，不算“超限”" },
]

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars }
