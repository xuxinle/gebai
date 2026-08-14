import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname, resolve } from "node:path"
import { readTool, writeTool, editTool, currentTimeTool, systemInfoTool, shTool, pyTool, drawTool, pageCaptureTool, renderHtmlTool, saveTool, deleteTool, normalizePlantUml, injectPlantUmlLayout, truncate, sliceLines, spillLongUserInput, USER_INPUT_SPILL_THRESHOLD, makePreviewServerTool, makeCronTools, assertPublicHttpUrl, fetchWithRedirectGuard, envDetectTool, applyPatchTool, gitTool, agentListTool, agentLoadTool } from "./tools"
import { createGlobalTools, resolvePythonCmd, _resetPythonCmdCache } from "./tools"
import { searchSymbolsTool } from "./analyzer"
import { resolveInSandbox, sessionPath } from "./paths"
import { getMiniTool } from "./mini-tools"
import type { ToolContext } from "./types"

function ctx(home: string, sessionId = "s1", env: Record<string, string> = {}): ToolContext {
  const base = home || tmpdir()
  const tmp = join(base, "users", "default", "sessions", sessionId, "tmp")
  mkdirSync(tmp, { recursive: true })
  return {
    user: "default",
    sessionId,
    workdir: tmp,
    home: base,
    env,
    sandboxed: false,
    // 与真实引擎（沙箱关闭）一致：resolve 对绝对路径直接采用，相对路径基于会话根解析
    resolvePath: (p) => resolve(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, content)
    },
    writeBinaryFile: async (p, data) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, data)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async (cmd, o) => {
      const { spawn } = await import("node:child_process")
      return new Promise((resolve) => {
        const child = spawn(cmd, { cwd: o?.workdir, shell: true, stdio: ["pipe", "pipe", "pipe"] })
        let stdout = ""
        let stderr = ""
        if (o?.input != null) child.stdin.write(o.input)
        child.stdin.end()
        child.stdout.on("data", (d) => (stdout += d.toString()))
        child.stderr.on("data", (d) => (stderr += d.toString()))
        child.on("close", (code) => resolve({ stdout, stderr, code: code ?? 1 }))
      })
    },
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => { throw new Error("未知预置项目") },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    runNewSession: async () => ({ output: "ok", archive: { runId: "r", agents: ["x"], input: "", output: "ok", messages: [] } }),
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
    waitForCapture: async () => null,
  }
}

