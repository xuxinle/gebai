/**
 * server.ts（推理进程管理）测试：命令组装与平台分支、服务状态文件、启动记录与日志访问、
 * status 的实例/探测输出、logs 的过滤与清单、stop 的四种定位方式。
 *
 * 环境封闭：临时 infer home（mkdtempSync）+ 假 runCommand + 本地 mock /health /props，
 * 不依赖真实 GPU、引擎、pwsh 或外网；平台分支一律以显式平台参数断言（不随宿主漂移）。
 */
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, utimesSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { afterAll, describe, expect, test } from "bun:test"
import type { ToolContext } from "@gebai/sdk"
import { clearServerState, listServerLogs, readLaunchRecords, readServerStates, reportsDir, stateDir, tailLines, writeServerState } from "./paths"
import {
  doLogs,
  doRestart,
  doStart,
  doStatus,
  doStop,
  engineIdFromState,
  inferEngineIdFromExe,
  isListening,
  killAllCmd,
  killPidCmd,
  latestLaunches,
  parsePortOwnerPid,
  pidAliveCmd,
  pidAliveVerdict,
  pwshCmd,
  requiresApproval,
  resolveAlias,
  resolveLaunch,
  scriptRun,
  suspectedErrors,
  tools,
} from "./server"
import { killPid } from "./launcher"

const tmp = mkdtempSync(join(tmpdir(), "gebai-infer-server-"))
afterAll(() => rmSync(tmp, { recursive: true, force: true }))

/** 临时子项目根（含 config/profiles.json，或按需留空）。 */
function makeHome(withProfiles = true): string {
  const home = join(tmp, `home-${Math.random().toString(36).slice(2)}`)
  mkdirSync(join(home, "config"), { recursive: true })
  if (withProfiles) {
    writeFileSync(
      join(home, "config", "profiles.json"),
      JSON.stringify({
        engine_dir: "vendor/engine",
        default_profile: "fast",
        default_model: "m.gguf",
        profiles: { fast: { model: "a.gguf", ctx: 32768, parallel: 1 }, quality: { model: "b.gguf", n_cpu_moe: 8 } },
      }),
    )
  }
  return home
}

interface CmdCall {
  cmd: string
  opts?: { workdir?: string; timeoutMs?: number }
}

/** 假 ToolContext：记录每条命令，按模式回放输出（默认全部成功、空输出）。 */
function makeCtx(
  home: string,
  run: (cmd: string, opts?: CmdCall["opts"]) => { stdout: string; stderr: string; code: number } | undefined = () => undefined,
): { ctx: ToolContext; calls: CmdCall[] } {
  const calls: CmdCall[] = []
  const workdir = join(home, "sessions", "s1", "tmp")
  mkdirSync(workdir, { recursive: true })
  const ctx: ToolContext = {
    user: "default",
    sessionId: "s1",
    workdir,
    sessionWorkdir: workdir,
    home,
    env: { LOCAL_INFER_HOME: home },
    sandboxed: false,
    resolvePath: (p) => join(workdir, p),
    readFile: async (p) => Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, c) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(join(p, ".."), { recursive: true })
      await writeFile(p, c)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async (cmd, opts) => {
      calls.push({ cmd, opts: opts as CmdCall["opts"] })
      return { stdout: "", stderr: "", code: 0, ...(run(cmd, opts as CmdCall["opts"]) ?? {}) }
    },
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
  return { ctx, calls }
}

/** 假 llama-server：/health 恒 200，/props 返回 n_ctx 与 slot 数。 */
function startMockServer(props: { n_ctx?: number; total_slots?: number } = {}): { server: ReturnType<typeof Bun.serve>; port: number } {
  const server = Bun.serve({
    port: 0,
    fetch(req) {
      const url = new URL(req.url)
      if (url.pathname === "/health") return Response.json({ status: "ok" })
      if (url.pathname === "/props") {
        return Response.json({
          default_generation_settings: { n_ctx: props.n_ctx ?? 32768 },
          total_slots: props.total_slots ?? 4,
          model_path: "C:\\models\\Qwen-AgentWorld-35B-A3B-UD-IQ3_XXS.gguf",
        })
      }
      return new Response("not found", { status: 404 })
    },
  })
  return { server, port: server.port as number }
}

/** 让假 runCommand 同时兼容 win32（tasklist）与 POSIX（kill -0）两条探活路径。 */
function aliveStub(pid: number) {
  return (cmd: string): { stdout: string; stderr: string; code: number } | undefined => {
    const m = cmd.match(/PID eq (\d+)/)
    if (m) {
      const target = Number(m[1])
      return target === pid
        ? { stdout: `"llama-server.exe","${target}","Console","1","13,000,000 K"\n`, stderr: "", code: 0 }
        : { stdout: "INFO: No tasks are running which match the specified criteria.\n", stderr: "", code: 0 }
    }
    if (cmd.startsWith("kill -0 ")) {
      const target = Number(cmd.split(" ")[2])
      return { stdout: "", stderr: "", code: target === pid ? 0 : 1 }
    }
    return undefined
  }
}

