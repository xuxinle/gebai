import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import type { ToolContext } from "../core/types"
import { def as codeDef } from "./code"
import { createGlobalTools } from "../core/tools"
import { SessionStore } from "../core/store"

function ctx(home: string, overrides: Partial<ToolContext> = {}): ToolContext {
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  const base: ToolContext = {
    user: "default",
    sessionId: "s1",
    workdir: tmp,
    sessionWorkdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(tmp, p),
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
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [{ name: "app", path: "", description: "测试项目" }],
    resolveProjectPath: (name) => {
      if (name !== "app") throw new Error(`未知预置项目: ${name}`)
      return join(home, "users", "default", "sessions", "s1", "proj")
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

describe("code sub-agent（独有工具集，文件工具复用全局）", () => {
  test("def 只声明编码专属工具：与全局重复的文件/交互工具一律缺席", () => {
    const names = Object.keys(codeDef.tools!)
    // 独有工具齐全（符号定位/结构概览/只读 git/验证服务/环境探测/系统信息）
    for (const t of ["search_symbols", "analyze", "git", "preview_server", "env_detect", "system_info"]) {
      expect(names).toContain(t)
    }
    // 重复工具彻底删除：文件读写查询/脚本/交互编排均用全局工具（同名全局名直接调用）
    for (const t of ["read", "write", "edit", "patch", "sh", "sh_task", "py", "ls", "grep", "glob", "file", "diff", "project", "fetch_url", "ask", "agent_run", "todo"]) {
      expect(names).not.toContain(t)
    }
    // project 参数路由：路径/工作目录类独有工具带 project 参数；纯环境信息类不带
    for (const t of ["search_symbols", "analyze", "git", "preview_server"]) {
      expect(codeDef.tools![t].parameters.properties).toHaveProperty("project")
    }
    for (const t of ["env_detect", "system_info"]) {
      expect(codeDef.tools![t].parameters.properties).not.toHaveProperty("project")
    }
    // 全部免审批（code 不再覆写全局工具审批姿态）
    expect(codeDef.requiresApproval).toBeUndefined()
    for (const t of names) expect(codeDef.tools![t].requiresApproval).toBeUndefined()
    expect(codeDef.preload).toBe(false)
  })

  test("project 参数路由 git 工作目录到项目根（预置项目名与路径形态）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-code-"))
    const root = join(home, "freeproj")
    mkdirSync(root, { recursive: true })
    const c = ctx(home)
    const workdirs: string[] = []
    c.runCommand = async (_cmd, o) => {
      workdirs.push(o?.workdir ?? "")
      return { stdout: "## main", stderr: "", code: 0 }
    }
    // 预置项目名：git 在预置根执行
    const g1 = await codeDef.tools!.git.execute({ project: "app", action: "status" }, c)
    expect(g1.output).toContain("main")
    expect(workdirs[workdirs.length - 1]).toBe(c.resolveProjectPath("app"))
    // 路径形态（自由项目）：git 在该根执行
    const g2 = await codeDef.tools!.git.execute({ project: root, action: "status" }, c)
    expect(g2.output).toContain("main")
    expect(workdirs[workdirs.length - 1]).toBe(root)
    rmSync(home, { recursive: true, force: true })
  })

  test("受限模式（CODE_RESTRICT_PROJECTS=true）：独有工具未传 project 被拒，带 project 正常；绑定根放行", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-code-restrict-"))
    const c = ctx(home, { env: { CODE_RESTRICT_PROJECTS: "true" } })
    c.runCommand = async () => ({ stdout: "## main", stderr: "", code: 0 })
    const denied = await codeDef.tools!.git.execute({ action: "status" }, c)
    expect(denied.output).toContain("受限模式")
    expect(denied.output).toContain("project 参数")
    const ok = await codeDef.tools!.git.execute({ project: "app", action: "status" }, c)
    expect(ok.output).toContain("main")
    // 绑定根（新会话执行模式 + CODE_PROJECT）：未传 project 放行
    const bound = join(home, "bound")
    mkdirSync(bound, { recursive: true })
    const c2 = ctx(home, {
      env: { CODE_RESTRICT_PROJECTS: "true" },
      workdir: bound,
      boundProjectRoot: bound,
      resolvePath: (p) => join(bound, p),
      runCommand: async () => ({ stdout: "## main", stderr: "", code: 0 }),
    })
    const r2 = await codeDef.tools!.git.execute({ action: "status" }, c2)
    expect(r2.output).toContain("main")
    rmSync(home, { recursive: true, force: true })
  })

  test("未知名 project 报「未知预置项目」；路径形态直用", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-code-name-"))
    const c = ctx(home)
    c.runCommand = async () => ({ stdout: "", stderr: "", code: 0 })
    let err = ""
    try {
      await codeDef.tools!.git.execute({ project: "nope", action: "status" }, c)
    } catch (e) {
      err = (e as Error).message
    }
    expect(err).toContain("未知预置项目")
    rmSync(home, { recursive: true, force: true })
  })
})

