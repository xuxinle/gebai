/**
 * vision 子代理 TS 侧贡献契约测试（跨语言合并后 TS 侧仅 analyze——识别四工具由
 * keqing/python/vision/ 贡献，冒烟在 e2e-keqing.ts 覆盖）：
 * 工具集只剩 analyze、description/PROMPT 合并语义、被依赖方约束保留。
 */
import { describe, expect, test, afterAll } from "bun:test"
import { setVisionProviderGetter } from "../../core/tools/vision"
import type { ToolContext } from "../../core/base/types"
import { mkdtempSync, mkdirSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import { def } from "./vision"

afterAll(() => {
  setVisionProviderGetter(null)
})

/* ---------- ctx 工厂 ---------- */

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
    runCommand: async () => ({ stdout: "", stderr: "", code: 1 }),
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
  return { ...base, ...overrides }
}

describe("vision TS 侧贡献（跨语言合并：仅 analyze）", () => {
  test("工具集仅 analyze（识别四工具由 客卿 侧贡献，onnxruntime 原生推理）", () => {
    expect(Object.keys(def.tools ?? {}).sort()).toEqual(["analyze"])
    expect(def.name).toBe("vision")
    expect(def.dependencies).toBeUndefined() // 零依赖（被依赖方）
    expect(def.requiresApproval).toEqual({}) // analyze 复用全局 makeVisionTool（只读免审批）
    expect(def.preload).toBe(false)
  })

  test("description 留空（本侧不贡献，客卿 侧 manifest description + 合并层兜底）", () => {
    expect(def.description).toBe("")
  })

  test("系统提示词：analyze 宿主侧引导 + 被依赖方职责保留", () => {
    const p = def.systemPrompt
    expect(p).toContain("analyze")
    expect(p).toContain("被依赖方")
    // 不复刻 客卿 侧内容（识别工具用法由 客卿 PROMPT.md 贡献，合并层拼接）
    expect(p).not.toContain("决策序")
  })

  test("analyze：无视觉 provider 时给出配置指引（provider 检查先行与全局一致）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vis-"))
    setVisionProviderGetter(null)
    const c = ctx(home)
    const r = await def.tools!.analyze.execute({ target: "图里有什么", image: "shot.png" }, c)
    expect(r.output).toContain("视觉能力不可用")
    expect(r.output).toContain("GEBAI_VISION_MODEL")
    expect(def.tools!.analyze.name).toBe("analyze") // 注册后即 vision_analyze（命名空间隔离）
  })
})

describe("依赖复用（方式一：dependencies 声明——self_optimize 依赖 vision）", () => {
  test("self_optimize def 声明依赖 vision（agent_run 新会话级联预加载，截图分析不依赖全局 vision 继承）", async () => {
    const { def: selfOptDef } = await import("../self_optimize")
    expect(selfOptDef.dependencies).toContain("code")
    expect(selfOptDef.dependencies).toContain("vision")
  })
})