// ── 命令组装与平台分支 ────────────────────────────────────────────────────

describe("命令组装（平台显式参数）", () => {
  test("pwsh 调用：Windows 加 -NoProfile，其它平台直接 pwsh", () => {
    expect(pwshCmd("win32")).toBe("pwsh -NoProfile")
    expect(pwshCmd("linux")).toBe("pwsh")
  })

  test("scriptRun 组装 -File 调用（路径带引号）", () => {
    expect(scriptRun("/x/infer", "run-server.ps1", ["-Port 8080"], "linux")).toBe(
      'pwsh -ExecutionPolicy Bypass -File "/x/infer/scripts/run-server.ps1" -Port 8080',
    )
    expect(scriptRun("C:\\infer", "bench.ps1", ["-Reps 2"], "win32")).toContain("pwsh -NoProfile -ExecutionPolicy Bypass")
  })

  test("探活命令：win32 用 tasklist 精确过滤，POSIX 用 kill -0", () => {
    expect(pidAliveCmd(4321, "win32")).toBe('tasklist /FI "PID eq 4321" /FO CSV /NH')
    expect(pidAliveCmd(4321, "linux")).toBe("kill -0 4321 2>/dev/null")
  })

  test("探活判定：win32 看输出含 PID，POSIX 看退出码", () => {
    expect(pidAliveVerdict(4321, { stdout: '"llama-server.exe","4321","Console","1","13,000 K"', stderr: "", code: 0 }, "win32")).toBe(true)
    expect(pidAliveVerdict(4321, { stdout: "INFO: No tasks are running which match the specified criteria.", stderr: "", code: 0 }, "win32")).toBe(false)
    expect(pidAliveVerdict(4321, { stdout: "", stderr: "", code: 0 }, "linux")).toBe(true)
    expect(pidAliveVerdict(4321, { stdout: "", stderr: "", code: 1 }, "linux")).toBe(false)
  })

  test("终止命令：win32 终止进程树（/T）", () => {
    expect(killPidCmd(777, "win32")).toBe("taskkill /PID 777 /T /F")
    expect(killPidCmd(777, "linux")).toBe("kill -9 777 2>/dev/null || true")
    expect(killAllCmd("win32")).toBe("taskkill /IM llama-server.exe /T /F")
    expect(killAllCmd("linux")).toBe("pkill -f llama-server || true")
  })

  test("端口监听与占用者解析（win32 netstat / POSIX ss）", () => {
    const net = "  TCP    127.0.0.1:8080         0.0.0.0:0              LISTENING       4321\r\n"
    expect(isListening(net)).toBe(true)
    expect(parsePortOwnerPid(net, "win32")).toBe(4321)
    const ss = 'LISTEN 0 4096 127.0.0.1:8080 0.0.0.0:* users:(("llama-server",pid=4321,fd=9))\n'
    expect(isListening(ss)).toBe(true)
    expect(parsePortOwnerPid(ss, "linux")).toBe(4321)
    expect(isListening("")).toBe(false)
    expect(parsePortOwnerPid("", "win32")).toBeUndefined()
  })

  test("日志疑似错误特征识别", () => {
    const text = "llama_model_load: failed to load model from x.gguf\nsomething fine\nCUDA out of memory\n"
    const hits = suspectedErrors(text)
    expect(hits.length).toBe(2)
    expect(hits[0]).toContain("模型加载失败")
    expect(hits[1]).toContain("CUDA")
    expect(suspectedErrors("all good")).toEqual([])
  })
})

// ── 服务状态文件 ──────────────────────────────────────────────────────────

describe("服务状态文件", () => {
  test("写 → 读 → 清（按端口索引，损坏文件跳过）", () => {
    const home = makeHome()
    writeServerState(home, { pid: 111, port: 8080, profile: "fast", model: "a.gguf", log: "x.log", started_at: "2026-01-01T00:00:00" })
    writeServerState(home, { pid: 222, port: 8081, profile: "quality", model: "b.gguf" })
    writeFileSync(join(stateDir(home), "server-9000.json"), "{ 不是 JSON")
    writeFileSync(join(stateDir(home), "server-abc.json"), "{}")

    const st = readServerStates(home)
    expect(st.map((s) => s.port)).toEqual([8080, 8081])
    expect(st[0].pid).toBe(111)
    expect(existsSync(join(stateDir(home), "server-8080.json"))).toBe(true)

    expect(clearServerState(home, 8080)).toBe(true)
    expect(readServerStates(home).map((s) => s.port)).toEqual([8081])
    expect(clearServerState(home, 8080)).toBe(false)
  })

  test("目录不存在时读出空数组", () => {
    expect(readServerStates(makeHome())).toEqual([])
  })
})

// ── 启动记录与日志 ────────────────────────────────────────────────────────

