import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import type { ToolContext } from "../../core/base/types"
import { def as exploreDef } from "./explore"

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

describe("explore sub-agent（只读代码探索）", () => {
  test("只声明探索专属工具：文件读取检索复用全局工具，无任何写或执行工具，全部免审批", () => {
    const names = Object.keys(exploreDef.tools!)
    for (const t of ["search_symbols", "analyze", "git"]) {
      expect(names).toContain(t)
    }
    // 重复工具彻底删除：read/ls/grep/glob/fetch_url/todo 均为全局工具（直接全局名调用）
    for (const t of ["read", "ls", "grep", "glob", "fetch_url", "todo"]) {
      expect(names).not.toContain(t)
    }
    // 硬约束：探索不修改——写/执行/删除类工具一律缺席
    for (const t of ["write", "edit", "patch", "sh", "py", "file"]) {
      expect(names).not.toContain(t)
    }
    for (const t of ["search_symbols", "analyze", "git"]) {
      expect(exploreDef.tools![t].parameters.properties).toHaveProperty("project")
    }
    expect(exploreDef.requiresApproval).toBeUndefined()
    expect(exploreDef.preload).toBe(false)
  })

  test("project 参数路由复用 projectAware：git 工作目录相对预置项目根解析", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-explore-"))
    const root = join(home, "proj")
    mkdirSync(root, { recursive: true })
    const c = ctx(home)
    const workdirs: string[] = []
    c.runCommand = async (_cmd, o) => {
      workdirs.push(o?.workdir ?? "")
      return { stdout: "## main", stderr: "", code: 0 }
    }
    const r = await exploreDef.tools!.git.execute({ project: "app", action: "status" }, c)
    expect(r.output).toContain("main")
    expect(workdirs).toEqual([c.resolveProjectPath("app")])
    rmSync(home, { recursive: true, force: true })
  })
})