describe("全局文件工具 project 参数（注册表形态，code 复用的同一份）", () => {
  const SID = "abcdef01abcdef01abcdef01abcdef01"

  test("全局 read/write/edit 带 project 参数：路径相对项目根解析，产物块携带项目内绝对路径", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-code-globals-"))
    try {
      const c = ctx(home, { sessionId: SID })
      const tools = createGlobalTools()
      const projRoot = join(home, "myproj")
      mkdirSync(join(projRoot, "src"), { recursive: true })
      writeFileSync(join(projRoot, "src", "a.ts"), "export const a = 1\n")
      // read 相对路径 + project（自由项目路径形态）
      const r = await tools.read.execute({ project: projRoot, path: "src/a.ts" }, c)
      expect(r.output).toContain("export const a = 1")
      expect((r.blocks as Array<{ path?: string }>)[0]?.path).toBe(join(projRoot, "src", "a.ts"))
      // write/edit 同基准：产物块同样为项目内绝对路径
      const w = await tools.write.execute({ project: projRoot, path: "src/b.ts", content: "const b = 2\n" }, c)
      expect((w.blocks as Array<{ path?: string }>)[0]?.path).toBe(join(projRoot, "src", "b.ts"))
      const e = await tools.edit.execute({ project: projRoot, path: "src/b.ts", edits: [{ oldString: "b = 2", newString: "b = 3" }] }, c)
      expect((e.blocks as Array<{ path?: string }>)[0]?.path).toBe(join(projRoot, "src", "b.ts"))
      // 未传 project：相对路径以会话工作目录为基准（默认语义不变）
      const s = await tools.write.execute({ path: "sess.txt", content: "x" }, c)
      expect(s.output).toContain("已写入")
      expect(await Bun.file(join(c.workdir, "sess.txt")).text()).toBe("x")
      // 块路径可由 files/preview 端点解析（本地模式绝对路径放行）
      const store = new SessionStore({ home })
      expect(store.resolvePreviewFile(SID, "default", join(projRoot, "src", "a.ts"), false)).toBe(join(projRoot, "src", "a.ts"))
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("沙箱模式：project 根映射进用户数据目录后 preview 边界放行；越界根被拒（工具与 preview 同一边界）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-code-sb-"))
    try {
      const c = ctx(home, { sessionId: SID, sandboxed: true })
      const tools = createGlobalTools()
      // 沙箱下路径形态项目根映射进 users/{user}/（resolveInSandbox）
      const mapped = join(home, "users", "default", "proj")
      mkdirSync(join(mapped, "src"), { recursive: true })
      writeFileSync(join(mapped, "src", "a.ts"), "x\n")
      const r = await tools.read.execute({ project: "./proj", path: "src/a.ts" }, c)
      const blockPath = (r.blocks as Array<{ path?: string }>)[0]?.path as string
      expect(blockPath).toBe(join(mapped, "src", "a.ts"))
      // preview 边界：用户数据目录内放行
      const store = new SessionStore({ home })
      expect(store.resolvePreviewFile(SID, "default", blockPath!, true)).toBe(blockPath)
      // 沙箱外绝对路径（工具本身就够不到）preview 同样拒绝——边界与工具能力一致
      expect(() => store.resolvePreviewFile(SID, "default", resolve(tmpdir(), "outside.ts"), true)).toThrow()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("受限模式：全局 write 未传 project 被拒，带 project 正常；默认关闭不限制", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-code-gres-"))
    try {
      const tools = createGlobalTools()
      const c1 = ctx(home, { env: { CODE_RESTRICT_PROJECTS: "true" } })
      const denied = await tools.write.execute({ path: "out.txt", content: "hi" }, c1)
      expect(denied.output).toContain("受限模式")
      const ok = await tools.write.execute({ project: "app", path: "out.txt", content: "hi" }, c1)
      expect(ok.output).toContain("已写入")
      // 默认关闭（未设置环境变量）：自由路径不限制
      const c2 = ctx(home)
      const r2 = await tools.write.execute({ path: "out.txt", content: "y" }, c2)
      expect(r2.output).toContain("已写入")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
