/**
 * 跨进程安全的 JSON 清单存储（用户级 `users/{user}/todos.json`、`cron.json` 等「多实例共库」文件）。
 *
 * 为什么需要：这两处此前是「进程启动读一次 → 内存镜像 → 整体覆盖式写回」（各 `saveUserEntries`），
 * 三个后果——
 * ① 多实例并存时后写者整体覆盖前者：同一条闲时待办被两个实例各跑一次、`idleAttempts` 只记 1 次、
 *    `idleSessionId` 只留后写者；
 * ② 本进程镜像陈旧或为空时一次覆盖即清空磁盘既有条目（曾把两条已完成待办连同 `idleResult` 抹掉）；
 * ③ 直接 `writeFile` 无原子性，读方可能看到半截 JSON。
 *
 * 本模块把写入收敛为 **RMW（读真值 → 合并 → 原子写）**：
 * - **合并基准是磁盘真值**而非本进程镜像——调用方只提供「变更函数」，镜像陈旧不再具有破坏性；
 * - **跨进程写锁**：`O_EXCL` 独占建锁文件（内容 `{pid,at}`）；持有者 PID 存活且租约未过期才等待，
 *   否则视为陈旧锁抢占（把「PID 复用」收敛为可判定：停止续租后租约到期即被接管）；
 * - **写前复核** `mtime`/`size`：期间被非协作写者（旧版本进程、外部编辑）改动则重读重试；
 * - **滚动备份**：覆盖前把磁盘现值留存为 `<file>.bak`（仅当原内容非空且与新内容不同）。
 *
 * 锁是**建议性**的：只有经由本模块写入的进程之间才互斥（旧版本进程或手工编辑不受约束，
 * 由写前 mtime/size 复核兜住「改坏了就重读」）。同一进程内对同一文件**不可重入**——内层等待
 * 自己持有的锁会直到超时抛错，故变更函数内不要再调用本模块写同一文件。
 */
import { mkdir, open, readFile, rename, stat, unlink, writeFile, type FileHandle } from "node:fs/promises"
import { dirname } from "node:path"

/** 写锁默认等待上限（ms）：写操作本身是毫秒级，超时即抛错而不是盲写。 */
export const JSON_LOCK_TIMEOUT_MS = 5_000
/** 写锁默认租约（ms）：持有者超期未释放视为已死。写是毫秒级，15s 足够宽裕（仅防进程崩溃残留）。 */
export const JSON_LOCK_LEASE_MS = 15_000
/** 空白/损坏锁的宽限期（ms）：`O_EXCL` 创建与写入内容之间有微小窗口，此间读到的锁文件是空的——
 *  宽限期内一律视为「正在持有」（等待），超出才按陈旧抢占（兼顾崩溃残留的清理）。 */
export const JSON_LOCK_GRACE_MS = 2_000
/** RMW 冲突重试次数（写前复核发现被并发改动时重读重试）。 */
export const JSON_RMW_RETRIES = 3

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms))

/** PID 存活探测（与 `core/schedule/primary.ts` 的 `defaultIsPidAlive` 同语义：信号 0，EPERM 视为存活）。
 *  此处独立实现以保持模块依赖单向（support 不反向依赖 schedule）。 */
function isPidAlive(pid: number): boolean {
  if (!Number.isInteger(pid) || pid <= 0) return false
  try {
    process.kill(pid, 0)
    return true
  } catch (err) {
    return (err as NodeJS.ErrnoException)?.code !== "ESRCH"
  }
}

async function statOrNull(file: string): Promise<{ mtimeMs: number; size: number } | null> {
  try {
    const s = await stat(file)
    return { mtimeMs: s.mtimeMs, size: s.size }
  } catch {
    return null
  }
}

/** 锁文件内容（诊断用）。 */
interface LockInfo {
  pid: number
  at: number
}

async function readLockInfo(lockPath: string): Promise<LockInfo | null> {
  try {
    const p = JSON.parse((await readFile(lockPath, "utf8")).replace(/^\uFEFF/, "")) as LockInfo
    if (!p || !Number.isInteger(p.pid) || p.pid <= 0 || typeof p.at !== "number") return null
    return { pid: p.pid, at: p.at }
  } catch {
    // 锁文件读不到或损坏：无法判定持有者 → 按陈旧处理（抢占），避免残留锁永久阻塞写入
    return null
  }
}

/** 空白/损坏锁是否已可抢占：仅当锁定文件比宽限期更旧（创建后未及时写入内容 → 崩溃残留）。 */
async function staleBlankLock(lockPath: string, graceMs: number, now: () => number): Promise<boolean> {
  try {
    const s = await stat(lockPath)
    return now() - s.mtimeMs > graceMs
  } catch {
    return true // 锁文件已消失（持有者刚好释放）→ 立即重试抢占
  }
}

/** 可注入的锁参数（测试用）。 */
export interface FileLockOptions {
  /** 等待上限（ms），超时抛错。 */
  timeoutMs?: number
  /** 租约（ms）：持有者超期未释放即视为已死。 */
  leaseMs?: number
  /** 空白/损坏锁的宽限期（ms，默认 2s）：区分「正在创建」与「崩溃残留」。 */
  graceMs?: number
  /** 可注入时钟（测试用）。 */
  now?: () => number
}

/**
 * 跨进程独占写锁：`fn` 执行期间独占 `lockPath`（调用方用「被保护文件 + .lock」派生锁路径）。
 * 持有者死亡或租约过期时抢占，等待超时抛错（调用方应把该错误暴露给用户而不是降级为无锁写）。
 */
