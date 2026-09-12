/** 服务端调度器跨进程互斥（主实例锁）。
 *
 *  调度判定全在**进程内**（`CronManager.tick` 的 nextRunAt 推进、`UserTodoManager.tick` 的 `firing` 单飞
 *  与 `engine.busy()`），`users/{user}/cron.json`、`todos.json` 也只在各自 `start()` 时读入内存 Map、
 *  各写各的镜像。因此同一 `GEBAI_HOME` 下多实例并存时，每个实例都会跑一份调度：闲时待办被重复领走
 *  （每个实例都认为「本进程没有运行中的会话」= 服务端空闲）、定时任务重复触发、通知重复投递。
 *
 *  锁文件 `{GEBAI_HOME}/.gebai-primary.json`，内容 `{ pid, port, at }`（`at` = 最近一次续租时刻）：
 *  - 文件不存在 / 损坏 / 持有者 PID 已死 / 租约过期（`at` 距今超过 leaseMs）→ 抢占成为主实例；
 *  - 持有者 PID 是自己 → 续租 `at`，保持主实例；
 *  - 持有者 PID 存活且租约未过期 → 从实例：只提供 HTTP/WS 服务，不跑调度。
 *
 *  租约把「PID 复用」这一不可判定项收敛为可判定：即便持有的 PID 被无关进程复用，只要主实例不再续租，
 *  租约到期（默认 5 分钟）即被接管——看门狗周期（默认 30 秒）远小于租约，正常运行时不会被误接管。
 *  写入一律「临时文件 + rename」原子落盘（读方不会看到半截 JSON）。
 *
 *  看门狗（`start`）：主实例每周期续租；从实例在主实例死亡/租约过期后**自动接管**并回调启动调度。
 *  模式（`GEBAI_SCHEDULER`）：auto=按锁判定（默认，唯一启动看门狗的模式）；on=强制本实例跑调度
 *  （忽略锁、不落锁——多实例同时 on 会重复调度，仅用于单实例部署/调试）；off=完全不跑调度。
 */
import { readFile, rename, unlink, writeFile } from "node:fs/promises"
import { join } from "node:path"
import { log as logger } from "@gebai/sdk/node"

/** 主实例锁文件名（`{GEBAI_HOME}/.gebai-primary.json`）。 */
export const PRIMARY_LOCK_FILE = ".gebai-primary.json"
/** 主实例租约有效期（ms）：主实例每看门狗周期续租，超期未续租即视为已死。 */
export const PRIMARY_LEASE_MS = 5 * 60_000
/** 看门狗周期（ms）：续租 + 判定是否该接管。 */
export const PRIMARY_WATCHDOG_INTERVAL_MS = 30_000

/** 调度器门控模式（`GEBAI_SCHEDULER`）。 */
export type SchedulerMode = "auto" | "on" | "off"

/** 锁文件内容。 */
export interface PrimaryLock {
  /** 主实例 PID。 */
  pid: number
  /** 主实例监听端口（诊断用；desktop 形态为 0）。 */
  port: number
  /** 最近一次续租时刻（ms）。 */
  at: number
}

/** 锁文件路径。 */
export function primaryLockFile(home: string): string {
  return join(home, PRIMARY_LOCK_FILE)
}

/** 解析门控模式（未设置/非法值按 auto）。 */
export function resolveSchedulerMode(env: Record<string, string | undefined> = process.env): SchedulerMode {
  const v = (env.GEBAI_SCHEDULER ?? "").trim().toLowerCase()
  return v === "on" || v === "off" ? v : "auto"
}

/** 默认 PID 存活探测（信号 0：ESRCH=不存在；EPERM 等=存在但非本进程，按存活计）。 */
export function defaultIsPidAlive(pid: number): boolean {
  if (!Number.isInteger(pid) || pid <= 0) return false
  try {
    process.kill(pid, 0)
    return true
  } catch (err) {
    return (err as NodeJS.ErrnoException)?.code !== "ESRCH"
  }
}

