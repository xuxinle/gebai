import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { def as codeDef } from "./index"

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
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
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
    for (const t of ["read", "write", "edit", "patch", "sh", "bg_task", "py", "ls", "grep", "glob", "file", "project", "fetch_url", "ask", "subsession_run", "todo"]) {
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

  test("preview_server 声明 hot 参数（前端热重建开关）与构建语义描述", () => {
    const tool = codeDef.tools!.preview_server
    expect(tool.parameters.properties).toHaveProperty("hot")
    expect(tool.description).toContain("前端热重建")
    // 工具边界说清：预览对象是歌白自身（不是任意项目的通用预览器）、改前端源码无需重启
    expect(tool.description).toContain("歌白自身")
    expect(tool.description).toContain("不再需要停止/重启") // 改前端源码后无需重启预览服务
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
    // 绑定根（子会话运行模式 + CODE_PROJECT）：未传 project 放行
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