export async function withFileLock<T>(lockPath: string, fn: () => Promise<T>, opts: FileLockOptions = {}): Promise<T> {
  const timeoutMs = opts.timeoutMs ?? JSON_LOCK_TIMEOUT_MS
  const leaseMs = opts.leaseMs ?? JSON_LOCK_LEASE_MS
  const graceMs = opts.graceMs ?? JSON_LOCK_GRACE_MS
  const now = opts.now ?? Date.now
  const deadline = now() + timeoutMs
  let handle: FileHandle | null = null
  let delay = 10
  for (;;) {
    try {
      await mkdir(dirname(lockPath), { recursive: true })
      handle = await open(lockPath, "wx")
      await handle.writeFile(JSON.stringify({ pid: process.pid, at: now() } satisfies LockInfo), "utf8")
      break
    } catch (err) {
      if ((err as NodeJS.ErrnoException)?.code !== "EEXIST") throw err
      // 已存在：判定是否陈旧（持有者已死 / 租约过期 / 内容损坏且超出宽限期）→ 抢占后立即重试
      const holder = await readLockInfo(lockPath)
      const stale = holder ? !isPidAlive(holder.pid) || now() - holder.at > leaseMs : await staleBlankLock(lockPath, graceMs, now)
      if (stale) {
        const removed = await unlink(lockPath).then(() => true, () => false)
        if (removed) continue
      }
      if (now() >= deadline) {
        throw new Error(`获取写锁超时（${lockPath}）${holder ? `，持有者 PID ${holder.pid}` : ""}：请稍后重试`)
      }
      await sleep(delay)
      delay = Math.min(delay * 2, 200)
    }
  }
  try {
    return await fn()
  } finally {
    await handle?.close().catch(() => {})
    await unlink(lockPath).catch(() => {})
  }
}

/** 宽容读取 JSON 数组（缺失/损坏/非数组 → 空清单；条目经 `normalize` 过滤，返回 null 即丢弃）。 */
export async function readJsonList<T>(file: string, normalize: (raw: unknown) => T | null): Promise<T[]> {
  try {
    const parsed = JSON.parse((await readFile(file, "utf8")).replace(/^\uFEFF/, ""))
    if (!Array.isArray(parsed)) return []
    const out: T[] = []
    for (const item of parsed) {
      const e = normalize(item)
      if (e) out.push(e)
    }
    return out
  } catch {
    return []
  }
}

/** 现值是否含实际内容（本模块写入的都是 JSON 数组：空数组/空串/null 视为无内容，不作备份）。 */
function hasContent(raw: string): boolean {
  const s = raw.trim()
  return s !== "" && s !== "[]" && s !== "null"
}

/** 原子写：临时文件写全 + rename 替换；覆盖前把磁盘现值留存 `<file>.bak`（原内容非空且不同才留）。 */
export async function writeJsonListAtomic<T>(file: string, data: T[], opts: { backup?: boolean } = {}): Promise<void> {
  await mkdir(dirname(file), { recursive: true })
  const body = JSON.stringify(data, null, 2)
  const tmp = `${file}.${process.pid}.tmp`
  try {
    await writeFile(tmp, body, "utf8")
    if (opts.backup !== false) {
      const cur = await readFile(file, "utf8").catch(() => null)
      // 只备份「有实际内容且与新内容不同」的现值：既避免无谓写，也避免用空清单（`[]`）覆盖掉上一份可用备份
      if (cur !== null && hasContent(cur) && cur !== body) await writeFile(`${file}.bak`, cur, "utf8").catch(() => {})
    }
    await rename(tmp, file)
  } catch (err) {
    await unlink(tmp).catch(() => {})
    throw err
  }
}

/** RMW 参数。 */
export interface MutateJsonListOptions<T> extends FileLockOptions {
  /** 条目归一化（读取真值时过滤损坏条目）。 */
  normalize: (raw: unknown) => T | null
  /** 冲突重试次数（写前复核发现并发改动）。 */
  retries?: number
  /** 是否滚动备份（默认 true）。 */
  backup?: boolean
}

/**
 * 读磁盘真值 → 应用变更函数 → 原子写回（全程持跨进程写锁）。
 * 返回**落盘后的磁盘真值**（调用方据此同步本地镜像，保证与磁盘一致）。
 */
export async function mutateJsonList<T>(
  file: string,
  mutate: (disk: T[]) => T[] | Promise<T[]>,
  opts: MutateJsonListOptions<T>,
): Promise<T[]> {
  const retries = opts.retries ?? JSON_RMW_RETRIES
  return await withFileLock(
    `${file}.lock`,
    async () => {
      for (let attempt = 0; attempt < retries; attempt++) {
        const before = await statOrNull(file)
        const disk = await readJsonList(file, opts.normalize)
        const next = await mutate(disk)
        // 无实质变更：不写（避免无谓覆盖、无谓备份与 mtime 抖动）
        if (JSON.stringify(disk) === JSON.stringify(next)) return disk
        if (!before && next.length === 0) return next
        // 写前复核：期间被非协作写者改动（旧版本进程/手工编辑）→ 重读重试，避免覆盖别人的新内容
        const after = await statOrNull(file)
        if (before && after && (before.mtimeMs !== after.mtimeMs || before.size !== after.size)) continue
        await writeJsonListAtomic(file, next, { backup: opts.backup })
        return next
      }
      throw new Error(`并发写入冲突，已重试 ${retries} 次仍失败：${file}`)
    },
    opts,
  )
}
