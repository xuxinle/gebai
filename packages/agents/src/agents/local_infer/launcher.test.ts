/**
 * launcher.ts（跨平台进程管理）测试：参数组装、平台命令双轨、引擎定位、真进程启动与就绪轮询。
 *
 * 环境封闭：临时目录（mkdtempSync）+ 随机端口；平台分支一律以显式平台参数断言（不随宿主漂移）；
 * 涉及真进程的用例用 `process.execPath`（即 bun）跑内联脚本当假 llama-server，并 try/finally 收尾不留残余。
 */
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { afterAll, describe, expect, test } from "bun:test"
import type { ToolContext } from "@gebai/sdk"
import {
  assertExecutable,
  buildServerArgs,
  defaultThreads,
  isListening,
  killAllCmd,
  killByPort,
  killPid,
  killPidCmd,
  listInstalledEngines,
  listProcsCmd,
  parsePortOwner,
  pidAlive,
  pidAliveCmd,
  pidAliveVerdict,
  portOwnerCmd,
  resolveEngineExe,
  spawnServer,
  spawnServerAsync,
  waitReady,
} from "./launcher"
import { tailLines } from "./paths"

const tmp = mkdtempSync(join(tmpdir(), "gebai-infer-launcher-"))
afterAll(() => rmSync(tmp, { recursive: true, force: true }))

/** 本机是否 POSIX（真进程用例依赖 sh/chmod；Windows 上跳过，命令组装的双轨断言仍全跑）。 */
const posix = process.platform !== "win32"

function mkTmp(prefix: string): string {
  return mkdtempSync(join(tmp, `${prefix}-`))
}

/** 假 ToolContext：runCommand 真的执行命令（真进程用例要能真的 kill）。 */
function realCtx(workdir: string): ToolContext {
  const run = async (cmd: string) => {
    const p = Bun.spawnSync(["sh", "-c", cmd])
    return { stdout: p.stdout.toString(), stderr: p.stderr.toString(), code: p.exitCode ?? -1 }
  }
  return {
    user: "default",
    sessionId: "s1",
    workdir,
    sessionWorkdir: workdir,
    home: workdir,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(workdir, p),
    readFile: async (p) => Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async () => {},
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async (cmd) => run(cmd),
    uploadAttachment: (r) => Promise.resolve(r.path),
    publish: () => {},
    projects: [],
    resolveProjectPath: () => {
      throw new Error("无预置项目")
    },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
  }
}

/** 记录命令并回放输出的桩 ctx（不真正执行）。 */
function stubCtx(workdir: string, run: (cmd: string) => { stdout: string; stderr: string; code: number } | undefined): { ctx: ToolContext; calls: string[] } {
  const calls: string[] = []
  const ctx = realCtx(workdir)
  ctx.runCommand = async (cmd: string) => {
    calls.push(cmd)
    return { stdout: "", stderr: "", code: 0, ...(run(cmd) ?? {}) }
  }
  return { ctx, calls }
}

/** 阻塞等待某文件出现并读出内容（假 llama-server 用它回传随机端口）。 */
async function waitFile(file: string, timeoutMs = 15000): Promise<string> {
  const t0 = Date.now()
  for (;;) {
    if (Bun.file(file).size > 0) return (await Bun.file(file).text()).trim()
    if (Date.now() - t0 > timeoutMs) throw new Error(`等待文件超时：${file}`)
    await new Promise((r) => setTimeout(r, 50))
  }
}

/** 取一个「刚被释放、因此几乎肯定空闲」的高位随机端口。 */
function freePort(): number {
  const s = Bun.serve({ port: 0, fetch: () => new Response("x") })
  const port = s.port as number
  s.stop(true)
  return port
}

/** 假 llama-server 的内联脚本：随机端口 + /health，并把实际端口写回文件。 */
function fakeServerCode(portFile: string, banner: string): string {
  return (
    `const s=Bun.serve({port:0,fetch:(r)=>{const ok=new URL(r.url).pathname==="/health";return new Response(ok?"ok":"nf",{status:ok?200:404})}});` +
    `console.log(${JSON.stringify(banner)}+" "+s.port);Bun.write(${JSON.stringify(portFile)},String(s.port));setInterval(()=>{},1000);`
  )
}

