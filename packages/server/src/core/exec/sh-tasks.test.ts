import { describe, expect, test } from "bun:test"
import { closeSync, mkdtempSync, mkdirSync, openSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { ShTaskRunner, shTaskLifetimeMs, shTaskStatus, type ShTaskProcess, type ShTaskSpawner } from "./sh-tasks"
import { Sandbox } from "../security/sandbox"

/** 假进程生成器：pid 用当前进程（恒存活），退出由测试显式触发；日志直接写目标文件模拟输出。 */
function fakeSpawner() {
  const calls: Array<{ cmd: string; cwd?: string; logPath: string; input?: string }> = []
  const handles: Array<{ pid: number | null; killed: boolean; exit: (code: number) => void; emit: (stream: "stdout" | "stderr", text: string) => void }> = []
  const spawner: ShTaskSpawner = (cmd, opts) => {
    calls.push({ cmd, cwd: opts.cwd, logPath: opts.logPath, input: opts.input })
    mkdirSync(join(opts.logPath, ".."), { recursive: true })
    writeFileSync(opts.logPath, `[fake-start] ${cmd}\n`)
    const h = {
      pid: process.pid,
      killed: false,
      exit: (_code: number) => {},
      /** 模拟进程输出：写日志，并按 onChunk 旁路分发（前台捕获路径据此拿到分离的 stdout/stderr）。 */
      emit: (stream: "stdout" | "stderr", text: string) => {
        writeFileSync(opts.logPath, text, { flag: "a" })
        opts.onChunk?.(stream, Buffer.from(text))
      },
    }
    const proc: ShTaskProcess = {
      pid: h.pid,
      exited: new Promise<number>((resolve) => {
        h.exit = (code) => {
          if (!h.killed) resolve(code)
        }
      }),
      kill: () => {
        h.killed = true
      },
    }
    handles.push(h)
    return proc
  }
  return { spawner, calls, handles }
}

function runner(dir: string, spawner: ShTaskSpawner, now?: () => number): ShTaskRunner {
  return new ShTaskRunner({ dir, spawner, now })
}

describe("sh async background tasks", () => {
  test("start returns record immediately and persists to disk; exited callback writes exit code", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    const rec = await r.start("bun run build", { cwd: "/repo", maxMs: 60000 })
    expect(rec.command).toBe("bun run build")
    expect(rec.id).toMatch(/^t[0-9a-f]{8}$/)
    expect(rec.endedAt).toBeUndefined()
    // 立即落盘（跨工具调用可见）
    const listed = await r.list()
    expect(listed).toHaveLength(1)
    expect(shTaskStatus(listed[0])).toBe("running")
    // 进程退出回调回写退出码
    f.handles[0].exit(0)
    await new Promise((res) => setTimeout(res, 20))
    const done = await r.refresh(rec.id)
    expect(shTaskStatus(done!)).toBe("done")
    expect(done!.exitCode).toBe(0)
    expect(done!.endedAt).toBeDefined()
    rmSync(home, { recursive: true, force: true })
  })

  test("wait blocks until exit or timeout; timeout-still-running returns running record", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-wait-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    const rec = await r.start("sleep 100", {})
    // 等待超时（仍在运行）：返回 running 记录
    const still = await r.wait(rec.id, 50)
    expect(shTaskStatus(still!)).toBe("running")
    // 退出后 wait 返回终态
    setTimeout(() => f.handles[0].exit(2), 30)
    const done = await r.wait(rec.id, 5000)
    expect(shTaskStatus(done!)).toBe("failed")
    expect(done!.exitCode).toBe(2)
    rmSync(home, { recursive: true, force: true })
  })

  test("lifetime cap kills process and marks timedOut on lazy refresh", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-timeout-"))
    const f = fakeSpawner()
    let t = 1000
    const r = new ShTaskRunner({ dir: home, spawner: f.spawner, now: () => t })
    const rec = await r.start("long-job", { maxMs: 1000 })
    t = 3500 // 超过生命周期上限
    const out = await r.refresh(rec.id)
    expect(shTaskStatus(out!)).toBe("timed_out")
    expect(f.handles[0].killed).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })

  test("kill terminates handle and marks killed; finished task is returned as-is", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-kill-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    const rec = await r.start("watch", {})
    const killed = await r.kill(rec.id)
    expect(shTaskStatus(killed!)).toBe("killed")
    expect(f.handles[0].killed).toBe(true)
    // 已结束的任务 kill 原样返回
    const again = await r.kill(rec.id)
    expect(shTaskStatus(again!)).toBe("killed")
    rmSync(home, { recursive: true, force: true })
  })

  test("dead pid without exit record is marked lost (server restart)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-lost-"))
    const f = fakeSpawner()
    // 用一个立即退出的真实子进程拿一个确定已死的 pid
    const dead = Bun.spawnSync({ cmd: [process.execPath, "-e", ""] })
    const r = runner(home, f.spawner)
    const rec = await r.start("job", {})
    // 手工改写记录模拟「服务重启后句柄丢失、进程已死」：pid 换成死 pid 且不带 endedAt
    const { writeFile } = await import("node:fs/promises")
    await writeFile(join(home, "tasks.json"), JSON.stringify([{ ...rec, pid: dead.pid }]), "utf8")
    const out = await r.refresh(rec.id)
    expect(shTaskStatus(out!)).toBe("lost")
    expect(out!.endedAt).toBeDefined()
    rmSync(home, { recursive: true, force: true })
  })

  test("concurrent cap rejects new starts at limit", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-cap-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    for (let i = 0; i < 8; i++) await r.start(`job${i}`, {})
    await expect(r.start("job9", {})).rejects.toThrow(/并发后台任务超限/)
    // 结束一个后可再启动
    f.handles[0].exit(0)
    await new Promise((res) => setTimeout(res, 20))
    const rec = await r.start("job9", {})
    expect(rec.id).toBeDefined()
    rmSync(home, { recursive: true, force: true })
  })

  test("并行 start 不丢记录（状态读改写串行化）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-parallel-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    // 同批并行启动（同批工具调用并发执行的真实形态）：load→save 无锁会让后写覆盖先写
    const recs = await Promise.all(Array.from({ length: 6 }, (_, i) => r.start(`job${i}`, {})))
    const ids = (await r.list()).map((x) => x.id)
    expect(new Set(ids).size).toBe(6)
    for (const rec of recs) expect(ids).toContain(rec.id)
    rmSync(home, { recursive: true, force: true })
  })

  test("readLog returns tail; lifetime param parsing defaults 1800s caps 3600s", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-log-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    const rec = await r.start("gen", {})
    const tail = await r.readLog(rec.id, 100)
    expect(tail).toContain("[fake-start] gen")
    expect(shTaskLifetimeMs(undefined)).toBe(1800_000)
    expect(shTaskLifetimeMs(0)).toBe(1800_000)
    expect(shTaskLifetimeMs("abc")).toBe(1800_000)
    expect(shTaskLifetimeMs(120)).toBe(120_000)
    expect(shTaskLifetimeMs(99999)).toBe(3600_000)
    rmSync(home, { recursive: true, force: true })
  })

  // 真实 spawn 子进程：分片并行时机器满载，bun 默认 5s 用例超时不够（wait 自身 15s）
  test("真实链路：Sandbox.spawnBackground 启动 echo 命令，日志落盘、退出码回写", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-real-"))
    const sandbox = new Sandbox({ home, enabled: false })
    const r = new ShTaskRunner({ dir: join(home, "tasks"), spawner: (cmd, o) => sandbox.spawnBackground(cmd, o) })
    const rec = await r.start("echo gebai-async-ok", {})
    expect(rec.pid).toBeGreaterThan(0)
    const done = await r.wait(rec.id, 15000)
    expect(shTaskStatus(done!)).toBe("done")
    expect(done!.exitCode).toBe(0)
    const log = await r.readLog(rec.id, 2000)
    expect(log).toContain("gebai-async-ok")
          rmSync(home, { recursive: true, force: true })
  }, 20000)

  // ── 稳定性回归：状态落盘绝不能把服务打崩（历史事故：EPERM rename → unhandled rejection）──
  //
  // 事故链：调用点裸 `await this.save(...)`，而 finish 由 `proc.exited.then(...)` 驱动，
  // 一旦 rename 报 EPERM（Windows 上无法覆盖被打开的文件）就变成未处理的 Promise 拒绝，Bun 直接终止进程。
  // 触发现场：一边高频读 tasks.json（如 bg_task status 轮询），一边后台任务写入重命名。

  test("tasks.json 被外部句柄占用时：落盘不抛错，且状态不丢", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-lock-"))
    const { spawner } = fakeSpawner()
    const dir = join(home, "tasks")
    const r = runner(dir, spawner)
    await r.start("echo one", {})
    const recordsPath = join(dir, "tasks.json")

    // 持一个读句柄不放（模拟“一边读一边写”的真实并发）：Windows 下此时 rename 会报 EPERM
    const fd = openSync(recordsPath, "r")
    try {
      // 不抛错是硬要求（抛错即 unhandled rejection → 进程终止）
      await r.start("echo two", {})
      await expect(r.list()).resolves.toBeDefined()
    } finally {
      closeSync(fd)
    }

    // 释放句柄后，记录必须完整（退避重试或降级直接写，都不能把状态弄丢）
    await r.start("echo three", {})
    const onDisk = JSON.parse(readFileSync(recordsPath, "utf8")) as Array<{ command: string }>
    const cmds = onDisk.map((x) => x.command)
    expect(cmds).toContain("echo one")
    expect(cmds).toContain("echo three")
    rmSync(home, { recursive: true, force: true })
  })

  test("落盘目标不可写（tasks.json 是目录）：save 只告警不抛出，任务仍可启动与查询", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-badpath-"))
    const { spawner } = fakeSpawner()
    const dir = join(home, "tasks")
    const r = runner(dir, spawner)
    // 用目录占住 recordsPath：rename 与“直接写”都会失败 → 确定性走最终兜底分支
    mkdirSync(join(dir, "tasks.json"), { recursive: true })

    const warnings: string[] = []
    const orig = console.warn
    console.warn = (...args: unknown[]) => void warnings.push(args.map(String).join(" "))
    try {
      // 关键断言：不抛错（落盘只是副作用，不该有进程级杀伤力）
      const rec = await r.start("echo resilient", {})
      expect(rec.id).toBeTruthy()
      expect(spawner).toBeDefined()
      await expect(r.list()).resolves.toBeDefined()
      expect(warnings.some((w) => w.includes("[sh-tasks]"))).toBe(true)
    } finally {
      console.warn = orig
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("sh 前台运行（run：同步等待窗口 + 超窗转后台）", () => {
  test("窗口内结束：返回捕获的 stdout/stderr 与退出码，记录与日志不留痕", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-run-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    setTimeout(() => {
      f.handles[0].emit("stdout", "hello\n")
      f.handles[0].emit("stderr", "warn\n")
      f.handles[0].exit(3)
    }, 40)
    const out = await r.run("echo hi", { waitMs: 5000 })
    if (!out.started) throw new Error("应已启动")
    expect(out.finished).toBe(true)
    expect(out.aborted).toBe(false)
    expect(out.stdout).toBe("hello\n")
    expect(out.stderr).toBe("warn\n")
    expect(out.record.exitCode).toBe(3)
    // 不留痕：同步调用不污染后台清单与 tasks.json
    expect(await r.list()).toHaveLength(0)
    rmSync(home, { recursive: true, force: true })
  })

  test("窗口到期仍在运行：不终止任务、返回 running 记录（转后台）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-run-out-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    const out = await r.run("sleep 100", { waitMs: 80 })
    if (!out.started) throw new Error("应已启动")
    expect(out.finished).toBe(false)
    expect(shTaskStatus(out.record)).toBe("running")
    expect(f.handles[0].killed).toBe(false)
    expect(await r.list()).toHaveLength(1)
    await r.kill(out.record.id)
    rmSync(home, { recursive: true, force: true })
  })

  test("等待期间 signal 中止：按进程树终止并标记 aborted", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-run-abort-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    const ac = new AbortController()
    const p = r.run("sleep 100", { waitMs: 10000, signal: ac.signal })
    setTimeout(() => ac.abort(), 30)
    const out = await p
    if (!out.started) throw new Error("应已启动")
    expect(out.aborted).toBe(true)
    expect(f.handles[0].killed).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })

  test("后台启动失败（并发超限）：返回 started=false 由调用方回退", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-run-cap-"))
    const f = fakeSpawner()
    const r = runner(home, f.spawner)
    for (let i = 0; i < 8; i++) await r.start(`job${i}`, {})
    const out = await r.run("echo x", { waitMs: 100 })
    expect(out.started).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })

  // 真实 spawn 子进程（同「真实链路」用例：分片并行时机器满载，放宽超时）
  test("真实链路：run 捕获 echo 输出且不留痕；慢命令转后台保留记录、不终止进程", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-shtask-run-real-"))
    const sandbox = new Sandbox({ home, enabled: false })
    const r = new ShTaskRunner({ dir: join(home, "tasks"), spawner: (cmd, o) => sandbox.spawnBackground(cmd, o) })
    const ok = await r.run("echo gebai-sync-ok", { waitMs: 15000 })
    if (!ok.started) throw new Error("应已启动")
    expect(ok.finished).toBe(true)
    expect(ok.stdout).toContain("gebai-sync-ok")
    expect(ok.record.exitCode).toBe(0)
    expect(await r.list()).toHaveLength(0)
    const slow = await r.run(process.platform === "win32" ? "Start-Sleep -Seconds 5" : "sleep 5", { waitMs: 300 })
    if (!slow.started) throw new Error("应已启动")
    expect(slow.finished).toBe(false)
    expect(shTaskStatus(slow.record)).toBe("running")
    expect((await r.list()).map((x) => x.id)).toContain(slow.record.id)
    await r.kill(slow.record.id)
    expect(shTaskStatus((await r.refresh(slow.record.id))!)).toBe("killed")
    rmSync(home, { recursive: true, force: true })
  }, 30000)
})
