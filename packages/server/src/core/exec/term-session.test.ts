/** 终端会话测试：注入假 spawner（不真起 shell），覆盖哨兵解析/增量游标/缓冲上限/并发上限/
 *  中断重建/空闲回收/关闭幂等/编码边界——这些行为与平台无关，假进程即足以精确驱动。 */
import { describe, expect, test } from "bun:test"
import { readFileSync, rmSync } from "node:fs"
import { dirname } from "node:path"
import { TerminalService, newToken, sentinelCommand, type ShellSpec, type TermSpawner } from "./term-session"

const SHELLS: ShellSpec[] = [
  { id: "bash", name: "Bash", path: "/bin/bash", available: true },
  { id: "cmd", name: "命令提示符", path: "C:/Windows/System32/cmd.exe", available: true },
]

const CREATE = { rootId: "proj:demo", rootAbs: "/repo", cwdAbs: "/repo" }

/** 假 shell 句柄：测试直接驱动输出（onData）与退出（onExit），写入按序记录。 */
interface FakeShell {
  pid: number
  cwd: string
  writes: string[]
  out(text: string): void
  outRaw(bytes: Uint8Array): void
  exit(code: number): void
}

/** 假 spawner + killTree：既能观测启动参数，又能观测进程树终止，且不碰真实进程。 */
function fakeProcesses() {
  const spawned: FakeShell[] = []
  const killed: Array<number | null> = []
  let pid = 1000
  const spawner: TermSpawner = (opts) => {
    const shell: FakeShell = {
      pid: pid++,
      cwd: opts.cwd,
      writes: [],
      out: (text) => opts.onData(new TextEncoder().encode(text)),
      outRaw: (bytes) => opts.onData(bytes),
      exit: (code) => opts.onExit(code),
    }
    spawned.push(shell)
    return { pid: shell.pid, write: (data) => shell.writes.push(data) }
  }
  const killTree = (target: number | null) => killed.push(target)
  return { spawner, spawned, killed, killTree }
}

function service(opts: { maxSessions?: number; idleMs?: number; maxBufferChars?: number; now?: () => number } = {}) {
  const procs = fakeProcesses()
  const svc = new TerminalService({ spawner: procs.spawner, shells: SHELLS, killTree: procs.killTree, ...opts })
  return { svc, ...procs }
}

/** 命令相关写入（剔除 Windows 启动时的编码初始化 chcp）。 */
const commands = (shell: FakeShell) => shell.writes.filter((w) => !w.startsWith("chcp 65001"))

/** 从写入序列里取本次命令的哨兵 TOKEN（最后一条写入即哨兵行）。 */
function tokenOf(shell: FakeShell): string {
  const token = commands(shell).at(-1)?.match(/__GBEND_[0-9a-f]{6}__/)?.[0]
  if (!token) throw new Error(`未找到哨兵 TOKEN: ${JSON.stringify(commands(shell))}`)
  return token
}

/** 捕获抛错（断言状态码/消息用）。 */
function catches(fn: () => unknown): { status?: number; message: string } {
  try {
    fn()
  } catch (err) {
    const e = err as { status?: number; message?: string }
    return { status: e.status, message: e.message ?? String(err) }
  }
  throw new Error("预期抛错但未抛")
}

describe("终端会话：非 ASCII 命令", () => {
  test("cmd：落成脚本文件并改写成 call 调用（管道 stdin 按本地代码页解析，中文会被解成乱码）", () => {
    const { svc, spawned } = service()
    const s = svc.create({ ...CREATE, shell: "cmd", cwdAbs: "/repo" })
    svc.input({ id: s.id, data: "echo 中文输出测试" })
    const launcher = commands(spawned[0]).filter((w) => !w.includes("__GBEND_")).at(-1) ?? ""
    expect(launcher.startsWith("call \"")).toBe(true)
    const file = launcher.trim().replace(/^call "/, "").replace(/"$/, "")
    expect(file).toContain("gebai-term")
    expect(readFileSync(file, "utf8")).toBe("echo 中文输出测试\r\n")
    // 关闭会话时清理脚本目录
    svc.close(s.id)
    expect(() => readFileSync(file, "utf8")).toThrow()
    rmSync(dirname(file), { recursive: true, force: true })
  })

  test("POSIX shell：非 ASCII 命令原样写入（管道字节原样传递，不做代码页解释）", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE) // 默认 bash
    svc.input({ id: s.id, data: "echo 中文" })
    expect(commands(spawned[0])).toContain("echo 中文\n")
  })

  test("纯 ASCII 命令不受影响（不落盘）", () => {
    const { svc, spawned } = service()
    const s = svc.create({ ...CREATE, shell: "cmd" })
    svc.input({ id: s.id, data: "dir /b" })
    expect(commands(spawned[0])).toContain("dir /b\n")
  })
})

