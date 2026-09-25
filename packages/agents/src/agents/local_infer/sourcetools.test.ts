/**
 * sourcetools（内网源码发现与自动编译的**工具层**）测试：契约、发现/工具链输出、编译任务的
 * 状态机（status/list/cancel）与失败快路径。
 *
 * 环境封闭：临时 infer home（含 ../resources/src 源码结构）+ 假 ctx；
 * **不跑真实编译**（真实编译的端到端验证另见 infer 子项目的实机验证报告）。
 */
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join } from "node:path"
import { afterAll, describe, expect, test } from "bun:test"
import type { ToolContext } from "@gebai/sdk"
import { requiresApproval, tools } from "./sourcetools"

const tmp = mkdtempSync(join(tmpdir(), "gebai-sourcetools-"))
afterAll(() => rmSync(tmp, { recursive: true, force: true }))

/** 造一个「{GEBAI_HOME}/infer」形态的临时子项目根（发现根 resources/src 位于其同级）。 */
function makeHome(): { home: string; srcRoot: string } {
  const gebai = join(tmp, `g-${Math.random().toString(36).slice(2)}`)
  const home = join(gebai, "infer")
  const srcRoot = join(gebai, "resources", "src")
  mkdirSync(join(home, "config"), { recursive: true })
  mkdirSync(srcRoot, { recursive: true })
  return { home, srcRoot }
}

interface CmdCall {
  cmd: string
}

function makeCtx(home: string, run?: (cmd: string) => { stdout: string; stderr: string; code: number } | undefined) {
  const calls: CmdCall[] = []
  const workdir = join(home, "..", ".work")
  mkdirSync(workdir, { recursive: true })
  const ctx = {
    user: "u",
    sessionId: "s",
    workdir,
    sessionWorkdir: workdir,
    home,
    env: { LOCAL_INFER_HOME: home },
    sandboxed: false,
    resolvePath: (p: string) => (p.startsWith("/") ? p : join(workdir, p)),
    readFile: async (p: string) => Bun.file(p).text(),
    readBinaryFile: async (p: string) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p: string, c: string) => void (await Bun.write(p, c)),
    writeBinaryFile: async () => {},
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    async runCommand(cmd: string) {
      calls.push({ cmd })
      const stub = run?.(cmd)
      if (stub) return stub
      // 缺省：什么都不认（探测类命令返回非 0），避免测试误依赖宿主工具链
      return { stdout: "", stderr: "", code: 127 }
    },
    uploadAttachment: async () => "",
    publish: () => {},
    projects: [],
    resolveProjectPath: () => home,
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => null,
  } as unknown as ToolContext
  return { ctx, calls }
}

describe("工具契约", () => {
  test("两个工具齐备，参数蛇形、描述含内网语义、审批与安全模式声明正确", () => {
    expect(Object.keys(tools).sort()).toEqual(["source_build", "sources"])
    for (const [name, tool] of Object.entries(tools)) {
      expect(tool.name).toBe(name)
      expect(tool.description.length).toBeGreaterThan(20)
      for (const [k, v] of Object.entries(tool.parameters.properties ?? {})) {
        expect(k).toMatch(/^[a-z][a-z0-9_]*$/)
        expect(String((v as { description?: string }).description ?? "").length).toBeGreaterThan(0)
      }
    }
    // 发现/探测只读 → 安全模式可用；编译会调编译器并写盘 → 需审批且安全模式不提供
    expect(tools.sources.safeMode).toBe(true)
    expect(tools.source_build.safeMode).toBe(false)
    expect(tools.source_build.requiresApproval).toBe(true)
    expect(requiresApproval).toEqual({ source_build: true })
    expect(tools.sources.outputSchema).toBeDefined()
    expect(tools.source_build.parameters.required).toEqual(["action"])
  })

  test("子项目根缺失时给出可操作提示（不抛错）", async () => {
    const { ctx } = makeCtx(join(tmp, "nope"))
    for (const name of ["sources", "source_build"] as const) {
      const r = await tools[name].execute({ action: name === "sources" ? "list" : "list" }, ctx)
      expect(r.output).toContain("未找到本地推理子项目")
    }
  })
})

