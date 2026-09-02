import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, writeFileSync, utimesSync, existsSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import { runGC } from "./gc"
import { sessionPath } from "../base/paths"

const NOW = Date.UTC(2026, 0, 15, 12) // 2026-01-15
const DAY = 24 * 3600 * 1000

function touch(p: string, mtime: number): void {
  mkdirSync(dirname(p), { recursive: true })
  writeFileSync(p, "x")
  utimesSync(p, new Date(mtime), new Date(mtime))
}

describe("gc", () => {
  test("archives idle sessions and keeps active ones", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-gc-"))
    const oldId = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" // 合法会话 id（32 位 hex）
    const activeId = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    const oldChat = join(sessionPath(home, "alice", oldId), "chat.json")
    touch(oldChat, NOW - 91 * DAY)
    const activeChat = join(sessionPath(home, "alice", activeId), "chat.json")
    touch(activeChat, NOW - 10 * DAY)

    const archived: string[] = []
    const stats = await runGC(home, { now: () => NOW, onArchive: (id) => archived.push(id) })
    expect(stats.sessionsArchived).toBe(1)
    expect(archived).toEqual([oldId])
    expect(existsSync(oldChat)).toBe(false)
    expect(existsSync(activeChat)).toBe(true)
    // 归档位置：trash/{date}/{sessionId}/（会话目录整体移动，含 tmp/truncated/）
    expect(existsSync(join(home, "users", "alice", "trash", "2026-01-15", oldId, "chat.json"))).toBe(true)
  })

  test("purges trash and feedback date dirs beyond retention", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-gc-"))
    // trash：8 天前的日期目录（超 7 天保留期）删除；1 天前的保留
    touch(join(home, "users", "alice", "trash", "2026-01-07", "s1", "chat.json"), NOW - 8 * DAY)
    touch(join(home, "users", "alice", "trash", "2026-01-14", "s2", "chat.json"), NOW - 1 * DAY)
    // feedback：200 天前（超 180 天保留期）删除；10 天前保留
    touch(join(home, "users", "alice", "feedback", "2025-06-29", "f1.json"), NOW - 200 * DAY)
    touch(join(home, "users", "alice", "feedback", "2026-01-05", "f2.json"), NOW - 10 * DAY)

    const stats = await runGC(home, { now: () => NOW })
    expect(stats.trashDirsDeleted).toBe(1)
    expect(stats.feedbackDirsDeleted).toBe(1)
    expect(existsSync(join(home, "users", "alice", "trash", "2026-01-07"))).toBe(false)
    expect(existsSync(join(home, "users", "alice", "trash", "2026-01-14"))).toBe(true)
    expect(existsSync(join(home, "users", "alice", "feedback", "2025-06-29"))).toBe(false)
    expect(existsSync(join(home, "users", "alice", "feedback", "2026-01-05"))).toBe(true)
  })

  test("removes legacy user-level truncated dir only after grace and tolerates missing home", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-gc-"))
    // 超 30 天宽限期未再写入 → 删除
    const legacyDir = join(home, "users", "alice", "truncated")
    touch(join(legacyDir, "2025-01-01", "ab", "cd", "sh_x.txt"), NOW - 300 * DAY)
    utimesSync(legacyDir, new Date(NOW - 300 * DAY), new Date(NOW - 300 * DAY))
    // 近期仍写入 → 保留（旧版本进程共存场景）
    const freshDir = join(home, "users", "bob", "truncated")
    touch(join(freshDir, "2026-01-01", "ab", "cd", "sh_x.txt"), NOW - 1 * DAY)
    utimesSync(freshDir, new Date(NOW - 1 * DAY), new Date(NOW - 1 * DAY))

    const stats = await runGC(home, { now: () => NOW })
    expect(stats.legacyTruncatedDeleted).toBe(true)
    expect(existsSync(legacyDir)).toBe(false)
    expect(existsSync(freshDir)).toBe(true)

    // 无 users 目录：幂等返回全零
    const empty = mkdtempSync(join(tmpdir(), "gebai-gc-"))
    const s2 = await runGC(join(empty, "nope"), { now: () => NOW })
    expect(s2.sessionsArchived).toBe(0)
    expect(s2.trashDirsDeleted).toBe(0)
    expect(s2.feedbackDirsDeleted).toBe(0)
  })
})
