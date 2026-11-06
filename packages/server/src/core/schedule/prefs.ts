/**
 * 用户级偏好（DESIGN「用户级偏好」）：`users/{user}/prefs.json`，跨会话、跨设备一致。
 *
 * 为什么要它（而不是继续堆在浏览器 localStorage）：
 * - 手机与桌面看到的东西必须一致——「我的闲时任务只在夜里跑」不是某台设备的偏好，
 *   而是这个用户的工作方式；存在浏览器里换个设备就丢，且无法在服务端调度时读取；
 * - **数据以后端为准**：调度判定（闲时时间段）本来就发生在服务端，前端只展示后端给的结论。
 *
 * 只放"影响服务端行为或需要跨设备一致"的偏好；纯观感项（主题、浮层位置、折叠态、
 * 输入历史）仍留在浏览器本地——它们没有跨设备一致的需求，走一趟网络反而更慢。
 *
 * 存储范式与用户级待办一致：**磁盘真值 RMW**（跨进程写锁 + 原子写），读时以磁盘为准，
 * 内存镜像只是加速；外部编辑损坏的字段逐个丢弃，不拒载整份文件。
 */
import { readFile } from "node:fs/promises"
import { join } from "node:path"
import { mutateJsonObject } from "../support/json-store"
import type { IdleWindow } from "@gebai/sdk"
import { normalizeIdleWindow } from "./idle-window"

/** 用户级偏好（字段全部可选：缺省即用系统默认）。 */
export interface UserPrefs {
  /**
   * 默认闲时执行时间段：新建的闲时任务/待办**未显式指定时段**时继承它。
   * `null`/缺省 = 不限时段。这条让用户设一次就能覆盖之后的所有闲时任务。
   */
  idleWindow?: IdleWindow
  /** 更新时刻（毫秒时间戳，便于排查"什么时候改的"）。 */
  updatedAt?: number
}

/** 偏好文件里允许出现的键（其余键一律丢弃：外部编辑写错键名不该悄悄生效）。 */
const KNOWN_KEYS = new Set(["idleWindow", "updatedAt"])

export class UserPrefsManager {
  private cache = new Map<string, UserPrefs>()

  constructor(private home: string) {}

  /** 偏好文件路径（users/{user}/prefs.json）。 */
  private file(user: string): string {
    return join(this.home, "users", user, "prefs.json")
  }

  /** 读取用户偏好（缺文件/损坏 → 空偏好；**字段级**宽容：坏字段丢弃不影响好字段）。 */
  async get(user: string): Promise<UserPrefs> {
    const cached = this.cache.get(user)
    if (cached) return cached
    return await this.loadFromDisk(user)
  }

  private async loadFromDisk(user: string): Promise<UserPrefs> {
    let raw: unknown
    try {
      raw = JSON.parse(await readFile(this.file(user), "utf8"))
    } catch {
      this.cache.set(user, {})
      return {}
    }
    const prefs = normalizePrefs(raw)
    this.cache.set(user, prefs)
    return prefs
  }

  /**
   * 更新偏好（浅合并：未提供的字段不改动；`idleWindow: null` 表示清除）。
   * 校验失败抛错（调用方转 400）——用户正在改，应该立刻知道写错了。
   */
  async update(user: string, patch: { idleWindow?: IdleWindow | null; updatedAt?: number }): Promise<UserPrefs> {
    // 先校验（在副本上）：非法时段不该落盘
    let nextWindow: IdleWindow | undefined
    let windowTouched = false
    if (patch.idleWindow !== undefined) {
      windowTouched = true
      nextWindow = patch.idleWindow === null ? undefined : normalizeIdleWindow(patch.idleWindow)
    }
    const unknown = Object.keys(patch).filter((k) => !KNOWN_KEYS.has(k))
    if (unknown.length) throw new Error(`未知的偏好字段: ${unknown.join(", ")}（可用: idleWindow）`)
    const now = Date.now()
    const next = await mutateJsonObject<UserPrefs>(this.file(user), (disk) => {
      const base = normalizePrefs(disk)
      const merged: UserPrefs = { ...base, updatedAt: now }
      if (windowTouched) {
        if (nextWindow) merged.idleWindow = nextWindow
        else delete merged.idleWindow
      }
      return merged
    })
    const prefs = normalizePrefs(next)
    this.cache.set(user, prefs)
    return prefs
  }
}

/** 偏好归一化（字段级宽容）：未知键与坏值丢弃，返回干净对象。 */
function normalizePrefs(raw: unknown): UserPrefs {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return {}
  const r = raw as Record<string, unknown>
  const out: UserPrefs = {}
  if (r.idleWindow !== undefined && r.idleWindow !== null) {
    try {
      const w = normalizeIdleWindow(r.idleWindow)
      if (w) out.idleWindow = w
    } catch {
      /* 坏时段丢弃（回到不限时段），不影响其他字段 */
    }
  }
  if (typeof r.updatedAt === "number" && Number.isFinite(r.updatedAt)) out.updatedAt = r.updatedAt
  return out
}
