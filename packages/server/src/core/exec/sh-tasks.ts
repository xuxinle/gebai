import { mkdir, rename, unlink, writeFile } from "node:fs/promises"
import { join } from "node:path"
import { randomUUID } from "node:crypto"
import { spawn } from "node:child_process"
import { decodeOutput } from "../security/sandbox"

/**
 * sh 的后台任务机制（DESIGN「sh 执行」）：命令统统经 `Sandbox.spawnBackground` 起进程并登记为任务——
 * `sh async:true` 立即返回 taskId；同步调用在等待窗口内等它（`run`），窗口到期仍未结束就转后台返回 taskId。
 * 模型可先处理其他任务，之后经 `bg_task`（status/wait/stop/list，与子Agent 运行统一管理面）回头查询/等待/终止。
 *
 * 状态落盘在会话 tmp/sh-tasks/（tasks.json 记录 + {id}.log 合并输出日志），跨工具调用与服务重启可见：
 * - 进程退出码由启动时注册的 exited 回调回写（同进程内准确）；
 * - 服务重启后 pid 失活而记录无 endedAt → 判定 lost（已结束、退出码未知），日志尾部仍可读。
 */

/** 后台任务记录（tasks.json 数组项）。 */
export interface ShTaskRecord {
  id: string
  command: string
  cwd: string
  pid: number | null
  startedAt: number
  /** 生命周期上限（毫秒）：超时仍在运行则终止并标记 timedOut（惰性检查——status/wait/list/kill 时触发）。 */
  maxMs: number
  endedAt?: number
  exitCode?: number
  timedOut?: boolean
  killed?: boolean
  /** 进程已失活但退出码未知（服务重启/外部死亡，close 回调未捕获）。 */
  lost?: boolean
  /** spawn 失败原因（进程启动即失败时记录）。 */
  spawnError?: string
}

/** 任务进程句柄（Sandbox.spawnBackground 返回；测试注入假实现）。 */
export interface ShTaskProcess {
  pid: number | null
  /** 进程退出（close）时以退出码 resolve；spawn 失败 reject。 */
  exited: Promise<number>
  /** 终止进程树（幂等）。 */
  kill: () => void
}

/** 进程生成器（依赖注入：引擎接 Sandbox.spawnBackground；测试接假实现）。 */
export type ShTaskSpawner = (
  cmd: string,
  opts: { cwd?: string; env?: Record<string, string>; logPath: string; input?: string; onChunk?: (stream: "stdout" | "stderr", chunk: Buffer) => void },
) => ShTaskProcess

export interface ShTaskService {
  /** 启动后台任务：spawn + 落盘记录，立即返回（不等待完成）。并发超限抛错。 */
  start(command: string, opts: ShTaskStartOptions): Promise<ShTaskRecord>
  /** 刷新单任务存活/超时状态并返回（不存在返回 undefined）。 */
  refresh(id: string): Promise<ShTaskRecord | undefined>
  /** 阻塞等待任务结束（或超时返回当前状态；不存在返回 undefined）。 */
  wait(id: string, timeoutMs: number): Promise<ShTaskRecord | undefined>
  /** 终止任务进程树并标记 killed（已结束的任务原样返回）。 */
  kill(id: string): Promise<ShTaskRecord | undefined>
  /** 全部任务（先统一刷新存活/超时）。 */
  list(): Promise<ShTaskRecord[]>
  /** 读取任务合并输出（stdout+stderr）尾部字符。 */
  readLog(id: string, tailChars: number): Promise<string>
  /** 前台运行（同步 sh 的统一执行路径）：后台启动 + 窗口内等待；窗口到期仍在运行则原样返回（不终止）。 */
  run(command: string, opts: ShTaskRunOptions): Promise<ShTaskRunOutcome>
}

/** 后台任务启动入参（输出旁路与 waitMs/signal 无关启动，单独传）。 */
export interface ShTaskStartOptions {
  cwd?: string
  env?: Record<string, string>
  input?: string
  maxMs?: number
  /** 输出旁路捕获（前台等待路径用，见 run）：日志照常落盘，同时按流分发原始输出块。 */
  onChunk?: (stream: "stdout" | "stderr", chunk: Buffer) => void
}