/** 读锁文件（不存在/损坏/字段非法返回 null）。 */
export async function readPrimaryLock(home: string): Promise<PrimaryLock | null> {
  try {
    const raw = await readFile(primaryLockFile(home), "utf8")
    const p = JSON.parse(raw.replace(/^\uFEFF/, "")) as PrimaryLock
    if (!p || !Number.isInteger(p.pid) || p.pid <= 0 || typeof p.at !== "number" || !Number.isFinite(p.at)) return null
    return { pid: p.pid, port: typeof p.port === "number" ? p.port : 0, at: p.at }
  } catch {
    return null
  }
}

/** 原子落盘：临时文件写全 + rename 替换（读方只会看到完整 JSON）。 */
async function writeLockAtomic(home: string, lock: PrimaryLock, pid: number): Promise<void> {
  const file = primaryLockFile(home)
  const tmp = `${file}.${pid}.tmp`
  try {
    await writeFile(tmp, JSON.stringify(lock), "utf8")
    await rename(tmp, file)
  } catch (err) {
    await unlink(tmp).catch(() => {})
    throw err
  }
}

/** 主实例门控依赖（结构接口 + 可注入时钟/PID/存活探测，便于单测）。 */
export interface PrimaryGateDeps {
  /** 数据根目录（锁文件所在处）。 */
  home: string
  /** 本实例监听端口（仅记录，desktop 形态 0 亦可）。 */
  port: number
  /** 门控模式（缺省读 `GEBAI_SCHEDULER`）。 */
  mode?: SchedulerMode
  /** 本实例 PID（缺省 `process.pid`）。 */
  pid?: number
  /** 可注入时钟（缺省 `Date.now`）。 */
  now?: () => number
  /** 租约有效期（缺省 `PRIMARY_LEASE_MS`）。 */
  leaseMs?: number
  /** 看门狗周期（缺省 `PRIMARY_WATCHDOG_INTERVAL_MS`）。 */
  intervalMs?: number
  /** 日志输出（缺省经 `log.info`，受 GEBAI_LOG_LEVEL 过滤）。 */
  log?: (msg: string) => void
  /** PID 存活探测（缺省 `defaultIsPidAlive`）。 */
  isAlive?: (pid: number) => boolean
  /** 定时器（缺省全局 setInterval/clearInterval；测试注入）。 */
  setInterval?: (fn: () => void, ms: number) => ReturnType<typeof setInterval>
  clearInterval?: (t: ReturnType<typeof setInterval>) => void
}

/** 主实例门控：一次性判定 + 看门狗（续租 / 接管 / 退让）。 */
export interface PrimaryGate {
  readonly mode: SchedulerMode
  /** 当前是否为主实例（`acquire` 之后有效）。 */
  isPrimary(): boolean
  /** 一次性判定并落锁/续租，返回是否为主实例。 */
  acquire(): Promise<boolean>
  /** 立即跑一轮看门狗判定（续租/接管/退让），返回判定后是否为主实例（看门狗内部同用）。 */
  checkNow(): Promise<boolean>
  /** 启动看门狗（仅 auto）：从实例接管时回调 `onBecomePrimary`，主实例被接管时回调 `onLosePrimary`。 */
  start(onBecomePrimary: () => void | Promise<void>, onLosePrimary?: () => void | Promise<void>): void
  /** 停止看门狗。 */
  stop(): void
  /** 释放锁（仅当锁属于自己时删除）。 */
  release(): Promise<void>
}

