import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { Tool, ToolContext } from "./types"
import { _resetSessionProjectRootsForTest, clearSessionProjectRoots, projectAware, projectTool, resolveProjectRoot } from "./projects"

function ctx(home: string, overrides: Partial<ToolContext> = {}): ToolContext {
  const workspace = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(workspace, { recursive: true })
  const base: ToolContext = {
    user: "default",
    sessionId: "s1",
    workdir: workspace,
    sessionWorkdir: workspace,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(workspace, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      const { dirname } = await import("node:path")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: (r) => Promise.resolve(r.path),
    publish: () => {},
    projects: [],
    resolveProjectPath: () => {
      throw new Error("未知预置项目")
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

/** 探针工具：回显路径解析结果（projectAware 包装后观察解析根切换）。 */
function probeTool(): Tool {
  return {
    name: "probe",
    description: "探针",
    parameters: { type: "object", properties: {}, required: [] },
    async execute(args, c) {
      return { output: c.resolvePath(String(args.path ?? "x")) }
    },
  }
}

describe("项目机制（core/projects）", () => {
  test("保留名 tmp 解析到会话工作区：sessionWorkdir 优先于 workdir（新会话绑定项目根时两者不同）", () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const workspace = join(home, "ws")
    const c = ctx(home, { workdir: join(home, "proj-root"), sessionWorkdir: workspace })
    expect(resolveProjectRoot("tmp", c)).toBe(workspace)
    // 未注入 sessionWorkdir（测试桩/旧环境）：回退 workdir
    expect(resolveProjectRoot("tmp", { ...c, sessionWorkdir: undefined })).toBe(join(home, "proj-root"))
    rmSync(home, { recursive: true, force: true })
  })

  test("projectAware：project 参数逐次选根——tmp 访问会话工作区，路径形态切项目根，未传走粘性根", async () => {
    _resetSessionProjectRootsForTest()
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const c = ctx(home)
    const proj = join(home, "myproj")
    mkdirSync(proj, { recursive: true })
    const tool = projectAware(probeTool())
    // 粘性根未设 + 未传 project：原样（会话工作区基准）
    expect((await tool.execute({ path: "a.txt" }, c)).output).toBe(join(c.workdir, "a.txt"))
    // 粘性根 = 项目根（project 工具 use 设定）
    await projectTool.execute({ action: "use", project: proj }, c)
    expect((await tool.execute({ path: "src/b.ts" }, c)).output).toBe(join(proj, "src", "b.ts"))
    // 保留名 tmp：逐次切回会话工作区（粘性根已设仍可访问）
    expect((await tool.execute({ path: "c.txt", project: "tmp" }, c)).output).toBe(join(c.workdir, "c.txt"))
    // project use tmp：粘性根切回会话工作区（≡ 回到默认基准）
    await projectTool.execute({ action: "use", project: "tmp" }, c)
    expect((await tool.execute({ path: "d.txt" }, c)).output).toBe(join(c.workdir, "d.txt"))
    // list 注记保留名
    const list = await projectTool.execute({ action: "list" }, c)
    expect(list.output).toContain("保留项目名: tmp")
    _resetSessionProjectRootsForTest()
    rmSync(home, { recursive: true, force: true })
  })

  test("会话工作区真实读写经保留名 tmp 打通（设定项目根后附件/中间文件仍可操作）", async () => {
    _resetSessionProjectRootsForTest()
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const c = ctx(home)
    const proj = join(home, "proj")
    mkdirSync(proj, { recursive: true })
    writeFileSync(join(c.workdir, "note.txt"), "workspace file\n")
    const tool = projectAware({
      name: "readish",
      description: "读",
      parameters: { type: "object", properties: {}, required: [] },
      async execute(args, cc) {
        try {
          return { output: await cc.readFile(cc.resolvePath(String(args.path))) }
        } catch (err) {
          return { output: `读取失败: ${(err as Error).message}` }
        }
      },
    })
    await projectTool.execute({ action: "use", project: proj }, c)
    // 未传 project：粘性根（项目根）——找不到会话文件
    const miss = await tool.execute({ path: "note.txt" }, c)
    expect(miss.output).not.toContain("workspace file")
    // project:tmp：读到会话工作区文件
    const hit = await tool.execute({ path: "note.txt", project: "tmp" }, c)
    expect(hit.output).toContain("workspace file")
    _resetSessionProjectRootsForTest()
    rmSync(home, { recursive: true, force: true })
  })

  test("clearSessionProjectRoots 按会话后缀清理粘性根（会话删除释放）", async () => {
    _resetSessionProjectRootsForTest()
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const c1 = ctx(home)
    const c2 = { ...ctx(home), sessionId: "s2" }
    const proj = join(home, "p1")
    mkdirSync(proj, { recursive: true })
    await projectTool.execute({ action: "use", project: proj }, c1)
    await projectTool.execute({ action: "use", project: proj }, c2)
    clearSessionProjectRoots("s1")
    const l1 = await projectTool.execute({ action: "list" }, c1)
    const l2 = await projectTool.execute({ action: "list" }, c2)
    expect(String((l1.data as { current: string | null }).current)).toBe("null")
    expect(String((l2.data as { current: string | null }).current)).toBe(proj)
    _resetSessionProjectRootsForTest()
    rmSync(home, { recursive: true, force: true })
  })
})
