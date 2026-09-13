/** PTY 会话服务单测（假驱动：不真起 ConPTY，只验证协议、缓冲、订阅与生命周期）。 */
import { describe, expect, test } from "bun:test"
import { PtySessionService, shellCommandLine, type PtyDriverEvent, type PtySpawner } from "./pty-session"
import type { ShellSpec } from "./term-session"

const SHELL: ShellSpec = { id: "cmd", name: "命令提示符", path: "C:\\Windows\\System32\\cmd.exe", available: true }

/** 假驱动：记录写入的协议行，并可手动触发驱动事件；每次 spawn 记一代（interrupt 会重建）。 */
function fakeDriver() {
  const writes: string[] = []
  const killed: number[] = []
  let spawnCount = 0
  let emit: (evt: PtyDriverEvent) => void = () => {}
  let exitCb: (code: number | null) => void = () => {}
  const spawner: PtySpawner = (opts) => {
    spawnCount++
    emit = opts.onEvent
    exitCb = opts.onExit
    return {
      pid: 4321 + spawnCount,
      write: (d: string) => writes.push(d),
      kill: () => killed.push(4321 + spawnCount),
    }
  }
  return {
    spawner,
    writes,
    killed,
    lines: () => writes.map((w) => JSON.parse(w.trim()) as Record<string, unknown>),
    event: (evt: PtyDriverEvent) => emit(evt),
    exited: (code: number | null) => exitCb(code),
    /** 当前 onExit 回调的引用（用于模拟「旧驱动退出回调晚到」）。 */
    exitedRef: () => exitCb,
    get spawns() {
      return spawnCount
    },
  }
}

function service(over: { maxSessions?: number; idleMs?: number; maxBufferChars?: number; now?: () => number } = {}) {
  const drv = fakeDriver()
  const svc = new PtySessionService({
    spawner: drv.spawner,
    launch: () => ({ ok: true, cmd: ["driver.exe"] }),
    killTree: (pid) => {
      if (pid != null) drv.killed.push(pid)
    },
    ...over,
  })
  return { svc, drv }
}

function create(svc: PtySessionService, cols = 100, rows = 30) {
  return svc.create({ rootId: "proj:x", rootAbs: "/repo", cwdAbs: "/repo", shell: SHELL, cols, rows })
}

describe("PtySessionService · 创建", () => {
  test("open 行携带 shell 命令行 / cwd / 尺寸，且会话视图给出根内相对 cwd", () => {
    const { svc, drv } = service()
    const info = create(svc, 120, 40)
    expect(info.alive).toBe(false) // ready 之前不算活
    expect(info.cols).toBe(120)
    expect(info.rows).toBe(40)
    expect(info.cwd).toBe("")
    const open = drv.lines()[0]!
    expect(open.t).toBe("open")
    expect(open.shell).toBe('"C:\\Windows\\System32\\cmd.exe"')
    expect(open.cwd).toBe("/repo")
    expect(open.cols).toBe(120)
    expect(open.rows).toBe(40)
    expect(svc.has(info.id)).toBe(true)
  })

  test("驱动不可用：create 抛错，available 给出原因", () => {
    const svc = new PtySessionService({ launch: () => ({ ok: false, reason: "未找到 csc.exe（降级为管道式终端）" }) })
    expect(svc.available()).toEqual({ ok: false, reason: "未找到 csc.exe（降级为管道式终端）" })
    expect(() => create(svc)).toThrow(/降级/)
  })

  test("并发上限：超出直接拒绝", () => {
    const { svc } = service({ maxSessions: 1 })
    create(svc)
    expect(() => create(svc)).toThrow(/上限/)
  })

  test("shell 命令行：PowerShell 关横幅、路径带引号", () => {
    expect(shellCommandLine({ id: "pwsh", name: "PowerShell", path: "C:\\Program Files\\PowerShell\\7\\pwsh.exe", available: true })).toBe(
      '"C:\\Program Files\\PowerShell\\7\\pwsh.exe" -NoLogo',
    )
    expect(shellCommandLine({ id: "bash", name: "Bash", path: "/bin/bash", available: true })).toBe('"/bin/bash"')
  })
})