describe("global tools", () => {
  test("env_detect reports toolchain versions, unavailable markers and dedups PATH", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-envdetect-"))
    const c = ctx(home, "s1", { PATH: "C:\\a;C:\\b;C:\\A;C:\\a;D:\\c" })
    c.runCommand = async (cmd) => {
      if (cmd.includes("node --version")) return { stdout: "v20.17.0\n", stderr: "", code: 0 }
      if (cmd.includes("cargo --version")) return { stdout: "", stderr: "cargo: command not found", code: 1 }
      if (cmd.includes("go version")) return { stdout: "", stderr: "", code: 0 }
      return { stdout: "1.0.0\n", stderr: "", code: 0 }
    }
    const r = await envDetectTool.execute({}, c)
    expect(r.output).toContain("环境探测（")
    expect(r.output).toContain("node: v20.17.0")
    expect(r.output).toContain("cargo: 不可用（exit 1）")
    expect(r.output).toContain("go: （成功但无版本输出）")
    // PATH 去重（Windows 大小写不敏感）：C:\a 与 C:\A 视为重复
    expect(r.output).toContain("PATH（去重 3/5 项）")
    expect(r.output).toContain("  - C:\\a")
    expect(r.output).toContain("  - C:\\b")
    expect(r.output).toContain("  - D:\\c")
    rmSync(home, { recursive: true, force: true })
  })

  test("sh tool reports success-with-no-output explicitly", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sh-empty-"))
    const c = ctx(home)
    c.runCommand = async () => ({ stdout: "", stderr: "", code: 0 })
    const r = await shTool.execute({ command: "echo nothing" }, c)
    expect(r.output).toBe("（命令执行成功，无输出）")
    rmSync(home, { recursive: true, force: true })
  })

  test("sh tool timeout param passes through, defaults 300s and clamps to 540s", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sh-timeout-"))
    const c = ctx(home)
    const seen: Array<number | undefined> = []
    c.runCommand = async (_cmd, o) => {
      seen.push(o?.timeoutMs)
      return { stdout: "ok", stderr: "", code: 0 }
    }
    await shTool.execute({ command: "echo hi", timeout: 10 }, c)
    await shTool.execute({ command: "echo hi" }, c)
    await shTool.execute({ command: "echo hi", timeout: 99999 }, c)
    await shTool.execute({ command: "echo hi", timeout: 0 }, c)
    await shTool.execute({ command: "echo hi", timeout: "abc" }, c)
    expect(seen).toEqual([10000, 300000, 540000, 300000, 300000])
    rmSync(home, { recursive: true, force: true })
  })

  test("py tool timeout param passes through to runCommand", async () => {
    _resetPythonCmdCache()
    const home = mkdtempSync(join(tmpdir(), "gebai-py-timeout-"))
    const c = ctx(home)
    let timeoutMs: number | undefined
    c.runCommand = async (cmd, o) => {
      if (cmd.endsWith("--version")) return { stdout: "Python 3.12.0\n", stderr: "", code: 0 }
      timeoutMs = o?.timeoutMs
      return { stdout: "hi", stderr: "", code: 0 }
    }
    const r = await pyTool.execute({ code: "print('hi')", timeout: 30 }, c)
    expect(r.output).toBe("hi")
    expect(timeoutMs).toBe(30000)
    rmSync(home, { recursive: true, force: true })
  })

  test("truncate writes full content to session tmp/truncated/ with logical path", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-trunc-"))
    const sid = "0123456789abcdef0123456789abcdef" // 合法会话 id（32 位 hex）
    const c = ctx(home, sid)
    const big = "x".repeat(15000)
    const r = await truncate(big, "read", c)
    expect(r.truncated).toBe(true)
    expect(r.filePath).toMatch(/^tmp\/truncated\/read_[0-9a-f]{64}\.txt$/)
    // 落盘位置：会话根/tmp/truncated/（会话根含分片目录）
    const abs = join(sessionPath(home, "default", sid), r.filePath!)
    expect(await Bun.file(abs).text()).toBe(big)
    // 沙箱模式下模型可经 resolveInSandbox 读取同一逻辑路径
    const safe = resolveInSandbox(sessionPath(home, "default", sid), r.filePath!)
    expect(await Bun.file(safe).text()).toBe(big)
    // 短内容不截断
    const short = await truncate("short", "read", c)
    expect(short.truncated).toBeUndefined()
    expect(short.output).toBe("short")
  })

  test("write + read roundtrip", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-"))
    const c = ctx(home)
    const w = await writeTool.execute({ path: "a.txt", content: "hello" }, c)
    expect(w.blocks?.[0].type).toBe("file")
    const r = await readTool.execute({ path: "a.txt" }, c)
    expect(r.output).toBe("hello")
    cleanup(home)
  })

  test("read with offset/limit slices by line", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-"))
    const c = ctx(home)
    await writeTool.execute({ path: "m.txt", content: "l1\nl2\nl3\nl4\nl5\n" }, c)
    expect((await readTool.execute({ path: "m.txt", offset: 3, limit: 2 }, c)).output).toBe("l3\nl4")
    expect((await readTool.execute({ path: "m.txt", limit: 2 }, c)).output).toBe("l1\nl2")
    expect((await readTool.execute({ path: "m.txt", limit: -2 }, c)).output).toBe("l4\nl5")
    expect((await readTool.execute({ path: "m.txt", offset: 100, limit: 2 }, c)).output).toBe("")
    expect((await readTool.execute({ path: "m.txt", offset: 1, limit: 5 }, c)).output).toBe("l1\nl2\nl3\nl4\nl5\n")
    // sliceLines 纯函数边界：无参数原样返回；无尾换行文件的末尾切片
    expect(sliceLines("a\nb", undefined, -1)).toBe("b")
    expect(sliceLines("x")).toBe("x")
    cleanup(home)
  })

  test("read image path returns image block", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-img-"))
    const c = ctx(home)
    await writeTool.execute({ path: "plot.png", content: "x" }, c)
    const r = await readTool.execute({ path: "plot.png" }, c)
    expect(r.blocks).toHaveLength(1)
    expect(r.blocks![0].type).toBe("image")
    cleanup(home)
  })

  test("normalizePlantUml handles @startmindmap/@startwbs and fills missing @end", () => {
    // 无包裹 → 自动补全 @startuml/@enduml
    expect(normalizePlantUml("Alice -> Bob")).toBe("@startuml\nAlice -> Bob\n@enduml")
    // 已含 @startuml 保留原样
    expect(normalizePlantUml("@startuml\nAlice -> Bob\n@enduml")).toBe("@startuml\nAlice -> Bob\n@enduml")
    // @startmindmap 不再被误包 @startuml（双重 start 会导致语法错误），缺 @endmindmap 时补全
    expect(normalizePlantUml("@startmindmap\n* 根\n** 分支")).toBe("@startmindmap\n* 根\n** 分支\n@endmindmap")
    // @startwbs 同理
    expect(normalizePlantUml("@startwbs\n* 项目\n@endwbs")).toBe("@startwbs\n* 项目\n@endwbs")
  })

  test("injectPlantUmlLayout injects spacing defaults for @startuml only", () => {
    // @startuml 未设置间距 → 注入 ranksep/nodesep（插在 @enduml 前）
    expect(injectPlantUmlLayout("@startuml\nAlice -> Bob\n@enduml")).toBe(
      "@startuml\nAlice -> Bob\nskinparam ranksep 80\nskinparam nodesep 40\n@enduml",
    )
    // 已显式设置间距 → 不再注入（尊重用户布局）
    expect(injectPlantUmlLayout("@startuml\nskinparam nodesep 10\nA -> B\n@enduml")).toBe(
      "@startuml\nskinparam nodesep 10\nA -> B\n@enduml",
    )
    // 无 @enduml 时补全注入
    expect(injectPlantUmlLayout("@startuml\nA -> B")).toBe("@startuml\nA -> B\nskinparam ranksep 80\nskinparam nodesep 40\n@enduml")
    // 非 uml 图型（mindmap/wbs 等）布局由结构决定，不注入
    expect(injectPlantUmlLayout("@startmindmap\n* 根\n@endmindmap")).toBe("@startmindmap\n* 根\n@endmindmap")
    expect(injectPlantUmlLayout("@startwbs\n* 项目\n@endwbs")).toBe("@startwbs\n* 项目\n@endwbs")
  })

  test("draw tool render 参数默认 frontend（首选前端渲染降低服务端负载）", () => {
    const props = drawTool.parameters?.properties as Record<string, { default?: string; enum?: string[] }> | undefined
    expect(props?.render?.default).toBe("frontend")
    expect(props?.render?.enum).toEqual(["frontend", "backend"])
  })

  test("draw tool renders via frontend, returns diagram block and writes .puml with layout defaults", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-"))
    const c = ctx(home)
    // 前端渲染成功 → 工具返回成功并落盘 .puml；渲染/落盘源码带布局兜底默认参数
    c.waitForDraw = async (render) => {
      expect(render.code).toContain("Alice -> Bob")
      expect(render.code).toContain("skinparam ranksep 80")
      expect(render.code).toContain("skinparam nodesep 40")
      expect(render.format).toBe("plantuml")
      return { ok: true }
    }
    const r = await drawTool.execute({ code: "Alice -> Bob: hello", name: "flow" }, c)
    expect(r.blocks![0].type).toBe("diagram")
    expect((r.blocks![0] as { format: string }).format).toBe("plantuml")
    expect(r.output).toContain("渲染成功")
    // 产物落盘会话 tmp/（描述与实现一致），模型可经 read 读同一逻辑路径
    expect(r.output).toContain("tmp/flow.puml")
    const file = await readTool.execute({ path: "tmp/flow.puml" }, c)
    // 落盘源码自动补全 @startuml/@enduml 并带布局默认参数
    expect(file.output).toContain("@startuml")
    expect(file.output).toContain("Alice -> Bob")
    expect(file.output).toContain("skinparam ranksep 80")
    expect(await Bun.file(join(c.workdir, "tmp", "flow.puml")).text()).toContain("@startuml")
    cleanup(home)
  })

  test("draw tool reports frontend render errors and timeout to the model", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-err-"))
    const c = ctx(home)
    // 渲染报错 → 错误返回给模型，不写文件
    c.waitForDraw = async () => ({ ok: false, error: "PlantUML 语法错误：Syntax Error?" })
    const failed = await drawTool.execute({ code: "Alice ->", name: "bad" }, c)
    expect(failed.output).toContain("画图失败（渲染错误）")
    expect(failed.output).toContain("Syntax Error")
    expect(failed.blocks).toBeUndefined()
    // 5 秒超时 → 返回画图能力受限
    c.waitForDraw = async () => null
    const timedOut = await drawTool.execute({ code: "Alice -> Bob", name: "slow" }, c)
    expect(timedOut.output).toContain("画图能力受限")
    cleanup(home)
  })

  test("draw tool render=backend renders PNG via server, returns image block and writes .puml + .png", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-backend-"))
    const c = ctx(home)
    // 后端渲染：注入 fake 渲染器返回 PNG 字节；渲染的是规范化（补全 @startuml）后的源码
    c.renderDiagram = async (code) => {
      expect(code).toContain("@startuml")
      expect(code).toContain("Alice -> Bob")
      return new Uint8Array([0x89, 0x50, 0x4e, 0x47])
    }
    const r = await drawTool.execute({ code: "Alice -> Bob: hello", name: "flow", render: "backend" }, c)
    // 返回 image 内容块（相对会话根路径，前端直接展示图片）
    expect(r.blocks![0].type).toBe("image")
    expect((r.blocks![0] as { path: string }).path).toBe("tmp/flow.png")
    expect((r.blocks![0] as { mime: string }).mime).toBe("image/png")
    expect(r.output).toContain("渲染为图片")
    expect(r.output).toContain("tmp/flow.png")
    // PNG 与 .puml 均落盘会话 tmp/（文件面板可见）
    const png = new Uint8Array(await Bun.file(join(c.workdir, "tmp", "flow.png")).arrayBuffer())
    expect(png).toEqual(new Uint8Array([0x89, 0x50, 0x4e, 0x47]))
    expect(await Bun.file(join(c.workdir, "tmp", "flow.puml")).text()).toContain("@startuml")
    cleanup(home)
  })

  test("draw tool render=backend reports render errors; unavailable without renderDiagram", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-backend-err-"))
    const c = ctx(home)
    // 渲染抛错 → 错误返回给模型供修正，不写文件
    c.renderDiagram = async () => {
      throw new Error("PlantUML 渲染错误：Syntax Error?")
    }
    const failed = await drawTool.execute({ code: "Alice ->", name: "bad", render: "backend" }, c)
    expect(failed.output).toContain("画图失败（后端渲染错误）")
    expect(failed.output).toContain("Syntax Error")
    expect(failed.blocks).toBeUndefined()
    // 未注入渲染器（后端能力未启用）→ 明确提示不可用
    const c2 = ctx(home)
    const unavailable = await drawTool.execute({ code: "Alice -> Bob", name: "x", render: "backend" }, c2)
    expect(unavailable.output).toContain("后端渲染不可用")
    cleanup(home)
  })

  test("draw tool renders an existing .puml file via path (file rendering)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-file-"))
    const c = ctx(home)
    // 会话内已有 .puml 文件（未经规范化）：draw path 直接读取渲染，不重发源码
    await writeTool.execute({ path: "notes/flow.puml", content: "Alice -> Bob: hello" }, c)
    c.waitForDraw = async (render) => {
      expect(render.code).toContain("Alice -> Bob")
      expect(render.code).toContain("skinparam ranksep 80")
      expect(render.name).toBe("flow")
      return { ok: true }
    }
    const r = await drawTool.execute({ path: "notes/flow.puml" }, c)
    expect(r.output).toContain("渲染成功")
    expect(r.output).toContain("源文件 notes/flow.puml")
    // 图表名/块名取自文件主名，块内 code 为文件原文
    const block = r.blocks![0] as { type: string; name: string; code: string; format: string }
    expect(block.type).toBe("diagram")
    expect(block.format).toBe("plantuml")
    expect(block.name).toBe("flow.puml")
    expect(block.code).toBe("Alice -> Bob: hello")
    // 规范化源码落盘会话 tmp/（UI 文件面板可见，可经 read 读取）
    expect(await readTool.execute({ path: "tmp/flow.puml" }, c)).toMatchObject({ output: expect.stringContaining("@startuml") })
    cleanup(home)
  })

  test("draw tool renders an existing .puml file via path with render=backend", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-file-backend-"))
    const c = ctx(home)
    await writeTool.execute({ path: "tmp/flow.puml", content: "Alice -> Bob" }, c)
    c.renderDiagram = async (code) => {
      expect(code).toContain("@startuml")
      expect(code).toContain("Alice -> Bob")
      return new Uint8Array([0x89, 0x50, 0x4e, 0x47])
    }
    const r = await drawTool.execute({ path: "tmp/flow.puml", render: "backend" }, c)
    expect(r.blocks![0].type).toBe("image")
    expect((r.blocks![0] as { path: string }).path).toBe("tmp/flow.png")
    expect(r.output).toContain("源文件 tmp/flow.puml")
    expect(r.output).toContain("tmp/flow.png")
    cleanup(home)
  })

  test("draw tool path mode reports unreadable file to the model", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-file-miss-"))
    const c = ctx(home)
    const r = await drawTool.execute({ path: "tmp/missing.puml" }, c)
    expect(r.output).toContain("无法读取文件 tmp/missing.puml")
    expect(r.blocks).toBeUndefined()
    cleanup(home)
  })

  test("draw tool mermaid format: passes format to frontend, writes .mmd and returns mermaid diagram block", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-mmd-"))
    const c = ctx(home)
    c.waitForDraw = async (render) => {
      expect(render.format).toBe("mermaid")
      expect(render.code).toBe("flowchart LR\nA --> B")
      return { ok: true }
    }
    const r = await drawTool.execute({ format: "mermaid", code: "flowchart LR\nA --> B", name: "flow" }, c)
    expect(r.output).toContain("tmp/flow.mmd")
    const block = r.blocks![0] as { type: string; name: string; code: string; format: string }
    expect(block.type).toBe("diagram")
    expect(block.format).toBe("mermaid")
    expect(block.name).toBe("flow.mmd")
    // mermaid 源码原样落盘（不做 PlantUML 规范化/布局注入）
    expect(await Bun.file(join(c.workdir, "tmp", "flow.mmd")).text()).toBe("flowchart LR\nA --> B")
    cleanup(home)
  })

  test("draw tool d2 format: passes format to frontend, writes .d2 and returns d2 diagram block", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-d2-"))
    const c = ctx(home)
    c.waitForDraw = async (render) => {
      expect(render.format).toBe("d2")
      expect(render.code).toBe("gateway -> auth")
      return { ok: true }
    }
    const r = await drawTool.execute({ format: "d2", code: "gateway -> auth", name: "arch" }, c)
    expect(r.output).toContain("tmp/arch.d2")
    const block = r.blocks![0] as { type: string; name: string; code: string; format: string }
    expect(block.type).toBe("diagram")
    expect(block.format).toBe("d2")
    expect(block.name).toBe("arch.d2")
    expect(await Bun.file(join(c.workdir, "tmp", "arch.d2")).text()).toBe("gateway -> auth")
    cleanup(home)
  })

  test("draw tool renders an existing .mmd/.d2 file via path with format inferred from extension", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-file-mmd-d2-"))
    const c = ctx(home)
    await writeTool.execute({ path: "notes/flow.mmd", content: "flowchart TD\nA --> B" }, c)
    await writeTool.execute({ path: "notes/arch.d2", content: "gw -> svc" }, c)
    c.waitForDraw = async (render) => {
      expect(render.format).toBe("mermaid")
      expect(render.code).toContain("A --> B")
      return { ok: true }
    }
    const mmd = await drawTool.execute({ path: "notes/flow.mmd" }, c)
    expect(mmd.output).toContain("源文件 notes/flow.mmd")
    expect((mmd.blocks![0] as { format: string }).format).toBe("mermaid")
    expect((mmd.blocks![0] as { name: string }).name).toBe("flow.mmd")
    c.waitForDraw = async (render) => {
      expect(render.format).toBe("d2")
      return { ok: true }
    }
    const d2 = await drawTool.execute({ path: "notes/arch.d2" }, c)
    expect((d2.blocks![0] as { format: string }).format).toBe("d2")
    expect((d2.blocks![0] as { name: string }).name).toBe("arch.d2")
    cleanup(home)
  })

  test("draw tool render=backend passes format to the backend renderer (three languages)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-backend-format-"))
    const c = ctx(home)
    const got: string[] = []
    c.renderDiagram = async (code, o) => {
      expect(code).toBeDefined()
      got.push(o?.format ?? "")
      return new Uint8Array([0x89, 0x50, 0x4e, 0x47])
    }
    const mmd = await drawTool.execute({ format: "mermaid", code: "flowchart LR\nA --> B", name: "f", render: "backend" }, c)
    expect(mmd.blocks![0].type).toBe("image")
    expect((mmd.blocks![0] as { path: string }).path).toBe("tmp/f.png")
    const d2 = await drawTool.execute({ format: "d2", code: "a -> b", name: "a", render: "backend" }, c)
    expect((d2.blocks![0] as { path: string }).path).toBe("tmp/a.png")
    const puml = await drawTool.execute({ code: "Alice -> Bob", name: "p", render: "backend" }, c)
    expect((puml.blocks![0] as { path: string }).path).toBe("tmp/p.png")
    // format 透传：mermaid/d2/plantuml（缺省）
    expect(got).toEqual(["mermaid", "d2", "plantuml"])
    // 后端渲染失败：错误信息指明语言并回传
    c.renderDiagram = async () => {
      throw new Error("D2 渲染错误：connection missing destination")
    }
    const failed = await drawTool.execute({ format: "d2", code: "x ->", name: "bad", render: "backend" }, c)
    expect(failed.output).toContain("画图失败（后端渲染错误）")
    expect(failed.output).toContain("请修正 D2 源码后重试")
    cleanup(home)
  })

  test("draw tool render error mentions the failing diagram language", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-draw-mmd-err-"))
    const c = ctx(home)
    c.waitForDraw = async () => ({ ok: false, error: "Parse error on line 2" })
    const r = await drawTool.execute({ format: "mermaid", code: "flowchart", name: "bad" }, c)
    expect(r.output).toContain("画图失败（渲染错误）")
    expect(r.output).toContain("请修正 Mermaid 源码后重试")
    cleanup(home)
  })

  test("draw tool schema guides model selection: format enum covers three languages and is required", () => {
    const params = drawTool.parameters
    const fmt = (params.properties as { format: { enum: string[] } }).format
    expect(fmt.enum).toEqual(["mermaid", "plantuml", "d2"])
    expect(params.required).toContain("format")
    // 工具描述与 format 参数说明内置三语言选择指南（触发词/适用场景），供模型按需选择
    expect(drawTool.description).toContain("Mermaid")
    expect(drawTool.description).toContain("PlantUML")
    expect(drawTool.description).toContain("D2")
    expect(fmt.enum.length).toBe(3)
  })

  test("page_capture writes captured html and png screenshot to session tmp/capture/", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-capture-"))
    const c = ctx(home)
    const html = "<!doctype html><html><body><h1>渲染后页面</h1></body></html>"
    const png = "data:image/png;base64," + Buffer.from("fake-png-bytes").toString("base64")
    // 缺省 fullPage=false；fullPage=true 透传给前端
    let gotOpts: { fullPage?: boolean; delayMs?: number } | undefined
    c.waitForCapture = async (opts) => {
      gotOpts = opts
      return { html, imageBase64: png }
    }
    const r = await pageCaptureTool.execute({}, c)
    expect(gotOpts?.fullPage).toBeFalsy()
    const full = await pageCaptureTool.execute({ fullPage: true }, c)
    expect(gotOpts?.fullPage).toBe(true)
    // delay 透传并 clamp 到 [0, 10000]
    await pageCaptureTool.execute({ delay: 1500 }, c)
    expect(gotOpts?.delayMs).toBe(1500)
    await pageCaptureTool.execute({ delay: 99999 }, c)
    expect(gotOpts?.delayMs).toBe(10000)
    await pageCaptureTool.execute({ delay: -5 }, c)
    expect(gotOpts?.delayMs).toBe(0)
    expect(full.output).toContain("已捕获当前页面")
    expect(r.output).toContain("tmp/capture/page-")
    expect(r.output).toContain("可用 read 读取完整内容")
    expect(r.output).toContain("可用 vision 工具分析图片内容")
    // blocks：html 文件块 + image 截图块
    const htmlBlock = r.blocks!.find((b) => b.type === "file") as { path: string; name: string }
    const imgBlock = r.blocks!.find((b) => b.type === "image") as { path: string; mime: string }
    expect(htmlBlock.path).toMatch(/^tmp\/capture\/page-\d+\.html$/)
    expect(imgBlock.mime).toBe("image/png")
    // 落盘校验（逻辑路径可经 read 读取）
    expect(await readTool.execute({ path: htmlBlock.path }, c)).toMatchObject({ output: html })
    const imgBytes = await Bun.file(join(c.workdir, ...imgBlock.path.split("/"))).arrayBuffer()
    expect(Buffer.from(imgBytes).toString()).toBe("fake-png-bytes")
    cleanup(home)
  })

  test("page_capture accepts jpeg data url and reports frontend error/timeout", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-capture-err-"))
    const c = ctx(home)
    // JPEG data URL → 落盘 .jpg（MIME 正确）
    c.waitForCapture = async () => ({ html: "<p>hi</p>", imageBase64: "data:image/jpeg;base64," + Buffer.from("jpeg-bytes").toString("base64") })
    const jpeg = await pageCaptureTool.execute({}, c)
    const imgBlock = jpeg.blocks!.find((b) => b.type === "image") as { path: string; mime: string }
    expect(imgBlock.path).toMatch(/\.jpg$/)
    expect(imgBlock.mime).toBe("image/jpeg")
    // 前端报错 → 失败信息返回模型
    c.waitForCapture = async () => ({ html: "", error: "截图 canvas 超限" })
    const failed = await pageCaptureTool.execute({}, c)
    expect(failed.output).toContain("页面捕获失败")
    expect(failed.output).toContain("canvas 超限")
    expect(failed.blocks).toBeUndefined()
    // 前端未返回截图 → 无 image block，提示可用 read
    c.waitForCapture = async () => ({ html: "<p>no shot</p>" })
    const noShot = await pageCaptureTool.execute({}, c)
    expect(noShot.blocks!.filter((b) => b.type === "image")).toHaveLength(0)
    expect(noShot.output).toContain("前端未返回截图")
    // 非法 base64（非白名单字符集）→ 拒绝解码，按无截图处理
    c.waitForCapture = async () => ({ html: "<p>bad b64</p>", imageBase64: "not-base64!!!" })
    const badB64 = await pageCaptureTool.execute({}, c)
    expect(badB64.blocks!.filter((b) => b.type === "image")).toHaveLength(0)
    expect(badB64.output).toContain("前端未返回截图")
    // 30 秒超时 → 返回捕获失败提示
    c.waitForCapture = async () => null
    const timedOut = await pageCaptureTool.execute({}, c)
    expect(timedOut.output).toContain("页面捕获失败")
    expect(timedOut.output).toContain("限定时间")
    cleanup(home)
  })

  test("render_html returns html block and writes .html to session tmp/", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-html-"))
    const c = ctx(home)
    const html = "<!doctype html><html><body><h1>报告</h1><p>数据</p></body></html>"
    const r = await renderHtmlTool.execute({ html, name: "report" }, c)
    expect(r.blocks![0].type).toBe("html")
    const block = r.blocks![0] as { html: string; name: string }
    expect(block.html).toBe(html)
    expect(block.name).toBe("report.html")
    expect(r.output).toContain("tmp/report.html")
    // 产物落盘会话 tmp/，模型可经 read 读同一逻辑路径
    expect(await readTool.execute({ path: "tmp/report.html" }, c)).toMatchObject({ output: html })
    expect(await Bun.file(join(c.workdir, "tmp", "report.html")).text()).toBe(html)
    cleanup(home)
  })

  test("render_html defaults name and strips .html suffix", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-html2-"))
    const c = ctx(home)
    const r = await renderHtmlTool.execute({ html: "<p>hi</p>", name: "page.html" }, c)
    expect((r.blocks![0] as { name: string }).name).toBe("page.html")
    expect(r.output).toContain("tmp/page.html")
    const d = await renderHtmlTool.execute({ html: "<p>hi</p>" }, c)
    expect((d.blocks![0] as { name: string }).name).toBe("page.html")
    cleanup(home)
  })

  test("render_html passes explicit width/height to block and ignores invalid values", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-html3-"))
    const c = ctx(home)
    const ok = await renderHtmlTool.execute({ html: "<p>hi</p>", width: 900, height: 640.5 }, c)
    const block = ok.blocks![0] as { width?: number; height?: number }
    expect(block.width).toBe(900)
    expect(block.height).toBe(641)
    const noSize = await renderHtmlTool.execute({ html: "<p>hi</p>" }, c)
    expect((noSize.blocks![0] as { width?: number }).width).toBeUndefined()
    const bad = await renderHtmlTool.execute({ html: "<p>hi</p>", width: -1, height: 999999 }, c)
    const badBlock = bad.blocks![0] as { width?: number; height?: number }
    expect(badBlock.width).toBeUndefined()
    expect(badBlock.height).toBeUndefined()
    cleanup(home)
  })

  test("render_html renders an existing .html file via path (file rendering)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-html-file-"))
    const c = ctx(home)
    // 会话内已有 .html 文件：render_html path 直接读取渲染，不重发源码
    await writeTool.execute({ path: "assets/report.html", content: "<p>from file</p>" }, c)
    const r = await renderHtmlTool.execute({ path: "assets/report.html", width: 800 }, c)
    expect(r.output).toContain("已渲染")
    expect(r.output).toContain("源文件 assets/report.html")
    // 页面名/块名取自文件主名，块内 html 为文件原文，显式尺寸仍生效
    const block = r.blocks![0] as { html: string; name: string; width?: number }
    expect(block.html).toBe("<p>from file</p>")
    expect(block.name).toBe("report.html")
    expect(block.width).toBe(800)
    // 内容按文件主名落盘会话 tmp/（可经 read 读取同一逻辑路径）
    expect(await readTool.execute({ path: "tmp/report.html" }, c)).toMatchObject({ output: "<p>from file</p>" })
    cleanup(home)
  })

  test("render_html path mode reports unreadable file to the model", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-html-file-miss-"))
    const c = ctx(home)
    const r = await renderHtmlTool.execute({ path: "tmp/missing.html" }, c)
    expect(r.output).toContain("无法读取文件 tmp/missing.html")
    expect(r.blocks).toBeUndefined()
    cleanup(home)
  })

  test("save_tool saves private tool by default and public tool on scope=public", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-save-"))
    const c = ctx(home)
    const priv = await saveTool.execute({ name: "calc", html: "<p>1</p>" }, c)
    expect(priv.output).toContain("calc")
    expect(priv.output).toContain("用户私有")
    const pub = await saveTool.execute({ name: "clock", html: "<p>2</p>", scope: "public" }, c)
    expect(pub.output).toContain("公用")
    // 存储位置：私有 → users/{user}/tools/，公用 → tools/
    expect(await getMiniTool(home, "default", "calc")).toMatchObject({ name: "calc", scope: "private" })
    expect(await getMiniTool(home, "default", "clock")).toMatchObject({ name: "clock", scope: "public" })
    cleanup(home)
  })

  test("save_tool rejects invalid name and delete_tool removes tool", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools-del-"))
    const c = ctx(home)
    await expect(saveTool.execute({ name: "bad name", html: "x" }, c)).rejects.toThrow("工具名仅限")
    await saveTool.execute({ name: "temp", html: "x", scope: "public" }, c)
    const del = await deleteTool.execute({ name: "temp", scope: "public" }, c)
    expect(del.output).toContain("已删除")
    expect(await getMiniTool(home, "default", "temp")).toBeNull()
    const missing = await deleteTool.execute({ name: "temp", scope: "public" }, c)
    expect(missing.output).toContain("不存在")
    cleanup(home)
  })

  test("delete_tool requires approval", async () => {
    expect(deleteTool.requiresApproval).toBe(true)
  })

  test("edit replaces matching substring", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools2-"))
    const c = ctx(home)
    await writeTool.execute({ path: "b.txt", content: "foo bar" }, c)
    await editTool.execute({ path: "b.txt", edits: [{ oldString: "foo", newString: "FOO" }] }, c)
    const r = await readTool.execute({ path: "b.txt" }, c)
    expect(r.output).toBe("FOO bar")
    cleanup(home)
  })

  test("edit fails on non-matching oldString", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools3-"))
    const c = ctx(home)
    await writeTool.execute({ path: "c.txt", content: "abc" }, c)
    await expect(editTool.execute({ path: "c.txt", edits: [{ oldString: "zzz", newString: "x" }] }, c)).rejects.toThrow()
    cleanup(home)
  })

  test("current_time returns multiple time formats directly (ISO/Unix 秒与毫秒/本地含星期与时区)", async () => {
    const out = (await currentTimeTool.execute({}, ctx(""))).output
    // 四种格式一应俱全，后续无需再做格式转换；星期几合并进本地日期时间行
    expect(out).toContain("ISO 8601: ")
    expect(out).toContain("Unix 秒: ")
    expect(out).toContain("Unix 毫秒: ")
    expect(out).toContain("本地日期时间: ")
    expect(out).toMatch(/本地日期时间: \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} 星期[日一二三四五六]（[+-]\d{2}:\d{2}）/)
    // 冗余的本地完整/UTC 完整两行已移除
    expect(out).not.toContain("本地完整")
    expect(out).not.toContain("UTC 完整")
    // ISO 与 Unix 毫秒数值一致（同一时刻）
    const iso = out.match(/ISO 8601: (.+)/)![1]
    const ms = Number(out.match(/Unix 毫秒: (\d+)/)![1])
    expect(Date.parse(iso)).toBe(ms)
    const si = await systemInfoTool.execute({}, ctx(""))
    expect(JSON.parse(si.output).platform).toBe(process.platform)
  })

  test("sh executes command", async () => {
    const r = await shTool.execute({ command: "echo hi" }, ctx(""))
    expect(r.output.trim()).toBe("hi")
  })

  test("createGlobalTools returns all tools", () => {
    const tools = createGlobalTools()
    for (const n of [
      "read", "write", "ls", "grep", "search_files", "delete_file", "move_file",
      "edit", "pipe", "sh", "py", "draw", "render_html", "save_tool", "delete_tool", "fetch_url",
      "todo", "ask_user", "ask_env", "preview_server", "current_time", "system_info",
      "agent_load", "agent_run",
    ]) {
      expect(tools[n]).toBeDefined()
    }
    // agent_list 不注册进总Agent 全局工具集（未装载清单已注入提示词，避免冗余干扰；仅新会话组合编排环境注入）
    expect(tools.agent_list).toBeUndefined()
  })

  test("cron tools delegate to ctx.cron and report disabled capability", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-cron-tools-"))
    const c = ctx(home)
    const cronTools = makeCronTools()
    expect(Object.keys(cronTools).sort()).toEqual(["cron_add", "cron_list", "cron_remove", "cron_update"])

    // 未启用（ctx.cron 为空）：明确提示，不抛错
    const disabled = await cronTools.cron_add.execute({ schedule: "0 9 * * *", type: "script", script: "echo" }, c)
    expect(disabled.output).toContain("未启用")

    // 启用：add 透传并格式化输出
    const added: import("./cron").CronTask = { id: "t1", sessionId: "s1", user: "default", name: "daily", type: "script", schedule: "0 9 * * *", script: "echo hi", enabled: true, createdAt: 1, updatedAt: 1, nextRunAt: 2, runCount: 0 }
    c.cron = {
      add: async (input) => ({ ...added, ...input, id: "t1" }),
      list: async () => [added],
      remove: async (id) => id === "t1",
      update: async (id, patch) => (id === "t1" ? { ...added, ...patch } : null),
    }
    const r = await cronTools.cron_add.execute({ name: "daily", schedule: "0 9 * * *", type: "script", script: "echo hi" }, c)
    expect(r.output).toContain("t1")
    expect(r.output).toContain("下次执行")

    const listed = await cronTools.cron_list.execute({}, c)
    expect(listed.output).toContain("t1")
    expect(listed.output).toContain("echo hi")

    const updated = await cronTools.cron_update.execute({ id: "t1", enabled: false }, c)
    expect(updated.output).toContain("已更新")

    const removed = await cronTools.cron_remove.execute({ id: "t1" }, c)
    expect(removed.output).toContain("已删除")
    const missing = await cronTools.cron_remove.execute({ id: "nope" }, c)
    expect(missing.output).toContain("不存在")
  })

  test("cron mutation tools require approval; list does not", async () => {
    const cronTools = makeCronTools()
    // 定时任务 = 无人值守的任意命令/会话执行：创建/修改/删除均需审批（防多用户模式绕过审批边界）
    expect(cronTools.cron_add.requiresApproval).toBe(true)
    expect(cronTools.cron_update.requiresApproval).toBe(true)
    expect(cronTools.cron_remove.requiresApproval).toBe(true)
    expect(cronTools.cron_list.requiresApproval).toBeFalsy()
  })

  test("ask_user tool blocks waiting for the user's choice (via waitForChoice)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-ask-user-"))
    const c = ctx(home)
    // 注入 waitForChoice：模拟用户选择「B」
    c.waitForChoice = async () => ({ kind: "option", value: "B" })
    const r = await createGlobalTools().ask_user.execute({ prompt: "选择方案", options: ["A", "B", "C"] }, c)
    expect(r.output).toContain("用户选择：B")
    // 自定义文本输入：用户直接输入不在选项中的答案
    c.waitForChoice = async () => ({ kind: "option", value: "自定义答案" })
    const custom = await createGlobalTools().ask_user.execute({ prompt: "选择方案", options: ["A", "B"] }, c)
    expect(custom.output).toContain("用户选择：自定义答案")
    // 拒绝回答：返回拒绝提示，模型停止询问自行决策
    c.waitForChoice = async () => ({ kind: "refuse" })
    const refused = await createGlobalTools().ask_user.execute({ prompt: "选择方案", options: ["A", "B"] }, c)
    expect(refused.output).toContain("拒绝")
    // 超时（返回 null）时降级提示
    c.waitForChoice = async () => null
    const timedOut = await createGlobalTools().ask_user.execute({ prompt: "选择方案", options: ["A"] }, c)
    expect(timedOut.output).toContain("未在时限内")
    // 多选：multi=true 时多选结果以「、」连接返回
    c.waitForChoice = async () => ({ kind: "multi", values: ["A", "B"] })
    const multi = await createGlobalTools().ask_user.execute({ prompt: "选择方案", options: ["A", "B", "C"], multi: true }, c)
    expect(multi.output).toContain("用户选择：A、B")
    // 复杂选项：{ title, description } 原样传递（提交值取 title），纯文本选项保持字符串
    let received: { prompt: string; options: unknown[]; multi: boolean } | undefined
    c.waitForChoice = async (prompt, options, multi) => {
      received = { prompt, options: options as unknown[], multi: !!multi }
      return null
    }
    await createGlobalTools().ask_user.execute(
      { prompt: "选择方案", options: [{ title: "方案A", description: "第一个方案" }, "方案B"], multi: true },
      c,
    )
    expect(received).toEqual({ prompt: "选择方案", options: [{ title: "方案A", description: "第一个方案" }, "方案B"], multi: true })
    // 无选项时报错
    await expect(createGlobalTools().ask_user.execute({ prompt: "x", options: [] }, c)).rejects.toThrow(/至少一个选项/)
    rmSync(home, { recursive: true, force: true })
  })

  test("agent_run routes through runNewSession and todo tools work", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tools4-"))
    const todos: import("@gebai/sdk").TodoItem[] = []
    const c: ToolContext = {
      ...ctx(home),
      runNewSession: async (agents, input) => ({ output: `${agents.join("+")}|${input}`, archive: { runId: "r", agents, input, output: `${agents.join("+")}|${input}`, messages: [] } }),
      getTodos: async () => todos,
      setTodos: async (t) => {
        todos.splice(0, todos.length, ...t)
      },
    }
    const tools = createGlobalTools()
    const r = await tools.agent_run.execute({ agents: ["code"], input: "do it" }, c)
    expect(r.output).toBe("code|do it")
    // 多 Agent 预加载：agents 列表透传
    const r2 = await tools.agent_run.execute({ agents: ["code", "playwright"], input: "verify" }, c)
    expect(r2.output).toBe("code+playwright|verify")
    expect(r2.sessionRun).toBeDefined()
    expect(r2.sessionRun!.agents).toEqual(["code", "playwright"])
    // 空 agents：明确报错
    const r3 = await tools.agent_run.execute({ agents: [], input: "x" }, c)
    expect(r3.output).toContain("非空子Agent 名称列表")

    const added = await tools.todo.execute({ entries: [{ op: "add", title: "t1", eta: 30 }] }, c)
    expect(added.output).toContain("新增: t1")
    expect(added.output).toContain("当前全部待办（1 项）")
    expect(todos.length).toBe(1)
    expect(todos[0].etaMin).toBe(30)
    await tools.todo.execute({ entries: [{ op: "update", id: todos[0].id, status: "in_progress" }] }, c)
    expect(todos[0].status).toBe("in_progress")
    const list = await tools.todo.execute({}, c) // 空列表 = 查询
    expect(list.output).toContain("查询待办")
    expect(list.output).toContain("t1")
    expect(list.output).toContain("预计 30 分钟")
    cleanup(home)
  })

  test("todo tool batches add/update/delete in one call and returns snapshot", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-todos-batch-"))
    const todos: import("@gebai/sdk").TodoItem[] = []
    const c: ToolContext = {
      ...ctx(home),
      getTodos: async () => todos,
      setTodos: async (t) => {
        todos.splice(0, todos.length, ...t)
      },
    }
    const tools = createGlobalTools()
    const r = await tools.todo.execute(
      { entries: [{ op: "add", title: "a" }, { op: "add", title: "b" }, { op: "add", title: "c" }] },
      c,
    )
    expect(r.output).toContain("3 成功")
    expect(todos).toHaveLength(3)
    const [a, b, cc] = todos
    // 一次调用混合 update（状态/标题）/delete，含一条未匹配 id 收集为失败
    const r2 = await tools.todo.execute(
      {
        entries: [
          { op: "update", id: a.id, status: "completed" },
          { op: "update", id: b.id, title: "b2" },
          { op: "delete", id: cc.id },
          { op: "update", id: "missing-id", status: "completed" },
        ],
      },
      c,
    )
    expect(r2.output).toContain("3 成功，1 失败")
    expect(r2.output).toContain("删除: c")
    expect(r2.output).toContain("更新未匹配 id: missing-id")
    expect(r2.output).toContain("当前全部待办（2 项）")
    expect(todos.map((t) => t.title)).toEqual(["a", "b2"])
    expect(todos.find((t) => t.id === a.id)!.status).toBe("completed")
    // 空列表 = 查询
    const q = await tools.todo.execute({}, c)
    expect(q.output).toContain("查询待办")
    // 未知操作 / add 缺 title：报错
    await expect(tools.todo.execute({ entries: [{ op: "move", title: "x" }] }, c)).rejects.toThrow(/未知操作/)
    await expect(tools.todo.execute({ entries: [{ op: "add" }] }, c)).rejects.toThrow(/缺少 title/)
  })

  test("todo tool locates by title when id omitted and snapshot exposes ids", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-todos-title-"))
    const todos: import("@gebai/sdk").TodoItem[] = []
    const c: ToolContext = {
      ...ctx(home),
      getTodos: async () => todos,
      setTodos: async (t) => {
        todos.splice(0, todos.length, ...t)
      },
    }
    const tools = createGlobalTools()
    await tools.todo.execute({ entries: [{ op: "add", title: "修复登录" }, { op: "add", title: "写测试" }] }, c)
    // 清单输出带 id，模型无需再单独查询
    const list = await tools.todo.execute({}, c)
    expect(list.output).toContain(`（id: ${todos[0].id}）`)
    expect(list.output).toContain(`（id: ${todos[1].id}）`)
    // update 无 id 时按 title 精确定位
    await tools.todo.execute({ entries: [{ op: "update", title: "修复登录", status: "completed" }] }, c)
    expect(todos.find((t) => t.title === "修复登录")!.status).toBe("completed")
    expect(todos.find((t) => t.title === "写测试")!.status).toBe("pending")
    // delete 无 id 时按 title 定位
    await tools.todo.execute({ entries: [{ op: "delete", title: "写测试" }] }, c)
    expect(todos.map((t) => t.title)).toEqual(["修复登录"])
    // update 无 id 且仅 title（无 status/progress 变更字段）→ 提示无字段变更
    const noop = await tools.todo.execute({ entries: [{ op: "update", title: "修复登录" }] }, c)
    expect(noop.output).toContain("title 仅用于定位，改标题请用 id")
    // 未匹配标题 → 失败收集，不抛错
    const miss = await tools.todo.execute({ entries: [{ op: "update", title: "不存在", status: "completed" }] }, c)
    expect(miss.output).toContain("1 失败")
    expect(miss.output).toContain("更新未匹配 标题「不存在」")
    // 同名多条 → 提示用 id，不误改
    await tools.todo.execute({ entries: [{ op: "add", title: "重复项" }, { op: "add", title: "重复项" }] }, c)
    const dup = await tools.todo.execute({ entries: [{ op: "update", title: "重复项", status: "completed" }] }, c)
    expect(dup.output).toContain("标题匹配多个待办，请用 id 指定")
    expect(todos.filter((t) => t.title === "重复项").every((t) => t.status === "pending")).toBe(true)
    cleanup(home)
  })
})

