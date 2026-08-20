import { mkdir, rename, writeFile } from "node:fs/promises"
import { join } from "node:path"
import { randomUUID } from "node:crypto"
import { spawn } from "node:child_process"

/**
 * sh 异步后台任务（DESIGN「sh 异步执行」）：`sh async:true` 启动命令进后台并立即返回 taskId，
 * 模型可先处理其他任务，之后经 `sh_task`（status/wait/kill/list）回头查询/等待/终止。
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
  opts: { cwd?: string; env?: Record<string, string>; logPath: string; input?: string },
) => ShTaskProcess

export interface ShTaskService {
  /** 启动后台任务：spawn + 落盘记录，立即返回（不等待完成）。并发超限抛错。 */
  start(command: string, opts: { cwd?: string; env?: Record<string, string>; input?: string; maxMs?: number }): Promise<ShTaskRecord>
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
}

/** 单会话并发后台任务上限（防失控堆积；超限拒绝新任务）。 */
export const SH_TASK_MAX_CONCURRENT = 8
/** 后台任务生命周期默认/上限（毫秒）：默认 30 分钟、上限 60 分钟（防僵尸进程常驻）。 */
export const SH_TASK_DEFAULT_MS = 30 * 60 * 1000
export const SH_TASK_MAX_MS = 60 * 60 * 1000
/** wait 轮询间隔（毫秒）。 */
const SH_TASK_POLL_MS = 300

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

  constructor(opts: { dir: string; spawner: ShTaskSpawner; now?: () => number }) {
    this.dir = opts.dir
    this.spawner = opts.spawner
    this.now = opts.now ?? Date.now
  }

  private get recordsPath(): string {
    return join(this.dir, "tasks.json")
  }

  private logPath(id: string): string {
    return join(this.dir, `${id}.log`)
  }

  /** 原子写（tmp + rename，防并发读到半写状态）。 */
  private async save(records: ShTaskRecord[]): Promise<void> {
    await mkdir(this.dir, { recursive: true })
    const tmp = `${this.recordsPath}.${randomUUID().slice(0, 8)}.tmp`
    await writeFile(tmp, JSON.stringify(records), "utf8")
    await rename(tmp, this.recordsPath)
  }

  private async load(): Promise<ShTaskRecord[]> {
    try {
      const raw = await Bun.file(this.recordsPath).json()
      return Array.isArray(raw) ? (raw as ShTaskRecord[]) : []
    } catch {
      return []
    }
  }

  /** 单任务存活/超时惰性刷新：pid 失活 → lost；超生命周期 → 终止并标记 timedOut。返回是否发生变化。 */
  private async refreshOne(r: ShTaskRecord): Promise<boolean> {
    if (r.endedAt) return false
    if (r.pid != null && !pidAlive(r.pid)) {
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

  private async refreshAll(): Promise<ShTaskRecord[]> {
    const records = await this.load()
    let dirty = false
    for (const r of records) {
      if (await this.refreshOne(r)) dirty = true
    }
    if (dirty) await this.save(records)
    return records
  }

  async start(command: string, opts: { cwd?: string; env?: Record<string, string>; input?: string; maxMs?: number }): Promise<ShTaskRecord> {
    const records = await this.refreshAll()
    if (records.filter((r) => !r.endedAt).length >= SH_TASK_MAX_CONCURRENT) {
      throw new Error(`并发后台任务超限（≥${SH_TASK_MAX_CONCURRENT}）：请先用 sh_task（action=kill/status）清理已完成的任务再启动。`)
    }
    const id = `t${randomUUID().replace(/-/g, "").slice(0, 8)}`
    await mkdir(this.dir, { recursive: true })
    const proc = this.spawner(command, { cwd: opts.cwd, env: opts.env, logPath: this.logPath(id), input: opts.input })
    const rec: ShTaskRecord = {
      id,
      command,
      cwd: opts.cwd ?? "",
      pid: proc.pid,
      startedAt: this.now(),
      maxMs: opts.maxMs ?? SH_TASK_DEFAULT_MS,
    }
    // 退出回写（闭包落盘，长任务跨工具调用存活）：lost 已置（失活竞态）时仅补退出码
    proc.exited.then(
      (code) => void this.finish(id, { exitCode: code }),
      (err) => void this.finish(id, { exitCode: 1, spawnError: String(err) }),
    )
    this.procs.set(id, proc)
    await this.save([...records, rec])
    return rec
  }

  /** 进程退出回写：填 endedAt/exitCode；已因失活判定 lost 的仅补退出码（endedAt 保留首次判定）。 */
  private async finish(id: string, patch: { exitCode: number; spawnError?: string }): Promise<void> {
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
    const r = await this.refresh(id)
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
    return this.refreshAll()
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
}