describe("启动记录与日志访问", () => {
  test("启动记录取最近 N 条（新的在前，损坏跳过不占额）", () => {
    const home = makeHome()
    const dir = reportsDir(home)
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, "launch-20260101-101010.json"), JSON.stringify({ started_at: "2026-01-01", profile: "fast", argv: ["-m", "a.gguf"] }))
    writeFileSync(join(dir, "launch-20260102-101010.json"), JSON.stringify({ started_at: "2026-01-02", profile: "quality", argv: ["-m", "b.gguf"] }))
    writeFileSync(join(dir, "launch-20260103-101010.json"), "{坏文件")
    const one = latestLaunches(home, 1)
    expect(one.length).toBe(1)
    expect(one[0].profile).toBe("quality")
    expect(latestLaunches(home, 5).length).toBe(2)
    expect(readLaunchRecords(home, 5).length).toBe(2) // 过取到 5 个名额时才看得到两条有效记录
  })

  test("日志清单一并识别 stdout 与 stderr（限条数）", () => {
    const home = makeHome()
    const dir = reportsDir(home)
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, "server-20260101-101010.log"), "out\n")
    writeFileSync(join(dir, "server-20260101-101010.log.err"), "err\n")
    writeFileSync(join(dir, "notes.txt"), "x")
    const list = listServerLogs(home, 10)
    expect(list.length).toBe(2)
    expect(new Set(list.map((l) => l.kind))).toEqual(new Set(["out", "err"]))
    expect(listServerLogs(home, 1).length).toBe(1)
  })

  test("tailLines：小文件全取，大文件只取尾部且首行不残留半行", () => {
    const home = makeHome()
    const dir = reportsDir(home)
    mkdirSync(dir, { recursive: true })
    const small = join(dir, "small.log")
    writeFileSync(small, "a\nb\nc\n")
    expect(tailLines(small, 10).text.trimEnd().split("\n")).toEqual(["a", "b", "c"])

    const big = join(dir, "big.log")
    const n = 20000
    writeFileSync(big, Array.from({ length: n }, (_, i) => `line-${i}`).join("\n"))
    const tail = tailLines(big, 5)
    expect(tail.text.split("\n")).toEqual([`line-${n - 5}`, `line-${n - 4}`, `line-${n - 3}`, `line-${n - 2}`, `line-${n - 1}`])
    expect(tailLines(join(dir, "nope.log"), 5).text).toBe("")
  })
})

// ── status ────────────────────────────────────────────────────────────────

/** 同时兼容 win32/POSIX 的常用桩：探活、进程列表、端口监听、GPU 不可用。 */
function serviceStub(pid: number, netstatLine: string, logTailText?: string) {
  return (cmd: string): { stdout: string; stderr: string; code: number } | undefined => {
    const alive = aliveStub(pid)(cmd)
    if (alive) return alive
    if (/tasklist .*IMAGENAME|pgrep -a llama-server/.test(cmd)) return { stdout: `"llama-server.exe","${pid}","Console","1","13,000,000 K"\n`, stderr: "", code: 0 }
    if (/netstat -ano|ss -ltnp/.test(cmd)) return { stdout: netstatLine, stderr: "", code: 0 }
    if (cmd.startsWith("nvidia-smi")) return { stdout: "", stderr: "nvidia-smi: command not found", code: 127 }
    if (logTailText && cmd.includes("tail")) return { stdout: logTailText, stderr: "", code: 0 }
    return undefined
  }
}

describe("status", () => {
  test("输出实例列表（PID 存活）、端口占用者与 /props 探测（n_ctx、slot 数）", async () => {
    const home = makeHome()
    const { server, port } = startMockServer({ n_ctx: 65536, total_slots: 2 })
    try {
      writeServerState(home, { pid: 555, port, profile: "fast", model: "a.gguf", log: join(reportsDir(home), "server-x.log"), started_at: new Date().toISOString() })
      const netstat = `  TCP    127.0.0.1:${port}         0.0.0.0:0              LISTENING       555\r\n`
      const { ctx } = makeCtx(home, serviceStub(555, netstat))
      const out = await doStatus({ port }, ctx, "win32")

      expect(out).toContain("实例（服务状态文件）")
      expect(out).toContain("PID 555（存活）")
      expect(out).toContain("档位 fast")
      expect(out).toContain(`占用者 PID: 555`)
      expect(out).toContain("n_ctx=65536")
      expect(out).toContain("slots=2")
      expect(out).toContain("并发上限（slot 数）: 2")
      expect(out).toContain("OpenAI 兼容端点")
      expect(out).toContain("nvidia-smi 不可用")
    } finally {
      server.stop(true)
    }
  })

  test("状态文件陈旧（进程已退出）时明确提示并附日志疑似错误", async () => {
    const home = makeHome()
    const dir = reportsDir(home)
    mkdirSync(dir, { recursive: true })
    const log = join(dir, "server-20260101-000000.log")
    writeFileSync(log, "ggml_backend_cuda_init: found 1 CUDA devices\nllama_model_load: failed to load model from a.gguf\n")
    writeServerState(home, { pid: 7777, port: 8080, profile: "fast", model: "a.gguf", log, started_at: "2026-01-01T00:00:00" })

    const { ctx } = makeCtx(home, (cmd) => (/kill -0 |tasklist .*PID eq/.test(cmd) ? { stdout: "", stderr: "", code: 1 } : undefined))
    const out = await doStatus({}, ctx, "linux")
    expect(out).toContain("（**已退出**）")
    expect(out).toContain("状态文件陈旧")
    expect(out).toContain("疑似错误")
    expect(out).toContain("模型加载失败")
  })

  test("子项目缺失时给出可操作提示", async () => {
    const ghost = join(tmp, "no-such-home")
    const { ctx } = makeCtx(ghost)
    rmSync(ghost, { recursive: true, force: true }) // makeCtx 会建出工作目录，这里清掉以模拟子项目缺失
    const out = await doStatus({}, ctx)
    expect(out).toContain("未找到本地推理子项目")
  })
})