describe("spillLongUserInput（超长用户输入落盘）", () => {
  test("未超阈值原样返回（不改变）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-spill-"))
    const short = "普通输入".repeat(100)
    const r = await spillLongUserInput(short, dir)
    expect(r).toEqual({ content: short, spilled: false })
    rmSync(dir, { recursive: true, force: true })
  })

  test("超阈值：全文落盘 tmp/user_inputs/{hash}.txt，正文保留头尾+引用", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-spill-"))
    const prefix = "开头部分".repeat(600)
    const middle = "中段大块内容".repeat(2000)
    const suffix = "结尾请求：请帮我分析这段内容"
    const full = prefix + middle + suffix
    expect(full.length).toBeGreaterThan(USER_INPUT_SPILL_THRESHOLD)
    const r = await spillLongUserInput(full, dir)
    expect(r.spilled).toBe(true)
    expect(r.filePath).toMatch(/^tmp\/user_inputs\/[0-9a-f]{16}\.txt$/)
    // 正文保留头尾与文件引用（省略中段）
    expect(r.content).toContain("开头部分")
    expect(r.content).toContain("结尾请求：请帮我分析这段内容")
    expect(r.content).toContain("tmp/user_inputs/")
    expect(r.content).toContain("省略中间")
    expect(r.content.length).toBeLessThan(full.length)
    // 原文完整落盘（内容哈希命名，与消息引用一致）
    const abs = join(dir, "user_inputs", `${r.filePath!.split("/").pop()}`)
    expect(await Bun.file(abs).text()).toBe(full)
    rmSync(dir, { recursive: true, force: true })
  })

  test("相同内容复用同一文件（内容哈希去重）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-spill-"))
    const full = "重复大块".repeat(3100)
    const r1 = await spillLongUserInput(full, dir)
    const r2 = await spillLongUserInput(full, dir)
    expect(r1.filePath).toBe(r2.filePath)
    const { readdir } = await import("node:fs/promises")
    expect(await readdir(join(dir, "user_inputs"))).toHaveLength(1)
    rmSync(dir, { recursive: true, force: true })
  })
})

  test("pipe chains tools passing previous output as input", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pipe-"))
    const calls: Array<Record<string, unknown>> = []
    const mockTool: import("./types").Tool = {
      name: "echo_input",
      description: "",
      parameters: { type: "object", properties: {} },
      async execute(args) {
        calls.push(args)
        return { output: `out:${args.input ?? ""}` }
      },
    }
    const c: ToolContext = {
      ...ctx(home),
      registry: {
        schemas: () => [],
        resolve: (name) => (name === "echo_input" ? { name, tool: mockTool } : undefined),
        getAgentNames: () => [],
      },
    }
    const r = await createGlobalTools().pipe.execute({ steps: [{ tool: "echo_input" }, { tool: "echo_input" }] }, c)
    expect(r.output).toContain("out:out:")
    expect(calls[1].input).toBe("out:")
    // 未知工具报错
    await expect(createGlobalTools().pipe.execute({ steps: [{ tool: "nope" }] }, c)).rejects.toThrow(/未知工具/)
    cleanup(home)
  })

  test("pipe passes previous output to script tools (sh/py) via stdin input param", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pipe-stdin-"))
    const calls: Array<Record<string, unknown>> = []
    const shMock: import("./types").Tool = {
      name: "sh",
      description: "",
      parameters: { type: "object", properties: {} },
      async execute(args) {
        calls.push(args)
        return { output: `cmd:${args.command ?? ""}:in:${args.input ?? ""}` }
      },
    }
    const c: ToolContext = {
      ...ctx(home),
      registry: {
        schemas: () => [],
        resolve: (name) => (name === "sh" ? { name, tool: shMock } : undefined),
        getAgentNames: () => [],
      },
    }
    const r = await createGlobalTools().pipe.execute(
      { steps: [{ tool: "sh", params: { command: "echo a" } }, { tool: "sh", params: { command: "cat" } }] },
      c,
    )
    // 第二步 sh 收到上一步输出作为 stdin（input 参数）
    expect(calls[1].input).toBe("cmd:echo a:in:")
    expect(r.output).toContain("in:cmd:echo a:in:")
    cleanup(home)
  })

  test("pipe maps JSON output fields into next tool params by schema", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pipe-map-"))
    const seen: Array<Record<string, unknown>> = []
    const producer: import("./types").Tool = {
      name: "producer",
      description: "",
      parameters: { type: "object", properties: {} },
      async execute() {
        return { output: JSON.stringify({ path: "data.json", extra: 1 }) }
      },
    }
    const mapper: import("./types").Tool = {
      name: "mapper",
      description: "",
      parameters: { type: "object", properties: { path: { type: "string" } } },
      async execute(args) {
        seen.push(args)
        return { output: "mapped" }
      },
    }
    const c: ToolContext = {
      ...ctx(home),
      registry: {
        schemas: () => [],
        resolve: (name) => (name === "producer" ? { name, tool: producer } : name === "mapper" ? { name, tool: mapper } : undefined),
        getAgentNames: () => [],
      },
    }
    await createGlobalTools().pipe.execute({ steps: [{ tool: "producer" }, { tool: "mapper" }] }, c)
    // JSON 字段 path 按 schema 映射注入；extra 不在 schema 中不注入
    expect(seen[0].path).toBe("data.json")
    expect(seen[0].extra).toBeUndefined()
    cleanup(home)
  })

  test("py executes python code via stdin", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-py-"))
    _resetPythonCmdCache()
    const r = await createGlobalTools().py.execute({ code: "print(6*7)" }, ctx(home))
    expect(r.output.trim()).toBe("42")
    cleanup(home)
  })

  test("resolvePythonCmd falls back to python when python3 missing", async () => {
    _resetPythonCmdCache()
    const c = ctx(tmpdir())
    const calls: string[] = []
    c.runCommand = async (cmd) => {
      calls.push(cmd)
      return cmd.startsWith("python3")
        ? { stdout: "", stderr: "python3: not found", code: 127 }
        : { stdout: "Python 3.12.0", stderr: "", code: 0 }
    }
    expect(await resolvePythonCmd(c)).toBe("python")
    expect(calls[0]).toBe("python3 --version")
    expect(calls[1]).toBe("python --version")
    _resetPythonCmdCache()
  })

  test("ls lists directory contents", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-ls-"))
    const c = ctx(home)
    c.listDir = async () => [
      { path: "a.txt", size: 12, modifiedAt: 0, isDir: false },
      { path: "sub", size: 0, modifiedAt: 0, isDir: true },
    ]
    const r = await createGlobalTools().ls.execute({}, c)
    expect(r.output).toContain("a.txt")
    expect(r.output).toContain("sub/")
    cleanup(home)
  })

  test("grep finds matching lines with file:line prefix", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-grep-"))
    const c = ctx(home)
    c.listFiles = async () => [
      { path: "src/a.ts", size: 100, modifiedAt: 0, isDir: false },
      { path: "src/b.ts", size: 200, modifiedAt: 0, isDir: false },
    ]
    c.readFile = async (p) => (p.endsWith("a.ts") ? "const x = 1\ntodo: fix this\n" : "no match here")
    const r = await createGlobalTools().grep.execute({ pattern: "todo" }, c)
    expect(r.output).toContain("src/a.ts:2: todo: fix this")
    // 无效正则
    const bad = await createGlobalTools().grep.execute({ pattern: "(" }, c)
    expect(bad.output).toContain("无效正则")
    cleanup(home)
  })

  test("search_files matches glob patterns", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-search-"))
    const c = ctx(home)
    c.listFiles = async () => [
      { path: "src/a.ts", size: 1, modifiedAt: 0, isDir: false },
      { path: "src/b.js", size: 1, modifiedAt: 0, isDir: false },
      { path: "test/c.ts", size: 1, modifiedAt: 0, isDir: false },
    ]
    const r = await createGlobalTools().search_files.execute({ pattern: "*.ts" }, c)
    expect(r.output).toContain("src/a.ts")
    expect(r.output).toContain("test/c.ts")
    expect(r.output).not.toContain("src/b.js")
    // path 限定子目录
    const sub = await createGlobalTools().search_files.execute({ pattern: "*.ts", path: "src" }, c)
    expect(sub.output).toContain("src/a.ts")
    expect(sub.output).not.toContain("test/c.ts")
    // 绝对路径 path：与 read/write 一致经 resolvePath 解析后按会话内逻辑路径匹配
    const abs = await createGlobalTools().search_files.execute({ pattern: "*.ts", path: join(c.workdir, "src") }, c)
    expect(abs.output).toContain("src/a.ts")
    expect(abs.output).not.toContain("test/c.ts")
    // path 指向会话外（本地模式放开沙箱）时无可列文件
    const outside = await createGlobalTools().search_files.execute({ pattern: "*.ts", path: join(c.workdir, "..", "outside") }, c)
    expect(outside.output).toBe("（无匹配文件）")
    cleanup(home)
  })

  test("delete_file and move_file call ctx", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-fsop-"))
    const c = ctx(home)
    const deleted: string[] = []
    const moved: Array<[string, string]> = []
    c.deleteFile = async (p) => void deleted.push(p)
    c.moveFile = async (from, to) => void moved.push([from, to])
    await createGlobalTools().delete_file.execute({ path: "old.txt" }, c)
    expect(deleted).toEqual([join(c.workdir, "old.txt")])
    await createGlobalTools().move_file.execute({ from: "a.txt", to: "b.txt" }, c)
    expect(moved).toEqual([[join(c.workdir, "a.txt"), join(c.workdir, "b.txt")]])
    cleanup(home)
  })

  test("fetch_url fetches text content and blocks private urls when sandboxed", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-fetch-"))
    const c = ctx(home)
    c.sandboxed = true
    // 沙箱模式拒绝私网/回环（返回错误消息，与网络失败/URL 非法一致）
    for (const bad of ["http://127.0.0.1:8080/x", "http://192.168.1.1/x"]) {
      const r = await createGlobalTools().fetch_url.execute({ url: bad }, c)
      expect(r.output).toMatch(/URL 不允许/)
    }
    // 公网 URL：mock 全局 fetch
    const original = globalThis.fetch
    ;(globalThis as { fetch: typeof fetch }).fetch = (async (_input: string | URL) => {
      return new Response("<html>hello world</html>", { headers: { "content-type": "text/html" } })
    }) as typeof fetch
    try {
      const r = await createGlobalTools().fetch_url.execute({ url: "https://example.com/" }, c)
      expect(r.output).toContain("hello world")
    } finally {
      ;(globalThis as { fetch: typeof fetch }).fetch = original
    }
    cleanup(home)
  })

  test("assertPublicHttpUrl blocks ssrf bypass forms", () => {
    // 常规回环/私网
    for (const u of ["http://127.0.0.1/", "http://10.0.0.1/", "http://172.16.0.1/", "http://192.168.1.1/", "http://169.254.169.254/", "http://localhost/", "http://[::1]/", "http://[fe80::1]/"]) {
      expect(() => assertPublicHttpUrl(u), u).toThrow(/URL 不允许/)
    }
    // 绕过形式：整数/十六进制 IPv4（WHATWG URL 已规范化）、IPv4-mapped IPv6（含完整形式）、
    // IPv4-compatible IPv6、ULA 私网、尾点 FQDN
    for (const u of [
      "http://2130706433/",
      "http://0x7f000001/",
      "http://0177.0.0.1/",
      "http://127.1/",
      "http://[::ffff:127.0.0.1]/",
      "http://[::ffff:7f00:1]/",
      "http://[0:0:0:0:0:ffff:7f00:1]/",
      "http://[::127.0.0.1]/",
      "http://[::ffff:192.168.1.1]/",
      "http://[fc00::1]/",
      "http://[fd00::1]/",
      "http://localhost./",
      "http://0.0.0.0/",
    ]) {
      expect(() => assertPublicHttpUrl(u), u).toThrow(/URL 不允许/)
    }
    // 公网 IPv6 放行
    expect(() => assertPublicHttpUrl("http://[2606:4700:4700::1111]/")).not.toThrow()
    // 非 http(s) 协议拒绝
    expect(() => assertPublicHttpUrl("file:///etc/passwd")).toThrow(/仅支持 http\/https/)
  })

  test("fetchWithRedirectGuard re-validates every redirect hop", async () => {
    const srv: { port: number | undefined; stop: (closeActiveConnections?: boolean) => void } = Bun.serve({
      port: 0,
      fetch: (req: Request) => {
        const u = new URL(req.url)
        if (u.pathname === "/public") return Response.redirect(`http://127.0.0.1:${srv.port}/internal`, 302)
        if (u.pathname === "/internal") return new Response("secret")
        if (u.pathname === "/chain1") return Response.redirect(`/chain2`, 302)
        if (u.pathname === "/chain2") return new Response("ok")
        return new Response("root")
      },
    })
    try {
      // 跳板到内网：守卫拦截（每跳校验）
      await expect(
        fetchWithRedirectGuard(`http://127.0.0.1:${srv.port}/public`, {}, assertPublicHttpUrl),
      ).rejects.toThrow(/URL 不允许/)
      // 同源合法重定向：正常跟随到达
      const res = await fetchWithRedirectGuard(`http://127.0.0.1:${srv.port}/chain1`, {}, () => {})
      expect(await res.text()).toBe("ok")
    } finally {
      srv.stop(true)
    }
  })

  test("fetchWithRedirectGuard caps redirect hops", async () => {
    const srv: { port: number | undefined; stop: (closeActiveConnections?: boolean) => void } = Bun.serve({
      port: 0,
      fetch: () => Response.redirect(`http://127.0.0.1:${srv.port}/loop`, 302),
    })
    try {
      await expect(fetchWithRedirectGuard(`http://127.0.0.1:${srv.port}/loop`, {}, () => {})).rejects.toThrow(/重定向次数超限/)
    } finally {
      srv.stop(true)
    }
  })

  test("agent_list and agent_load", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-subs-"))
    const c: ToolContext = {
      ...ctx(home),
      listSubAgentDefs: () => [
        { name: "code", description: "code", preload: true, loaded: true },
        { name: "writer", description: "docs", preload: false, loaded: false },
      ],
      loadSubAgent: async (name) => {
        expect(name).toBe("writer")
      },
    }
    const list = await agentListTool.execute({}, c)
    expect(list.output).toContain("code")
    expect(list.output).toContain("已装载")
    expect(list.output).toContain("未装载")
    // 工具名已注册进工具集，列表不再重复列出
    expect(list.output).not.toContain("工具")
    const load = await agentLoadTool.execute({ name: "writer" }, c)
    expect(load.output).toContain("已装载")
    cleanup(home)
  })

