/**
 * 会话派生的**子孙进程树**：定位 → 干跑计划 → 回收；以及「工作树是否停在回退态」的只读告警。
 *
 * 本模块是 `collect action=retire mode=stop` 的伴随动作，独立成文件的两个理由：
 * ① 判据（进程表、环境标记、pid 复用）与调度逻辑无关，混进 `shengsheng.ts` 会让那一份 3000 行的
 *    文件再长一截，且逆向验证/单测都无法单独盯住它；
 * ② 干跑（`dryRun`）与真杀必须走**同一份**计划代码——两处各写一份就会出现「干跑看到的清单
 *    与真杀的清单不是一回事」，而干跑存在的全部意义就是"先看再决定"。
 *
 * ## 为什么需要它（两条独立演进线各自实证到同一件事）
 *
 * `retire mode=stop` 的语义是「停止这个 worker 干的事」，而它过去只做 `engine.cancel(sessionId)`
 * ——那只打断会话的任务循环。worker 经 `sh`/`py`/`js` 派出去的子进程（并发的构建、长跑脚本、
 * 包装 shell、验证门禁）**不是会话的一部分**：会话被终止后它们照旧在跑，照旧在写同一棵树。两例实测：
 * · 会话 7dd7b830：终止 `revverify-guard` 后它起的 `kill_drill`（91576/91578/91579）仍在运行，
 *   继续持有 `shengsheng.ts` 的回退态（源码第 2026 行是 `const round = seq`，而 `.revbak` 才是修复版），
 *   靠人工 `diff` + `kill -TERM` 才清掉。
 * · 会话 b1351488 第 12 息：一个 worker 用 10 路并发 `st build` 写坏共享对象缓存，`retire mode=stop`
 *   止损——当时核过 `st build` 计数为 0，但更下层的子脚本没人查。
 * 一句话：**被终止的 worker 会「死后继续破坏共享工作树」**。
 *
 * ## 归因：怎么知道「这是该会话派生的」
 *
 * 歌白里**没有「会话进程」**：所有会话的任务都跑在同一个服务进程里，子进程是**那个进程**的子进程。
 * 所以「拿会话进程 pid 再往下爬」没有起点可用。判据改为**环境标记**：派工时给 worker 会话注入
 * `SHENGSHENG_WORKER_SESSION=<会话 id>`，子进程继承环境 ⇒ 读到该标记的进程必属该会话，
 * 读不到的必不属于。**绝不按命令行/进程名猜**——那正是「把全机同名进程一起杀掉」的来源
 * （跨线祸及比不杀更坏：被杀的那条线可能正跑着别人的最终门禁）。
 *
 * 标记之上再加一层**子孙递归**：`env -i <cmd>` 会把标记洗掉，而它仍可能是该会话亲手起的——
 * 父进程带标记 ⇒ 一并算进来。
 *
 * ## 安全边界（每条都有用例兜）
 *
 * ① **只杀该会话派生**：标记属于别的会话、或无标记且不在该会话的子孙里 → 一律不碰，
 *    并在计划里点名「看到了但没杀、为什么」（可审计）。
 * ② **永不误杀自己与宿主**：自身 pid、自身祖先链、1 号进程一律进保护集。
 * ③ **pid 复用防护**：SIGTERM 之后补杀前**重读启动时刻**（`/proc/<pid>/stat` 第 22 字段）——
 *    变了就当它「已死」，绝不把 SIGKILL 发给一个复用了该 pid 的无关进程。
 * ④ **无法定位如实报告**：归因不到（无标记进程、进程表不可读、非 POSIX 平台）时返回
 *    `via="none"` 或 `error`，由调用方写进回显。**静默当成「已清理」是这条最坏的失败方式**
 *    ——下一次息会以为院子干净了，而破坏还在继续。
 *
 * ## 跨平台
 *
 * 进程表与 `/proc/<pid>/environ` 是 **Linux/POSIX** 能力：Windows 没有等价物，本模块**不做**
 * Windows 实现，只返回带原因的 `error`（调用方如实报告）。这条边界是刻意的：在拿不到真值的地方
 * 「尽力猜」比「说不知道」更危险。
 */
import { readdir, readFile } from "node:fs/promises"