describe("PtySessionService · 输出与订阅", () => {
  test("out 事件按 UTF-8 增量解码入缓冲并推送；跨块多字节字符不裂", () => {
    const { svc, drv } = service()
    const info = create(svc)
    const got: string[] = []
    svc.subscribe(info.id, (evt) => {
      if (evt.type === "out") got.push(evt.data)
    })
    drv.event({ t: "ready", pid: 4321 })
    const bytes = Buffer.from("中文输出\n", "utf8")
    drv.event({ t: "out", d: bytes.subarray(0, 2).toString("base64") })
    drv.event({ t: "out", d: bytes.subarray(2).toString("base64") })
    expect(got.join("")).toBe("中文输出\n")
    expect(svc.list()[0]!.alive).toBe(true)
  })

  test("订阅时回放已有缓冲（重连场景），并补推 ready", () => {
    const { svc, drv } = service()
    const info = create(svc)
    drv.event({ t: "ready", pid: 4321 })
    drv.event({ t: "out", d: Buffer.from("banner\r\n").toString("base64") })
    const seen: string[] = []
    svc.subscribe(info.id, (evt) => seen.push(evt.type))
    expect(seen).toEqual(["out", "ready"])
  })

  test("detach(key) 后不再推送（连接断开语义）", () => {
    const { svc, drv } = service()
    const info = create(svc)
    drv.event({ t: "ready", pid: 1 })
    const key = {}
    let count = 0
    svc.subscribe(info.id, () => count++, key)
    expect(count).toBe(1) // 回放 ready
    svc.detach(key)
    drv.event({ t: "out", d: Buffer.from("x").toString("base64") })
    expect(count).toBe(1)
  })

  test("缓冲上限：超出丢最旧，回放只给尾部", () => {
    const { svc, drv } = service({ maxBufferChars: 8 })
    const info = create(svc)
    drv.event({ t: "ready", pid: 1 })
    drv.event({ t: "out", d: Buffer.from("0123456789").toString("base64") })
    let replay = ""
    svc.subscribe(info.id, (evt) => {
      if (evt.type === "out") replay += evt.data
    })
    expect(replay.length).toBeLessThanOrEqual(8)
    expect(replay).toBe("23456789")
  })
})

describe("PtySessionService · 输入与尺寸", () => {
  test("write 以 base64 原样透传（含控制字符）", () => {
    const { svc, drv } = service()
    const info = create(svc)
    drv.event({ t: "ready", pid: 1 })
    svc.write(info.id, "\u0003")
    svc.write(info.id, "dir\r\n")
    const input = drv.lines().filter((l) => l.t === "in")
    expect(Buffer.from(String(input[0]!.d), "base64").toString("utf8")).toBe("\u0003")
    expect(Buffer.from(String(input[1]!.d), "base64").toString("utf8")).toBe("dir\r\n")
  })

  test("resize：同尺寸不重复下发，变化时更新视图并下发", () => {
    const { svc, drv } = service()
    const info = create(svc, 100, 30)
    drv.event({ t: "ready", pid: 1 })
    svc.resize(info.id, 100, 30)
    expect(drv.lines().filter((l) => l.t === "resize")).toHaveLength(0)
    svc.resize(info.id, 80, 24)
    const resize = drv.lines().find((l) => l.t === "resize")!
    expect([resize.cols, resize.rows]).toEqual([80, 24])
    expect(svc.list()[0]!.cols).toBe(80)
  })

  test("会话结束后写入抛 409", () => {
    const { svc, drv } = service()
    const info = create(svc)
    drv.event({ t: "ready", pid: 1 })
    drv.event({ t: "exit", code: 0 })
    expect(() => svc.write(info.id, "x")).toThrow(/已结束/)
  })
})