/** 构造主实例门控（见文件头语义说明）。 */
export function createPrimaryGate(deps: PrimaryGateDeps): PrimaryGate {
  const home = deps.home
  const port = deps.port
  const mode = deps.mode ?? resolveSchedulerMode()
  const pid = deps.pid ?? process.pid
  const now = deps.now ?? (() => Date.now())
  const leaseMs = deps.leaseMs ?? PRIMARY_LEASE_MS
  const intervalMs = deps.intervalMs ?? PRIMARY_WATCHDOG_INTERVAL_MS
  const log = deps.log ?? ((m: string) => logger.info(m))
  const isAlive = deps.isAlive ?? defaultIsPidAlive
  const setTimer = deps.setInterval ?? ((fn, ms) => setInterval(fn, ms))
  const clearTimer = deps.clearInterval ?? ((t) => clearInterval(t))

  let primary = false
  let timer: ReturnType<typeof setInterval> | null = null
  /** 看门狗重入防护（磁盘读写与接管回调可能跨多个周期）。 */
  let watching = false
  let onBecome: (() => void | Promise<void>) | undefined
  let onLose: (() => void | Promise<void>) | undefined

  /** 他人持有且仍有效（PID 存活 + 租约未过期）= 本实例须退让。 */
  function heldByOther(lock: PrimaryLock | null, at: number): lock is PrimaryLock {
    return !!lock && lock.pid !== pid && isAlive(lock.pid) && at - lock.at <= leaseMs
  }

  /** 抢占/续租：原子写入后回读校验——被并发抢占者覆盖（其内容仍存活）即退让。
   *  锁不可写（只读家目录/权限异常）时按主实例继续：锁是协作式互斥，写不了就没有互斥可言，
   *  此时退让会让定时任务与闲时待办在单实例部署下静默罢工。 */
  async function claim(at: number): Promise<boolean> {
    try {
      await writeLockAtomic(home, { pid, port, at }, pid)
    } catch (err) {
      log(`[scheduler] 主实例锁写入失败（${String((err as Error)?.message || err)}），本实例按主实例继续`)
      return true
    }
    const back = await readPrimaryLock(home)
    if (back && back.pid !== pid && isAlive(back.pid)) return false
    return true
  }

  async function acquire(): Promise<boolean> {
    if (mode === "off") {
      primary = false
      log("[scheduler] GEBAI_SCHEDULER=off：本实例不运行定时任务与闲时待办")
      return false
    }
    if (mode === "on") {
      primary = true
      log("[scheduler] GEBAI_SCHEDULER=on：本实例强制运行定时任务与闲时待办（忽略主实例锁）")
      return true
    }
    const at = now()
    const lock = await readPrimaryLock(home)
    if (heldByOther(lock, at)) {
      primary = false
      log(`[scheduler] 未获得主实例锁（PID ${lock.pid} 持有），本实例不跑定时任务与闲时待办`)
      return false
    }
    if (!(await claim(at))) {
      primary = false
      log("[scheduler] 未获得主实例锁（另一实例同时抢占），本实例不跑定时任务与闲时待办")
      return false
    }
    primary = true
    log(`[scheduler] 已获得主实例锁（PID ${pid}），本实例运行定时任务与闲时待办`)
    return true
  }

  /** 看门狗一轮：主实例续租（含锁文件被外部删除后重建）；从实例在主实例失效后接管；被接管则退让。 */
  async function watch(): Promise<boolean> {
    if (watching) return primary
    watching = true
    try {
      const at = now()
      const lock = await readPrimaryLock(home)
      if (!heldByOther(lock, at)) {
        const wasPrimary = primary
        if (!(await claim(at))) return primary
        primary = true
        if (!wasPrimary) {
          log(`[scheduler] 已接管主实例调度（本实例 PID ${pid}）`)
          await onBecome?.()
        }
        return primary
      }
      if (primary) {
        primary = false
        log(`[scheduler] 主实例锁已被 PID ${lock.pid} 接管，本实例停止定时任务与闲时待办调度`)
        await onLose?.()
      }
      return primary
    } finally {
      watching = false
    }
  }

  return {
    mode,
    isPrimary: () => primary,
    acquire,
    checkNow: watch,
    start(onBecomePrimary, onLosePrimary) {
      onBecome = onBecomePrimary
      onLose = onLosePrimary
      // 仅 auto 参与锁协商：on/off 是显式部署决策，不启用看门狗
      if (mode !== "auto" || timer) return
      timer = setTimer(() => {
        void watch().catch((err) => log(`[scheduler] 主实例看门狗异常: ${String((err as Error)?.message || err)}`))
      }, intervalMs)
      timer.unref?.()
    },
    stop() {
      if (timer !== null) clearTimer(timer)
      timer = null
    },
    async release() {
      const lock = await readPrimaryLock(home)
      if (lock && lock.pid === pid) await unlink(primaryLockFile(home)).catch(() => {})
      primary = false
    },
  }
}