describe("终端会话：哨兵与增量读取", () => {
  test("创建会话：启动 shell、写入编码初始化、回契约字段", () => {
    const { svc, spawned } = service()
    const created = svc.create({ ...CREATE, cwdAbs: "/repo/sub" })
    expect(created.id).toMatch(/^t[0-9a-f]{8}$/)
    expect(created.shell).toBe("bash")
    expect(created.shellName).toBe("Bash")
    expect(created.cwd).toBe("sub") // 根内相对路径
    expect(created.root).toBe("proj:demo")
    expect(created.cursor).toBe(0)
    expect(created.output).toBe("")
    expect(typeof created.startedAt).toBe("number")
    expect(spawned).toHaveLength(1)
    expect(spawned[0].cwd).toBe("/repo/sub")
  })

  test("exec 输入：写完命令立即写哨兵行；输出中的哨兵行被剥离并解码为退出事件", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    const res = svc.input({ id: s.id, data: "ls -l" })
    const shell = spawned[0]
    const token = tokenOf(shell)
    expect(commands(shell)).toEqual(["ls -l\n", `${sentinelCommand("bash", token)}\n`])
    expect(res.cursor).toBe(0) // 写入前的位置：命令刚产生的输出不会被当成已消费
    expect(svc.list()[0].busy).toBe(true)

    shell.out("total 0\nfile.txt\n")
    shell.out(`${token}0|/repo\n`)
    const read = svc.read(s.id, res.cursor)
    expect(read.text).toBe("total 0\nfile.txt\n")
    expect(read.exits).toEqual([{ token, code: 0, cwd: "/repo" }])
    expect(read.alive).toBe(true)
    expect(read.cursor).toBe(read.text.length)
    expect(svc.list()[0].busy).toBe(false)

    // 游标推进后再读：无增量、无重复事件
    const again = svc.read(s.id, read.cursor)
    expect(again.text).toBe("")
    expect(again.exits).toEqual([])
    expect(again.cursor).toBe(read.cursor)
  })

  test("哨兵行被拆到两个输出块：不泄漏进文本，事件照常解析", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    const res = svc.input({ id: s.id, data: "pwd" })
    const token = tokenOf(spawned[0])
    spawned[0].out(`${token}0|/re`)
    const mid = svc.read(s.id, res.cursor)
    expect(mid.text).toBe("") // 半截哨兵必须留住，不能当普通输出推给前端
    expect(mid.exits).toEqual([])
    spawned[0].out("po\n")
    const done = svc.read(s.id, mid.cursor)
    expect(done.text).toBe("")
    expect(done.exits).toEqual([{ token, code: 0, cwd: "/repo" }])
  })

  test("上一条命令以不换行收尾时：拼在同一行的哨兵仍能识别并剥离", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    const res = svc.input({ id: s.id, data: "prog" })
    const token = tokenOf(spawned[0])
    spawned[0].out("progress 50%") // 无换行的进度输出
    spawned[0].out(`${token}0|/repo\n`)
    const read = svc.read(s.id, res.cursor)
    expect(read.text).toBe("progress 50%\n")
    expect(read.exits).toEqual([{ token, code: 0, cwd: "/repo" }])
  })

  test("无输出命令：事件位置等于客户端游标，仍投递一次且不重复", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    const res = svc.input({ id: s.id, data: "cd sub" })
    const token = tokenOf(spawned[0])
    spawned[0].out(`${token}0|/repo/sub\n`)
    const first = svc.read(s.id, res.cursor)
    expect(first.text).toBe("")
    expect(first.cursor).toBe(res.cursor)
    expect(first.exits).toEqual([{ token, code: 0, cwd: "/repo/sub" }])
    const second = svc.read(s.id, first.cursor)
    expect(second.exits).toEqual([])
  })

  test("非零退出码与 shell 自报 cwd 一并回报", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    svc.input({ id: s.id, data: "exit 3" })
    const token = tokenOf(spawned[0])
    spawned[0].out(`boom\n${token}3|/repo/deep\n`)
    const read = svc.read(s.id, 0)
    expect(read.text).toBe("boom\n")
    expect(read.exits).toEqual([{ token, code: 3, cwd: "/repo/deep" }])
    expect(svc.list()[0].cwd).toBe("deep")
  })

  test("read 未知会话抛 404（路由据此回 404）", () => {
    const { svc } = service()
    expect(catches(() => svc.read("t-unknown", 0))).toMatchObject({ status: 404 })
  })

  test("CRLF 输出：行尾 \\r 被剥（否则哨兵里的 cwd 会带 \\r，重建 shell 时目录名非法）", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    svc.input({ id: s.id, data: "cd sub" })
    const token = tokenOf(spawned[0])
    spawned[0].out(`ok\r\n${token}0|/repo/sub\r\n`)
    const read = svc.read(s.id, 0)
    expect(read.text).toBe("ok\n")
    expect(read.exits).toEqual([{ token, code: 0, cwd: "/repo/sub" }])
    expect(svc.list()[0].cwd).toBe("sub")
  })

  test("命令回显里的 TOKEN（用户执行 echo on）不误判为结果行", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    svc.input({ id: s.id, data: "echo on" })
    const token = tokenOf(spawned[0])
    // 回显行的载荷是字面量（%errorlevel%^|%CD%）而非 `{code}|{cwd}`：跳过它，等真正的输出行
    spawned[0].out(`C:\\repo>echo ${token}%errorlevel%^|%CD%\r\nC:\\repo>${token}0|/repo\r\n`)
    const read = svc.read(s.id, 0)
    expect(read.text).toBe(`C:\\repo>echo ${token}%errorlevel%^|%CD%\nC:\\repo>\n`)
    expect(read.exits).toEqual([{ token, code: 0, cwd: "/repo" }])
  })
})

