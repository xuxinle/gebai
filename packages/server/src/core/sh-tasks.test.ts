import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { ShTaskRunner, shTaskLifetimeMs, shTaskStatus, type ShTaskProcess, type ShTaskSpawner } from "./sh-tasks"
import { Sandbox } from "./sandbox"

/** 假进程生成器：pid 用当前进程（恒存活），退出由测试显式触发；日志直接写目标文件模拟输出。 */
function fakeSpawner() {
  const calls: Array<{ cmd: string; cwd?: string; logPath: string; input?: string }> = []
  const handles: Array<{ pid: number | null; killed: boolean; exit: (code: number) => void }> = []
  const spawner: ShTaskSpawner = (cmd, opts) => {
    calls.push({ cmd, cwd: opts.cwd, logPath: opts.logPath, input: opts.input })
    mkdirSync(join(opts.logPath, ".."), { recursive: true })
    writeFileSync(opts.logPath, `[fake-start] ${cmd}\n`)
    const h = { pid: process.pid, killed: false, exit: (_code: number) => {} }
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
  })
})