describe("sources：发现与工具链", () => {
  test("list：报出发现根（存在与否）与候选（含 llama.cpp 目录的离线可行性）", async () => {
    const { home, srcRoot } = makeHome()
    const proj = join(srcRoot, "llama.cpp-b9")
    mkdirSync(join(proj, "vendor", "nlohmann"), { recursive: true })
    writeFileSync(join(proj, "CMakeLists.txt"), "cmake_minimum_required(VERSION 3.14)\nproject(llama.cpp VERSION 9.9)\n")
    const { ctx } = makeCtx(home)
    const r = await tools.sources.execute({ action: "list" }, ctx)
    expect(r.output).toContain("源码发现")
    expect(r.output).toContain("llama.cpp-b9")
    expect(r.output).toContain("离线可编译") // 目录候选自带 vendor/ 时如实标注可编译
    const data = r.data as { count: number; candidates: Array<{ id: string; kind: string }>; roots: Array<{ exists: boolean }> }
    expect(data.count).toBe(1)
    expect(data.candidates[0].id).toBe("llama.cpp-b9")
    expect(data.candidates[0].kind).toBe("llama_cpp")
    expect(data.roots.some((x) => x.exists)).toBe(true)
  })

  test("list：无候选时给出「放哪儿 + 怎么拉源码」的指引", async () => {
    const { home } = makeHome()
    const { ctx } = makeCtx(home)
    const r = await tools.sources.execute({ action: "list" }, ctx)
    expect(r.output).toContain("（无）")
    expect(r.output).toContain("LOCAL_INFER_SOURCE_DIRS")
    expect(r.output).toContain("resources:download")
  })

  test("toolchain：工具缺失时给内网补齐办法，且如实报不可编译后端", async () => {
    const { home } = makeHome()
    const { ctx } = makeCtx(home) // 假 runCommand 全 127 → 什么都探不到
    const r = await tools.sources.execute({ action: "toolchain" }, ctx)
    expect(r.output).toContain("缺失")
    expect(r.output).toContain("内网补齐提示")
    expect(r.output).toContain("可编译后端：无")
    const data = r.data as { ready: string[] }
    expect(data.ready).toEqual([])
  })

  test("toolchain：工具就绪时给出可编译后端与下一步", async () => {
    const { home } = makeHome()
    const { ctx } = makeCtx(home, (cmd) => {
      if (/cmake --version/.test(cmd)) return { stdout: "cmake version 3.28.3\n", stderr: "", code: 0 }
      if (/ninja --version/.test(cmd)) return { stdout: "1.11.1\n", stderr: "", code: 0 }
      if (/(^|\s)(cc|gcc) --version/.test(cmd)) return { stdout: "cc (GCC) 13.3.0\n", stderr: "", code: 0 }
      if (cmd.includes("command -v cmake")) return { stdout: "/usr/bin/cmake\n", stderr: "", code: 0 }
      return { stdout: "", stderr: "", code: 127 }
    })
    const r = await tools.sources.execute({ action: "toolchain" }, ctx)
    expect(r.output).toContain("cmake version 3.28.3")
    expect(r.output).toContain("→ /usr/bin/cmake") // 工具绝对路径（便携工具链不必进 PATH）
    expect(r.output).toContain("cpu")
  })

  test("inspect：缺 id 时提示先 list；未知 id 时列出可用清单", async () => {
    const { home, srcRoot } = makeHome()
    mkdirSync(join(srcRoot, "llama.cpp-x"), { recursive: true })
    writeFileSync(join(srcRoot, "llama.cpp-x", "CMakeLists.txt"), "project(llama.cpp)\n")
    const { ctx } = makeCtx(home)
    const noId = await tools.sources.execute({ action: "inspect" }, ctx)
    expect(noId.output).toContain("需要 id")
    const unknown = await tools.sources.execute({ action: "inspect", id: "不存在的源码" }, ctx)
    expect(unknown.output).toContain("既不是存在的路径") // 可操作错误：说清为什么找不到 + 该怎么办
  })

  test("inspect：llama.cpp 候选给出离线可行性与可编译后端", async () => {
    const { home, srcRoot } = makeHome()
    const proj = join(srcRoot, "llama.cpp-y")
    mkdirSync(join(proj, "vendor", "stb"), { recursive: true })
    writeFileSync(join(proj, "CMakeLists.txt"), "project(llama.cpp VERSION 1.2.3)\n")
    const { ctx } = makeCtx(home, (cmd) =>
      /cmake --version/.test(cmd) ? { stdout: "cmake version 3.28.3\n", stderr: "", code: 0 } : { stdout: "", stderr: "", code: 127 },
    )
    const r = await tools.sources.execute({ action: "inspect", id: "llama.cpp-y" }, ctx)
    expect(r.output).toContain("离线可行性")
    expect(r.output).toContain("1.2.3")
  })
})