/** 会话归属标记的环境变量名（派工时注入 worker 会话 env，子进程继承）。 */
export const PROC_MARKER_ENV = "SHENGSHENG_WORKER_SESSION"

/** 回退备份后缀（`reverse_verify.py` 的崩溃兜底文件：它与源文件不一致 = 上一轮回退未完成）。 */
export const ROLLBACK_SUFFIX = ".revbak"

/** 进程表里的一行。 */
export interface ProcRow {
  pid: number
  ppid: number
  /** 进程组 id（回显用：能看出它是不是一个组长）。 */
  pgid: number
  /** 启动时刻（jiffies since boot，/proc/<pid>/stat 第 22 字段）：pid 复用判据，不可得为 0。 */
  startTime: number
  /** 命令行（审计回显用；内核线程为空串）。 */
  cmd: string
}

export interface ProcTable {
  rows: ProcRow[]
  /** 数据来源（`procfs /proc`），回显里要说清。 */
  source: string
  /** 读不到进程表的原因（非 Linux / 无 /proc / 权限）：此时 `rows` 为空且**不得**当成「没有进程」。 */
  error?: string
}

const describe = (err: unknown): string => String((err as { message?: string; code?: string })?.message ?? (err as { code?: string })?.code ?? err)

/**
 * 保护集：自身 pid 与自身祖先链（**永不发信号**——杀了宿主就等于把整条演进线一起干掉）。
 *
 * 读不到祖先链时（非 /proc 平台、权限）至少保住自身 pid：退一步也不会杀自己。
 */
export async function protectLineage(opts: { procRoot?: string; self?: number } = {}): Promise<number[]> {
  const root = (opts.procRoot ?? "/proc").replace(/[\\/]+$/, "")
  const self = opts.self ?? process.pid
  const out = [self]
  let cur = self
  for (let i = 0; i < 64; i++) {
    let stat: string
    try {
      stat = await readFile(`${root}/${cur}/stat`, "utf8")
    } catch {
      break
    }
    const parsed = parseProcStat(cur, stat)
    if (!parsed || !Number.isFinite(parsed.ppid) || parsed.ppid <= 1 || out.includes(parsed.ppid)) break
    out.push(parsed.ppid)
    cur = parsed.ppid
  }
  return out
}

/** 读进程表（Linux `/proc`）。不可读时返回 `error`——调用方必须如实报告，不得当成空表。 */export async function readProcTable(opts: { procRoot?: string } = {}): Promise<ProcTable> {
  const root = (opts.procRoot ?? "/proc").replace(/[\\/]+$/, "")
  let names: string[]
  try {
    names = await readdir(root)
  } catch (err) {
    return { rows: [], source: "", error: `进程表不可读（${root}）：${describe(err)}——本平台无法定位进程树` }
  }
  const pids = names.filter((n) => /^\d+$/.test(n)).map(Number)
  const rows = (await Promise.all(pids.map((pid) => readProcRow(root, pid)))).filter((r): r is ProcRow => r !== null)
  return { rows, source: `procfs ${root}` }
}

/** 读单个进程行（已退出/无权限 → null：进程表里"看不到"与"不存在"在判据上等价）。 */
async function readProcRow(root: string, pid: number): Promise<ProcRow | null> {
  let stat: string
  try {
    stat = await readFile(`${root}/${pid}/stat`, "utf8")
  } catch {
    return null
  }
  const parsed = parseProcStat(pid, stat)
  if (!parsed) return null
  let cmd = ""
  try {
    cmd = (await readFile(`${root}/${pid}/cmdline`)).toString("utf8").split("\u0000").filter(Boolean).join(" ")
  } catch {
    /* 内核线程无 cmdline（或已退出）：cmd 留空，不影响判据 */
  }
  return { pid, ppid: parsed.ppid, pgid: parsed.pgid, startTime: parsed.startTime, cmd }
}

/**
 * 解析 `/proc/<pid>/stat` 的字段（`comm` 可含空格与右括号，故从**最后一个** `)` 之后切）。
 *
 * 切出来第一格是第 3 字段（state），于是：ppid = 第 4 字段 = `fields[1]`、
 * pgrp = 第 5 字段 = `fields[2]`、starttime = 第 22 字段 = `fields[19]`。
 */
