import { readdir, readFile, stat, mkdir, rename, rm } from "node:fs/promises"
import { join, dirname } from "node:path"
import { walkDir } from "../base/paths"

/** 对齐 DESIGN.md 常量表：会话闲置过期 90 天 / trash 保留 7 天 / feedback 保留 180 天。 */
export const DEFAULT_SESSION_IDLE_MS = 90 * 24 * 60 * 60 * 1000
export const DEFAULT_TRASH_RETENTION_MS = 7 * 24 * 60 * 60 * 1000
export const DEFAULT_FEEDBACK_RETENTION_MS = 180 * 24 * 60 * 60 * 1000
/** GC 周期：24 小时；启动时立即执行一次。 */
export const GC_INTERVAL_MS = 24 * 60 * 60 * 1000

export interface GCOptions {
  now?: () => number
  sessionIdleMs?: number
  trashRetentionMs?: number
  feedbackRetentionMs?: number
  /** 会话归档到 trash/ 后回调（供上层失效会话缓存，防止 save() 重建目录）。 */
  onArchive?: (sessionId: string) => void
}

/** 遗留用户级 truncated/ 目录删除前的宽限期：旧版本进程可能仍在写入（原保留期 30 天）。 */
const LEGACY_TRUNCATED_GRACE_MS = 30 * 24 * 60 * 60 * 1000

export interface GCStats {
  sessionsArchived: number
  trashDirsDeleted: number
  feedbackDirsDeleted: number
  legacyTruncatedDeleted: boolean
}

/** 日期目录名 YYYY-MM-DD → UTC 时间戳；解析失败返回 null（跳过，不误删）。 */
function dateDirMs(name: string): number | null {
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(name)
  if (!m) return null
  const t = Date.UTC(Number(m[1]), Number(m[2]) - 1, Number(m[3]))
  return Number.isNaN(t) ? null : t
}

/**
 * 数据生命周期清理（DESIGN「数据生命周期」）：
 * 1. 会话 chat.json 超过闲置期未活跃 → 归档到 `users/{user}/trash/{date}/{id}`（保留期内可恢复）
 * 2. `trash/` 日期目录超过保留期 → 物理删除
 * 3. `feedback/` 日期目录超过保留期 → 删除（管理导出不受影响）
 * 4. 遗留用户级 `truncated/` → 一次性迁移清理（截断文件已并入会话 tmp/，随会话归档/删除）
 */
export async function runGC(home: string, opts: GCOptions = {}): Promise<GCStats> {
  const now = opts.now?.() ?? Date.now()
  const sessionIdleMs = opts.sessionIdleMs ?? DEFAULT_SESSION_IDLE_MS
  const trashRetentionMs = opts.trashRetentionMs ?? DEFAULT_TRASH_RETENTION_MS
  const feedbackRetentionMs = opts.feedbackRetentionMs ?? DEFAULT_FEEDBACK_RETENTION_MS
  const stats: GCStats = { sessionsArchived: 0, trashDirsDeleted: 0, feedbackDirsDeleted: 0, legacyTruncatedDeleted: false }

  let users: string[] = []
  try {
    users = (await readdir(join(home, "users"), { withFileTypes: true })).filter((e) => e.isDirectory()).map((e) => e.name)
  } catch {
    return stats // 无用户数据目录
  }

  for (const user of users) {
    const uDir = join(home, "users", user)

    // 1. 过期会话 → trash/{date}/{id}（目录整体移动，含 tmp/truncated/ 等）
    await walkDir(join(uDir, "sessions"), 3, async (chatPath) => {
      if (!chatPath.endsWith("chat.json")) return
      let mtime: number
      try {
        mtime = (await stat(chatPath)).mtimeMs
      } catch {
        return
      }
      if (now - mtime <= sessionIdleMs) return
      const sessionId = chatPath.split(/[\\/]/).at(-2)
      if (!sessionId) return
      const date = new Date(now).toISOString().slice(0, 10)
      try {
        await mkdir(join(uDir, "trash", date), { recursive: true })
        await rename(dirname(chatPath), join(uDir, "trash", date, sessionId))
        stats.sessionsArchived++
        opts.onArchive?.(sessionId)
      } catch {
        /* 归档失败不影响其他清理 */
      }
    })

    // 2. trash 日期目录超保留期 → 物理删除
    stats.trashDirsDeleted += await purgeDateDirs(join(uDir, "trash"), now, trashRetentionMs)

    // 3. feedback 日期目录超保留期 → 删除
    stats.feedbackDirsDeleted += await purgeDateDirs(join(uDir, "feedback"), now, feedbackRetentionMs)

    // 4. 遗留用户级 truncated/（已并入会话 tmp，迁移清理）：目录超过宽限期（30 天）未再写入才删除，
    //    避免与仍在运行、尚在写入该目录的旧版本进程冲突
    try {
      const st = await stat(join(uDir, "truncated"))
      if (now - st.mtimeMs > LEGACY_TRUNCATED_GRACE_MS) {
        await rm(join(uDir, "truncated"), { recursive: true, force: true })
        stats.legacyTruncatedDeleted = true
      }
    } catch {
      /* 不存在或删除失败忽略 */
    }
  }
  return stats
}