describe("preview_server", () => {
  // 测试用入口：按 GEBAI_PORT 环境变量监听端口，模拟独立服务实例
  function makeEntry(dir: string): string {
    const entry = join(dir, "entry.ts")
    writeFileSync(entry, `Bun.serve({ port: Number(process.env.GEBAI_PORT || 0), fetch: () => new Response("preview-ok") })\n`)
    return entry
  }

  function makeTool(tmpDir: string, entry: string) {
    return makePreviewServerTool({ host: "127.0.0.1", entry, binary: false, tmpDir, timeoutMs: 8000, intervalMs: 100 })
  }

  function parseStart(r: { output: string }): { port: number; pid: number } {
    const url = r.output.match(/http:\/\/127\.0\.0\.1:(\d+)/)
    const pid = r.output.match(/PID: (\d+)/)
    expect(url).not.toBeNull()
    expect(pid).not.toBeNull()
    return { port: Number(url![1]), pid: Number(pid![1]) }
  }

  test("start spawns a detached service on a new port and stop kills it", async () => {
    const tmp = mkdtempSync(join(tmpdir(), "gebai-preview-test-"))
    const tool = makeTool(tmp, makeEntry(tmp))
    const c = ctx(tmp)

    const started = await tool.execute({}, c)
    expect(started.output).toContain("不中断当前会话")
    const { port, pid } = parseStart(started)

    // 独立进程可访问
    expect(await (await fetch(`http://127.0.0.1:${port}`)).text()).toBe("preview-ok")
    expect(pidAlive(pid)).toBe(true)

    // 状态文件落盘（供 stop 按 pid/port 定位）
    const state = await Bun.file(join(tmp, "gebai-preview.json")).json()
    expect(state.some((e: { port: number }) => e.port === port)).toBe(true)

    // stop 按 pid 停止，端口释放
    const stopped = await tool.execute({ action: "stop", pid }, c)
    expect(stopped.output).toContain("已停止")
    await expect(fetch(`http://127.0.0.1:${port}`)).rejects.toThrow()
    await new Promise((r) => setTimeout(r, 300))
    expect(pidAlive(pid)).toBe(false)
    rmSync(tmp, { recursive: true, force: true })
  })

  test("stop by port works and unknown pid reports not found", async () => {
    const tmp = mkdtempSync(join(tmpdir(), "gebai-preview-test2-"))
    const tool = makeTool(tmp, makeEntry(tmp))
    const c = ctx(tmp)

    const started = await tool.execute({}, c)
    const { port } = parseStart(started)
    expect(await (await fetch(`http://127.0.0.1:${port}`)).text()).toBe("preview-ok")

    const stopped = await tool.execute({ action: "stop", port }, c)
    expect(stopped.output).toContain("已停止")

    const unknown = await tool.execute({ action: "stop", pid: 99999999 }, c)
    expect(unknown.output).toContain("未找到")
    rmSync(tmp, { recursive: true, force: true })
  })

  test("refuses an occupied port and validates port range", async () => {
    const tmp = mkdtempSync(join(tmpdir(), "gebai-preview-test3-"))
    const tool = makeTool(tmp, makeEntry(tmp))
    const c = ctx(tmp)
    const busy = Bun.serve({ port: 0, hostname: "127.0.0.1", fetch: () => new Response("busy") })
    try {
      const r = await tool.execute({ port: busy.port }, c)
      expect(r.output).toContain("已被占用")
      const invalid = await tool.execute({ port: 99999 }, c)
      expect(invalid.output).toContain("无效端口")
    } finally {
      busy.stop()
      rmSync(tmp, { recursive: true, force: true })
    }
  })
})

