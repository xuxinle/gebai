import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname, isAbsolute } from "node:path"
import type { ToolContext } from "../core/types"
import { setVisionProviderGetter } from "../core/vision"
import { writeTool } from "../core/tools"
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
    resolvePath: (p) => (isAbsolute(p) ? p : join(tmp, p)),
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
  test("工具集只含 code 没有的独有工具（复用 code 通用能力，不重复注册）", () => {
    const names = Object.keys(selfOptimizeDef.tools!)
    // 独有能力：反馈读取、测试准入、回滚、验证服务、页面捕获、视觉分析
    for (const t of ["read_feedback", "run_tests", "rollback", "preview_server", "page_capture", "vision"]) {
      expect(names).toContain(t)
    }
    // 不复刻 code 的通用工具（装载/预加载时连带装载 code，文件/分析类直接用 code_* 命名空间）
    for (const t of Object.keys(codeDef.tools!)) {
      expect(names).not.toContain(t)
    }
    // 审批：仅自优化专属的 run_tests/rollback（code_* 写类工具的审批由 code 声明承接）
    expect(selfOptimizeDef.requiresApproval).toEqual({ run_tests: true, rollback: true })
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

describe("self_optimize 写范围守卫（SubAgentDef.writeGuard，代码级强制而非仅提示词）", () => {
  /** 构造最小歌白仓库结构（sub-agents 目录 + DESIGN.md + core 目录）。 */
  function makeRepo(): { root: string; sub: string } {
    const root = mkdtempSync(join(tmpdir(), "gebai-selfopt-repo-"))
    mkdirSync(join(root, "packages", "server", "src", "sub-agents"), { recursive: true })
    mkdirSync(join(root, "packages", "server", "src", "core"), { recursive: true })
    return { root, sub: join(root, "packages", "server", "src", "sub-agents") }
  }

  /** 模拟引擎注入：ctx.writeGuard 绑定 self_optimize 声明的守卫政策（SELF_OPTIMIZE_PROJECT 指向临时仓库）。 */
  function guardedCtx(root: string): ToolContext {
    const c = ctx(root, { env: { SELF_OPTIMIZE_PROJECT: root } })
    c.writeGuard = (absPaths) => selfOptimizeDef.writeGuard!(c.env, absPaths)
    return c
  }

  test("默认只读模式：子Agent 目录与仓库级文档可写，核心引擎源码拒绝", async () => {
    const { root, sub } = makeRepo()
    delete process.env.GEBAI_SELF_MODIFY
    const c = guardedCtx(root)
    try {
      const ok = await writeTool.execute({ path: join(sub, "new_agent.ts"), content: "x" }, c)
      expect(ok.output).toContain("已写入")
      const doc = await writeTool.execute({ path: join(root, "DESIGN.md"), content: "d" }, c)
      expect(doc.output).toContain("已写入")
      const denied = await writeTool.execute({ path: join(root, "packages", "server", "src", "core", "engine.ts"), content: "x" }, c)
      expect(denied.output).toContain("拒绝写入")
      // 守卫保护的是歌白仓库本身：仓库根外的常规写入（会话 tmp 产物等）不受限
      const outside = await writeTool.execute({ path: join(dirname(root), "outside.txt"), content: "x" }, c)
      expect(outside.output).toContain("已写入")
    } finally {
      cleanup(root)
      rmSync(join(dirname(root), "outside.txt"), { force: true })
    }
  })

  test("GEBAI_SELF_MODIFY=true：仓库内任意路径放行", async () => {
    const { root } = makeRepo()
    process.env.GEBAI_SELF_MODIFY = "true"
    const c = guardedCtx(root)
    try {
      const ok = await writeTool.execute({ path: join(root, "packages", "server", "src", "core", "engine.ts"), content: "x" }, c)
      expect(ok.output).toContain("已写入")
    } finally {
      delete process.env.GEBAI_SELF_MODIFY
      cleanup(root)
    }
  })

  test("目标不在守卫仓库范围内（未配置 SELF_OPTIMIZE_PROJECT）时放行", async () => {
    const { root } = makeRepo()
    delete process.env.GEBAI_SELF_MODIFY
    // 未配置 SELF_OPTIMIZE_PROJECT：守卫的仓库根不含此临时目录（dev 模式自动推导的是真实仓库检出），
    // 目标不在保护范围内即放行——守卫只保护歌白仓库，不约束无关路径
    const c = ctx(root, { env: {} })
    c.writeGuard = (absPaths) => selfOptimizeDef.writeGuard!(c.env, absPaths)
    try {
      const r = await writeTool.execute({ path: join(root, "packages", "server", "src", "core", "engine.ts"), content: "x" }, c)
      expect(r.output).toContain("已写入")
    } finally {
      cleanup(root)
    }
  })

  test("run_tests 与 rollback 工具存在且需审批（测试准入凭证 + 回滚恢复路径）", () => {
    expect(selfOptimizeDef.tools!.run_tests).toBeTruthy()
    expect(selfOptimizeDef.tools!.rollback).toBeTruthy()
    expect(selfOptimizeDef.requiresApproval!.run_tests).toBe(true)
    expect(selfOptimizeDef.requiresApproval!.rollback).toBe(true)
  })
})