describe("source_build：任务状态机", () => {
  function writeJob(home: string, id: string, patch: Record<string, unknown> = {}): void {
    const dir = join(home, "run", "builds")
    mkdirSync(dir, { recursive: true })
    writeFileSync(
      join(dir, `${id}.json`),
      `${JSON.stringify({
        build_id: id,
        source: "llama.cpp-b1",
        engine_id: "linux-cpu-src",
        device: "cpu",
        jobs: 4,
        offline: true,
        clean: false,
        state: "done",
        phase: "done",
        steps: [{ phase: "build", message: "build 完成（10.0s）", at: new Date().toISOString() }],
        started_at: "2026-09-25T00:00:00.000Z",
        updated_at: "2026-09-25T00:01:00.000Z",
        owner_pid: process.pid,
        ...patch,
      })}\n`,
      "utf-8",
    )
  }

  test("list：列出任务与状态；空时给编译用法", async () => {
    const { home } = makeHome()
    const { ctx } = makeCtx(home)
    const empty = await tools.source_build.execute({ action: "list" }, ctx)
    expect(empty.output).toContain("暂无编译任务记录")
    writeJob(home, "build-a-20260101-000000")
    const r = await tools.source_build.execute({ action: "list" }, ctx)
    expect(r.output).toContain("build-a-20260101-000000")
    expect(r.output).toContain("linux-cpu-src")
    expect((r.data as { count: number }).count).toBe(1)
  })

  test("status：进行中（本进程属主）如实报「进行中」", async () => {
    const { home } = makeHome()
    writeJob(home, "build-run", { state: "running", phase: "build" })
    const { ctx } = makeCtx(home)
    const r = await tools.source_build.execute({ action: "status", build_id: "build-run" }, ctx)
    expect(r.output).toContain("running（进行中）")
    expect((r.data as { live: boolean }).live).toBe(true)
  })

  test("status：属主进程已退出 → 改判失败并说明（与批量同款存活判定）", async () => {
    const { home } = makeHome()
    writeJob(home, "build-dead", { state: "running", phase: "configure", owner_pid: 999999 })
    const { ctx } = makeCtx(home, () => ({ stdout: "", stderr: "", code: 1 })) // kill -0 失败 = 进程不在
    const r = await tools.source_build.execute({ action: "status", build_id: "build-dead" }, ctx)
    expect(r.output).toContain("failed")
    expect(r.output).toContain("执行进程已退出")
    // 幂等：再查一次仍为 failed，不重复改写
    const again = readFileSync(join(home, "run", "builds", "build-dead.json"), "utf-8")
    expect(JSON.parse(again).state).toBe("failed")
  })

  test("status：未知 id 时列出已知任务", async () => {
    const { home } = makeHome()
    writeJob(home, "build-known")
    const { ctx } = makeCtx(home)
    const r = await tools.source_build.execute({ action: "status", build_id: "build-unknown" }, ctx)
    expect(r.output).toContain("未找到编译任务")
    expect(r.output).toContain("build-known")
  })

  test("log：无日志文件时如实说明阶段；有日志时给尾部", async () => {
    const { home } = makeHome()
    writeJob(home, "build-nolog", { state: "running", phase: "configure" })
    const { ctx } = makeCtx(home)
    const r1 = await tools.source_build.execute({ action: "log", build_id: "build-nolog" }, ctx)
    expect(r1.output).toContain("尚未产生构建日志")

    const logFile = join(home, "bench", "reports", "build-x.log")
    mkdirSync(dirname(logFile), { recursive: true })
    writeFileSync(logFile, Array.from({ length: 100 }, (_, i) => `line ${i + 1}`).join("\n"))
    writeJob(home, "build-withlog", { state: "failed", log: logFile })
    const r2 = await tools.source_build.execute({ action: "log", build_id: "build-withlog", lines: 3 }, ctx)
    expect(r2.output).toContain("line 100")
    expect(r2.output).toContain("line 98")
    expect(r2.output).not.toContain("line 97")
  })

  test("cancel：未完成的任务写取消标记（阶段之间生效，不强杀 cmake）", async () => {
    const { home } = makeHome()
    writeJob(home, "build-cancel", { state: "running", phase: "build" })
    const { ctx } = makeCtx(home)
    const r = await tools.source_build.execute({ action: "cancel", build_id: "build-cancel" }, ctx)
    expect(r.output).toContain("已写入取消标记")
    expect(r.output).toContain("阶段之间")
    expect(existsSync(join(home, "run", "builds", "build-cancel.cancel"))).toBe(true)
    // 已结束的任务无需取消
    writeJob(home, "build-done", { state: "done" })
    const r2 = await tools.source_build.execute({ action: "cancel", build_id: "build-done" }, ctx)
    expect(r2.output).toContain("无需取消")
  })

  test("cancel：属主已退出时直接置 cancelled（不留下永远的 running）", async () => {
    const { home } = makeHome()
    writeJob(home, "build-orphan", { state: "running", phase: "build", owner_pid: 999999 })
    const { ctx } = makeCtx(home)
    const r = await tools.source_build.execute({ action: "cancel", build_id: "build-orphan" }, ctx)
    expect(r.output).toContain("cancelled")
    expect(JSON.parse(readFileSync(join(home, "run", "builds", "build-orphan.json"), "utf-8")).state).toBe("cancelled")
  })

  test("build：参数与源码校验在动编译器前失败（缺 source / 未知 device / 找不到源码）", async () => {
    const { home } = makeHome()
    const { ctx, calls } = makeCtx(home)
    const noSource = await tools.source_build.execute({ action: "build" }, ctx)
    expect(noSource.output).toContain("需要 source")
    const badDevice = await tools.source_build.execute({ action: "build", source: "x", device: "tpu" }, ctx)
    expect(badDevice.output).toContain("未知设备后端")
    const missing = await tools.source_build.execute({ action: "build", source: "不存在的源码" }, ctx)
    expect(missing.output).toContain("既不是存在的路径")
    // 三种失败都不该跑任何编译命令
    expect(calls.length).toBe(0)
  })

  test("build：源码目录无构建入口时快速失败，且任务状态如实落盘为 failed", async () => {
    const { home, srcRoot } = makeHome()
    const proj = join(srcRoot, "not-a-project")
    mkdirSync(proj, { recursive: true })
    writeFileSync(join(proj, "README.md"), "nothing to build\n")
    const { ctx } = makeCtx(home)
    const r = await tools.source_build.execute({ action: "build", source: "not-a-project", engine_id: "test-fail", offline: true }, ctx)
    expect(r.output).toContain("编译未完成")
    const data = r.data as { build_id: string; state: string }
    expect(data.state).toBe("failed")
    const st = JSON.parse(readFileSync(join(home, "run", "builds", `${data.build_id}.json`), "utf-8"))
    expect(st.state).toBe("failed")
    expect(st.error).toBeTruthy()
  })

  test("build：background=true 立即返回 build_id 并留下 running 状态（不阻塞）", async () => {
    const { home, srcRoot } = makeHome()
    const proj = join(srcRoot, "llama.cpp-bg")
    mkdirSync(join(proj, "vendor", "x"), { recursive: true })
    writeFileSync(join(proj, "CMakeLists.txt"), "project(llama.cpp)\n")
    const { ctx } = makeCtx(home)
    const t0 = Date.now()
    const r = await tools.source_build.execute({ action: "build", source: "llama.cpp-bg", engine_id: "bg-test", background: true }, ctx)
    const elapsed = Date.now() - t0
    expect(elapsed).toBeLessThan(3000)
    expect(r.output).toContain("已后台启动")
    const data = r.data as { build_id: string; background: boolean }
    expect(data.background).toBe(true)
    expect(r.output).toContain(data.build_id)
    // 状态文件已落盘（后台任务在场，稍后由其自身收尾——本用例不等待它编译完）
    const stPath = join(home, "run", "builds", `${data.build_id}.json`)
    expect(existsSync(stPath)).toBe(true)
  })
})