describe("终端会话：缓冲、并发与生命周期", () => {
  test("滚动缓冲有界：超限只保留尾部，游标仍是绝对位置", () => {
    const { svc, spawned } = service({ maxBufferChars: 20 })
    const s = svc.create(CREATE)
    spawned[0].out("0123456789\n")
    const first = svc.read(s.id, 0)
    expect(first.text).toBe("0123456789\n")
    expect(first.cursor).toBe(11)
    spawned[0].out("abcdefghij\n")
    const second = svc.read(s.id, first.cursor)
    expect(second.text).toBe("abcdefghij\n")
    expect(second.cursor).toBe(22)
    // 缓冲起点前移：更早的游标只能拿到保留区（前端不会读到穿帮的错位内容）
    expect(svc.read(s.id, 0).text).toBe("23456789\nabcdefghij\n")
    expect(svc.read(s.id, 0).cursor).toBe(22)
  })

  test("并发会话上限：超出抛 400 中文错误", () => {
    const { svc } = service({ maxSessions: 2 })
    svc.create(CREATE)
    svc.create(CREATE)
    const err = catches(() => svc.create(CREATE))
    expect(err.status).toBe(400)
    expect(err.message).toContain("上限")
  })

  test("exec:false 原样写入（交互输入/控制字符，不追加换行与哨兵）", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    const res = svc.input({ id: s.id, data: "y\n\u0003", exec: false })
    expect(commands(spawned[0])).toEqual(["y\n\u0003"])
    expect(res.cursor).toBe(0)
    expect(svc.list()[0].busy).toBe(false)
    svc.input({ id: s.id, data: "sleep 100" })
    expect(svc.list()[0].busy).toBe(true)
  })

  test("interrupt：按进程树终止、以原 cwd 重建 shell、保留会话 id 与滚动缓冲", () => {
    const { svc, spawned, killed } = service()
    const s = svc.create(CREATE)
    const res = svc.input({ id: s.id, data: "cd sub" })
    const token = tokenOf(spawned[0])
    spawned[0].out(`${token}0|/repo/sub\n`) // 哨兵回报新 cwd
    spawned[0].out("partial...") // 半截输出（无换行，已被即时推送）
    const out = svc.interrupt(s.id)
    expect(out).toEqual({ ok: true, cwd: "sub" })
    expect(killed).toEqual([spawned[0].pid])
    expect(svc.list()[0].busy).toBe(false)
    // 重建：同一会话 id、新进程、沿用中断前的 cwd
    expect(spawned).toHaveLength(2)
    expect(spawned[1].cwd).toBe("/repo/sub")
    expect(svc.list()[0].id).toBe(s.id)
    const read = svc.read(s.id, res.cursor)
    expect(read.text).toBe("partial...\n[已中断当前命令]\n")
    expect(read.alive).toBe(true)
    // 重建后的新进程输出照常进入缓冲
    spawned[1].out("fresh\n")
    expect(svc.read(s.id, read.cursor).text).toBe("fresh\n")
  })

  test("shell 退出：alive 置否并提示，缓冲仍可回看", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    spawned[0].out("bye\n")
    spawned[0].exit(0)
    const read = svc.read(s.id, 0)
    expect(read.alive).toBe(false)
    expect(read.text).toBe("bye\n[Shell 已退出（退出码 0）]\n")
    expect(catches(() => svc.input({ id: s.id, data: "ls" })).status).toBe(409)
  })

  test("sweep：空闲超阈值的会话被关闭并终止进程，执行中的会话不回收", () => {
    let now = 0
    const { svc, spawned, killed } = service({ idleMs: 1000, now: () => now })
    const idle = svc.create(CREATE)
    const busy = svc.create(CREATE)
    svc.input({ id: busy.id, data: "sleep 100" })
    now = 1000
    expect(svc.sweep()).toEqual([idle.id])
    expect(killed).toEqual([spawned[0].pid])
    expect(svc.list().map((x) => x.id)).toEqual([busy.id])
    // 空闲阈值内不回收
    now = 1500
    expect(svc.sweep()).toEqual([])
  })

  test("close 幂等：重复关闭与未知会话都返回成功", () => {
    const { svc, spawned, killed } = service()
    const s = svc.create(CREATE)
    expect(svc.close(s.id)).toEqual({ ok: true })
    expect(killed).toEqual([spawned[0].pid])
    expect(svc.close(s.id)).toEqual({ ok: true })
    expect(svc.close("t-unknown")).toEqual({ ok: true })
    expect(svc.list()).toEqual([])
  })

  test("能力元信息：清单/默认 shell/上限", () => {
    const { svc } = service({ maxSessions: 3, idleMs: 500 })
    const info = svc.info()
    expect(info.enabled).toBe(true)
    expect(info.shells.map((x) => x.id)).toEqual(["bash", "cmd"])
    expect(info.defaultShell).toBe("bash")
    expect(info.maxSessions).toBe(3)
    expect(info.idleMs).toBe(500)
    // GEBAI_TERMINAL_SHELL 指定默认 shell（按 id 或路径匹配）
    const custom = new TerminalService({ shells: SHELLS, defaultShell: "cmd.exe", spawner: fakeProcesses().spawner })
    expect(custom.info().defaultShell).toBe("cmd")
  })

  test("指定的 shell 不可用：400 且列出可用项；无可用 shell：503", () => {
    const { svc } = service()
    const err = catches(() => svc.create({ ...CREATE, shell: "pwsh" }))
    expect(err.status).toBe(400)
    expect(err.message).toContain("bash")
    const none = new TerminalService({ spawner: fakeProcesses().spawner, shells: [{ id: "bash", name: "Bash", path: "/bin/bash", available: false }] })
    expect(catches(() => none.create(CREATE)).status).toBe(503)
  })
})