// ── buildServerArgs（逐字段） ─────────────────────────────────────────────

describe("buildServerArgs", () => {
  test("必填与缺省：-m/--host/--port/-c/-ngl 恒有，缺省 host/ctx/ngl 生效", () => {
    expect(buildServerArgs({ modelPath: "/m/a.gguf", port: 8080 })).toEqual([
      "-m", "/m/a.gguf", "--host", "127.0.0.1", "--port", "8080", "-c", "32768", "-ngl", "99",
    ])
    expect(buildServerArgs({ modelPath: "/m/a.gguf", port: 9, host: "0.0.0.0", ctx: 65536, ngl: 42 })).toEqual([
      "-m", "/m/a.gguf", "--host", "0.0.0.0", "--port", "9", "-c", "65536", "-ngl", "42",
    ])
  })

  test("ngl/ncmoe 的 0 与 undefined 之别：0 是有效值必须显式输出，空值/未给必须省略", () => {
    // ngl=0（全 CPU 档）不能被缺省 99 吞掉；ncmoe=0（全 GPU）同样必须显式给出
    const a = buildServerArgs({ modelPath: "/m/a.gguf", port: 8080, ngl: 0, ncmoe: 0 })
    expect(a).toContain("-ngl")
    expect(a[a.indexOf("-ngl") + 1]).toBe("0")
    expect(a[a.indexOf("-ncmoe") + 1]).toBe("0")

    // ncmoe 未给 / null：省略该参数
    for (const ncmoe of [undefined, null]) {
      const b = buildServerArgs({ modelPath: "/m/a.gguf", port: 8080, ncmoe })
      expect(b.includes("-ncmoe")).toBe(false)
      expect(b[b.indexOf("-ngl") + 1]).toBe("99") // ngl 未给 → 缺省 99
    }
  })

  test("全字段顺序稳定（含 -np/-a/-ncmoe/--cont-batching/-bs 与 extraArgs 追加）", () => {
    expect(
      buildServerArgs({
        modelPath: "/m/a.gguf",
        host: "127.0.0.1",
        port: 8080,
        ctx: 32768,
        ngl: 99,
        ncmoe: 8,
        parallel: 4,
        threads: 14,
        batch: 4096,
        ubatch: 1024,
        flashAttn: "on",
        cacheTypeK: "q8_0",
        cacheTypeV: "q8_0",
        alias: "agentworld-35b-a3b",
        contBatching: true,
        backendSampling: true,
        extraArgs: ["--jinja"],
      }),
    ).toEqual([
      "-m", "/m/a.gguf",
      "--host", "127.0.0.1",
      "--port", "8080",
      "-c", "32768",
      "-ngl", "99",
      "-fa", "on",
      "-ctk", "q8_0",
      "-ctv", "q8_0",
      "-t", "14",
      "-b", "4096",
      "-ub", "1024",
      "-np", "4",
      "-a", "agentworld-35b-a3b",
      "-ncmoe", "8",
      "--cont-batching",
      "-bs",
      "--jinja", // extraArgs 恒在末尾（可与前面参数合并成 llama.cpp 自己解析的一串）
    ])
  })

  test("可选字段未给就不产出（不塞空串/NaN）；必填缺失显式报错", () => {
    const a = buildServerArgs({ modelPath: "/m/a.gguf", port: 8080 })
    for (const flag of ["-fa", "-ctk", "-ctv", "-t", "-b", "-ub", "-np", "-a", "-ncmoe", "--cont-batching", "-bs"]) {
      expect(a.includes(flag)).toBe(false)
    }
    expect(() => buildServerArgs({ modelPath: "", port: 8080 })).toThrow(/缺少模型路径/)
    expect(() => buildServerArgs({ modelPath: "/m/a.gguf", port: Number.NaN })).toThrow(/缺少端口/)
  })
})

// ── 平台命令（win32 / POSIX 双轨显式注入） ────────────────────────────────