describe("PtySessionService · 中断（Ctrl+C）", () => {
  test("interrupt：终止当前进程树、以原 cwd 与尺寸重建、会话 id 与订阅保留", () => {
    const { svc, drv } = service()
    const info = create(svc, 90, 25)
    drv.event({ t: "ready", pid: 4321 })
    const seen: string[] = []
    svc.subscribe(info.id, (evt) => seen.push(evt.type))
    seen.length = 0
    const res = svc.interrupt(info.id)
    expect(res.ok).toBe(true)
    expect(drv.killed).toEqual([4322]) // 上一代驱动 pid
    expect(drv.spawns).toBe(2)
    const open = drv.lines().filter((l) => l.t === "open").at(-1)!
    expect([open.cwd, open.cols, open.rows]).toEqual(["/repo", 90, 25])
    expect(seen).toContain("out") // 中断提示行即时推送
    expect(svc.has(info.id)).toBe(true)
    expect(svc.list()[0]!.cols).toBe(90)
  })

  test("interrupt 后新进程 ready 前写入不报错（alive 由驱动 ready 重置）", () => {
    const { svc, drv } = service()
    const info = create(svc)
    drv.event({ t: "ready", pid: 1 })
    svc.interrupt(info.id)
    expect(() => svc.write(info.id, "echo x\r")).toThrow(/已结束/)
    drv.event({ t: "ready", pid: 2 })
    svc.write(info.id, "echo x\r")
    expect(svc.list()[0]!.alive).toBe(true)
  })

  test("重建后旧驱动的退出回调不把新会话标死（世代号）", () => {
    const { svc, drv } = service()
    const info = create(svc)
    drv.event({ t: "ready", pid: 1 })
    const staleExit = drv.exitedRef()
    svc.interrupt(info.id)
    drv.event({ t: "ready", pid: 2 })
    // 旧驱动的退出回调（interrupt 之前捕获的那个）晚到：不该影响会话
    staleExit(1)
    expect(svc.list()[0]!.alive).toBe(true)
  })

  test("已结束的会话再 interrupt：抛 409", () => {
    const { svc, drv } = service()
    const info = create(svc)
    drv.event({ t: "ready", pid: 1 })
    drv.event({ t: "exit", code: 0 })
    expect(() => svc.interrupt(info.id)).toThrow(/已结束/)
  })
})

describe("PtySessionService · 收尾与回收", () => {
  test("exit 事件：推送 exit + 结束提示，notify 一次", () => {
    const { svc, drv } = service()
    const info = create(svc)
    drv.event({ t: "ready", pid: 1 })
    const events: string[] = []
    svc.subscribe(info.id, (evt) => events.push(evt.type))
    events.length = 0
    drv.event({ t: "exit", code: 3 })
    expect(events).toEqual(["exit", "out"])
    let replay = ""
    svc.subscribe(info.id, (evt) => {
      if (evt.type === "out") replay += evt.data
    })
    expect(replay).toContain("退出码 3")
    expect(svc.list()[0]!.alive).toBe(false)
  })

  test("驱动进程异常退出（未走 exit 事件）也能收尾", () => {
    const { svc, drv } = service()
    create(svc)
    drv.event({ t: "ready", pid: 1 })
    drv.exited(null)
    expect(svc.list()[0]!.alive).toBe(false)
  })

  test("close：幂等、下发 close 行并终止进程树", () => {
    const { svc, drv } = service()
    const info = create(svc)
    svc.close(info.id)
    svc.close(info.id)
    expect(drv.lines().some((l) => l.t === "close")).toBe(true)
    expect(drv.killed).toEqual([4322])
    expect(svc.has(info.id)).toBe(false)
  })

  test("空闲回收：超过 idleMs 自动关闭", () => {
    let now = 1000
    const { svc } = service({ idleMs: 100, now: () => now })
    const info = create(svc)
    now += 50
    svc.sweep()
    expect(svc.has(info.id)).toBe(true)
    now += 200
    svc.sweep()
    expect(svc.has(info.id)).toBe(false)
  })
})