/** 前台运行入参：waitMs 为同步等待窗口（到期不终止任务），maxMs 为任务生命周期上限，signal 中止按进程树终止。 */
export interface ShTaskRunOptions {
  cwd?: string
  env?: Record<string, string>
  input?: string
  maxMs?: number
  waitMs: number
  signal?: AbortSignal
}

/** 前台运行结果：窗口内结束 → finished=true（record 为终态）；窗口到期仍在运行 → finished=false（任务继续后台执行）。 */
export interface ShTaskRunResult {
  started: true
  record: ShTaskRecord
  finished: boolean
  /** 等待期间被外部取消：任务已按进程树终止（退出码未知）。 */
  aborted: boolean
  stdout: string
  stderr: string
  /** 输出超过捕获上限（尾部未捕获；该任务保留记录与日志，完整输出见日志文件）。 */
  truncated: boolean
}

/** 前台运行整体结果：后台启动失败（并发超限等）返回 started=false，由调用方回退同步执行。 */
export type ShTaskRunOutcome = ShTaskRunResult | { started: false; error: string }

/** 单会话并发后台任务上限（防失控堆积；超限拒绝新任务）。 */
export const SH_TASK_MAX_CONCURRENT = 8
/** 后台任务生命周期默认/上限（毫秒）：默认 30 分钟、上限 60 分钟（防僵尸进程常驻）。 */
export const SH_TASK_DEFAULT_MS = 30 * 60 * 1000
export const SH_TASK_MAX_MS = 60 * 60 * 1000
/** wait 轮询间隔（毫秒）。 */
const SH_TASK_POLL_MS = 300
/** 前台运行的等待轮询起步间隔（毫秒）：短命令（echo/git status）结束即返，不白等一轮轮询，随后退避到 SH_TASK_POLL_MS。 */
const SH_TASK_POLL_MIN_MS = 20
/** 前台运行的输出捕获上限（每流字节）：超限停止捕获（不驻留内存），该任务保留记录与日志供回查。 */
const SH_TASK_CAPTURE_CAP = 4 * 1024 * 1024

/** sh async 超时参数解析（秒 → 毫秒）：默认 1800（30 分钟），上限 3600；与同步超时（默认 300/上限 540）独立。 */
export function shTaskLifetimeMs(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SH_TASK_DEFAULT_MS
  return Math.min(n, SH_TASK_MAX_MS / 1000) * 1000
}

export function shTaskStatus(r: ShTaskRecord): "running" | "done" | "failed" | "killed" | "timed_out" | "lost" {
  if (!r.endedAt) return "running"
  if (r.timedOut) return "timed_out"
  if (r.killed) return "killed"
  if (r.lost) return "lost"
  return r.exitCode === 0 ? "done" : "failed"
}

function pidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0)
    return true
  } catch {
    return false
  }
}

/** pid 进程树终止（句柄丢失/服务重启后兜底；Windows taskkill /T，Unix 进程组）。 */
function killPidTree(pid: number): void {
  const isWin = process.platform === "win32"
  try {
    if (isWin) spawn("taskkill", ["/pid", String(pid), "/T", "/F"], { stdio: "ignore" }).on("error", () => {})
    else process.kill(-pid, "SIGKILL")
  } catch {
    /* 进程组不存在（已退出） */
  }
}

/** 磁盘持久化实现（会话 tmp/sh-tasks/）：每次调用重建状态（无内存前提），跨调用/跨重启可见。 */
export class ShTaskRunner implements ShTaskService {
  private dir: string
  private spawner: ShTaskSpawner
  private now: () => number
  /** 运行中任务的进程句柄（本进程内 kill 精确终止用；重启后为空走 pid 兜底）。 */
  private procs = new Map<string, ShTaskProcess>()
  /** 状态读改写串行队列（见 lock）。 */
  private queue: Promise<unknown> = Promise.resolve()

  constructor(opts: { dir: string; spawner: ShTaskSpawner; now?: () => number }) {
    this.dir = opts.dir
    this.spawner = opts.spawner
    this.now = opts.now ?? Date.now
  }

  /** 状态读改写串行化：start/finish/refresh/kill/list/purge 依次执行——各自「load → 改 → save」，
   *  并发执行时后写会覆盖先写（同批并行调用 sh 极常见），任务记录凭空消失、bg_task 再也查不到。 */
  private lock<T>(fn: () => Promise<T>): Promise<T> {
    const run = this.queue.then(fn, fn)
    this.queue = run.then(
      () => undefined,
      () => undefined,
    )
    return run
  }