describe("apply_patch tool", () => {
  test("applies unified diff to existing file", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-applypatch-"))
    const c = ctx(home)
    await writeTool.execute({ path: "src/a.ts", content: "const a = 1\nconst b = 2\nconst c = 3\n" }, c)
    const r = await applyPatchTool.execute({ path: "src/a.ts", patch: "--- a/src/a.ts\n+++ b/src/a.ts\n@@ -2,1 +2,1 @@\n-const b = 2\n+const b = 22\n" }, c)
    expect(r.output).toContain("已写入 src/a.ts")
    expect(r.output).toContain("1 处 hunk")
    expect(await Bun.file(join(home, "users", "default", "sessions", "s1", "tmp", "src", "a.ts")).text()).toBe("const a = 1\nconst b = 22\nconst c = 3\n")
    cleanup(home)
  })

  test("dryRun validates without writing", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-applypatch-dry-"))
    const c = ctx(home)
    await writeTool.execute({ path: "a.ts", content: "a\nb\nc\n" }, c)
    const r = await applyPatchTool.execute({ path: "a.ts", patch: "@@ -2,1 +2,1 @@\n-b\n+B\n", dryRun: true }, c)
    expect(r.output).toContain("预演")
    expect(r.output).toContain("未写入")
    expect(await Bun.file(join(home, "users", "default", "sessions", "s1", "tmp", "a.ts")).text()).toBe("a\nb\nc\n")
    cleanup(home)
  })

  test("mismatched patch fails atomically without modifying file", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-applypatch-fail-"))
    const c = ctx(home)
    await writeTool.execute({ path: "a.ts", content: "a\nb\nc\n" }, c)
    const r = await applyPatchTool.execute({ path: "a.ts", patch: "@@ -2,1 +2,1 @@\n-zzz\n+ZZZ\n" }, c)
    expect(r.output).toContain("未匹配")
    expect(await Bun.file(join(home, "users", "default", "sessions", "s1", "tmp", "a.ts")).text()).toBe("a\nb\nc\n")
    cleanup(home)
  })

  test("creates new file from /dev/null patch", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-applypatch-new-"))
    const c = ctx(home)
    const r = await applyPatchTool.execute({ path: "new.ts", patch: "--- /dev/null\n+++ b/new.ts\n@@ -0,0 +1,2 @@\n+export const x = 1\n+export const y = 2\n" }, c)
    expect(r.output).toContain("已写入 new.ts")
    expect(await Bun.file(join(home, "users", "default", "sessions", "s1", "tmp", "new.ts")).text()).toBe("export const x = 1\nexport const y = 2\n")
    cleanup(home)
  })

  test("multi-file patch rejected; empty patch reports no hunks", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-applypatch-multi-"))
    const c = ctx(home)
    const multi = await applyPatchTool.execute({ path: "x.ts", patch: "--- a/x.ts\n+++ b/x.ts\n@@ -1 +1 @@\n-a\n+b\n--- a/y.ts\n+++ b/y.ts\n@@ -1 +1 @@\n-c\n+d\n" }, c)
    expect(multi.output).toContain("2 个文件")
    const empty = await applyPatchTool.execute({ path: "x.ts", patch: "--- a/x.ts\n+++ b/x.ts\n" }, c)
    expect(empty.output).toContain("未解析到任何 hunk")
    cleanup(home)
  })
})