// ── logs ──────────────────────────────────────────────────────────────────

describe("logs", () => {
  function homeWithLogs(): { home: string; outLog: string; errLog: string } {
    const home = makeHome()
    const dir = reportsDir(home)
    mkdirSync(dir, { recursive: true })
    const outLog = join(dir, "server-20260101-120000.log")
    const errLog = `${outLog}.err`
    writeFileSync(outLog, ["loading model a.gguf", "main: server is listening on 127.0.0.1:8080", "slot released", "prompt done 12 tokens"].join("\n") + "\n")
    writeFileSync(errLog, "ggml_cuda_host_malloc: CUDA error 2 at x.cu:1\n")
    return { home, outLog, errLog }
  }

  test("list_only 只列清单（不读内容）", async () => {
    const { home } = homeWithLogs()
    const { ctx } = makeCtx(home)
    const out = await doLogs({ list_only: true }, ctx)
    expect(out).toContain("日志文件（新 → 旧）")
    expect(out).toContain("server-20260101-120000.log")
    expect(out).toContain("[out]")
    expect(out).not.toContain("prompt done 12 tokens")
  })

  test("尾读 + grep（大小写不敏感）统计命中行数", async () => {
    const { home } = homeWithLogs()
    const { ctx } = makeCtx(home)
    const out = await doLogs({ lines: 50, grep: "LOADING" }, ctx)
    expect(out).toContain('grep "LOADING"：尾部窗口内命中 1 行')
    expect(out).toContain("loading model a.gguf")
    expect(out).not.toContain("slot released")
  })

  test("kind=err 只取 stderr，并给出疑似错误摘要", async () => {
    const { home } = homeWithLogs()
    const { ctx } = makeCtx(home)
    const out = await doLogs({ kind: "err" }, ctx)
    expect(out).toContain("[err]")
    expect(out).toContain("CUDA error")
    expect(out).toContain("疑似错误")
  })

  test("缺省取最新 **stdout** 为主日志（同毫秒写入的 .log.err 不抢主位）", async () => {
    const { home, outLog } = homeWithLogs()
    // 把 err 的 mtime 推后，模拟「err 比 out 更新」——主日志仍应是 stdout
    const now = Date.now() / 1000
    utimesSync(`${outLog}.err`, now + 10, now + 10)
    const { ctx } = makeCtx(home)
    const out = await doLogs({ lines: 20 }, ctx)
    expect(out).toContain("[out]")
    expect(out).toContain("loading model a.gguf")
    expect(out).toContain("slot released")
  })

  test("port 定位实例日志（依据状态文件）", async () => {
    const { home, outLog } = homeWithLogs()
    writeServerState(home, { pid: 555, port: 9099, profile: "fast", model: "a.gguf", log: outLog, err_log: `${outLog}.err` })
    const { ctx } = makeCtx(home)
    const out = await doLogs({ port: 9099, lines: 3 }, ctx)
    expect(out).toContain("实例 :9099（PID 555，档位 fast）")
    expect(out).toContain("prompt done 12 tokens")
  })

  test("无日志时给出可操作提示", async () => {
    const { ctx } = makeCtx(makeHome())
    const out = await doLogs({}, ctx)
    expect(out).toContain("无服务日志")
  })
})

// ── stop ──────────────────────────────────────────────────────────────────

