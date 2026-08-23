import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import type { ToolContext } from "../../core/types"
import { PRIMARY_STAGE, SECONDARY_STAGE, createTutorTools } from "./tools"

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

const EXPECTED_TOOLS = ["demo", "knowledge", "mistake_add", "mistake_list", "mistake_remove", "mistake_review", "profile"]

describe("createTutorTools（学段工厂）", () => {
  test("两学段工具集一致、审批声明一致、学科/年级注入学段配置", () => {
    const sec = createTutorTools(SECONDARY_STAGE)
    const pri = createTutorTools(PRIMARY_STAGE)
    expect(Object.keys(sec.tools).sort()).toEqual(EXPECTED_TOOLS)
    expect(Object.keys(pri.tools).sort()).toEqual(EXPECTED_TOOLS)
    expect(sec.requiresApproval).toEqual({ mistake_remove: true })
    expect(pri.requiresApproval).toEqual({ mistake_remove: true })
    // 学科清单注入 subject 参数描述
    expect(JSON.stringify(sec.tools.mistake_add.parameters)).toContain("物理")
    expect(JSON.stringify(pri.tools.mistake_add.parameters)).toContain("口算")
    expect(JSON.stringify(pri.tools.mistake_add.parameters)).not.toContain("物理")
    // 年级说明注入 profile
    expect(JSON.stringify(pri.tools.profile.parameters)).toContain("小学一年级")
    expect(JSON.stringify(sec.tools.profile.parameters)).toContain("初一")
  })

  test("输出文案引用各学段命名空间前缀", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-tools-"))
    try {
      const c = ctx(home)
      const sec = createTutorTools(SECONDARY_STAGE)
      const added = await sec.tools.mistake_add.execute({ subject: "数学", topic: "一元二次方程", question: "q" }, c)
      expect(added.output).toContain("tutor_secondary_mistake_list")

      const pri = createTutorTools(PRIMARY_STAGE)
      const added2 = await pri.tools.mistake_add.execute({ subject: "数学", topic: "进位加法", question: "q" }, c)
      expect(added2.output).toContain("tutor_primary_mistake_review")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("profile：无参读取（未建档提示建档）、传参合并更新、空串清除", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-tools-"))
    try {
      const c = ctx(home)
      const { tools } = createTutorTools(SECONDARY_STAGE)
      const empty = await tools.profile.execute({}, c)
      expect(empty.output).toContain("尚未建立学习档案")
      const saved = await tools.profile.execute({ grade: "初三", goal: "中考 650" }, c)
      expect(saved.output).toContain("学习档案已更新")
      expect(saved.output).toContain("年级: 初三")
      const read = await tools.profile.execute({}, c)
      expect(read.output).toContain("目标: 中考 650")
      const cleared = await tools.profile.execute({ goal: "" }, c)
      expect(cleared.output).not.toContain("中考 650")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("knowledge：设置掌握度与轨迹、查询薄弱在前、max_mastery 过滤", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-tools-"))
    try {
      const c = ctx(home)
      const { tools } = createTutorTools(SECONDARY_STAGE)
      const empty = await tools.knowledge.execute({}, c)
      expect(empty.output).toContain("暂无掌握度记录")

      const set = await tools.knowledge.execute({ subject: "数学", topic: "一元二次方程", mastery: 2, evidence: "作业 4/5 对" }, c)
      expect(set.output).toContain("掌握度已更新")
      expect(set.output).toContain("设为 2 作业 4/5 对")

      const down = await tools.knowledge.execute({ subject: "数学", topic: "一元二次方程", mastery: 1, evidence: "练习 3/5 对" }, c)
      expect(down.output).toContain("2→1 练习 3/5 对")

      await tools.knowledge.execute({ subject: "物理", topic: "浮力", mastery: 3 }, c)
      const list = await tools.knowledge.execute({}, c)
      expect(list.output).toContain("薄弱在前")
      expect(list.output.indexOf("一元二次方程")).toBeLessThan(list.output.indexOf("浮力"))

      const weak = await tools.knowledge.execute({ max_mastery: 2 }, c)
      expect(weak.output).toContain("一元二次方程")
      expect(weak.output).not.toContain("浮力")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("错题本闭环：登记 → 清单（摘要/full）→ 复习汇报 → 删除", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-tools-"))
    try {
      const c = ctx(home)
      const { tools } = createTutorTools(PRIMARY_STAGE)
      const added = await tools.mistake_add.execute(
        { subject: "数学", topic: "进位加法", question: "36+27=?", correct_answer: "63", analysis: "个位满十未进位（计算失误）" },
        c,
      )
      expect(added.output).toContain("错题已登记")
      const id = (added.data as { id: string }).id

      const list = await tools.mistake_list.execute({ subject: "数学" }, c)
      expect(list.output).toContain("复习中错题")
      expect(list.output).toContain("题目摘要")
      expect(list.output).not.toContain("正确答案")

      const full = await tools.mistake_list.execute({ full: true }, c)
      expect(full.output).toContain("正确答案: 63")
      expect(full.output).toContain("计算失误")

      const pass = await tools.mistake_review.execute({ id, result: "pass" }, c)
      expect(pass.output).toContain("已通过 1/5 次")

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

  test("demo：模板渲染经 show 展示（html 内容块）；未知模板/参数错误给出引导", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-tools-"))
    try {
      const c = ctx(home)
      const { tools } = createTutorTools(SECONDARY_STAGE)
      // quiz 模板闭环：返回 html 内容块 + 简短回执
      const ok = await tools.demo.execute(
        { template: "quiz", params: { title: "小测", questions: [{ q: "1+1", options: ["1", "2"], answer: "2" }] } },
        c,
      )
      expect(ok.output).toContain("已生成并在聊天内展示")
      expect(ok.blocks?.some((b) => (b as { type?: string }).type === "html")).toBe(true)
      expect((ok.blocks?.[0] as { html?: string }).html).toContain("提交批改")
      // 未知模板
      const miss = await tools.demo.execute({ template: "nope" }, c)
      expect(miss.output).toContain("未知模板")
      // 参数校验错误（questions 空）
      const bad = await tools.demo.execute({ template: "quiz", params: {} }, c)
      expect(bad.output).toContain("参数错误")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("mistake_add 必填缺失时以工具错误反馈", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-tutor-tools-"))
    try {
      const c = ctx(home)
      const { tools } = createTutorTools(SECONDARY_STAGE)
      await expect(tools.mistake_add.execute({ subject: "", topic: "t", question: "q" }, c)).rejects.toThrow("学科")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