describe("git tool", () => {
  test("status runs git in resolved dir with structured output", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-git-status-"))
    const c = ctx(home)
    const seen: Array<{ cmd: string; workdir: string }> = []
    c.runCommand = async (cmd, o) => {
      seen.push({ cmd, workdir: o?.workdir ?? "" })
      return { stdout: " M src/a.ts\n", stderr: "", code: 0 }
    }
    const r = await gitTool.execute({ action: "status", dir: "repo" }, c)
    expect(seen).toEqual([{ cmd: "git status --short --branch", workdir: join(home, "users", "default", "sessions", "s1", "tmp", "repo") }])
    expect(r.output).toContain("M src/a.ts")
    cleanup(home)
  })

  test("diff command reflects staged flag; empty output reports no changes", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-git-diff-"))
    const c = ctx(home)
    const seen: string[] = []
    c.runCommand = async (cmd) => {
      seen.push(cmd)
      return { stdout: "", stderr: "", code: 0 }
    }
    const plain = await gitTool.execute({ action: "diff" }, c)
    expect(plain.output).toContain("无变更")
    await gitTool.execute({ action: "diff", staged: true }, c)
    expect(seen).toEqual(["git diff --no-color", "git diff --staged --no-color"])
    cleanup(home)
  })

  test("log caps maxEntries at 50", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-git-log-"))
    const c = ctx(home)
    const seen: string[] = []
    c.runCommand = async (cmd) => {
      seen.push(cmd)
      return { stdout: "a1 commit1\n", stderr: "", code: 0 }
    }
    await gitTool.execute({ action: "log", maxEntries: 999 }, c)
    expect(seen).toEqual(["git log --oneline -n 50"])
    cleanup(home)
  })

  test("non-zero exit reports failure with stderr", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-git-err-"))
    const c = ctx(home)
    c.runCommand = async () => ({ stdout: "", stderr: "fatal: not a git repository", code: 128 })
    const r = await gitTool.execute({ action: "status" }, c)
    expect(r.output).toContain("失败（exit 128")
    expect(r.output).toContain("not a git repository")
    cleanup(home)
  })

  test("unknown action rejected", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-git-unknown-"))
    const r = await gitTool.execute({ action: "push" }, ctx(home))
    expect(r.output).toContain("未知操作")
    cleanup(home)
  })
})