describe("平台命令组装（双轨显式注入）", () => {
  test("探活：win32 tasklist 精确过滤，POSIX kill -0", () => {
    expect(pidAliveCmd(4321, "win32")).toBe('tasklist /FI "PID eq 4321" /FO CSV /NH')
    expect(pidAliveCmd(4321, "linux")).toBe("kill -0 4321 2>/dev/null")
    expect(pidAliveVerdict(4321, { stdout: '"llama-server.exe","4321","Console","1","13,000 K"', stderr: "", code: 0 }, "win32")).toBe(true)
    expect(pidAliveVerdict(4321, { stdout: "INFO: No tasks are running which match the specified criteria.", stderr: "", code: 0 }, "win32")).toBe(false)
    expect(pidAliveVerdict(4321, { stdout: "", stderr: "", code: 0 }, "linux")).toBe(true)
    expect(pidAliveVerdict(4321, { stdout: "", stderr: "", code: 1 }, "linux")).toBe(false)
  })

  test("终止：win32 终止进程树（/T /F），POSIX kill -9", () => {
    expect(killPidCmd(777, "win32")).toBe("taskkill /PID 777 /T /F")
    expect(killPidCmd(777, "linux")).toBe("kill -9 777 2>/dev/null || true")
    expect(killPidCmd(777, "darwin")).toBe("kill -9 777 2>/dev/null || true")
  })

  test("按名终止：win32 自动补 .exe，POSIX pkill -f", () => {
    expect(killAllCmd("llama-server", "win32")).toBe("taskkill /IM llama-server.exe /T /F")
    expect(killAllCmd("llama-server.exe", "win32")).toBe("taskkill /IM llama-server.exe /T /F")
    expect(killAllCmd("llama-server", "linux")).toBe("pkill -f llama-server || true")
    expect(killAllCmd("llama-server", "darwin")).toBe("pkill -f llama-server || true")
  })

  test("进程列表：win32 tasklist /FI，POSIX pgrep -a", () => {
    expect(listProcsCmd("llama-server", "win32")).toBe('tasklist /FI "IMAGENAME eq llama-server.exe" /FO CSV /NH')
    expect(listProcsCmd("llama-server", "linux")).toBe("pgrep -a llama-server || true")
  })

  test("端口查询与占用者解析：netstat（win32）与 ss（POSIX）两种样例都要认", () => {
    expect(portOwnerCmd(8080, "win32")).toBe('netstat -ano | findstr ":8080 "')
    expect(portOwnerCmd(8080, "linux")).toBe('ss -ltnp 2>/dev/null | grep ":8080 " || true')

    const netstat = "  TCP    127.0.0.1:8080         0.0.0.0:0              LISTENING       4321\r\n"
    expect(isListening(netstat)).toBe(true)
    expect(parsePortOwner(netstat, "win32")).toBe(4321)

    const ss = 'LISTEN 0 4096 127.0.0.1:8080 0.0.0.0:* users:(("llama-server",pid=4322,fd=9))\n'
    expect(isListening(ss)).toBe(true)
    expect(parsePortOwner(ss, "linux")).toBe(4322)

    // ss 无 -p（拿不到 pid）→ undefined；netstat -ltnp 形态的 <pid>/<name> 末列要认
    expect(parsePortOwner("LISTEN 0 4096 127.0.0.1:8080 0.0.0.0:*\n", "linux")).toBeUndefined()
    expect(parsePortOwner("tcp 0 0 127.0.0.1:8080 0.0.0.0:* LISTEN 4323/llama-server\n", "linux")).toBe(4323)

    expect(isListening("")).toBe(false)
    expect(parsePortOwner("", "win32")).toBeUndefined()
    expect(parsePortOwner("", "linux")).toBeUndefined()
  })

  test("defaultThreads 至少 1（0.5×逻辑核）", () => {
    expect(defaultThreads()).toBeGreaterThanOrEqual(1)
  })

  test("执行型封装：pidAlive / killPid / killByPort 走各自的纯函数命令", async () => {
    const dir = mkTmp("launcher-exec")
    const { ctx, calls } = stubCtx(dir, (cmd) => {
      if (/netstat -ano/.test(cmd)) return { stdout: "  TCP    127.0.0.1:8080   0.0.0.0:0   LISTENING   999\r\n", stderr: "", code: 0 }
      if (/PID eq/.test(cmd)) return { stdout: '"llama-server.exe","555","Console","1","1 K"', stderr: "", code: 0 }
      return undefined
    })
    expect(await pidAlive(555, ctx, "win32")).toBe(true)
    expect(await pidAlive(556, ctx, "win32")).toBe(false)
    expect(await killPid(555, ctx, "win32")).toEqual({ ok: true, output: "" })
    expect(await killByPort(8080, ctx, "win32")).toEqual({ ok: true, pid: 999, output: "" })
    expect(calls).toContain("taskkill /PID 555 /T /F")
    expect(calls).toContain("taskkill /PID 999 /T /F")

    // 端口没人监听：ok=false 且不尝试 kill
    const empty = stubCtx(dir, () => ({ stdout: "", stderr: "", code: 0 }))
    expect((await killByPort(8080, empty.ctx, "linux")).ok).toBe(false)
    expect(empty.calls.some((c) => c.startsWith("kill "))).toBe(false)
  })
})

