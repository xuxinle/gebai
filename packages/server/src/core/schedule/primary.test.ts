import { describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import {
  PRIMARY_LEASE_MS,
  createPrimaryGate,
  defaultIsPidAlive,
  primaryLockFile,
  readPrimaryLock,
  resolveSchedulerMode,
  type PrimaryGate,
  type SchedulerMode,
} from "./primary"

/** 调度器主实例锁（core/schedule/primary.ts）单测：抢占 / 退让 / 续租 / 看门狗接管 / 损坏锁 / 模式门控。
 *  范式同 cron.test.ts：注入 now / isAlive / 定时器与 pid，不依赖真实时钟与真实进程。 */

const T0 = 1_780_000_000_000

interface Harness {
  home: string
  /** 虚拟时钟（测试推进）。 */
  clock: { t: number }
  /** 判定为存活的 PID 集合（默认 100/200 存活）。 */
  alive: Set<number>
  logs: string[]
  /** 看门狗注册的 tick 回调（setInterval 替身捕获）。 */
  ticks: Array<() => void>
  /** clearInterval 调用次数。 */
  cleared: { n: number }
}

function setup(): Harness {
  return { home: mkdtempSync(join(tmpdir(), "gebai-primary-")), clock: { t: T0 }, alive: new Set([100, 200]), logs: [], ticks: [], cleared: { n: 0 } }
}

function gateOf(h: Harness, opts: { pid: number; port?: number; mode?: SchedulerMode; leaseMs?: number }): PrimaryGate {
  return createPrimaryGate({
    home: h.home,
    port: opts.port ?? 3000,
    pid: opts.pid,
    mode: opts.mode ?? "auto",
    now: () => h.clock.t,
    isAlive: (pid) => h.alive.has(pid),
    log: (m) => h.logs.push(m),
    setInterval: (fn) => {
      h.ticks.push(fn)
      return {} as unknown as ReturnType<typeof setInterval>
    },
    clearInterval: () => {
      h.cleared.n++
    },
    ...(opts.leaseMs !== undefined ? { leaseMs: opts.leaseMs } : {}),
  })
}

/** 外部实例直接改写锁文件（模拟并发抢占/手工编辑）。 */
function writeLock(h: Harness, body: unknown): void {
  writeFileSync(primaryLockFile(h.home), typeof body === "string" ? body : JSON.stringify(body), "utf8")
}

describe("调度器主实例锁", () => {
  test("空目录：首实例抢占成功并落锁（pid/port/at）", async () => {
    const h = setup()
    try {
      const g = gateOf(h, { pid: 100, port: 4321 })
      expect(await g.acquire()).toBe(true)
      expect(g.isPrimary()).toBe(true)
      expect(await readPrimaryLock(h.home)).toEqual({ pid: 100, port: 4321, at: T0 })
      expect(h.logs.join("\n")).toContain("已获得主实例锁（PID 100）")
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("第二实例：持有者存活且租约未过期 → 从实例退让且不改写锁", async () => {
    const h = setup()
    try {
      expect(await gateOf(h, { pid: 100 }).acquire()).toBe(true)
      h.clock.t = T0 + 60_000
      const b = gateOf(h, { pid: 200, port: 3001 })
      expect(await b.acquire()).toBe(false)
      expect(b.isPrimary()).toBe(false)
      expect((await readPrimaryLock(h.home))?.pid).toBe(100)
      expect(h.logs.join("\n")).toContain("未获得主实例锁（PID 100 持有）")
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("租约边界：恰好到期仍退让，超过租约即接管", async () => {
    const h = setup()
    try {
      expect(await gateOf(h, { pid: 100 }).acquire()).toBe(true)
      h.clock.t = T0 + PRIMARY_LEASE_MS
      expect(await gateOf(h, { pid: 200 }).acquire()).toBe(false)
      h.clock.t = T0 + PRIMARY_LEASE_MS + 1
      expect(await gateOf(h, { pid: 200 }).acquire()).toBe(true)
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("持有者 PID 已死 → 接管（租约未过期也立即生效）", async () => {
    const h = setup()
    try {
      expect(await gateOf(h, { pid: 100 }).acquire()).toBe(true)
      h.alive.delete(100)
      const b = gateOf(h, { pid: 200, port: 3001 })
      expect(await b.acquire()).toBe(true)
      expect(await readPrimaryLock(h.home)).toEqual({ pid: 200, port: 3001, at: T0 })
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("同一 PID 再次判定 = 续租（at 推进，仍是主实例）", async () => {
    const h = setup()
    try {
      const g = gateOf(h, { pid: 100 })
      expect(await g.acquire()).toBe(true)
      h.clock.t = T0 + 30_000
      expect(await g.acquire()).toBe(true)
      expect((await readPrimaryLock(h.home))?.at).toBe(T0 + 30_000)
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("锁文件损坏/字段非法 → 视为可抢占（不阻断启动）", async () => {
    const h = setup()
    try {
      writeLock(h, "{ 半截 json")
      expect(await gateOf(h, { pid: 100 }).acquire()).toBe(true)
      expect((await readPrimaryLock(h.home))?.pid).toBe(100)
      writeLock(h, { pid: "x", at: "y" })
      h.clock.t = T0 + 1000
      expect(await gateOf(h, { pid: 200 }).acquire()).toBe(true)
      expect((await readPrimaryLock(h.home))?.pid).toBe(200)
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("看门狗：主实例每轮续租（锁文件被外部删除时重建，不误退让）", async () => {
    const h = setup()
    try {
      const a = gateOf(h, { pid: 100 })
      expect(await a.acquire()).toBe(true)
      a.start(() => {})
      expect(h.ticks).toHaveLength(1) // auto 模式挂看门狗
      h.clock.t = T0 + 30_000
      expect(await a.checkNow()).toBe(true)
      expect((await readPrimaryLock(h.home))?.at).toBe(T0 + 30_000)
      rmSync(primaryLockFile(h.home), { force: true })
      expect(await a.checkNow()).toBe(true)
      expect(existsSync(primaryLockFile(h.home))).toBe(true)
      a.stop()
      expect(h.cleared.n).toBe(1)
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("看门狗：从实例在主实例死后自动接管并回调启动调度（只回调一次）", async () => {
    const h = setup()
    try {
      expect(await gateOf(h, { pid: 100 }).acquire()).toBe(true)
      const b = gateOf(h, { pid: 200, port: 3001 })
      expect(await b.acquire()).toBe(false)
      let became = 0
      b.start(() => {
        became++
      })
      expect(await b.checkNow()).toBe(false)
      expect(became).toBe(0)
      h.alive.delete(100)
      h.clock.t = T0 + 60_000
      expect(await b.checkNow()).toBe(true)
      expect(became).toBe(1)
      expect(await readPrimaryLock(h.home)).toEqual({ pid: 200, port: 3001, at: T0 + 60_000 })
      expect(h.logs.join("\n")).toContain("已接管主实例调度")
      // 已是主实例：后续轮次只续租，不重复回调
      h.clock.t += 30_000
      expect(await b.checkNow()).toBe(true)
      expect(became).toBe(1)
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("看门狗：主实例被他人接管（错过续租）→ 停调度退让并回调", async () => {
    const h = setup()
    try {
      const a = gateOf(h, { pid: 100 })
      expect(await a.acquire()).toBe(true)
      let lost = 0
      a.start(
        () => {},
        () => {
          lost++
        },
      )
      writeLock(h, { pid: 200, port: 3002, at: T0 + 1000 })
      expect(await a.checkNow()).toBe(false)
      expect(a.isPrimary()).toBe(false)
      expect(lost).toBe(1)
      expect(h.logs.join("\n")).toContain("主实例锁已被 PID 200 接管")
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("模式门控：off 不判定不落锁不挂看门狗；on 强制主实例但忽略锁（不落锁）", async () => {
    const h = setup()
    try {
      const off = gateOf(h, { pid: 100, mode: "off" })
      expect(await off.acquire()).toBe(false)
      expect(off.isPrimary()).toBe(false)
      off.start(() => {})
      expect(h.ticks).toHaveLength(0)
      expect(existsSync(primaryLockFile(h.home))).toBe(false)
      expect(h.logs.join("\n")).toContain("GEBAI_SCHEDULER=off")

      // off 下不参与锁协商：他人持有的锁不受影响
      writeLock(h, { pid: 200, port: 3002, at: T0 })
      const on = gateOf(h, { pid: 100, mode: "on" })
      expect(await on.acquire()).toBe(true)
      on.start(() => {})
      expect(h.ticks).toHaveLength(0)
      expect((await readPrimaryLock(h.home))?.pid).toBe(200)
      expect(h.logs.join("\n")).toContain("GEBAI_SCHEDULER=on")
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("release：只删自己的锁（他人锁不动）", async () => {
    const h = setup()
    try {
      const a = gateOf(h, { pid: 100 })
      expect(await a.acquire()).toBe(true)
      await gateOf(h, { pid: 200 }).release()
      expect(existsSync(primaryLockFile(h.home))).toBe(true)
      await a.release()
      expect(existsSync(primaryLockFile(h.home))).toBe(false)
      expect(a.isPrimary()).toBe(false)
    } finally {
      rmSync(h.home, { recursive: true, force: true })
    }
  })

  test("resolveSchedulerMode：on/off 显式生效，缺省与非法值回落 auto", () => {
    expect(resolveSchedulerMode({})).toBe("auto")
    expect(resolveSchedulerMode({ GEBAI_SCHEDULER: "auto" })).toBe("auto")
    expect(resolveSchedulerMode({ GEBAI_SCHEDULER: "ON" })).toBe("on")
    expect(resolveSchedulerMode({ GEBAI_SCHEDULER: " off " })).toBe("off")
    expect(resolveSchedulerMode({ GEBAI_SCHEDULER: "offf" })).toBe("auto")
  })

  test("defaultIsPidAlive：自身 PID 存活、非法 PID 与已退出进程判死（Windows 信号 0 语义）", async () => {
    expect(defaultIsPidAlive(process.pid)).toBe(true)
    expect(defaultIsPidAlive(0)).toBe(false)
    expect(defaultIsPidAlive(-1)).toBe(false)
    const proc = Bun.spawn([process.execPath, "-e", "0"], { stdout: "ignore", stderr: "ignore" })
    const dead = proc.pid
    await proc.exited
    expect(defaultIsPidAlive(dead)).toBe(false)
  })
})