export function parseProcStat(pid: number, stat: string): { ppid: number; pgid: number; startTime: number } | null {
  void pid
  const close = stat.lastIndexOf(")")
  if (close < 0) return null
  const fields = stat.slice(close + 1).trim().split(/\s+/)
  const ppid = Number(fields[1])
  if (!Number.isFinite(ppid)) return null
  const pgid = Number(fields[2])
  const startTime = Number(fields[19])
  return {
    ppid,
    pgid: Number.isFinite(pgid) ? pgid : ppid,
    startTime: Number.isFinite(startTime) ? startTime : 0,
  }
}

/** 读一个进程的环境标记（无标记 / 无权读 → undefined：两种都按「不属本会话」处理，安全侧失败）。 */
export async function readMarkerOf(pid: number, opts: { procRoot?: string } = {}): Promise<string | undefined> {
  const root = (opts.procRoot ?? "/proc").replace(/[\\/]+$/, "")
  try {
    const raw = (await readFile(`${root}/${pid}/environ`)).toString("utf8")
    for (const kv of raw.split("\u0000")) {
      if (kv.startsWith(`${PROC_MARKER_ENV}=`)) return kv.slice(PROC_MARKER_ENV.length + 1)
    }
    return undefined
  } catch {
    return undefined
  }
}

/** 批量读环境标记（只返回带标记的）。 */
export async function readSessionMarkers(pids: Iterable<number>, opts: { procRoot?: string } = {}): Promise<Map<number, string>> {
  const out = new Map<number, string>()
  await Promise.all(
    [...pids].map(async (pid) => {
      const sid = await readMarkerOf(pid, opts)
      if (sid) out.set(pid, sid)
    }),
  )
  return out
}

/** 后代集合（按 ppid 建树向下遍历）。
 *
 * 起点 pid **不算后代**（初始就放进 visited）：真实进程树没有环，但 pid 表在“子孙挂回自身”
 * （自指/父子同 pid 的怪行、测试构造）时不该把起点当成它的后代又杀一次。 */
export function descendantsOf(rows: ProcRow[], roots: Iterable<number>): Set<number> {
  const children = new Map<number, number[]>()
  for (const r of rows) {
    const list = children.get(r.ppid)
    if (list) list.push(r.pid)
    else children.set(r.ppid, [r.pid])
  }
  const seen = new Set<number>(roots)
  const out = new Set<number>()
  const queue = [...seen]
  while (queue.length) {
    const cur = queue.pop()!
    for (const child of children.get(cur) ?? []) {
      if (seen.has(child)) continue
      seen.add(child)
      out.add(child)
      queue.push(child)
    }
  }
  return out
}

/** 看到了但**故意不杀**的进程（回显里点名，用来证明「没误伤」而不是"什么也没看见"）。 */
export interface SkippedProc {
  pid: number
  cmd: string
  why: string
}

/** 回收计划（干跑与真杀共用；纯函数产出，不碰进程）。 */
export interface ReapPlan {
  sessionId: string
  /** 归因方式：`marker`=按环境标记（含子孙递归）；`none`=没定位到（见 `unresolved`） */
  via: "marker" | "none"
  targets: ProcRow[]
  skipped: SkippedProc[]
  /** 归因不到时的原因（人读） */
  unresolved?: string
  /** 本次判定扫过的进程数（诚实回显：不是"扫了 3 个"却显得像全机） */
  scanned: number
  source: string
}

export interface PlanReapInput {
  rows: ProcRow[]
  markers: Map<number, string>
  sessionId: string
  /** 保护集（自身、自身祖先链）：**永不发信号**，命中即只记 skipped */
  protect?: Iterable<number>
  /** 进程表来源（回显用） */
  source?: string
}

/**
 * 算出「该会话派生的、可以安全回收的」进程集合（纯函数）。
 *
 * 三段判据，缺一不可：
 * ① 有本会话标记 → 种子；
 * ② 种子的后代 → 一并算（`env -i` 洗掉标记的那些，仍是该会话亲手起的）；
 * ③ 命中 ①② 但落在保护集、或标记属于**别的会话** → 进 skipped，不进 targets。
 */