  private get recordsPath(): string {
    return join(this.dir, "tasks.json")
  }

  private logPath(id: string): string {
    return join(this.dir, `${id}.log`)
  }

  /**
   * 状态落盘（tmp + rename 原子写，防并发读到半写状态）。
   *
   * **本方法永不抛错**——记录落盘只是副作用，不该有进程级杀伤力。
   *
   * 事故背景（真实发生过，服务被打崩）：
   * - Windows 上 `rename` **不能覆盖一个正被打开的文件**。
   * - 调用点（`refreshAll` / `start` / `finish` / `refresh` / `kill`）全是裸 `await this.save(...)`，
   *   而 `finish` 又是由 `proc.exited.then(...)` 驱动的 —— 一旦 `rename` 抛 EPERM，
   *   拒绝就变成 **unhandled rejection**，Bun 直接终止进程。
   * - 触发条件很平常：一边高频读 `tasks.json`（如 `bg_task status` 轮询），一边后台任务写入重命名。
   *
   * 因此两道防线：瞬态占用**退避重试**（Windows 锁通常毫秒级释放）；
   * 重试仍不成功则**降级为直接写**（非原子，但保住状态），并只告警不抛出。
   */
  private async save(records: ShTaskRecord[]): Promise<void> {
    const payload = JSON.stringify(records)
    try {
      await mkdir(this.dir, { recursive: true })
      const tmp = `${this.recordsPath}.${randomUUID().slice(0, 8)}.tmp`
      await writeFile(tmp, payload, "utf8")
      // 退避重试：EPERM/EBUSY/EACCES 在 Windows 上多为「目标文件正被打开」的瞬态占用
      let lastErr: unknown
      for (let attempt = 0; attempt < 5; attempt++) {
        try {
          await rename(tmp, this.recordsPath)
          return
        } catch (err) {
          lastErr = err
          const code = (err as NodeJS.ErrnoException).code
          if (code !== "EPERM" && code !== "EBUSY" && code !== "EACCES") break
          await new Promise((r) => setTimeout(r, 20 * 2 ** attempt))
        }
      }
      // 降级：直接覆盖写（失去原子性但保住状态）——宁可短暂可读到半写，也不能丢任务记录或崩进程
      await writeFile(this.recordsPath, payload, "utf8").catch(() => undefined)
      await unlink(tmp).catch(() => undefined)
      console.warn(`[sh-tasks] tasks.json 原子替换失败，已降级为直接写：${String((lastErr as Error)?.message ?? lastErr)}`)
    } catch (err) {
      // 最后一道兑底：落盘彻底失败也只告警（任务日志 {id}.log 仍可读，不丢诊断能力）
      console.warn(`[sh-tasks] tasks.json 落盘失败（不影响任务执行与查询）：${String((err as Error)?.message ?? err)}`)
    }
  }

  private async load(): Promise<ShTaskRecord[]> {
    try {
      const raw = await Bun.file(this.recordsPath).json()
      return Array.isArray(raw) ? (raw as ShTaskRecord[]) : []
    } catch {
      return []
    }
  }

  /** 单任务存活/超时惰性刷新：pid 失活 → lost；超生命周期 → 终止并标记 timedOut。返回是否发生变化。
   *  本进程持有**同一 pid** 的句柄时不做 pid 探测——退出由 exited 回调回写 endedAt，
   *  中间态的 pid 探测与 close 竞态会误判 lost；句柄缺失或 pid 不符（服务重启/记录被外部改写）
   *  才走 pid 兜底判定。 */
  private async refreshOne(r: ShTaskRecord): Promise<boolean> {
    if (r.endedAt) return false
    const handle = this.procs.get(r.id)
    const handleOwnsRecord = handle != null && handle.pid != null && handle.pid === r.pid
    if (!handleOwnsRecord && r.pid != null && !pidAlive(r.pid)) {
      r.lost = true
      r.endedAt = this.now()
      return true
    }
    if (this.now() - r.startedAt > r.maxMs) {
      const proc = this.procs.get(r.id)
      if (proc) proc.kill()
      else if (r.pid != null) killPidTree(r.pid)
      r.timedOut = true
      r.endedAt = this.now()
      return true
    }
    return false
  }

