import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import type { ToolContext } from "../core/types"
import { setVisionProviderGetter } from "../core/vision"
import { def as selfOptimizeDef } from "./self_optimize"
import { def as codeDef } from "./code"

function ctx(home: string, overrides: Partial<ToolContext> = {}): ToolContext {
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  const base: ToolContext = {
    user: "default",
    sessionId: "s1",
    workdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: (name) => {
      throw new Error(`未知预置项目: ${name}`)
    },
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
  return { ...base, ...overrides }
}

function cleanup(home: string) {
  rmSync(home, { recursive: true, force: true })
}

describe("self_optimize sub-agent", () => {
  test("is a superset of code toolset plus preview_server/page_capture/vision", () => {
    const names = Object.keys(selfOptimizeDef.tools!)
    // 继承 code 全部工具（含 project 路由、todo、agent_run、fetch_url、diff、py）
    for (const t of Object.keys(codeDef.tools!)) {
      expect(names).toContain(t)
    }
    // 自优化专属扩展
    for (const t of ["preview_server", "page_capture", "vision"]) {
      expect(names).toContain(t)
    }
    // 审批策略继承 code（写操作/命令执行需审批）；新增工具免审批
    for (const t of ["edit", "write", "apply_patch", "sh", "py"]) {
      expect(selfOptimizeDef.requiresApproval![t]).toBe(true)
    }
    for (const t of ["preview_server", "page_capture", "vision"]) {
      expect(selfOptimizeDef.tools![t].requiresApproval).toBeUndefined()
    }
    expect(selfOptimizeDef.preload).toBe(false)
  })

  test("page_capture executes and writes captured html to session tmp/capture/", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-"))
    const c = ctx(home, { waitForCapture: async () => ({ html: "<html><body>渲染后</body></html>", imageBase64: "data:image/png;base64," + Buffer.from("png").toString("base64") }) })
    const r = await selfOptimizeDef.tools!.page_capture.execute({}, c)
    expect(r.output).toContain("已捕获当前页面")
    expect(r.output).toContain("tmp/capture/page-")
    expect(r.blocks!.some((b) => b.type === "image")).toBe(true)
    cleanup(home)
  })

  test("vision tool falls back to a friendly hint when no vision provider is registered", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-selfopt-vision-"))
    const c = ctx(home)
    // 模块级 vision provider 可能被其他测试（如 integration 启动真实服务）注册污染：
    // 该用例语义即「未注册时降级」，显式清空后断言（测试环境本就期望 null）
    setVisionProviderGetter(null)
    // 测试环境未注册视觉 provider → 返回配置提示而非抛异常
    const r = await selfOptimizeDef.tools!.vision.execute({ target: "描述页面", image: "tmp/capture/page-1.png" }, c)
    expect(r.output).toContain("视觉能力不可用")
    expect(r.output).toContain("GEBAI_VISION_MODEL")
    cleanup(home)
  })
})