export function planReap(input: PlanReapInput): ReapPlan {
  const protect = new Set<number>([1, ...(input.protect ?? [])])
  const scanned = input.rows.length
  const source = input.source ?? ""
  const seeds = new Set<number>()
  for (const r of input.rows) if (input.markers.get(r.pid) === input.sessionId) seeds.add(r.pid)
  if (!seeds.size) {
    return {
      sessionId: input.sessionId,
      via: "none",
      targets: [],
      skipped: [],
      unresolved:
        `该会话没有带标记（${PROC_MARKER_ENV}）的进程——可能是它还没派生过子进程，` +
        "也可能是那些子进程起于标记生效之前（旧派工）或标记在 exec 时被清掉（`env -i`）",
      scanned,
      source,
    }
  }
  const subtree = descendantsOf(input.rows, seeds)
  const targets: ProcRow[] = []
  const skipped: SkippedProc[] = []
  for (const r of input.rows) {
    if (!seeds.has(r.pid) && !subtree.has(r.pid)) continue
    if (protect.has(r.pid)) {
      skipped.push({ pid: r.pid, cmd: r.cmd, why: "属自身/祖先进程（保护集，绝不发信号）" })
      continue
    }
    const marker = input.markers.get(r.pid)
    if (marker && marker !== input.sessionId) {
      skipped.push({ pid: r.pid, cmd: r.cmd, why: `属另一个会话（${marker}）——跨线不碰` })
      continue
    }
    targets.push(r)
  }
  targets.sort((a, b) => a.pid - b.pid)
  skipped.sort((a, b) => a.pid - b.pid)
  return { sessionId: input.sessionId, via: "marker", targets, skipped, scanned, source }
}

export type ReapOutcome = "dry-run" | "reaped" | "partial" | "nothing"

export interface ReapResult {
  outcome: ReapOutcome
  /** 干跑 = 将要杀的清单；实杀 = 实际发过信号的清单 */
  targets: ProcRow[]
  /** 实杀后仍活着的（`partial` 时非空） */
  survivors: ProcRow[]
  /** 逐进程处理结论（审计：谁收到什么信号、有没有退） */
  log: string[]
}

export interface ReapDeps {
  /** 发信号（缺省 `process.kill`；测试可注入以断言"发了什么、发给谁"） */
  signal?: (pid: number, sig: "SIGTERM" | "SIGKILL") => void
  /** 仍存活且**启动时刻未变**（pid 被复用即视为已死） */
  stillSame?: (row: ProcRow) => Promise<boolean>
  sleep?: (ms: number) => Promise<void>
  termWaitMs?: number
  killWaitMs?: number
  pollMs?: number
  /** 干跑：只出清单，不发任何信号 */
  dryRun?: boolean
}

const defaultSleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms))

/** 缺省信号发送（进程已退出 = 无需处理，不抛）。 */
function defaultSignal(pid: number, sig: "SIGTERM" | "SIGKILL"): void {
  process.kill(pid, sig)
}

/**
 * 缺省存活判定：**启动时刻未变**才算同一进程。
 *
 * 为什么不只看 `process.kill(pid, 0)`：pid 会被内核复用。SIGTERM 之后等一会儿再补 SIGKILL 时，
 * 原进程可能已退出、该 pid 已被一个无关进程接手——此时"存活"是真的，但"它还是我们的目标"是假的。
 * 启动时刻（`/proc/<pid>/stat` 第 22 字段）是同一个 pid 的**唯一区分**，用它把这一类误杀关掉。
 */
async function defaultStillSame(row: ProcRow, procRoot = "/proc"): Promise<boolean> {
  try {
    const stat = await readFile(`${procRoot}/${row.pid}/stat`, "utf8")
    const now = parseProcStat(row.pid, stat)
    if (!now) return false
    if (row.startTime && now.startTime && now.startTime !== row.startTime) return false
    return true
  } catch {
    return false
  }
}

/** 等一下再看，返回仍存活的行。 */
async function waitGone(
  rows: ProcRow[],
  stillSame: (row: ProcRow) => Promise<boolean>,
  sleep: (ms: number) => Promise<void>,
  budgetMs: number,
  pollMs: number,
): Promise<ProcRow[]> {
  let alive = [...rows]
  const deadline = Date.now() + budgetMs
  while (alive.length && Date.now() < deadline) {
    await sleep(pollMs)
    const next: ProcRow[] = []
    for (const row of alive) if (await stillSame(row)) next.push(row)
    alive = next
  }
  return alive
}