describe("stop", () => {
  test("按端口：用状态文件的 PID 终止进程树并清理状态文件", async () => {
    const home = makeHome()
    writeServerState(home, { pid: 555, port: 8080, profile: "fast", model: "a.gguf" })
    const { ctx, calls } = makeCtx(home)
    const out = await doStop({ port: 8080 }, ctx, "win32")
    expect(calls.some((c) => c.cmd === "taskkill /PID 555 /T /F")).toBe(true)
    expect(out).toContain("已停止")
    expect(out).toContain("状态文件已清理")
    expect(readServerStates(home).length).toBe(0)
  })

  test("按 PID：POSIX 用 kill -9；无匹配状态文件时如实说明", async () => {
    const { ctx, calls } = makeCtx(makeHome())
    const out = await doStop({ pid: 999 }, ctx, "linux")
    expect(calls.some((c) => c.cmd.includes("kill -9 999"))).toBe(true)
    expect(out).toContain("PID 999：已停止")
    expect(out).toContain("无匹配的实例状态文件")
  })

  test("无参数：停止全部并清理所有状态文件", async () => {
    const home = makeHome()
    writeServerState(home, { pid: 1, port: 8080, profile: "fast", model: "a.gguf" })
    writeServerState(home, { pid: 2, port: 8081, profile: "quality", model: "b.gguf" })
    const { ctx, calls } = makeCtx(home)
    const out = await doStop({}, ctx, "win32")
    expect(calls.some((c) => c.cmd === "taskkill /IM llama-server.exe /T /F")).toBe(true)
    expect(out).toContain("清理状态文件：:8080, :8081")
    expect(readServerStates(home).length).toBe(0)
  })

  test("端口未监听且无状态文件：提示无需停止", async () => {
    const { ctx } = makeCtx(makeHome())
    const out = await doStop({ port: 8123 }, ctx, "win32")
    expect(out).toContain(":8123 未被占用，无需停止。")
  })

  test("无状态文件但有端口占用者：退化按占用者 PID 停止", async () => {
    const home = makeHome()
    const netstat = "  TCP    127.0.0.1:8080         0.0.0.0:0              LISTENING       4242\r\n"
    const { ctx, calls } = makeCtx(home, (cmd) => (/netstat -ano/.test(cmd) ? { stdout: netstat, stderr: "", code: 0 } : undefined))
    const out = await doStop({ port: 8080 }, ctx, "win32")
    expect(out).toContain("占用者 PID 4242")
    expect(calls.some((c) => c.cmd === "taskkill /PID 4242 /T /F")).toBe(true)
  })

  test("按档位：列出不匹配时的现存实例", async () => {
    const home = makeHome()
    writeServerState(home, { pid: 555, port: 8080, profile: "fast", model: "a.gguf" })
    const { ctx } = makeCtx(home)
    const out = await doStop({ profile: "quality" }, ctx, "win32")
    expect(out).toContain("档位 quality 没有运行中的实例")
    expect(out).toContain(":8080 PID 555 档位 fast")
  })
})

// ── restart ───────────────────────────────────────────────────────────────

describe("restart", () => {
  test("先停后启：切换档位并等待就绪、写入状态文件", async () => {
    const home = makeHome()
    const { server, port } = startMockServer()
    try {
      const netstat = `  TCP    127.0.0.1:${port}         0.0.0.0:0              LISTENING       4242\r\n`
      const { ctx, calls } = makeCtx(home, (cmd) => {
        if (/run-server\.ps1/.test(cmd)) return { stdout: "档位: fast\n已启动 PID=4242（已脱离当前进程树）\n", stderr: "", code: 0 }
        if (/netstat -ano/.test(cmd)) return { stdout: netstat, stderr: "", code: 0 }
        return undefined
      })
      const out = await doRestart({ port, profile: "fast" }, ctx, "win32")
      expect(out).toContain("1/2 停止")
      expect(out).toContain("2/2 启动")
      expect(out).toContain("服务就绪")
      expect(out).toContain("换档位/换模型必须重启")
      expect(calls.some((c) => /run-server\.ps1/.test(c.cmd) && c.cmd.includes('-Profile "fast"'))).toBe(true)
      const st = readServerStates(home)
      expect(st.length).toBe(1)
      expect(st[0].pid).toBe(4242)
      expect(st[0].port).toBe(port)
    } finally {
      server.stop(true)
    }
  })
})

/** 取一个「刚被释放、因此几乎肯定空闲」的高位随机端口（e2e 用例用）。 */
function freePort(): number {
  const s = Bun.serve({ port: 0, fetch: () => new Response("x") })
  const port = s.port as number
  s.stop(true)
  return port
}

// ── start 的启动方式（launcher / script） ────────────────────────────────

/** 真执行命令的 ctx（e2e 用例要能真 spawn / 真探活 / 真 kill）。 */
function realCtx(home: string, env: Record<string, string>): ToolContext {
  const base = makeCtx(home).ctx
  return {
    ...base,
    env,
    runCommand: async (cmd, opts) => {
      const p = Bun.spawnSync(["sh", "-c", cmd], { cwd: opts?.workdir ?? home })
      return { stdout: p.stdout.toString(), stderr: p.stderr.toString(), code: p.exitCode ?? -1 }
    },
  }
}

/** 起一个「带引擎+模型」的临时子项目：vendor/engine/llama-server 为假 exe，模型目录可另配。 */
function makeHomeWithEngine(opts: { fakeExe: boolean; modelsDir?: string; profile?: Record<string, unknown> }): string {
  const home = mkdtempSync(join(tmp, "home-eng-"))
  const dir = join(home, "vendor", "engine")
  mkdirSync(dir, { recursive: true })
  mkdirSync(join(home, "config"), { recursive: true })
  if (opts.fakeExe) writeFileSync(join(dir, "llama-server"), "#!/bin/sh\nexit 0\n", { mode: 0o755 })
  writeFileSync(
    join(home, "config", "profiles.json"),
    JSON.stringify({
      engine_dir: "vendor/engine",
      default_profile: "fast",
      default_model: "a.gguf",
      profiles: { fast: { model: "a.gguf", ctx: 32768, n_gpu_layers: 99, n_cpu_moe: 0, parallel: 1, ...(opts.profile ?? {}) } },
    }),
  )
  return home
}