// ── resolveEngineExe ──────────────────────────────────────────────────────

describe("resolveEngineExe", () => {
  /** 造一个「已安装引擎」：vendor/<id>/.engine.json + 假 exe。 */
  function withEngine(home: string, id: string, marker: Record<string, unknown>): string {
    const dir = join(home, "vendor", id)
    mkdirSync(dir, { recursive: true })
    const exe = join(dir, "llama-server")
    writeFileSync(exe, "#!/bin/sh\n")
    writeFileSync(join(dir, ".engine.json"), JSON.stringify({ id, exe, tag: "b11100", asset: "x.tar.gz", installed_at: "2026-01-01", dir, ...marker }))
    return exe
  }

  test("引擎标记存在 → via engine（指定 id 与不指定都要走引擎）", () => {
    const home = mkTmp("launcher-engine")
    const exe = withEngine(home, "linux-cpu-x64", {})
    expect(resolveEngineExe({ home, platform: "linux" })).toEqual({ exe, via: "engine", note: `引擎 linux-cpu-x64（${join(home, "vendor", "linux-cpu-x64")}）` })
    expect(resolveEngineExe({ home, engineId: "linux-cpu-x64", platform: "linux" }).via).toBe("engine")
    expect(listInstalledEngines(home, "linux").map((e) => e.id)).toEqual(["linux-cpu-x64"])
    // 标记里的 exe 支持相对路径
    const home2 = mkTmp("launcher-engine-rel")
    const dir2 = join(home2, "vendor", "win-cuda")
    mkdirSync(join(dir2, "bin"), { recursive: true })
    writeFileSync(join(dir2, "bin", "llama-server.exe"), "x")
    writeFileSync(join(dir2, ".engine.json"), JSON.stringify({ id: "win-cuda", exe: join("bin", "llama-server.exe") }))
    expect(resolveEngineExe({ home: home2, engineId: "win-cuda", platform: "win32" }).exe).toBe(join(dir2, "bin", "llama-server.exe"))
  })

  test("标记存在但 exe 不在（半损安装）→ 不算已安装，继续走下一步", () => {
    const home = mkTmp("launcher-engine-broken")
    const dir = join(home, "vendor", "linux-cpu-x64")
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, ".engine.json"), JSON.stringify({ id: "linux-cpu-x64", exe: join(dir, "llama-server") }))
    expect(listInstalledEngines(home, "linux")).toEqual([])
    const r = resolveEngineExe({ home, platform: "linux" })
    expect(r.via).toBe("none")
    expect(r.note).toContain("local_infer_engine_fetch")
  })

  test("无引擎标记、只有 engine_dir 下同名 exe → via engine_dir", () => {
    const home = mkTmp("launcher-enginedir")
    const dir = join(home, "vendor", "llama-b11100")
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, "llama-server"), "#!/bin/sh\n")
    const r = resolveEngineExe({ home, engineDir: "vendor/llama-b11100", platform: "linux" })
    expect(r.via).toBe("engine_dir")
    expect(r.exe).toBe(join(dir, "llama-server"))
    // Windows 形态认 .exe（POSIX 侧也允许 .exe，方便跨平台共享的解压包）
    const home2 = mkTmp("launcher-enginedir-win")
    const dir2 = join(home2, "vendor", "win")
    mkdirSync(dir2, { recursive: true })
    writeFileSync(join(dir2, "llama-server.exe"), "x")
    expect(resolveEngineExe({ home: home2, engineDir: "vendor/win", platform: "win32" }).exe).toBe(join(dir2, "llama-server.exe"))
  })

  test("都没有 → via none 且 note 是可操作指引；指定引擎未安装时如实说明", () => {
    const home = mkTmp("launcher-none")
    const r = resolveEngineExe({ home, platform: "linux" })
    expect(r.via).toBe("none")
    expect(r.exe).toBeUndefined()
    expect(r.note).toContain("local_infer_engines")
    expect(r.note).toContain("local_infer_engine_fetch")

    // 指定了一个没装的引擎、engine_dir 也没有 → note 里要提到它，别只报「无引擎」
    const home2 = mkTmp("launcher-none-id")
    withEngine(home2, "linux-cpu-x64", {})
    const r2 = resolveEngineExe({ home: home2, engineId: "linux-vulkan-x64", engineDir: "vendor/nope", platform: "linux" })
    expect(r2.via).toBe("none")
    expect(r2.note).toContain("linux-vulkan-x64 未安装")
    expect(r2.note).toContain("engine_dir")
  })

  test("engine_dir 为绝对路径也认（跨盘/共享目录配置）", () => {
    const home = mkTmp("launcher-abs")
    const abs = mkTmp("launcher-abs-dir")
    writeFileSync(join(abs, "llama-server"), "#!/bin/sh\n")
    const r = resolveEngineExe({ home, engineDir: abs, platform: "linux" })
    expect(r.via).toBe("engine_dir")
    expect(r.exe).toBe(join(abs, "llama-server"))
  })

  test("目录约定最后兑底：vendor/<id>/ 里手工解压（无标记）的 llama-server 也认，但不冒充「已安装」", () => {
    const home = mkTmp("launcher-manual")
    const nested = join(home, "vendor", "llama-b11175-linux-x64", "llama-b11175")
    mkdirSync(nested, { recursive: true })
    const exe = join(nested, "llama-server")
    writeFileSync(exe, "#!/bin/sh\n", { mode: 0o755 })
    // 没标记 → 不算已安装；但也别因此就报「无引擎」（本机现成可跑）
    expect(listInstalledEngines(home, "linux")).toEqual([])
    const r = resolveEngineExe({ home, engineDir: "vendor/不存在的目录", platform: "linux" })
    expect(r.via).toBe("engine_dir")
    expect(r.exe).toBe(exe)
    expect(r.note).toContain("无安装标记")

    // POSIX 不拿 .exe 去 spawn：只有 Windows 二进制的 vendor 目录不能算候选
    const home2 = mkTmp("launcher-manual-win")
    const dir2 = join(home2, "vendor", "win-only")
    mkdirSync(dir2, { recursive: true })
    writeFileSync(join(dir2, "llama-server.exe"), "x")
    expect(resolveEngineExe({ home: home2, platform: "linux" }).via).toBe("none")
    expect(resolveEngineExe({ home: home2, platform: "win32" }).via).toBe("engine_dir")
  })
})