/**
 * 执行回收：SIGTERM → 等 → 仍在者 SIGKILL → 等着 → 如实报告。
 *
 * 为什么两段：worker 起的可能是 shell 包装/脚本解释器，SIGTERM 让它跑收尾（把回退的文件写回、
 * 删临时文件）；只有**收尾无望的**才 KILL。若一上来就 KILL，那棵树上正在写文件的进程会被砍在
 * 半截写入上——比不杀更糟（留下半个 JSON / 半个源码）。
 */
export async function reapProcesses(plan: ReapPlan, opts: ReapDeps = {}): Promise<ReapResult> {
  if (opts.dryRun) return { outcome: "dry-run", targets: plan.targets, survivors: [], log: [] }
  if (!plan.targets.length) return { outcome: "nothing", targets: [], survivors: [], log: [] }
  const signal = opts.signal ?? defaultSignal
  const stillSame = opts.stillSame ?? ((row: ProcRow) => defaultStillSame(row))
  const sleep = opts.sleep ?? defaultSleep
  const pollMs = opts.pollMs ?? 100
  const log: string[] = []

  const pending: ProcRow[] = []
  for (const row of plan.targets) {
    if (!(await stillSame(row))) {
      log.push(`pid ${row.pid}：已不在（无需处理）`)
      continue
    }
    try {
      signal(row.pid, "SIGTERM")
      log.push(`pid ${row.pid} ← SIGTERM`)
      pending.push(row)
    } catch (err) {
      log.push(`pid ${row.pid}：SIGTERM 失败（${describe(err)}）`)
      pending.push(row)
    }
  }

  const afterTerm = await waitGone(pending, stillSame, sleep, opts.termWaitMs ?? 1200, pollMs)
  const killed: ProcRow[] = []
  for (const row of afterTerm) {
    try {
      signal(row.pid, "SIGKILL")
      log.push(`pid ${row.pid}：SIGTERM 未退 → SIGKILL`)
    } catch (err) {
      log.push(`pid ${row.pid}：SIGKILL 失败（${describe(err)}）`)
    }
    killed.push(row)
  }
  const survivors = await waitGone(killed, stillSame, sleep, opts.killWaitMs ?? 800, pollMs)
  for (const row of survivors) log.push(`pid ${row.pid}：SIGKILL 后仍在（需人工处理）`)
  // 一个信号都没发出去（目标全在发信号前就没了）= 「无需回收」：不能报成「已回收 0/N 个」
  if (pending.length === 0) return { outcome: "nothing", targets: plan.targets, survivors: [], log }
  return { outcome: survivors.length ? "partial" : "reaped", targets: plan.targets, survivors, log }
}

/** 一行可读的命令行（截断：审计要看清"是哪个命令"，但回显不该被一条巨长的命令行撑爆）。 */
export function cmdBrief(cmd: string, max = 160): string {
  const flat = (cmd ?? "").replace(/\s+/g, " ").trim()
  if (!flat) return "（无命令行：内核线程或已退出）"
  return flat.length > max ? `${flat.slice(0, max)}…` : flat
}

/** 计划回显（**杀之前先列出来**：谁要杀、谁看到了但不杀、为什么）。 */
export function renderPlan(plan: ReapPlan, opts: { dryRun?: boolean } = {}): string {
  const head = opts.dryRun ? "干跑（未发任何信号）" : "回收计划"
  if (plan.via === "none") {
    return `子孙进程树：**无法定位**（${plan.unresolved ?? "未知原因"}；扫过 ${plan.scanned} 个进程，来源 ${plan.source || "不可用"}）`
  }
  const lines = [`${head}（归因：${PROC_MARKER_ENV} 标记 + 子孙递归；扫过 ${plan.scanned} 个进程，来源 ${plan.source || "不可用"}）`]
  if (!plan.targets.length) lines.push("- 要杀的进程：（无）——该会话已没有在跑的子进程")
  else {
    lines.push(`- 要杀的进程（${plan.targets.length} 个，pid+命令行）：`)
    for (const t of plan.targets) lines.push(`  · ${t.pid}｜${cmdBrief(t.cmd)}`)
  }
  if (plan.skipped.length) {
    lines.push(`- 看到但不杀（${plan.skipped.length} 个，防跨线祸及）：`)
    for (const s of plan.skipped) lines.push(`  · ${s.pid}｜${cmdBrief(s.cmd)} —— ${s.why}`)
  }
  return lines.join("\n")
}