  private async refreshAllLocked(): Promise<ShTaskRecord[]> {
    const records = await this.load()
    let dirty = false
    for (const r of records) {
      if (await this.refreshOne(r)) dirty = true
    }
    if (dirty) await this.save(records)
    return records
  }

  async start(command: string, opts: ShTaskStartOptions): Promise<ShTaskRecord> {
    return this.lock(() => this.startLocked(command, opts))
  }

  private async startLocked(command: string, opts: ShTaskStartOptions): Promise<ShTaskRecord> {
    const records = await this.refreshAllLocked()
    if (records.filter((r) => !r.endedAt).length >= SH_TASK_MAX_CONCURRENT) {
      throw new Error(`并发后台任务超限（≥${SH_TASK_MAX_CONCURRENT}）：请先用 bg_task（action=stop/status）清理已完成的任务再启动。`)
    }
    const id = `t${randomUUID().replace(/-/g, "").slice(0, 8)}`
    await mkdir(this.dir, { recursive: true })
    const proc = this.spawner(command, { cwd: opts.cwd, env: opts.env, logPath: this.logPath(id), input: opts.input, onChunk: opts.onChunk })
    const rec: ShTaskRecord = {
      id,
      command,
      cwd: opts.cwd ?? "",
      pid: proc.pid,
      startedAt: this.now(),
      maxMs: opts.maxMs ?? SH_TASK_DEFAULT_MS,
    }
    this.procs.set(id, proc)
    // 先落盘再注册退出回写：命令可能瞬时退出（echo），回调先于记录落盘时 finish 读不到记录
    // 会静默丢弃退出码，任务永久停在 running
    await this.save([...records, rec])
    // 退出回写（闭包落盘，长任务跨工具调用存活）：lost 已置（失活竞态）时仅补退出码。
    // catch 兵底：这两个回调在事件循环里无人 await，一旦拒绝就是 **unhandled rejection**（Bun 会终止进程）——
    // 退出回写只是记账，绝不能让它打崩服务（历史事故正是这条路径）。
    proc.exited.then(
      (code) => this.finish(id, { exitCode: code }),
      (err) => this.finish(id, { exitCode: 1, spawnError: String(err) }),
    ).catch((err: unknown) => {
      console.warn(`[sh-tasks] 任务 ${id} 退出回写失败（不影响进程存活）：${String((err as Error)?.message ?? err)}`)
    })
    return rec
  }

  /** 进程退出回写：填 endedAt/exitCode；已因失活判定 lost 的仅补退出码（endedAt 保留首次判定）。 */
  private finish(id: string, patch: { exitCode: number; spawnError?: string }): Promise<void> {
    return this.lock(() => this.finishLocked(id, patch))
  }

  private async finishLocked(id: string, patch: { exitCode: number; spawnError?: string }): Promise<void> {
    const records = await this.load()
    const r = records.find((x) => x.id === id)
    if (!r) return
    r.exitCode = patch.exitCode
    if (!r.endedAt) {
      r.endedAt = this.now()
      if (patch.spawnError) r.spawnError = patch.spawnError
    }
    await this.save(records)
    const proc = this.procs.get(id)
    if (proc && proc.pid != null && !pidAlive(proc.pid)) this.procs.delete(id)
  }

  async refresh(id: string): Promise<ShTaskRecord | undefined> {
    return this.lock(() => this.refreshLocked(id))
  }

  private async refreshLocked(id: string): Promise<ShTaskRecord | undefined> {
    const records = await this.load()
    const r = records.find((x) => x.id === id)
    if (!r) return undefined
    if (await this.refreshOne(r)) await this.save(records)
    return r
  }

  async wait(id: string, timeoutMs: number): Promise<ShTaskRecord | undefined> {
    const deadline = this.now() + Math.max(0, timeoutMs)
    for (;;) {
      const r = await this.refresh(id)
      if (!r || r.endedAt) return r
      if (this.now() >= deadline) return r
      await new Promise((res) => setTimeout(res, SH_TASK_POLL_MS))
    }
  }

  async kill(id: string): Promise<ShTaskRecord | undefined> {
    return this.lock(() => this.killLocked(id))
  }