describe("start 的启动方式", () => {
  test("resolveLaunch：mode 参数 > LOCAL_INFER_LAUNCH 环境变量 > 缺省 launcher", () => {
    expect(resolveLaunch({ env: {} }).mode).toBe("launcher") // 缺省不再依赖 PowerShell
    expect(resolveLaunch({ env: {} }).forced).toBe(false)
    expect(resolveLaunch({ env: { LOCAL_INFER_LAUNCH: "script" } }).mode).toBe("script")
    expect(resolveLaunch({ env: { LOCAL_INFER_LAUNCH: "SCRIPT" } }).mode).toBe("script") // 大小写不敏感
    expect(resolveLaunch({ mode: "launcher", env: { LOCAL_INFER_LAUNCH: "script" } }).mode).toBe("launcher") // 参数优先
    expect(resolveLaunch({ mode: "不认识的", env: {} }).mode).toBe("launcher") // 非法值不报错，回缺省
  })

  test("mode=script：走 run-server.ps1（回归兼容路径）", async () => {
    const home = makeHome()
    const { server, port } = startMockServer()
    try {
      const { ctx, calls } = makeCtx(home, (cmd) => (/run-server\.ps1/.test(cmd) ? { stdout: "档位: fast\n", stderr: "", code: 0 } : undefined))
      const out = await doStart({ mode: "script", port }, ctx, "linux")
      expect(calls.some((c) => /run-server\.ps1/.test(c.cmd) && c.cmd.includes(`-Port ${port}`))).toBe(true)
      expect(out).toContain("启动方式: script")
      expect(out).toContain("服务就绪")
    } finally {
      server.stop(true)
    }
  })

  test("LOCAL_INFER_LAUNCH=script 环境变量同样强制脚本模式", async () => {
    const home = makeHome()
    const { server, port } = startMockServer()
    try {
      const { ctx, calls } = makeCtx(home, (cmd) => (/run-server\.ps1/.test(cmd) ? { stdout: "ok\n", stderr: "", code: 0 } : undefined))
      ;(ctx as { env: Record<string, string> }).env.LOCAL_INFER_LAUNCH = "script"
      await doStart({ port }, ctx, "linux")
      expect(calls.some((c) => /run-server\.ps1/.test(c.cmd))).toBe(true)
    } finally {
      server.stop(true)
    }
  })

  test("launcher 模式无引擎：给出可操作提示（local_infer_engines / engine_fetch），不写状态文件", async () => {
    const home = makeHomeWithEngine({ fakeExe: false })
    const { ctx, calls } = makeCtx(home)
    const out = await doStart({ port: 8123 }, ctx, "linux")
    expect(out).toContain("没有可用的 llama-server")
    expect(out).toContain("local_infer_engines")
    expect(out).toContain("local_infer_engine_fetch")
    expect(readServerStates(home).length).toBe(0)
    expect(calls.length).toBe(0) // 引擎都没找到，不该去跑任何命令
  })

  test("Windows 上无引擎时自动回退 run-server.ps1（显式 mode=launcher 则不回退）", async () => {
    const { server, port } = startMockServer()
    try {
      const home = makeHomeWithEngine({ fakeExe: false })
      const netstat = `  TCP    127.0.0.1:${port}         0.0.0.0:0              LISTENING       4242\r\n`
      const { ctx, calls } = makeCtx(home, (cmd) => {
        if (/run-server\.ps1/.test(cmd)) return { stdout: "已启动 PID=4242\n", stderr: "", code: 0 }
        if (/netstat -ano/.test(cmd)) return { stdout: netstat, stderr: "", code: 0 }
        return undefined
      })
      const out = await doStart({ port }, ctx, "win32")
      expect(out).toContain("回退 run-server.ps1 兼容路径")
      expect(calls.some((c) => /run-server\.ps1/.test(c.cmd))).toBe(true)
      expect((readServerStates(home)[0] as { launch_mode?: string }).launch_mode).toBe("script")

      // 显式要求 launcher 时不当软脚虾回退（宁可报清楚没引擎）
      const home2 = makeHomeWithEngine({ fakeExe: false })
      const { ctx: ctx2, calls: calls2 } = makeCtx(home2, () => undefined)
      const out2 = await doStart({ port, mode: "launcher" }, ctx2, "win32")
      expect(out2).toContain("没有可用的 llama-server")
      expect(calls2.some((c) => /run-server\.ps1/.test(c.cmd))).toBe(false)
    } finally {
      server.stop(true)
    }
  })

  test("launcher 模式：模型缺失与端口被占用都提前说清楚", async () => {
    const models = mkdtempSync(join(tmp, "models-"))
    const home = makeHomeWithEngine({ fakeExe: true, modelsDir: models })
    const env = { LOCAL_INFER_HOME: home, LOCAL_INFER_MODELS_DIR: models }

    // ① 引擎在、模型缺
    const miss = makeCtx(home)
    ;(miss.ctx as { env: Record<string, string> }).env = env
    const outMiss = await doStart({ port: 8123 }, miss.ctx, "linux")
    expect(outMiss).toContain("找不到模型文件")

    // ② 引擎/模型都在、端口已被占用（POSIX ss）
    writeFileSync(join(models, "a.gguf"), "x")
    const ss = `LISTEN 0 4096 127.0.0.1:8123 0.0.0.0:* users:(("llama-server",pid=4242,fd=9))\n`
    const busy = makeCtx(home, (cmd) => (/ss -ltnp/.test(cmd) ? { stdout: ss, stderr: "", code: 0 } : undefined))
    ;(busy.ctx as { env: Record<string, string> }).env = env
    const outBusy = await doStart({ port: 8123 }, busy.ctx, "linux")
    expect(outBusy).toContain("端口 :8123 已被占用（PID 4242）")
    expect(outBusy).toContain("local_infer_stop")
  })

  test("launcher 模式全链路：真 spawn 假 llama-server → 写状态文件 → 就绪", async () => {
    if (process.platform === "win32") return // 假 exe 用 sh + chmod，仅 POSIX
    const models = mkdtempSync(join(tmp, "models-e2e-"))
    writeFileSync(join(models, "a.gguf"), "x")
    const home = makeHomeWithEngine({ fakeExe: false, profile: { ctx: 4096, batch: 1024, extra_args: ["--jinja", "-bs", "--cont-batching"] } })
    // 假引擎：忽略 llama.cpp 参数，按 --port 起一个 /health 端点，并把 argv 落到文件（验证真有人把参数传进来）
    const js = join(home, "fake-server.cjs")
    const argvFile = join(home, "argv.json")
    const exe = join(home, "vendor", "engine", "llama-server")
    writeFileSync(
      js,
      "const argv=process.argv.slice(2);const i=argv.indexOf('--port');const port=Number(argv[i+1]);" +
        "require('fs').writeFileSync(argv[argv.length-1],JSON.stringify(argv));" +
        "const s=Bun.serve({port,fetch:(r)=>{const ok=new URL(r.url).pathname==='/health';return new Response(ok?'ok':'nf',{status:ok?200:404})}});" +
        "console.log('fake llama-server up');setInterval(()=>{},1000);\n",
    )
    // 假引擎：忽略 llama.cpp 的其它参数，按 --port 起一个 /health 端点，并把收到的 argv 落盘
    writeFileSync(
      exe,
      "#!/bin/sh\nport=\"\"\nprev=\"\"\nfor a in \"$@\"; do if [ \"$prev\" = \"--port\" ]; then port=\"$a\"; fi; prev=\"$a\"; done\n" +
        `exec "${process.execPath}" "${js}" --port "$port" "${argvFile}"\n`,
      { mode: 0o755 },
    )
    const env = { LOCAL_INFER_HOME: home, LOCAL_INFER_MODELS_DIR: models }
    const ctx = realCtx(home, env)
    const port = freePort()
    let pid = 0
    try {
      const out = await doStart({ port }, ctx, "linux")
      expect(out).toContain("启动方式: launcher")
      expect(out).toContain("服务就绪")
      const st = readServerStates(home)[0] as { pid: number; engine?: string; launch_mode?: string; log?: string; argv?: string[] } | undefined
      expect(st).toBeTruthy()
      pid = st?.pid ?? 0
      expect(pid).toBeGreaterThan(0)
      expect(st?.engine).toBe("engine")
      expect(st?.launch_mode).toBe("launcher")
      expect(st?.argv?.[0]).toBe(exe)
      expect(st?.argv).toContain("--host")
      expect(st?.argv).toContain("--port")
      expect(st?.argv).toContain(`-np`)
      expect(st?.argv).toContain("--cont-batching") // 档位 extra_args 里的开关由专用标志产出（不重复）
      expect(st?.argv?.filter((a) => a === "-bs").length).toBe(1)
      expect(st?.argv).toContain("--jinja")
      // 真进程真拿到了这些参数（假引擎把它落盘了）
      expect(JSON.parse(readFileSync(argvFile, "utf-8")) as string[]).toContain(`${port}`)
      // 日志被写入（stdout 重定向）
      expect(tailLines(st?.log ?? "", 20).text).toContain("fake llama-server up")
    } finally {
      if (pid > 0) {
        await killPid(pid, ctx, "linux") // 走 launcher 的 killPid（POSIX kill -9）收尾
        try {
          process.kill(pid, "SIGKILL")
        } catch {
          /* 已退出 */
        }
      }
      rmSync(home, { recursive: true, force: true })
      rmSync(models, { recursive: true, force: true })
    }
  })
})