// ── spawnServer / spawnServerAsync / waitReady（真进程） ─────────────────

describe("启动与就绪", () => {
  test("spawnServer + waitReady：进程起来、日志被写入、killPid 能收尾", async () => {
    if (!posix) return
    const dir = mkTmp("launcher-spawn")
    const portFile = join(dir, "port.txt")
    const logPath = join(dir, "logs", "server-test.log") // 父目录不存在 → 由 openLogFd 创建
    let pid = 0
    try {
      const res = spawnServer({ exe: process.execPath, args: ["-e", fakeServerCode(portFile, "fake llama-server up")], logPath, cwd: dir })
      pid = res.pid
      expect(pid).toBeGreaterThan(0)
      expect(res.logPath).toBe(logPath)
      expect(res.argv[0]).toBe(process.execPath)
      expect(res.argv.slice(1)).toEqual(["-e", fakeServerCode(portFile, "fake llama-server up")]) // argv 原样回传（可审计）

      const port = Number(await waitFile(portFile))
      expect(port).toBeGreaterThan(0)

      const ready = await waitReady(`http://127.0.0.1:${port}`, { timeoutMs: 15000, intervalMs: 100, isAlive: () => pidAlive(pid, realCtx(dir)) })
      expect(ready.ok).toBe(true)
      expect(ready.error).toBeUndefined()

      // 日志被写入（stdout 重定向到日志文件，未继承调用方 stdio）
      expect(tailLines(logPath, 20).text).toContain("fake llama-server up")

      // 收尾：killPid 真杀（POSIX kill -9）
      const killed = await killPid(pid, realCtx(dir))
      expect(killed.ok).toBe(true)
      await new Promise((r) => setTimeout(r, 200))
      expect(await pidAlive(pid, realCtx(dir))).toBe(false)
      pid = 0
    } finally {
      if (pid > 0) {
        try {
          process.kill(pid, "SIGKILL")
        } catch {
          /* 已退出 */
        }
      }
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("waitReady 超时：端口没人监听 → ok:false 且不抛错", async () => {
    const port = freePort()
    const t0 = Date.now()
    const r = await waitReady(`http://127.0.0.1:${port}`, { timeoutMs: 700, intervalMs: 100 })
    expect(r.ok).toBe(false)
    expect(r.error).toBeTruthy()
    expect(Date.now() - t0).toBeLessThan(5000)
    expect(r.secs).toBeGreaterThanOrEqual(0)
  })

  test("waitReady：isAlive=false（进程已退出）→ 立即返回，不白等到超时", async () => {
    const port = freePort()
    const t0 = Date.now()
    const r = await waitReady(`http://127.0.0.1:${port}`, { timeoutMs: 60000, intervalMs: 100, isAlive: async () => false })
    expect(r.ok).toBe(false)
    expect(r.error).toContain("进程已退出")
    expect(Date.now() - t0).toBeLessThan(3000)
  })

  test("waitReady：探活抛错不阻断等待（超时后仍如实返回 ok:false）", async () => {
    const port = freePort()
    const r = await waitReady(`http://127.0.0.1:${port}`, {
      timeoutMs: 500,
      intervalMs: 100,
      isAlive: async () => {
        throw new Error("命令不可用")
      },
    })
    expect(r.ok).toBe(false)
  })

  test("spawnServerAsync：exe 不存在 → reject，错误里带日志尾部与安装指引", async () => {
    const dir = mkTmp("launcher-enoent")
    const logPath = join(dir, "server.log")
    const p = spawnServerAsync({ exe: join(dir, "no-such-llama-server"), args: ["-m", "x.gguf"], logPath })
    await expect(p).rejects.toThrow(/找不到可执行文件/)
    await p.catch((e: Error) => {
      expect(e.message).toContain("日志尾部")
      expect(e.message).toContain("local_infer_engine_fetch")
    })
    expect(() => assertExecutable(join(dir, "nope"))).toThrow(/local_infer_engines/)
  })

  test("spawnServerAsync：进程启动后立即退出 → reject，错误里带日志尾部（stderr 内容）", async () => {
    const dir = mkTmp("launcher-exit")
    const logPath = join(dir, "server.log")
    const p = spawnServerAsync({ exe: process.execPath, args: ["-e", 'console.error("boom: no CUDA device");process.exit(3);'], logPath })
    await expect(p).rejects.toThrow(/立即退出/)
    await p.catch((e: Error) => {
      expect(e.message).toContain("boom: no CUDA device")
      expect(e.message).toContain("日志尾部")
    })
  })

  test("spawnServerAsync：活着的进程正常 resolve（短窗内不退出即算启动成功）", async () => {
    if (!posix) return
    const dir = mkTmp("launcher-async")
    const portFile = join(dir, "port.txt")
    const logPath = join(dir, "server.log")
    let pid = 0
    try {
      const res = await spawnServerAsync({ exe: process.execPath, args: ["-e", fakeServerCode(portFile, "async up")], logPath, cwd: dir })
      pid = res.pid
      expect(res.pid).toBeGreaterThan(0)
      expect(Number(await waitFile(portFile))).toBeGreaterThan(0)
    } finally {
      if (pid > 0) {
        try {
          process.kill(pid, "SIGKILL")
        } catch {
          /* 已退出 */
        }
      }
      rmSync(dir, { recursive: true, force: true })
    }
  })
})