describe("search_symbols tool", () => {
  test("finds symbol definitions across files via tree-sitter", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sym-"))
    const c = ctx(home)
    await writeTool.execute({ path: "src/engine.ts", content: "export function handleRun() {}\nexport class Engine {\n  run() {}\n}\n" }, c)
    await writeTool.execute({ path: "src/other.ts", content: "const x = 1\n" }, c)
    c.listFiles = async () => [
      { path: "src/engine.ts", size: 200, modifiedAt: 0, isDir: false },
      { path: "src/other.ts", size: 20, modifiedAt: 0, isDir: false },
    ]
    const r = await searchSymbolsTool.execute({ symbol: "handleRun" }, c)
    expect(r.output).toContain("找到 1 处定义")
    expect(r.output).toContain("src/engine.ts:1: export_statement function_declaration handleRun")
    cleanup(home)
  })

  test("exact match sorts first; kind filter narrows results", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sym-kind-"))
    const c = ctx(home)
    await writeTool.execute({ path: "a.ts", content: "export function runner() {}\nexport class runner {}\n" }, c)
    c.listFiles = async () => [{ path: "a.ts", size: 100, modifiedAt: 0, isDir: false }]
    const r = await searchSymbolsTool.execute({ symbol: "runner" }, c)
    expect(r.output).toContain("function_declaration runner")
    expect(r.output).toContain("class_declaration runner")
    const f = await searchSymbolsTool.execute({ symbol: "runner", kind: "function" }, c)
    expect(f.output).toContain("function_declaration runner")
    expect(f.output).not.toContain("class_declaration")
    cleanup(home)
  })

  test("no hits returns hint with scanned count", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sym-none-"))
    const c = ctx(home)
    await writeTool.execute({ path: "a.ts", content: "const x = 1\n" }, c)
    c.listFiles = async () => [{ path: "a.ts", size: 20, modifiedAt: 0, isDir: false }]
    const r = await searchSymbolsTool.execute({ symbol: "missing" }, c)
    expect(r.output).toContain("未找到符号定义: missing")
    expect(r.output).toContain("grep")
    cleanup(home)
  })
})

function pidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0)
    return true
  } catch {
    return false
  }
}

function cleanup(home: string) {
  rmSync(home, { recursive: true, force: true })
}