  private async killLocked(id: string): Promise<ShTaskRecord | undefined> {
    const r = await this.refreshLocked(id)
    if (!r) return undefined
    if (r.endedAt) return r
    const proc = this.procs.get(id)
    if (proc) proc.kill()
    else if (r.pid != null) killPidTree(r.pid)
    r.killed = true
    r.endedAt = this.now()
    const records = await this.load()
    const t = records.find((x) => x.id === id)
    if (t && !t.endedAt) {
      t.killed = true
      t.endedAt = r.endedAt
      await this.save(records)
    }
    this.procs.delete(id)
    return r
  }

  async list(): Promise<ShTaskRecord[]> {
    return this.lock(() => this.refreshAllLocked())
  }

  async readLog(id: string, tailChars: number): Promise<string> {
    try {
      const buf = Buffer.from(await Bun.file(this.logPath(id)).arrayBuffer())
      const text = buf.toString("utf8")
      return text.length > tailChars ? text.slice(-tailChars) : text
    } catch {
      return ""
    }
  }

  /**
   * 前台运行（同步 sh 的统一执行路径）：后台启动 + 窗口内等待。窗口内结束则按同步语义返回捕获的 stdout/stderr；
   * 窗口到期仍在运行则**不终止**命令，按 running 记录返回（由调用方转后台，后续经 bg_task 跟踪）；
   * 等待期间 signal 中止即按进程树终止任务并标记 aborted。后台启动失败（并发超限等）返回 started=false。
   *
   * 记录清理：窗口内结束且输出未超捕获上限的任务**不留痕**（记录与日志一并删除——它从未成为后台任务，
   * 留在清单里只会污染 bg_task list 并让 tasks.json 无限增长）；转后台（未结束）与输出被截断的任务保留。
   */
  async run(command: string, opts: ShTaskRunOptions): Promise<ShTaskRunOutcome> {
    const chunks = { stdout: [] as Buffer[], stderr: [] as Buffer[], outBytes: 0, errBytes: 0 }
    let capturing = true
    let truncated = false
    const onChunk = (stream: "stdout" | "stderr", chunk: Buffer): void => {
      if (!capturing || truncated) return
      const used = stream === "stdout" ? chunks.outBytes : chunks.errBytes
      if (used + chunk.length > SH_TASK_CAPTURE_CAP) {
        truncated = true
        return
      }
      if (stream === "stdout") {
        chunks.stdout.push(chunk)
        chunks.outBytes += chunk.length
      } else {
        chunks.stderr.push(chunk)
        chunks.errBytes += chunk.length
      }
    }
    let rec: ShTaskRecord
    try {
      rec = await this.start(command, { cwd: opts.cwd, env: opts.env, input: opts.input, maxMs: opts.maxMs, onChunk })
    } catch (err) {
      return { started: false, error: String((err as Error)?.message ?? err) }
    }
    const pack = async (record: ShTaskRecord, finished: boolean, aborted: boolean): Promise<ShTaskRunResult> => {
      capturing = false
      const out: ShTaskRunResult = {
        started: true,
        record,
        finished,
        aborted,
        stdout: decodeOutput(Buffer.concat(chunks.stdout)),
        stderr: decodeOutput(Buffer.concat(chunks.stderr)),
        truncated,
      }
      if ((finished || aborted) && !truncated) await this.purge(record.id)
      return out
    }
    const deadline = this.now() + Math.max(0, opts.waitMs)
    let delay = SH_TASK_POLL_MIN_MS
    for (;;) {
      const r = (await this.refresh(rec.id)) ?? rec
      if (r.endedAt) return pack(r, true, false)
      if (opts.signal?.aborted) return pack((await this.kill(rec.id)) ?? r, false, true)
      const remain = deadline - this.now()
      if (remain <= 0) return pack(r, false, false)
      await new Promise((res) => setTimeout(res, Math.min(delay, remain)))
      delay = Math.min(delay * 2, SH_TASK_POLL_MS)
    }
  }

  /** 清理任务记录与日志（同步窗口内已结束的任务不留痕）。永不抛错：清理失败不影响已捕获的结果。 */
  private purge(id: string): Promise<void> {
    return this.lock(() => this.purgeLocked(id))
  }

  private async purgeLocked(id: string): Promise<void> {
    try {
      const records = await this.load()
      const rest = records.filter((r) => r.id !== id)
      if (rest.length !== records.length) await this.save(rest)
      this.procs.delete(id)
      await unlink(join(this.dir, `${id}.log`)).catch(() => undefined)
    } catch {
      /* 清理尽力而为 */
    }
  }
}
