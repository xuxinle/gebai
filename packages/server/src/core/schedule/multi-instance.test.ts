import { describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, readFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { EventBus } from "../base/event-bus"
import { EnvManager } from "../session/env"
import { SessionStore } from "../session/store"
import { CronManager } from "./cron"
import { UserTodoManager } from "./todos"

/**
 * 多实例共库（同一 GEBAI_HOME 下两个进程）的写路径回归：
 *
 * 旧实现是「启动读一次 → 内存镜像 → 整体覆盖式写回」，两个实例各写各的镜像，后写者把前者
 * 写入的条目整体抹掉（曾造成：同一条闲时待办被两实例各跑一次、计次只留一份；一次「空清单写回」
 * 把已完成待办连同 idleResult 记录整体清空）。
 *
 * 本组用例用**两个管理器实例指向同一 home** 模拟两个进程：任一实例的写入都必须以磁盘真值为基准
 * 合并，不得抹掉另一个实例写入的条目。用旧实现跑这些用例会失败（覆盖语义）。
 */

const T0 = 1_780_000_000_000

function home(): string {
  const h = mkdtempSync(join(tmpdir(), "gebai-multi-"))
  mkdirSync(join(h, "users", "default"), { recursive: true })
  return h
}

const readTodos = (h: string, user = "default") =>
  JSON.parse(readFileSync(join(h, "users", user, "todos.json"), "utf8")) as Array<{ id: string; text: string }>
const readCron = (h: string, user = "default") =>
  JSON.parse(readFileSync(join(h, "users", user, "cron.json"), "utf8")) as Array<{ id: string; name?: string }>

describe("用户级待办：多实例共库不互相覆盖", () => {
  test("B 实例新增不抹掉 A 实例已写入的条目（旧实现整体覆盖会丢）", async () => {
    const h = home()
    try {
      const a = new UserTodoManager({ home: h, store: new SessionStore({ home: h }), now: () => T0 })
      const b = new UserTodoManager({ home: h, store: new SessionStore({ home: h }), now: () => T0 })
      await a.start()
      await b.start() // 两个实例都在启动时读入（此后各自持有镜像）

      await a.add("default", { text: "A 的待办" })
      expect(readTodos(h).map((t) => t.text)).toEqual(["A 的待办"])

      await b.add("default", { text: "B 的待办" }) // B 的镜像里没有 A 的条目
      expect(readTodos(h).map((t) => t.text)).toEqual(["A 的待办", "B 的待办"]) // 两条都在

      a.stop()
      b.stop()
    } finally {
      rmSync(h, { recursive: true, force: true })
    }
  })

  test("陈旧/空镜像写回不清空磁盘既有条目（曾把已完成待办连同结果一起抹掉）", async () => {
    const h = home()
    try {
      const a = new UserTodoManager({ home: h, store: new SessionStore({ home: h }), now: () => T0 })
      await a.start()
      const done = await a.add("default", { text: "已完成的任务" })
      await a.update("default", done.id, { done: true })

      // 新实例（空镜像）执行一次「看似无害」的写入：旧实现会用空镜像整体覆盖 → 磁盘清空
      const b = new UserTodoManager({ home: h, store: new SessionStore({ home: h }), now: () => T0 })
      await b.start()
      await b.add("default", { text: "新任务" })

      const onDisk = readTodos(h)
      expect(onDisk.map((t) => t.text)).toEqual(["已完成的任务", "新任务"])
      expect((onDisk[0] as unknown as { done: boolean }).done).toBe(true) // 完成状态与结果字段保留

      a.stop()
      b.stop()
    } finally {
      rmSync(h, { recursive: true, force: true })
    }
  })

  test("删除只影响目标条目：另一实例新增的条目不被连带删除", async () => {
    const h = home()
    try {
      const a = new UserTodoManager({ home: h, store: new SessionStore({ home: h }), now: () => T0 })
      const b = new UserTodoManager({ home: h, store: new SessionStore({ home: h }), now: () => T0 })
      await a.start()
      await b.start()
      const mine = await a.add("default", { text: "我的" })
      await b.add("default", { text: "别人的" })
      await a.remove("default", mine.id)
      expect(readTodos(h).map((t) => t.text)).toEqual(["别人的"])
      a.stop()
      b.stop()
    } finally {
      rmSync(h, { recursive: true, force: true })
    }
  })
})

describe("定时任务：多实例共库不互相覆盖", () => {
  function cronOf(h: string): CronManager {
    const store = new SessionStore({ home: h })
    return new CronManager({
      home: h,
      store,
      env: new EnvManager(store),
      sandbox: { exec: async () => ({ stdout: "", stderr: "", code: 0 }) } as never,
      events: new EventBus(),
      now: () => T0,
      tickIntervalMs: 3_600_000,
    })
  }

  test("B 实例新增任务不抹掉 A 实例的任务；删除只影响目标", async () => {
    const h = home()
    try {
      const a = cronOf(h)
      const b = cronOf(h)
      await a.start()
      await b.start()
      const t1 = await a.add("default", { type: "script", schedule: "@every 1h", script: "echo A", name: "A任务" })
      await b.add("default", { type: "script", schedule: "@every 1h", script: "echo B", name: "B任务" })
      expect(readCron(h).map((t) => t.name)).toEqual(["A任务", "B任务"])
      await b.remove("default", t1.id)
      expect(readCron(h).map((t) => t.name)).toEqual(["B任务"])
      a.stop()
      b.stop()
    } finally {
      rmSync(h, { recursive: true, force: true })
    }
  })

  test("落盘为原子写：文件中始终是完整 JSON（无半截写入）", async () => {
    const h = home()
    try {
      const a = cronOf(h)
      await a.start()
      await a.add("default", { type: "script", schedule: "@every 1h", script: "echo x" })
      // 原子性由「临时文件 + rename」保证：读到的永远是完整可解析 JSON
      expect(() => JSON.parse(readFileSync(join(h, "users", "default", "cron.json"), "utf8"))).not.toThrow()
      a.stop()
    } finally {
      rmSync(h, { recursive: true, force: true })
    }
  })
})