// ── status 的引擎信息 ─────────────────────────────────────────────────────

describe("status 引擎信息", () => {
  test("显示状态文件里的引擎/启动方式，并核对引擎是否仍安装", async () => {
    const home = makeHome()
    // 造一个已安装引擎（.engine.json + 假 exe）
    const dir = join(home, "vendor", "linux-cpu-x64")
    mkdirSync(dir, { recursive: true })
    const exe = join(dir, "llama-server")
    writeFileSync(exe, "#!/bin/sh\n")
    writeFileSync(join(dir, ".engine.json"), JSON.stringify({ id: "linux-cpu-x64", exe, tag: "b1", asset: "a", installed_at: "2026-01-01", dir }))
    writeServerState(home, { pid: 555, port: 8080, profile: "fast", model: "a.gguf", engine: "linux-cpu-x64", launch_mode: "launcher" } as never)
    const { ctx } = makeCtx(home)
    const out = await doStatus({}, ctx, "linux")
    expect(out).toContain("── 引擎 ──")
    expect(out).toContain("当前: linux-cpu-x64")
    expect(out).toContain("启动方式: launcher")
    expect(out).toContain(exe)
    expect(out).toContain("已安装引擎")
  })

  test("引擎被删（状态文件还指着）时如实报「未找到」并给下一步", async () => {
    const home = makeHome()
    writeServerState(home, { pid: 555, port: 8080, profile: "fast", model: "a.gguf", engine: "win-cuda-12.4-x64" } as never)
    const { ctx } = makeCtx(home)
    const out = await doStatus({}, ctx, "linux")
    expect(out).toContain("当前: win-cuda-12.4-x64")
    expect(out).toContain("未找到")
    expect(out).toContain("local_infer_engine_fetch")
  })

  test("engineIdFromState：引擎 id 原样，旧的 exe 全路径取末级目录名", () => {
    expect(engineIdFromState({ engine: "linux-cpu-x64" } as never)).toBe("linux-cpu-x64")
    expect(engineIdFromState({ engine: "C:\\infer\\vendor\\llama-b1\\llama-server.exe" } as never)).toBe("llama-b1")
    expect(engineIdFromState({ engine: "/x/vendor/llama-b1/llama-server" } as never)).toBe("llama-b1")
    expect(engineIdFromState({} as never)).toBeUndefined()
  })
})

