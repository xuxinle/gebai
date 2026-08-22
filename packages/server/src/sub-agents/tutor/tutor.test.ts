import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import type { ToolContext } from "../../core/types"
import { def, name, tools } from "./tutor"

function ctx(home: string): ToolContext {
  return {
    user: "u1",
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

describe("tutor sub-agent", () => {
  test("def 结构：tutor 工具（profile/mistake_add/mistake_list/mistake_review/mistake_remove）", () => {
    expect(def.name).toBe("tutor")
    expect(name).toBe("tutor")
    expect(Object.keys(tools).sort()).toEqual(["mistake_add", "mistake_list", "mistake_remove", "mistake_review", "profile"])
    expect(def.preload).toBe(false)
    // 删除不可恢复，需审批；其余辅导流程工具免审批（用户自己的学习数据）
    expect(def.requiresApproval).toEqual({ mistake_remove: true })
    expect(tools.mistake_remove.requiresApproval).toBe(true)
    expect(tools.mistake_add.requiresApproval).toBeFalsy()
    expect(tools.profile.requiresApproval).toBeFalsy()
  })

  test("profile：无参读取（未建档提示建档）、传参合并更新、回读", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-subagent-"))
    try {
      const c = ctx(home)
      const empty = await tools.profile.execute({}, c)
      expect(empty.output).toContain("尚未建立学习档案")
      const saved = await tools.profile.execute({ grade: "初三", goal: "中考 650" }, c)
      expect(saved.output).toContain("学习档案已更新")
      expect(saved.output).toContain("年级: 初三")
      const read = await tools.profile.execute({}, c)
      expect(read.output).toContain("目标: 中考 650")
      // 空串清除
      const cleared = await tools.profile.execute({ goal: "" }, c)
      expect(cleared.output).not.toContain("中考 650")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("错题本闭环：登记 → 清单（摘要/full）→ 复习汇报 → 删除", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-subagent-"))
    try {
      const c = ctx(home)
      const added = await tools.mistake_add.execute(
        { subject: "数学", topic: "一元二次方程", question: "解方程 x²-3x+2=0", correct_answer: "x=1 或 x=2", analysis: "因式分解未取全解" },
        c,
      )
      expect(added.output).toContain("错题已登记")
      const id = (added.data as { id: string }).id
      expect(id).toBeTruthy()

      const list = await tools.mistake_list.execute({ subject: "数学" }, c)
      expect(list.output).toContain("复习中错题")
      expect(list.output).toContain("题目摘要")
      expect(list.output).not.toContain("正确答案")

      const full = await tools.mistake_list.execute({ full: true }, c)
      expect(full.output).toContain("正确答案: x=1 或 x=2")
      expect(full.output).toContain("解析: 因式分解未取全解")

      const pass = await tools.mistake_review.execute({ id, result: "pass" }, c)
      expect(pass.output).toContain("已通过 1/5 次")
      expect(pass.output).toContain("下次复习")

      const master = await tools.mistake_review.execute({ id, result: "master" }, c)
      expect(master.output).toContain("已掌握归档")

      const afterMaster = await tools.mistake_list.execute({}, c)
      expect(afterMaster.output).toContain("没有符合条件的错题")

      const missing = await tools.mistake_review.execute({ id: "noexist123", result: "pass" }, c)
      expect(missing.output).toContain("不存在")

      const removed = await tools.mistake_remove.execute({ id }, c)
      expect(removed.output).toContain("错题已删除")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("mistake_add 必填缺失时以工具错误反馈", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-subagent-"))
    try {
      const c = ctx(home)
      await expect(tools.mistake_add.execute({ subject: "", topic: "t", question: "q" }, c)).rejects.toThrow("学科")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
