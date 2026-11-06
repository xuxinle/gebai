import { describe, expect, test } from "bun:test"
import { mkdtempSync, readFileSync, rmSync, writeFileSync, mkdirSync, existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { UserPrefsManager } from "./prefs"
import { isInIdleWindow } from "./idle-window"

/**
 * 用户级偏好（users/{user}/prefs.json）：磁盘真值 RMW、字段级宽容、默认闲时时段。
 *
 * 这些用例守的是「后端为准」这条不变式：偏好是**服务端**读的东西（调度要用），
 * 所以它必须落盘、必须能在新进程里读回来、损坏时必须只丢坏字段而不是整份。
 */
function setup(): { home: string; prefs: UserPrefsManager; cleanup: () => void } {
  const home = mkdtempSync(join(tmpdir(), "gebai-prefs-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  return {
    home,
    prefs: new UserPrefsManager(home),
    cleanup: () => rmSync(home, { recursive: true, force: true }),
  }
}

const fileOf = (home: string): string => join(home, "users", "default", "prefs.json")

describe("用户级偏好", () => {
  test("默认空偏好（缺文件不报错）", async () => {
    const s = setup()
    try {
      expect(await s.prefs.get("default")).toEqual({})
    } finally {
      s.cleanup()
    }
  })

  test("写入后落盘，换一个实例（模拟进程重启）仍读得到", async () => {
    const s = setup()
    try {
      const saved = await s.prefs.update("default", { idleWindow: { start: "22:00", end: "06:00", days: [1, 2, 3, 4, 5] } })
      expect(saved.idleWindow).toEqual({ days: [1, 2, 3, 4, 5], start: "22:00", end: "06:00" })
      expect(existsSync(fileOf(s.home))).toBe(true)
      // 新实例（无内存缓存）从磁盘读回同一份
      const fresh = new UserPrefsManager(s.home)
      expect((await fresh.get("default")).idleWindow).toEqual({ days: [1, 2, 3, 4, 5], start: "22:00", end: "06:00" })
    } finally {
      s.cleanup()
    }
  })

  test("浅合并：未提供的字段不改动", async () => {
    const s = setup()
    try {
      await s.prefs.update("default", { idleWindow: { start: "22:00", end: "06:00" } })
      // 只改星期：start/end 保留
      const next = await s.prefs.update("default", { idleWindow: { start: "22:00", end: "06:00", days: [0] } })
      expect(next.idleWindow).toEqual({ days: [0], start: "22:00", end: "06:00" })
    } finally {
      s.cleanup()
    }
  })

  test("idleWindow: null 清除（回到不限时段）", async () => {
    const s = setup()
    try {
      await s.prefs.update("default", { idleWindow: { start: "22:00", end: "06:00" } })
      const cleared = await s.prefs.update("default", { idleWindow: null })
      expect(cleared.idleWindow).toBeUndefined()
      // 清空同样落盘（重启后不会“复活”）
      const fresh = new UserPrefsManager(s.home)
      expect((await fresh.get("default")).idleWindow).toBeUndefined()
    } finally {
      s.cleanup()
    }
  })

  test("非法时段拒绝写入（目录里不留坏数据）", async () => {
    const s = setup()
    try {
      await expect(s.prefs.update("default", { idleWindow: { start: "25:00", end: "06:00" } })).rejects.toThrow(/start 非法/)
      expect(existsSync(fileOf(s.home))).toBe(false)
    } finally {
      s.cleanup()
    }
  })

  test("未知字段拒绝（写错键名不该悄悄生效）", async () => {
    const s = setup()
    try {
      await expect(s.prefs.update("default", { nope: 1 } as never)).rejects.toThrow(/未知的偏好字段/)
    } finally {
      s.cleanup()
    }
  })

  test("磁盘上手改坏了：坏字段丢弃，好字段照读（不整份失效）", async () => {
    const s = setup()
    try {
      writeFileSync(
        fileOf(s.home),
        JSON.stringify({ idleWindow: { start: "坏", end: "06:00" }, extra: "unknown", updatedAt: 123 }),
        "utf8",
      )
      const prefs = await new UserPrefsManager(s.home).get("default")
      expect(prefs.idleWindow).toBeUndefined() // 坏时段丢弃 → 回到不限时段
      expect(prefs.updatedAt).toBe(123) // 好字段保留
      expect("extra" in prefs).toBe(false) // 未知键丢弃
    } finally {
      s.cleanup()
    }
  })

  test("文件整体损坏（非 JSON）退化为空偏好，不抛错", async () => {
    const s = setup()
    try {
      writeFileSync(fileOf(s.home), "{ 不是 json", "utf8")
      expect(await new UserPrefsManager(s.home).get("default")).toEqual({})
    } finally {
      s.cleanup()
    }
  })

  test("落盘为可 diff 的缩进 JSON（人可直接看/改）", async () => {
    const s = setup()
    try {
      await s.prefs.update("default", { idleWindow: { start: "22:00", end: "06:00" } })
      const raw = readFileSync(fileOf(s.home), "utf8")
      expect(raw).toContain("\n  ")
      expect(JSON.parse(raw).idleWindow.start).toBe("22:00")
    } finally {
      s.cleanup()
    }
  })

  test("跨零点时段在偏好里读回后仍按跨零点判定（端到端语义一致）", async () => {
    const s = setup()
    try {
      await s.prefs.update("default", { idleWindow: { start: "22:00", end: "06:00" } })
      const w = (await new UserPrefsManager(s.home).get("default")).idleWindow!
      expect(isInIdleWindow(w, new Date(2026, 2, 4, 23, 0).getTime())).toBe(true) // 深夜
      expect(isInIdleWindow(w, new Date(2026, 2, 5, 3, 0).getTime())).toBe(true) // 次日凌晨
      expect(isInIdleWindow(w, new Date(2026, 2, 5, 12, 0).getTime())).toBe(false) // 白天
    } finally {
      s.cleanup()
    }
  })
})