// ── 工具契约 ──────────────────────────────────────────────────────────────

describe("工具契约", () => {
  test("八个工具齐备，命名与参数符合规范", () => {
    expect(Object.keys(tools).sort()).toEqual(["bench", "inspect", "logs", "models", "restart", "start", "status", "stop"])
    for (const [name, tool] of Object.entries(tools)) {
      expect(name).toMatch(/^[a-zA-Z0-9_]+$/)
      expect(tool.name).toBe(name)
      expect(tool.description.length).toBeGreaterThan(10)
      for (const [k, v] of Object.entries(tool.parameters.properties ?? {})) {
        expect(k).toMatch(/^[a-z][a-z0-9_]*$/)
        expect(String((v as { description?: string }).description ?? "").length).toBeGreaterThan(0)
      }
    }
  })

  test("审批策略：改变系统状态的工具需审批，只读工具免审批", () => {
    expect(requiresApproval).toEqual({ start: true, stop: true, restart: true, bench: true })
    for (const t of ["status", "models", "logs", "inspect"]) expect(tools[t].requiresApproval).toBeUndefined()
    for (const t of ["start", "stop", "restart", "bench"]) expect(tools[t].requiresApproval).toBe(true)
  })
})

describe("引擎标识与模型别名（真机验证暴露的缺陷回归）", () => {
  test("inferEngineIdFromExe：取 vendor/ 下的一级目录名（引擎 id），而非归档内目录名", () => {
    const home = makeHome()
    // 引擎归档内部会套一层发行版目录（linux 归档为 <tag>/，windows 在 build/bin 下）
    expect(inferEngineIdFromExe(home, join(home, "vendor", "linux-cpu-x64", "llama-b11175", "llama-server"))).toBe("linux-cpu-x64")
    expect(inferEngineIdFromExe(home, join(home, "vendor", "win-cuda-12.4-x64", "llama-server.exe"))).toBe("win-cuda-12.4-x64")
    expect(inferEngineIdFromExe(home, "C:\\other\\llama-server.exe")).toBeUndefined()
    expect(inferEngineIdFromExe(home, join(home, "vendor", ".cache", "x", "llama-server"))).toBeUndefined()
  })

  test("engineIdFromState：兼容旧脚本写入的 exe 全路径", () => {
    expect(engineIdFromState({ engine: "linux-cpu-x64" } as never)).toBe("linux-cpu-x64")
    expect(engineIdFromState({ engine: "/x/vendor/linux-cpu-x64/llama-b11175/llama-server" } as never)).toBe("llama-b11175")
    expect(engineIdFromState({} as never)).toBeUndefined()
  })

  test("resolveAlias：档位 alias > 模型名推导 > 缺省（不能把小模型也叫成 35B）", () => {
    expect(resolveAlias("my-alias", "whatever.gguf")).toBe("my-alias")
    expect(resolveAlias(undefined, "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf")).toBe("qwen2.5-0.5b-instruct")
    expect(resolveAlias(undefined, "Qwen-AgentWorld-35B-A3B-UD-IQ3_XXS.gguf")).toBe("qwen-agentworld-35b-a3b")
    expect(resolveAlias(undefined, "/abs/path/Model.gguf")).toBe("model")
    // 推导不出（如只有扩展名）时回退到官方别名
    expect(resolveAlias(undefined, ".gguf")).toBe("agentworld-35b-a3b")
  })
})