/**
 * 在 trash/ 中查找已归档会话（恢复用）：遍历各用户 trash/{date}/{sessionId} 目录，
 * 读取 chat.json 的 userId 作为归属（文件缺失时以目录所属用户兜底）。未找到返回 null。
 */
export async function findInTrash(home: string, sessionId: string): Promise<{ trashDir: string; owner: string } | null> {
  let users: string[] = []
  try {
    users = (await readdir(join(home, "users"), { withFileTypes: true })).filter((e) => e.isDirectory()).map((e) => e.name)
  } catch {
    return null
  }
  for (const user of users) {
    const trashRoot = join(home, "users", user, "trash")
    let dates: import("node:fs").Dirent[]
    try {
      dates = await readdir(trashRoot, { withFileTypes: true })
    } catch {
      continue
    }
    for (const d of dates) {
      if (!d.isDirectory()) continue
      const cand = join(trashRoot, d.name, sessionId)
      try {
        await stat(cand)
      } catch {
        continue
      }
      let owner = user
      try {
        const data = JSON.parse(await readFile(join(cand, "chat.json"), "utf8")) as { userId?: string }
        if (data.userId) owner = data.userId
      } catch {
        /* chat.json 缺失/损坏：以目录所属用户兜底 */
      }
      return { trashDir: cand, owner }
    }
  }
  return null
}

/** 按日期目录名清理超保留期目录（trash/feedback 共用）。 */
async function purgeDateDirs(dir: string, now: number, retentionMs: number): Promise<number> {
  let entries
  try {
    entries = await readdir(dir, { withFileTypes: true })
  } catch {
    return 0
  }
  let deleted = 0
  for (const e of entries) {
    if (!e.isDirectory()) continue
    const t = dateDirMs(e.name)
    if (t === null) continue // 非日期目录不误删
    if (now - t > retentionMs) {
      try {
        await rm(join(dir, e.name), { recursive: true, force: true })
        deleted++
      } catch {
        /* 忽略 */
      }
    }
  }
  return deleted
}

/** 启动即执行一次，之后每 intervalMs 周期执行；失败不影响服务。返回 stop 供宿主关闭。 */
export function scheduleGC(home: string, opts: GCOptions = {}, intervalMs = GC_INTERVAL_MS): { stop: () => void } {
  let timer: ReturnType<typeof setTimeout> | null = null
  let stopped = false
  const tick = async () => {
    try {
      const stats = await runGC(home, opts)
      // 日志脱敏：仅统计数字，不打印用户/路径明细
      console.log(
        `[gebai] gc: archived=${stats.sessionsArchived} trashDirs=${stats.trashDirsDeleted} feedbackDirs=${stats.feedbackDirsDeleted} legacyTruncated=${stats.legacyTruncatedDeleted}`,
      )
    } catch {
      /* GC 失败不影响服务 */
    }
    if (!stopped) timer = setTimeout(tick, intervalMs)
  }
  timer = setTimeout(tick, 0)
  return {
    stop: () => {
      stopped = true
      if (timer) clearTimeout(timer)
    },
  }
}
