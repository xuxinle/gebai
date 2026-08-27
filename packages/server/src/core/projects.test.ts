import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { Tool, ToolContext } from "./types"
import { projectAware, resolveProjectRoot } from "./projects"

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
    projects: [{ name: "app", path: "", description: "测试项目" }],
    resolveProjectPath: (name) => {
      if (name !== "app") throw new Error(`未知预置项目: ${name}`)
      return join(home, "proj")
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

/** 探针工具：回显路径解析结果与 workdir（projectAware 包装后观察解析根切换）。 */
function probeTool(): Tool {
  return {
    name: "probe",
    description: "探针",
    parameters: { type: "object", properties: {}, required: [] },
    async execute(args, c) {
      return { output: `${c.resolvePath(String(args.path ?? "x"))}|${c.workdir}` }
    },
  }
}

describe("项目机制（core/projects，全局工具 project 参数）", () => {
  test("保留名 tmp 解析到会话工作区：sessionWorkdir 优先于 workdir（新会话绑定项目根时两者不同）", () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const workspace = join(home, "ws")
    const c = ctx(home, { workdir: join(home, "proj-root"), sessionWorkdir: workspace })
    expect(resolveProjectRoot("tmp", c)).toBe(workspace)
    // 未注入 sessionWorkdir（测试桩/旧环境）：回退 workdir
    expect(resolveProjectRoot("tmp", { ...c, sessionWorkdir: undefined })).toBe(join(home, "proj-root"))
    rmSync(home, { recursive: true, force: true })
  })

  test("projectAware：project 参数逐次选根——未传走会话工作目录，预置名/路径形态切项目根，tmp 访问会话工作区", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const c = ctx(home)
    const proj = join(home, "myproj")
    mkdirSync(proj, { recursive: true })
    const tool = projectAware(probeTool())
    // 未传 project：会话工作目录基准（相对路径不漂移）
    expect(String((await tool.execute({ path: "a.txt" }, c)).output).split("|")[0]).toBe(join(c.workdir, "a.txt"))
    // 预置项目名：切到预置根
    expect((await tool.execute({ path: "b.txt", project: "app" }, c)).output.split("|")[0]).toBe(join(home, "proj", "b.txt"))
    // 路径形态（自由项目）：切到该根
    expect(String((await tool.execute({ path: "src/c.ts", project: proj }, c)).output).split("|")[0]).toBe(join(proj, "src", "c.ts"))
    // 保留名 tmp：切回会话工作区
    expect((await tool.execute({ path: "d.txt", project: "tmp" }, c)).output.split("|")[0]).toBe(join(c.workdir, "d.txt"))
    rmSync(home, { recursive: true, force: true })
  })

  test("workdir 类工具（sh/py/git）：project 参数把 workdir 一并切到项目根", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const c = ctx(home)
    const proj = join(home, "myproj")
    mkdirSync(proj, { recursive: true })
    const tool = projectAware(probeTool(), { workdir: true })
    expect((await tool.execute({ path: ".", project: proj }, c)).output.split("|")[1]).toBe(proj)
    rmSync(home, { recursive: true, force: true })
  })

  test("projectAware 把 project 并入卡片头 titleParams：传 project 时「操作哪个项目」卡片头可见", () => {
    const mk = (card?: Tool["card"]): Tool => ({ name: "t", description: "", parameters: { type: "object", properties: {} }, ...(card ? { card } : {}), execute: async () => ({ output: "" }) })
    // 带 titleParams 的 card：前插 project、其余字段保留
    const withCard = projectAware(mk({ titleParams: ["path"], file: "path" }))
    expect(withCard.card?.titleParams).toEqual(["project", "path"])
    expect(withCard.card?.file).toBe("path")
    // 无 titleParams 的 card：获得仅含 project 的 titleParams（未传 project 时卡片头不变）
    const bareCard = projectAware(mk({ args: "code", codeField: "command" }))
    expect(bareCard.card?.titleParams).toEqual(["project"])
    expect(bareCard.card?.codeField).toBe("command")
    // 无 card：不新增；titleParams 已含 project：不重复插入
    expect(projectAware(mk()).card).toBeUndefined()
    expect(projectAware(mk({ titleParams: ["project", "path"] })).card?.titleParams).toEqual(["project", "path"])
  })

  test("会话工作区真实读写经保留名 tmp 打通（绑定项目根的会话内附件/中间文件仍可操作）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const bound = join(home, "proj-root")
    const c = ctx(home, { workdir: bound, resolvePath: (p) => join(bound, p) })
    const proj = join(home, "proj")
    mkdirSync(proj, { recursive: true })
    writeFileSync(join(c.sessionWorkdir!, "note.txt"), "workspace file\n")
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
    // 未传 project：绑定根基准（workdir = 项目根）——找不到会话文件
    const miss = await tool.execute({ path: "note.txt" }, c)
    expect(miss.output).not.toContain("workspace file")
    // project:tmp：读到会话工作区文件
    const hit = await tool.execute({ path: "note.txt", project: "tmp" }, c)
    expect(hit.output).toContain("workspace file")
    rmSync(home, { recursive: true, force: true })
  })

  test("受限模式（CODE_RESTRICT_PROJECTS=true）：未传 project 的自由路径被拒绝，带 project/绑定根放行", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const c = ctx(home, { env: { CODE_RESTRICT_PROJECTS: "true" } })
    const tool = projectAware(probeTool())
    const denied = await tool.execute({ path: "a.txt" }, c)
    expect(denied.output).toContain("受限模式")
    expect(denied.output).toContain("project 参数")
    const ok = await tool.execute({ path: "a.txt", project: "app" }, c)
    expect(ok.output).toContain(join(home, "proj"))
    // 绑定根会话（新会话执行模式）：未传 project 放行（路径基准即绑定根）
    const bound = join(home, "bound")
    mkdirSync(bound, { recursive: true })
    const c2 = ctx(home, { env: { CODE_RESTRICT_PROJECTS: "true" }, workdir: bound, boundProjectRoot: bound, resolvePath: (p) => join(bound, p) })
    const r2 = await tool.execute({ path: "a.txt" }, c2)
    expect(r2.output).toContain(bound)
    rmSync(home, { recursive: true, force: true })
  })

  test("沙箱模式：project 参数路径形态映射进用户数据目录（与预置项目同边界）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-proj-"))
    const c = ctx(home, { sandboxed: true })
    const tool = projectAware(probeTool())
    const mapped = join(home, "users", "default", "proj")
    mkdirSync(mapped, { recursive: true })
    expect((await tool.execute({ path: "a.ts", project: "./proj" }, c)).output.split("|")[0]).toBe(join(mapped, "a.ts"))
    rmSync(home, { recursive: true, force: true })
  })
})