describe("终端会话：输出解码", () => {
  test("UTF-8 多字节序列跨输出块：不留替换字符，行内容按块拼接正确", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    const bytes = new TextEncoder().encode("中文\n")
    spawned[0].outRaw(bytes.slice(0, 4)) // 「中」+ 下一个字的半个首字节
    expect(svc.read(s.id, 0).text).toBe("中")
    spawned[0].outRaw(bytes.slice(4))
    const read = svc.read(s.id, 0)
    expect(read.text).toBe("中文\n")
    expect(read.text).not.toContain("\uFFFD")
  })

  test("GBK 输出（未遵循 chcp 65001）：出现替换字符时按 GBK 重解", () => {
    const { svc, spawned } = service()
    const s = svc.create(CREATE)
    spawned[0].out("中文\n")
    spawned[0].outRaw(new Uint8Array([0xd6, 0xd0, 0xce, 0xc4, 0xca, 0xe4, 0xb3, 0xf6, 0x0a]))
    const read = svc.read(s.id, 0)
    expect(read.text).toBe("中文\n中文输出\n")
    expect(read.text).not.toContain("\uFFFD")
  })
})

describe("哨兵命令与 TOKEN 格式", () => {
  test("各 shell 的哨兵命令与随机 TOKEN", () => {
    const token = "__GBEND_ab12cd__"
    expect(sentinelCommand("cmd", token)).toBe(`echo ${token}%errorlevel%^|%CD%`)
    expect(sentinelCommand("bash", token)).toBe(`echo "${token}$?|$PWD"`)
    expect(sentinelCommand("powershell", token)).toBe(`Write-Output ("${token}" + $(if ($?) {0} else {1}) + "|" + (Get-Location).Path)`)
    expect(newToken()).toMatch(/^__GBEND_[0-9a-f]{6}__$/)
    expect(newToken()).not.toBe(newToken()) // 每次命令一次性随机
  })
})
