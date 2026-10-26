import { describe, expect, test } from "bun:test"
import { spawn } from "node:child_process"
import { createWriteStream, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import { Readable } from "node:stream"
import { _resetPosixShellCache, _resetWinShellCache, bwrapArgs, decodeOutput, pipeToSharedLog, posixShellPlan, resolvePosixShell, resolveWinShell, Sandbox, scriptEnvDirs, scriptEnvOverride, winShellPlan, wrapPowerShellCommand } from "./sandbox"
import { sessionPath } from "../base/paths"
import { which } from "../exec/which"

describe("decodeOutput", () => {
  test("utf-8 bytes decode as-is", () => {
    expect(decodeOutput(Buffer.from("hello 世界", "utf8"))).toBe("hello 世界")
  })

  test("gbk bytes fall back to gbk decoding when utf-8 yields replacement chars", () => {
    // GBK 编码的「中文输出」
    const gbk = Buffer.from([0xd6, 0xd0, 0xce, 0xc4, 0xca, 0xe4, 0xb3, 0xf6])
    expect(decodeOutput(gbk)).toBe("中文输出")
  })

  test("empty buffer yields empty string", () => {
    expect(decodeOutput(Buffer.alloc(0))).toBe("")
  })

  test("ansi control sequences are stripped (color/cursor codes are noise)", () => {
    expect(decodeOutput(Buffer.from("\u001b[31;1mGet-Content: \u001b[0m找不到路径", "utf8"))).toBe("Get-Content: 找不到路径")
  })
})

describe("Sandbox 豁免用户（auth=none 默认用户不受用户沙箱控制）", () => {
  test("enforcedFor: 豁免用户不受约束，其余用户受约束；全局关闭时均不受约束", () => {
    const on = new Sandbox({ home: "/tmp/h", enabled: true, isExempt: (u) => u === "default" })
    expect(on.isExempt("default")).toBe(true)
    expect(on.isExempt("alice")).toBe(false)
    expect(on.enforcedFor("default")).toBe(false)
    expect(on.enforcedFor("alice")).toBe(true)
    const off = new Sandbox({ home: "/tmp/h", enabled: false, isExempt: (u) => u === "default" })
    expect(off.enforcedFor("default")).toBe(false)
    expect(off.enforcedFor("alice")).toBe(false)
  })

  test("resolvePath: 豁免用户绝对路径/../ 放行（本地模式语义），其余用户仍被沙箱拒绝", () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-exempt-"))
    const sid = "0123456789abcdef0123456789abcdef"
    const sb = new Sandbox({ home, enabled: true, isExempt: (u) => u === "default" })
    try {
      // 豁免用户（默认用户）：绝对路径直接使用、../ 可越界（相对路径基准 = 会话 tmp/，../ 越界到会话根）
      // （Windows 下 resolve("/etc/passwd") 为盘符根路径，断言与解析器同口径）
      expect(sb.resolvePath("default", sid, "/etc/passwd")).toBe(resolve("/etc/passwd"))
      expect(sb.resolvePath("default", sid, "../secret")).toBe(resolve(join(sessionPath(home, "default", sid), "tmp"), "../secret"))
      // 其余用户（含多用户模式同名 default 之外的用户）：沙箱拒绝
      expect(() => sb.resolvePath("alice", sid, "/etc/passwd")).toThrow()
      expect(() => sb.resolvePath("alice", sid, "../secret")).toThrow()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("exec: 豁免用户脚本子进程不剔除敏感变量（操作者本人环境），其余用户仍脱敏", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-exempt-exec-"))
    const sb = new Sandbox({ home, enabled: true, isExempt: (u) => u === "default" })
    try {
      const cmd = 'node -e "console.log(process.env.SECRET_KEY || \'EMPTY\')"'
      // 豁免用户（默认用户）：SECRET_KEY 可见
      const exempt = await sb.exec(cmd, { env: { SECRET_KEY: "s3cret" }, user: "default" })
      expect(exempt.stdout.trim()).toBe("s3cret")
      // 非豁免用户：敏感变量被剔除（防任意用户经 sh/py 外泄服务端密钥）
      const restricted = await sb.exec(cmd, { env: { SECRET_KEY: "s3cret" }, user: "alice" })
      expect(restricted.stdout.trim()).toBe("EMPTY")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("Sandbox.exec (Windows 编码)", () => {
  test("cmd output with chinese decodes correctly", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-"))
    const sb = new Sandbox({ home, enabled: false })
    try {
      const r = await sb.exec('echo 中文输出测试')
      expect(r.code).toBe(0)
      // chcp 65001 后 cmd 输出 UTF-8：解码后包含原文本（可能有换行/回车差异）
      expect(r.stdout.replace(/\r?\n/g, "")).toContain("中文输出测试")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("exec: cwd 不存在时自动创建（会话 tmp/ 缺失不再全体 ENOENT）；spawnBackground 同语义", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-cwd-"))
    const sb = new Sandbox({ home, enabled: false })
    const sid = "0123456789abcdef0123456789abcdef"
    const cwd = sb.workdir("default", sid) // 从未创建的会话 tmp 目录
    try {
      const r = await sb.exec("echo ok", { cwd })
      expect(r.code).toBe(0)
      expect(r.stdout.trim()).toBe("ok")
      // 后台任务：cwd 与 logPath 父目录缺失同样自动创建
      const { existsSync } = await import("node:fs")
      const logPath = join(cwd, "sh-tasks", "t1.log")
      const h = sb.spawnBackground("echo bg", { cwd, logPath })
      await h.exited
      expect(existsSync(logPath)).toBe(true)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("node --version output is captured (not swallowed)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-"))
    const sb = new Sandbox({ home, enabled: false })
    try {
      const r = await sb.exec("node --version")
      expect(r.code).toBe(0)
      expect(r.stdout.trim()).toMatch(/^v\d+\./)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("abort signal kills the running command and returns interrupted result", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-"))
    const sb = new Sandbox({ home, enabled: false })
    try {
      const controller = new AbortController()
      const started = Date.now()
      const p = sb.exec('node -e "setTimeout(()=>{}, 30000)"', { signal: controller.signal })
      // 等子进程真正启动后再中断（立即 abort 会在 spawn 完成前杀掉，仍可接受但时序不确定）
      await new Promise((r) => setTimeout(r, 300))
      controller.abort()
      const r = await p
      // 中断立即返回（远早于 30 秒），code 124 + [interrupted by user] 标记，与超时区分
      expect(Date.now() - started).toBeLessThan(10000)
      expect(r.code).toBe(124)
      expect(r.stderr).toContain("[interrupted by user]")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("timeout kills the running command with timed-out marker", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-"))
    const sb = new Sandbox({ home, enabled: false })
    try {
      const started = Date.now()
      const r = await sb.exec('node -e "setTimeout(()=>{}, 30000)"', { timeoutMs: 400 })
      expect(Date.now() - started).toBeLessThan(10000)
      expect(r.code).toBe(124)
      expect(r.stderr).toContain("[timed out after 400ms]")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("Windows 命令解释器（PowerShell）", () => {
  const prevShell = process.env.GEBAI_SH_SHELL
  const prevPath = process.env.PATH
  /** 还原环境并清缓存：缓存会记住本组用例造的临时解释器路径。 */
  const restore = () => {
    if (prevShell === undefined) delete process.env.GEBAI_SH_SHELL
    else process.env.GEBAI_SH_SHELL = prevShell
    process.env.PATH = prevPath
    _resetWinShellCache()
  }

  test("wrapPowerShellCommand：引导段 + 用户命令 + 收尾段逐行拼接", () => {
    const wrapped = wrapPowerShellCommand("echo hi")
    const lines = wrapped.split("\n")
    expect(lines).toContain("echo hi")
    // 引导段：输出编码切 UTF-8（解释器自身输出与原生命令输出同一解码入口）+ 关进度输出
    expect(wrapped).toContain("[Console]::OutputEncoding = [Text.Encoding]::UTF8")
    expect(wrapped).toContain("$ProgressPreference = 'SilentlyContinue'")
    // 收尾段：原生命令退出码带出（-Command 默认把退出码归一成 0/1）+ 失败兜底 1
    expect(wrapped).toContain("if ($LASTEXITCODE) { exit $LASTEXITCODE }")
    expect(lines[lines.length - 1]).toBe("exit 1")
    // 用户命令自占一行：以 # 注释结尾时不吞收尾段
    expect(lines[lines.indexOf("echo hi") + 1]).toBe("if ($?) { exit 0 }")
  })

  // UTF-8 引导段是**四条独立通道**，每条都要单独钉住——只测一条会让另三条静默退化。
  // 实测（2026-10-26）：只设 ①② 时 `Get-Content` 读 UTF-8 文件仍得到 `鏋勫缓瀹屾垚`
  // （UTF-8 字节被按 ANSI 代码页解码），而查日志正是它的唯一用途。
  test("wrapPowerShellCommand：UTF-8 引导段覆盖四条独立通道（输出/输入/管道/文件）", () => {
    const wrapped = wrapPowerShellCommand("echo hi")
    // ① 控制台输出编码（原生命令 stdout 的解码入口）
    expect(wrapped).toContain("[Console]::OutputEncoding = [Text.Encoding]::UTF8")
    // ② 控制台输入编码（交互/管道 stdin）
    expect(wrapped).toContain("[Console]::InputEncoding = [Text.Encoding]::UTF8")
    // ③ 管道给原生命令的数据编码（默认 ASCII，中文会丢）
    expect(wrapped).toContain("$OutputEncoding = [Text.Encoding]::UTF8")
    // ④ 文件读写默认编码：这一条与 ①②③ **毫无关系**（PS 5.1 的 Get-Content
    //    默认是 ANSI 代码页），必须走 $PSDefaultParameterValues 才能覆盖未写 -Encoding 的调用
    expect(wrapped).toContain("$PSDefaultParameterValues[")
    for (const cmd of ["Get-Content", "Set-Content", "Add-Content", "Out-File", "Select-String", "Import-Csv", "Export-Csv"]) {
      expect(wrapped).toContain(`'${cmd}'`)
    }
    // ⚠ 值**只能**是字符串 `'utf8'`：PS 5.1 的 Out-File -Encoding 是 ValidateSet，
    // 传 [Text.UTF8Encoding]::new($false) 对象会绑定失败并**静默回落到 UTF-16**
    // （实测写出 `ff fe` 开头）。这一条是对那个坑的钉子。
    expect(wrapped).toContain("= 'utf8'")
    expect(wrapped).not.toContain("UTF8Encoding]::new")
  })

  test("winShellPlan：PowerShell 形态以 -Command 承载包装后的完整命令；cmd 回落返回 null", () => {
    try {
      // 不存在的解释器名：which 探测不命中即原样使用（形态判定不依赖 PATH 内容）
      process.env.GEBAI_SH_SHELL = "gebai-test-shell"
      _resetWinShellCache()
      const plan = winShellPlan("bun test")
      expect(plan?.file).toBe("gebai-test-shell")
      expect(plan?.args.slice(0, 5)).toEqual(["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command"])
      expect(plan?.args[5]).toBe(wrapPowerShellCommand("bun test"))
      // GEBAI_SH_SHELL=cmd：回落 cmd.exe 形态（winShellPlan 返回 null，调用方沿用 chcp 65001 + shell:true）
      process.env.GEBAI_SH_SHELL = "cmd"
      _resetWinShellCache()
      expect(resolveWinShell()).toEqual({ file: "cmd.exe", powershell: false })
      expect(winShellPlan("echo hi")).toBeNull()
    } finally {
      restore()
    }
  })

  test("resolveWinShell：PATH 中的 pwsh 优先，缺失回落 powershell.exe", () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-winshell-"))
    try {
      writeFileSync(join(dir, "pwsh.exe"), "")
      writeFileSync(join(dir, "powershell.exe"), "")
      delete process.env.GEBAI_SH_SHELL
      process.env.PATH = dir
      _resetWinShellCache()
      expect(resolveWinShell()).toEqual({ file: join(dir, "pwsh.exe"), powershell: true })
      rmSync(join(dir, "pwsh.exe"))
      _resetWinShellCache()
      expect(resolveWinShell()).toEqual({ file: join(dir, "powershell.exe"), powershell: true })
    } finally {
      rmSync(dir, { recursive: true, force: true })
      restore()
    }
  })

  // 本机有真实 PowerShell（Windows 开发机 / 装了 pwsh 的环境）时执行包装后的命令：验证收尾段的退出码带出与中文输出解码
  const shellAvailable = which(resolveWinShell().file) != null
  test.if(shellAvailable)("PowerShell 形态真实执行：exitCode 带出原生命令退出码、中文输出按 UTF-8 解码", async () => {
    const run = async (cmd: string) => {
      const plan = winShellPlan(cmd)
      if (!plan) throw new Error("期望 PowerShell 形态")
      const child = spawn(plan.file, plan.args, { stdio: ["pipe", "pipe", "pipe"] })
      const stdout: Buffer[] = []
      const stderr: Buffer[] = []
      child.stdout.on("data", (d) => stdout.push(Buffer.isBuffer(d) ? d : Buffer.from(d)))
      child.stderr.on("data", (d) => stderr.push(Buffer.isBuffer(d) ? d : Buffer.from(d)))
      const code = await new Promise<number>((res) => child.on("close", (c) => res(c ?? 1)))
      return { stdout: decodeOutput(Buffer.concat(stdout)), stderr: decodeOutput(Buffer.concat(stderr)), code }
    }
    // PowerShell 单引号字面量（内含单引号翻倍转义）：`& <路径>` 调用运算符启动宿主机运行时
    const quote = (s: string) => `'${s.replace(/'/g, "''")}'`
    const bad = await run(`& ${quote(process.execPath)} -e ${quote("process.exit(7)")}`)
    expect(bad.code).toBe(7)
    const ok = await run(`& ${quote(process.execPath)} -e ${quote("console.log('中文 ok')")}`)
    expect(ok.code).toBe(0)
    expect(ok.stdout).toContain("中文 ok")
  })

  // 端到端：**真实进程读真实 UTF-8 文件**。
  //
  // 为何单靠「preamble 里有那行」不够：它只能证明“写了”，证明不了“生效”。
  // 实测（2026-10-26）就有一层没想到的差异：`[Console]::OutputEncoding` 与
  // `Get-Content` 的默认编码是**两条独立通道**——前者设了，后者照样按 ANSI 代码页读，
  // 于是 UTF-8 日志被读成 `鏋勫缓瀹屾垚`（构建通过/失败完全看不出来）。
  //
  // ⚠ **必须同时覆盖 Windows PowerShell 5.1**：`resolveWinShell` 优先 pwsh 7，
  // 而 pwsh 7 的默认编码本来就是 UTF-8——只测它会恒绿，把缺陷（只在 5.1 上现形）
  // 完全掩住。而 5.1 恰恰是没装 pwsh 的机器上的回落路径，即**大多数用户**。
  // ⚠ **必须同时覆盖 Windows PowerShell 5.1 与 pwsh 7**：两者默认编码不同
  // （5.1 = ANSI 代码页，7 = UTF-8），只测一条就会把另一条上的缺陷完全掩住。
  //
  // 为何不只用 `resolveWinShell().file`：它优先 pwsh，而 `which("pwsh.exe")` 对
  // Microsoft Store 版（WindowsApps 的 App Execution Alias）返回 null —— 那台机器上
  // 它就落到 5.1 了。所以这里**按可执行名各自探测**，两条都跑。
  const candidates: Array<[string, string | null]> = [
    ["Windows PowerShell 5.1", which("powershell.exe")],
    ["pwsh 7", which("pwsh") ?? which("pwsh.exe")],
  ]
  const seen = new Set<string>()
  const runs: Array<[string, string]> = []
  for (const [label, file] of candidates) {
    if (!file || seen.has(file)) continue
    seen.add(file)
    runs.push([label, file])
  }
  for (const [label, shellFile] of runs) {
    test(`真实执行（${label}）：Get-Content 读 UTF-8 文件不乱码、Out-File 写的是 UTF-8`, async () => {
      const dir = mkdtempSync(join(tmpdir(), "gebai-utf8-read-"))
      try {
        const file = join(dir, "utf8.txt")
        const expected = "构建完成 [dev] 单元 53（重编 53 / 命中缓存 0）"
        writeFileSync(file, `${expected}\n`, "utf8")
        const runShell = async (command: string) => {
          const child = spawn(
            shellFile,
            ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", wrapPowerShellCommand(command)],
            { stdio: ["pipe", "pipe", "pipe"] },
          )
          const stdout: Buffer[] = []
          child.stdout.on("data", (d) => stdout.push(Buffer.isBuffer(d) ? d : Buffer.from(d)))
          const code = await new Promise<number>((res) => child.on("close", (c) => res(c ?? 1)))
          return { text: decodeOutput(Buffer.concat(stdout)), code }
        }
        const quote = (s: string) => `'${s.replace(/'/g, "''")}'`
        // ① 读：默认 -Encoding 必须已是 UTF-8（不传 -Encoding）
        const read = await runShell(`Get-Content -LiteralPath ${quote(file)} -TotalCount 1`)
        expect(read.code).toBe(0)
        expect(read.text.includes("\uFFFD")).toBe(false)
        expect(read.text.replace(/\r?\n/g, "")).toBe(expected)
        // ② 写：必须是 **UTF-8**（PS 5.1 的 `'utf8'` 带 BOM、pwsh 7 不带——两者都接受；
        //    不可接受的是 UTF-16，那是“设了参数却绑定失败”的静默回落症状）
        const written = join(dir, "written.txt")
        await runShell(`${quote(expected)} | Out-File -LiteralPath ${quote(written)}`)
        const bytes = readFileSync(written)
        expect(bytes[0] === 0xff && bytes[1] === 0xfe).toBe(false) // 不是 UTF-16LE
        expect(bytes[0] === 0xfe && bytes[1] === 0xff).toBe(false) // 不是 UTF-16BE
        // UTF-8 字节序列（可带可不带 BOM）：解码后必须逐字相等
        expect(bytes.toString("utf8").replace(/^\uFEFF/, "").replace(/\r?\n/g, "")).toBe(expected)
      } finally {
        rmSync(dir, { recursive: true, force: true })
      }
    })
  }
})

describe("POSIX 命令解释器（bash）", () => {
  const prevShell = process.env.GEBAI_SH_SHELL
  const prevPath = process.env.PATH
  /** 还原环境并清缓存：缓存会记住本组用例造的临时解释器路径。 */
  const restore = () => {
    if (prevShell === undefined) delete process.env.GEBAI_SH_SHELL
    else process.env.GEBAI_SH_SHELL = prevShell
    process.env.PATH = prevPath
    _resetPosixShellCache()
  }

  test("resolvePosixShell：PATH 中的 bash 优先，缺失回落 /bin/sh", () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-posixshell-"))
    try {
      // Windows 上 which 按 PATHEXT 补扩展名探测，POSIX 探裸名
      const bashPath = join(dir, process.platform === "win32" ? "bash.exe" : "bash")
      writeFileSync(bashPath, "")
      delete process.env.GEBAI_SH_SHELL
      process.env.PATH = dir
      _resetPosixShellCache()
      // which 在 Windows 上按 PATHEXT 常量拼扩展名（.EXE），比对大小写无关
      expect(resolvePosixShell().toLowerCase()).toBe(bashPath.toLowerCase())
      rmSync(bashPath)
      _resetPosixShellCache()
      expect(resolvePosixShell()).toBe("/bin/sh")
    } finally {
      rmSync(dir, { recursive: true, force: true })
      restore()
    }
  })

  test("posixShellPlan：以 <解释器> -c 承载命令；GEBAI_SH_SHELL 可指定解释器", () => {
    try {
      process.env.GEBAI_SH_SHELL = "gebai-test-shell"
      _resetPosixShellCache()
      expect(posixShellPlan("echo hi")).toEqual({ file: "gebai-test-shell", args: ["-c", "echo hi"] })
    } finally {
      restore()
    }
  })

  // 真实执行（POSIX 宿主）：命令经显式解释器启动（不再走 Node 的隐式 /bin/sh）——退出码、多命令串接、stdin 语义不变
  test.if(process.platform !== "win32")("Sandbox.exec：命令经 bash 执行，退出码/串联/stdin 原样", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-posix-exec-"))
    const sb = new Sandbox({ home, enabled: false })
    try {
      const r = await sb.exec("echo one && echo two; exit 3")
      expect(r.code).toBe(3)
      expect(r.stdout).toContain("one")
      expect(r.stdout).toContain("two")
      const piped = await sb.exec("cat", { input: "piped-in" })
      expect(piped.stdout).toBe("piped-in")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("脚本运行根与会话隔离（服务模式：脚本环境收敛到会话目录）", () => {
  const SID = "0123456789abcdef0123456789abcdef"

  test("scriptEnvOverride / scriptEnvDirs：HOME、TEMP、XDG 指向会话内目录", () => {
    const sessionDir = join("/home", "users", "alice", "sessions", "s1")
    const base = join(sessionDir, "script-env")
    const env = scriptEnvOverride(sessionDir)
    expect(env.HOME).toBe(join(base, "home"))
    expect(env.USERPROFILE).toBe(env.HOME)
    expect(env.TMPDIR).toBe(join(base, "tmp"))
    expect(env.TMP).toBe(env.TMPDIR)
    expect(env.TEMP).toBe(env.TMPDIR)
    expect(env.XDG_CONFIG_HOME).toBe(join(base, "config"))
    expect(env.XDG_CACHE_HOME).toBe(join(base, "cache"))
    expect(env.XDG_DATA_HOME).toBe(join(base, "data"))
    expect(env.XDG_STATE_HOME).toBe(join(base, "state"))
    // 目录清单与变量同源且去重（多个变量共用同一目录时只创建一次）
    const dirs = scriptEnvDirs(sessionDir)
    expect(new Set(dirs).size).toBe(dirs.length)
    expect(dirs).toContain(join(base, "home"))
    expect(dirs).toContain(join(base, "tmp"))
  })

  test("bwrapArgs：系统目录只读绑定（仅存在的）、会话目录读写、chdir 到工作目录", () => {
    const args = bwrapArgs({ sessionDir: "/home/s1", cwd: "/home/s1/tmp", exists: (p) => p === "/usr" || p === "/bin" })
    expect(args.slice(0, 5)).toEqual(["--die-with-parent", "--unshare-pid", "--unshare-ipc", "--unshare-uts", "--unshare-cgroup-try"])
    const line = args.join(" ")
    expect(line).toContain("--ro-bind /usr /usr")
    expect(line).toContain("--ro-bind /bin /bin")
    expect(line).not.toContain("--ro-bind /etc /etc") // 不存在的目录不绑定（不同发行版目录集不同）
    expect(line).toContain("--proc /proc")
    expect(line).toContain("--dev /dev")
    expect(line).toContain("--chdir /home/s1/tmp")
    // 唯一可写入口是会话目录（--bind；其余均为只读绑定）
    expect(args[args.indexOf("--bind") + 1]).toBe("/home/s1")
    expect(args.filter((a) => a === "--bind")).toHaveLength(1)
  })

  test("isolationFor：本地模式/豁免用户恒 off；服务模式默认 env 收敛，bwrap 不可用时回落", () => {
    expect(new Sandbox({ home: "/tmp/h", enabled: false }).isolationFor("alice")).toBe("off")
    expect(new Sandbox({ home: "/tmp/h", enabled: true, isExempt: (u) => u === "admin" }).isolationFor("admin")).toBe("off")
    expect(new Sandbox({ home: "/tmp/h", enabled: true, scriptIsolation: "off" }).isolationFor("alice")).toBe("off")
    expect(new Sandbox({ home: "/tmp/h", enabled: true, scriptIsolation: "env" }).isolationFor("alice")).toBe("env")
    if (process.platform === "win32") {
      // Windows 无 bubblewrap：请求 bwrap 时如实回落到环境收敛
      expect(new Sandbox({ home: "/tmp/h", enabled: true, scriptIsolation: "bwrap" }).isolationFor("alice")).toBe("env")
    }
  })

  test("不变量：受约束用户（服务模式）在任何配置下都不会拿到 off（会话目录隔离恒开）", () => {
    for (const mode of [undefined, "auto", "env", "bwrap"] as const) {
      const sb = new Sandbox(mode === undefined ? { home: "/tmp/h", enabled: true } : { home: "/tmp/h", enabled: true, scriptIsolation: mode })
      // 默认（含未配置）与服务模式显式模式：隔离强度可降，但不得为 off；且脚本环境覆盖非空
      const got = sb.isolationFor("alice")
      expect(got === "env" || got === "bwrap").toBe(true)
      expect(Object.keys(sb.scriptEnv("alice", SID)).length).toBeGreaterThan(0)
    }
  })

  test("scriptEnv：服务模式给出会话内覆盖；本地/豁免用户与缺会话信息时为空（行为不变）", () => {
    const server = new Sandbox({ home: "/tmp/h", enabled: true })
    expect(server.scriptEnv("alice", SID).HOME).toBe(join(sessionPath("/tmp/h", "alice", SID), "script-env", "home"))
    expect(server.scriptEnv("alice", null)).toEqual({})
    expect(server.scriptEnv(undefined, SID)).toEqual({})
    expect(new Sandbox({ home: "/tmp/h", enabled: false }).scriptEnv("alice", SID)).toEqual({})
    expect(new Sandbox({ home: "/tmp/h", enabled: true, isExempt: () => true }).scriptEnv("alice", SID)).toEqual({})
  })

  /** 环境变量探针：只用系统 shell 读环境（**不依赖 node**——运行环境可能根本没装 node，容器镜像即是；
   *  Windows 经 PowerShell（该平台 sandbox 本就以 PowerShell 承载命令）、POSIX 经 sh）。 */
  const envProbe = process.platform === "win32" ? `$env:HOME + ';' + $env:TEMP + ';' + $env:TMPDIR` : `printf '%s;%s;%s' "$HOME" "$TEMP" "$TMPDIR"`

  test("真实执行：服务模式脚本的 HOME/TEMP 落在会话目录内（目录预创建）；本地模式不变", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-root-"))
    const server = new Sandbox({ home, enabled: true, scriptIsolation: "env" })
    const local = new Sandbox({ home, enabled: true, isExempt: () => true })
    const probe = envProbe
    const base = join(sessionPath(home, "alice", SID), "script-env")
    try {
      const r = await server.exec(probe, { cwd: server.workdir("alice", SID), user: "alice", sessionId: SID })
      expect(r.code).toBe(0)
      const [h, t, td] = r.stdout.trim().split(";")
      expect(h).toBe(join(base, "home"))
      expect(t).toBe(join(base, "tmp"))
      expect(td).toBe(join(base, "tmp"))
      const { existsSync } = await import("node:fs")
      expect(existsSync(join(base, "home"))).toBe(true)
      expect(existsSync(join(base, "tmp"))).toBe(true)
      // 本地模式（豁免用户）：宿主环境原样，不收敛
      const l = await local.exec(probe, { cwd: local.workdir("alice", SID), user: "alice", sessionId: SID })
      expect(l.code).toBe(0)
      expect(l.stdout.trim().split(";")[0]).not.toBe(join(base, "home"))
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("bwrap 分支真实隔离（仅 Linux 且容器/宿主授予 user namespace 时可跑）：命令在沙箱内执行", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-bwrap-"))
    const sb = new Sandbox({ home, enabled: true, scriptIsolation: "bwrap" })
    // bwrap 不可用（Windows、或容器未授予 user namespace）→ 本用例不适用；
    // 该情形的降级路径由「isolationFor 回落 env」与「真实执行（env 档）」两例覆盖
    if (sb.isolationFor("alice") !== "bwrap") {
      rmSync(home, { recursive: true, force: true })
      return
    }
    try {
      const cwd = sb.workdir("alice", SID)
      // ① 环境收敛在沙箱内仍生效（HOME 指向会话内目录）
      const r = await sb.exec(envProbe, { cwd, user: "alice", sessionId: SID })
      expect(r.code).toBe(0)
      expect(r.stdout.trim().split(";")[0]).toBe(join(sessionPath(home, "alice", SID), "script-env", "home"))
      // ② 会话目录是沙箱内唯一可写挂载点；越出它的路径（宿主家目录）不可见
      const w = await sb.exec(`${envProbe} && touch inside.txt && echo WRITABLE && (ls /home >/dev/null 2>&1 && echo HOME-DIR-VISIBLE || echo HOME-DIR-HIDDEN)`, {
        cwd,
        user: "alice",
        sessionId: SID,
      })
      expect(w.code).toBe(0)
      expect(w.stdout).toContain("WRITABLE")
      expect(w.stdout).toContain("HOME-DIR-HIDDEN")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("pipeToSharedLog（子进程输出汇入共享日志流）", () => {
  /** 收集目的流的异步错误：任一未被处理的错误都会成为进程级异常。 */
  const withLog = (path: string) => {
    const logStream = createWriteStream(path, { flags: "a" })
    const errors: NodeJS.ErrnoException[] = []
    return { logStream, errors }
  }

  test("任一源先 EOF 不会关掉共享日志流（否则另一条源的数据会 write after end）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-logpipe-"))
    try {
      const { logStream } = withLog(join(dir, "a.log"))
      const early = new Readable({ read() {} })
      const late = new Readable({ read() {} })
      pipeToSharedLog([early, late], logStream)
      early.push("early-eof\n")
      early.push(null) // 先 EOF
      await new Promise((r) => setTimeout(r, 30))
      // 关键不变式：源 EOF 不传导到共享日志流
      expect(logStream.writableEnded).toBe(false)
      expect(logStream.destroyed).toBe(false)
      // 后到的源照常写入（修复前就是在这里抛 ERR_STREAM_WRITE_AFTER_END 打崩进程）
      late.push("late-data\n")
      late.push(null)
      await new Promise((r) => setTimeout(r, 30))
      logStream.end()
      await new Promise((r) => setTimeout(r, 60))
      expect(readFileSync(join(dir, "a.log"), "utf8")).toContain("late-data")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("日志流写入失败只走 onError 回调，不成为进程级异常", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-logpipe-"))
    try {
      // 只读目录下建日志：createWriteStream 异步报 EACCES/EPERM
      const readonlyDir = join(dir, "ro")
      mkdirSync(readonlyDir)
      const logStream = createWriteStream(join(readonlyDir, "x.log"), { flags: "a" })
      const seen: string[] = []
      let resolveErr: (v: string) => void = () => {}
      const reported = new Promise<string>((r) => (resolveErr = r))
      pipeToSharedLog([], logStream, {
        onError: (err) => {
          seen.push(String(err?.code ?? err?.message))
          resolveErr("reported")
        },
      })
      // 人为制造写入失败：目录被移走后 flush 报错（与真实盘满/无权限同一路径）
      rmSync(readonlyDir, { recursive: true, force: true })
      logStream.write("payload")
      await Promise.race([reported, new Promise((r) => setTimeout(r, 1500))])
      // 断言只针对「错误被 onError 接住」这一点；具体 code 随平台而异（ENOENT/EPERM/EACCES）
      expect(seen.length).toBeGreaterThan(0)
      try {
        logStream.destroy()
      } catch {
        /* 已销毁 */
      }
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})

describe("Sandbox.spawnBackground 输出落盘", () => {
  test("stdout 先 EOF、stderr 后到：尾部输出不丢，进程事件流不抛异常", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-bg-log-"))
    const sb = new Sandbox({ home, enabled: false })
    const logPath = join(home, "sh-tasks", "bg.log")
    // 子进程：先关掉 stdout（父侧 stdout 立即 EOF），稍后才往 stderr 写
    const script = [
      'const fs=require("node:fs")',
      'fs.writeSync(1,"stdout-only\\n")',
      "fs.closeSync(1)",
      'setTimeout(()=>{ fs.writeSync(2,"stderr-late\\n"); setTimeout(()=>process.exit(0),150) }, 300)',
    ].join("\n")
    const scriptPath = join(home, "child.cjs")
    writeFileSync(scriptPath, script)
    try {
      const h = sb.spawnBackground(`& ${JSON.stringify(process.execPath)} ${JSON.stringify(scriptPath)}`, { cwd: home, logPath })
      // 修复前：stdout 先 EOF 会把共享日志流关掉，stderr 迟到数据触发 write after end 打崩宿主进程
      await h.exited
      await new Promise((r) => setTimeout(r, 200))
      const text = readFileSync(logPath, "utf8")
      // 不变量：先 EOF 的管道不影响另一条的落盘——迟到输出不丢
      expect(text).toContain("stdout-only")
      expect(text).toContain("stderr-late")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("日志目录不可写：任务照常退出，宿主进程不受影响（落盘失败只降级）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-bg-logfail-"))
    const sb = new Sandbox({ home, enabled: false })
    // 日志路径指向一个已存在的目录：open 必然失败（跨平台稳定的失败路径）
    const dirAsFile = join(home, "not-a-file")
    mkdirSync(dirAsFile, { recursive: true })
    try {
      const h = sb.spawnBackground("echo hi", { cwd: home, logPath: dirAsFile })
      // 进程本身正常退出（日志流错误被 onError 接住，不升级为进程级异常）
      await expect(h.exited).resolves.toBeTypeOf("number")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