/** 回收结果回显（如实：杀了几个、补了几刀、还剩什么）。 */
export function renderReapResult(result: ReapResult): string {
  if (result.outcome === "dry-run") return "（干跑：以上清单未执行）"
  if (result.outcome === "nothing") return "子孙进程树：无需回收（该会话没有可归因的子进程）"
  const n = result.targets.length
  const dead = n - result.survivors.length
  const lines = [`子孙进程树：${result.outcome === "reaped" ? `已回收 ${dead}/${n} 个` : `**只回收了 ${dead}/${n} 个**`}`]
  if (result.log.length) lines.push(`- 处理记录：${result.log.join("；")}`)
  if (result.survivors.length) {
    lines.push(`- **仍在运行**（SIGKILL 也无效，需人工）：${result.survivors.map((s) => `${s.pid}｜${cmdBrief(s.cmd, 80)}`).join("；")}`)
  }
  return lines.join("\n")
}

// ---------------------------------------------------------------------------
// 工作树可疑状态：`*.revbak` 残留（只告警，**不自动恢复**）
// ---------------------------------------------------------------------------

/** 一处回退备份残留。 */
export interface RollbackResidue {
  /** 对应源文件绝对路径（`.revbak` 去掉后缀） */
  source: string
  /** 备份文件绝对路径 */
  backup: string
  /** 内容与源文件不一致 ⇒ 源码可能停在回退态（`reverse_verify.py` 的语义：残留备份即未完成的回退） */
  differs: boolean
  /** 读失败原因（读不到就无从判断，按"可疑"报告） */
  error?: string
}

export interface ResidueScan {
  found: RollbackResidue[]
  /** 扫过的文件数（回显用：说清这个结论覆盖了多大的范围） */
  scanned: number
  /** 触到扫描上限（结论只是"扫到的这一片里没有"，不是"全树没有"） */
  truncated: boolean
  error?: string
}

/** 扫描时跳过的目录（与被 grep/项目遍历跳过的同一批大目录：`*.revbak` 不会长在 node_modules 里）。 */
const SKIP_DIRS = new Set([".git", "node_modules", "dist", "build", ".next", ".cache", "__pycache__", ".venv", "venv", "target", ".idea", ".vscode", "coverage", ".turbo", ".evolve-shared"])

/** 单个备份文件的大小上限（比对用；超限按"可疑"报告，不整文件读进内存）。 */
const MAX_RESIDUE_BYTES = 4 * 1024 * 1024

/** 目录列举与文件读取的注入面：生产走宿主上下文（`listDir`/`readBinaryFile`，服务模式下仍受它的既有
 *  可达性口径约束），测试走直读 fs——**比对逻辑只有一份**。 */
export interface ResidueIo {
  list: (dir: string) => Promise<Array<{ name: string; isDir: boolean }>>
  read: (file: string) => Promise<Uint8Array>
}

/**
 * 扫一棵树里的 `*.revbak` 残留，并与对应源文件比对内容。
 *
 * **只读**：本函数不写、不恢复、不删除任何文件。自动恢复是刻意不做的——
 * 别人可能正拿那份回退态在做逆向验证（`reverse_verify.py` 会在回退态下跑测试），
 * 自动 `cp` 回去就会把他的实验覆盖掉，且覆盖者与被覆盖者都不会知道。
 */
