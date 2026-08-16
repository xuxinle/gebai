import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import type { ToolContext } from "../../core/types"
import type { CronTask } from "../../core/cron"
import { def, name, tools } from "./cron"

function ctx(home: string): ToolContext {
  return {
    user: "default",
    sessionId: "s1",
    workdir: home,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => p,
    readFile: async () => "x",
    readBinaryFile: async () => new Uint8Array(),
    writeFile: async () => {},
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (ref) => ref.name,
    publish: () => {},
    registry: {
      schemas: () => [],
      resolve: () => undefined,
      getAgentNames: () => [],
    },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    runNewSession: async () => ({ output: "", archive: {} as never }),
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => null,
    waitForCapture: async () => null,
    projects: [],
    resolveProjectPath: () => home,
    getTodos: async () => [],
    setTodos: async () => {},
  }
}

describe("cron sub-agent", () => {
  test("def 结构与命名空间：cron_* 工具（add/list/update/remove）", () => {
    expect(def.name).toBe("cron")
    expect(name).toBe("cron")
    expect(Object.keys(tools).sort()).toEqual(["add", "list", "remove", "update"])
    expect(def.preload).toBe(false)
    // 定时任务 = 无人值守的任意命令/会话执行：创建/修改/删除均需审批（防多用户模式绕过审批边界）
    expect(def.requiresApproval).toEqual({ add: true, update: true, remove: true })
    expect(tools.add.requiresApproval).toBe(true)
    expect(tools.update.requiresApproval).toBe(true)
    expect(tools.remove.requiresApproval).toBe(true)
    expect(tools.list.requiresApproval).toBeFalsy()
  })

  test("tools delegate to ctx.cron and report disabled capability", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-cron-subagent-"))
    try {
      const c = ctx(home)

      // 未启用（ctx.cron 为空）：明确提示，不抛错
      const disabled = await tools.add.execute({ schedule: "0 9 * * *", type: "script", script: "echo" }, c)
      expect(disabled.output).toContain("未启用")

      // 启用：add 透传并格式化输出
      const added: CronTask = { id: "t1", sessionId: "s1", user: "default", name: "daily", type: "script", schedule: "0 9 * * *", script: "echo hi", enabled: true, createdAt: 1, updatedAt: 1, nextRunAt: 2, runCount: 0 }
      c.cron = {
        add: async (input) => ({ ...added, ...input, id: "t1" }),
        list: async () => [added],
        remove: async (id) => id === "t1",
        update: async (id, patch) => (id === "t1" ? { ...added, ...patch } : null),
      }
      const r = await tools.add.execute({ name: "daily", schedule: "0 9 * * *", type: "script", script: "echo hi" }, c)
      expect(r.output).toContain("t1")
      expect(r.output).toContain("下次执行")

      const listed = await tools.list.execute({}, c)
      expect(listed.output).toContain("t1")
      expect(listed.output).toContain("echo hi")

      const updated = await tools.update.execute({ id: "t1", enabled: false }, c)
      expect(updated.output).toContain("已更新")

      const removed = await tools.remove.execute({ id: "t1" }, c)
      expect(removed.output).toContain("已删除")
      const missing = await tools.remove.execute({ id: "nope" }, c)
      expect(missing.output).toContain("不存在")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