export async function detectRollbackResidue(root: string, opts: { maxDepth?: number; maxFiles?: number; io?: ResidueIo } = {}): Promise<ResidueScan> {
  const maxDepth = opts.maxDepth ?? 8
  const maxFiles = opts.maxFiles ?? 20000
  const io: ResidueIo = opts.io ?? {
    list: async (dir) => {
      const entries = await readdir(dir, { withFileTypes: true })
      return entries.map((e) => ({ name: e.name, isDir: e.isDirectory() }))
    },
    read: (file) => readFile(file),
  }
  const backups: string[] = []
  let scanned = 0
  let truncated = false
  let walkError: string | undefined
  const walk = async (dir: string, depth: number): Promise<void> => {
    if (truncated) return
    if (depth > maxDepth) {
      truncated = true
      return
    }
    let entries: Array<{ name: string; isDir: boolean }>
    try {
      entries = await io.list(dir)
    } catch (err) {
      // 根目录本身就不可读 = 「无从检查」（必须说出来）；深处的子目录只是覆盖不到的一片。
      if (depth === 0) walkError = `${dir}：${describe(err)}`
      return
    }
    for (const e of entries) {
      if (truncated) return
      const full = `${dir.replace(/[\\/]+$/, "")}/${e.name}`
      if (e.isDir) {
        if (SKIP_DIRS.has(e.name)) continue
        await walk(full, depth + 1)
        continue
      }
      scanned++
      if (scanned > maxFiles) {
        truncated = true
        return
      }
      if (e.name.endsWith(ROLLBACK_SUFFIX)) backups.push(full)
    }
  }
  await walk(root, 0)
  if (walkError) return { found: [], scanned, truncated, error: walkError }
  const found: RollbackResidue[] = []
  for (const backup of backups) {
    const source = backup.slice(0, -ROLLBACK_SUFFIX.length)
    let src: Uint8Array
    let bak: Uint8Array
    try {
      src = await io.read(source)
      bak = await io.read(backup)
    } catch (err) {
      found.push({ source, backup, differs: true, error: `读不到（${describe(err)}）——无从判断，按可疑处理` })
      continue
    }
    if (src.length > MAX_RESIDUE_BYTES || bak.length > MAX_RESIDUE_BYTES) {
      found.push({ source, backup, differs: true, error: `文件超过 ${MAX_RESIDUE_BYTES} 字节，未逐字节比对——按可疑处理` })
      continue
    }
    found.push({ source, backup, differs: !bytesEqual(src, bak) })
  }
  found.sort((a, b) => a.backup.localeCompare(b.backup))
  return { found, scanned, truncated }
}

/** 逐字节相等（Buffer 与 Uint8Array 通吃；不用 `Buffer.equals` 免得把注入的 IO 钉在 node Buffer 上）。 */
function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false
  return true
}

/**
 * 残留回显：**只告警 + 给核对命令**，并明确说"没有自动恢复"。
 *
 * 覆盖：源码可能停在回退态（内容不一致）与"只是没删掉的备份"（内容一致）两种，
 * 因为后者同样说明「上一轮门禁没有正常收尾」——值得人看一眼，但不该喊成事故。
 */
export function renderResidue(scan: ResidueScan): string {
  if (scan.error) return `工作树回退态检查：无法检查（${scan.error}）——请人工确认`
  if (!scan.found.length) return ""
  const differs = scan.found.filter((r) => r.differs)
  const same = scan.found.filter((r) => !r.differs)
  const lines: string[] = []
  if (differs.length) {
    lines.push(
      `⚠ **源码可能停在回退态**（${differs.length} 处 \`${ROLLBACK_SUFFIX}\` 与源文件不一致；扫过 ${scan.scanned} 个文件${scan.truncated ? "，**触到扫描上限**（结论只覆盖扫到的部分）" : ""}）：`,
    )
    for (const r of differs) {
      lines.push(`- ${r.source} ← ${r.backup}${r.error ? `（${r.error}）` : ""}`)
      lines.push(`  核对：\`diff '${r.backup}' '${r.source}'\`——确认哪一边该留，再手工 \`cp\` 或 \`git checkout -- '${r.source}'\``)
    }
  }
  if (same.length) {
    lines.push(`- 另有 ${same.length} 处 \`${ROLLBACK_SUFFIX}\` 与源文件**一致**：上一轮门禁没正常收尾（残留而来），可人工删除`)
  }
  lines.push("（本工具**只告警、不自动恢复**：自动恢复可能覆盖别人正在验的回退态。）")
  return lines.join("\n")
}
