import type { ToolSchema } from "@gebai/sdk"
import type { SubAgentDef, Tool, ToolResult } from "../../core/types"
import {
  addMistake,
  listKnowledge,
  listMistakes,
  loadProfile,
  removeMistake,
  reviewMistake,
  saveProfile,
  upsertKnowledge,
} from "../../core/tutor"
import type { MistakeRecord, ReviewResult, TutorProfile } from "../../core/tutor"
import { MASTERY_LABELS } from "../../core/tutor"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./tutor.md"

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

export const name = "tutor"
export const description =
  "中学（初中+高中）九科学习辅导：引导式解题（先诊断卡点、四级提示逐步放手）、知识点讲解、出题练习与批改、知识点掌握度评估（0-4 级诊断图，按用户持久保存）、错题本（自动 1/3/7/14/30 天间隔复习）、学习档案与备考复习规划。中学生本人或家长的学习辅导需求装载本子Agent。输入：问题/题目/学习需求；输出：引导讲解、练习与批改、掌握度与错题管理。"
export const systemPrompt = systemPromptBase

function fmtDate(ts: number): string {
  return new Date(ts).toLocaleString("zh-CN", { hour12: false })
}

const PROFILE_LABELS: Record<string, string> = {
  grade: "年级",
  textbook: "教材",
  goal: "目标",
  weaknesses: "薄弱点",
  exams: "考试日程",
  notes: "备注",
}

function formatProfile(p: TutorProfile): string {
  const lines = Object.entries(PROFILE_LABELS)
    .filter(([k]) => p[k as keyof TutorProfile] != null)
    .map(([k, label]) => `${label}: ${p[k as keyof TutorProfile]}`)
  return lines.length ? `学习档案（更新 ${fmtDate(p.updatedAt)}）：\n${lines.join("\n")}` : "学习档案为空。"
}

const profile: Tool = {
  name: "profile",
  description:
    "读取/更新学习档案（按用户持久保存：年级、教材、目标、薄弱点、考试日程、教学偏好；跨会话延续辅导进度）。不带任何参数 = 读取当前档案；传任意字段 = 合并更新（传空串清除该字段）。",
  card: { titleParams: ["grade"] },
  parameters: schema({
    grade: { type: "string", description: "年级（初一/初二/初三/高一/高二/高三）" },
    textbook: { type: "string", description: "教材版本（如 人教版、北师大版，多科可分述）" },
    goal: { type: "string", description: "学习目标（如 中考冲刺、期末数学上 110）" },
    weaknesses: { type: "string", description: "薄弱学科与知识点" },
    exams: { type: "string", description: "考试日程（如 6月17-19 中考、11月初期中）" },
    notes: { type: "string", description: "教学偏好与其他备注（如 只要思路不要答案）" },
  }),
  async execute(args, ctx): Promise<ToolResult> {
    const patch: Record<string, string> = {}
    for (const key of Object.keys(PROFILE_LABELS)) {
      if (args[key] != null) patch[key] = String(args[key])
    }
    if (!Object.keys(patch).length) {
      const p = await loadProfile(ctx.home, ctx.user)
      if (!p) {
        return { output: "尚未建立学习档案。传任意字段即可建档；或先用 ask 询问学生年级/教材/目标后建档。", data: null }
      }
      return { output: formatProfile(p), data: p }
    }
    const p = await saveProfile(ctx.home, ctx.user, patch)
    return { output: `学习档案已更新。\n${formatProfile(p)}`, data: p }
  },
}

const knowledge: Tool = {
  name: "knowledge",
  description:
    "知识点掌握度评估图（按用户持久保存）：讲解/出题前查掌握度定切入深度，练习/复习后按表现更新。设置：传 subject+topic+mastery（+可选 evidence 评估依据，如「练习 3/5 对」）；查询：不传 mastery（可按 subject 过滤、max_mastery 只列薄弱项，规划时用）。掌握度是引导式教学的诊断依据——薄弱项优先讲练。",
  card: { titleParams: ["subject", "topic"] },
  parameters: schema({
    subject: { type: "string", description: "学科（设置必填；查询时可单独传作过滤）" },
    topic: { type: "string", description: "知识点（如 一元二次方程；设置必填，查询留空列整科）" },
    mastery: { type: "number", description: "掌握度（设置必填）：0 未学 / 1 薄弱 / 2 一般 / 3 良好 / 4 掌握" },
    evidence: { type: "string", description: "评估依据（如：练习 3/5 对、口答完整、月考失分、复习又错）" },
    max_mastery: { type: "number", description: "查询过滤：只列掌握度 ≤ 该值的项（找薄弱点，如传 2）" },
  }),
  async execute(args, ctx): Promise<ToolResult> {
    if (args.mastery != null) {
      const k = await upsertKnowledge(ctx.home, ctx.user, {
        subject: String(args.subject ?? ""),
        topic: String(args.topic ?? ""),
        mastery: Number(args.mastery),
        evidence: args.evidence != null ? String(args.evidence) : undefined,
      })
      return {
        output: `掌握度已更新：${k.subject}·${k.topic} → ${MASTERY_LABELS[k.mastery]}（${k.mastery}/4）${k.history[0] ? `\n轨迹: ${k.history.join("；")}` : ""}`,
        data: k,
      }
    }
    const list = await listKnowledge(ctx.home, ctx.user, {
      subject: args.subject != null ? String(args.subject) : undefined,
      maxMastery: args.max_mastery != null ? Number(args.max_mastery) : undefined,
    })
    if (!list.length) {
      return { output: "暂无掌握度记录。讲解/练习后按表现评估登记（传 subject+topic+mastery）。", data: { items: [] } }
    }
    const lines = list.map(
      (k) => `- ${k.subject}·${k.topic}：${MASTERY_LABELS[k.mastery]}（${k.mastery}/4）${k.history.length ? `，轨迹: ${k.history.join("；")}` : ""}`,
    )
    return { output: `共 ${list.length} 项掌握度（薄弱在前）：\n${lines.join("\n")}`, data: { items: list } }
  },
}

const mistakeAdd: Tool = {
  name: "mistake_add",
  description:
    "错题登记（按用户持久保存、自动排期复习：次日首复，通过后 1/3/7/14/30 天递进，连续 5 次通过视为掌握）。仅登记有价值的典型错题（知识点漏洞、易错陷阱），经学生认可后登记，不全录。",
  card: { titleParams: ["subject", "topic"] },
  parameters: schema(
    {
      subject: { type: "string", description: "学科：语文/数学/英语/物理/化学/生物/政治/历史/地理" },
      topic: { type: "string", description: "知识点（如 一元二次方程、定语从句）" },
      question: { type: "string", description: "题目原文（完整题干与选项）" },
      student_answer: { type: "string", description: "学生当时的错误答案/思路" },
      correct_answer: { type: "string", description: "正确答案" },
      analysis: { type: "string", description: "错因分析与正确思路（简明）" },
      source: { type: "string", description: "题目来源（如 作业、月考卷二）" },
    },
    ["subject", "topic", "question"],
  ),
  async execute(args, ctx): Promise<ToolResult> {
    const m = await addMistake(ctx.home, ctx.user, {
      subject: String(args.subject ?? ""),
      topic: String(args.topic ?? ""),
      question: String(args.question ?? ""),
      studentAnswer: args.student_answer != null ? String(args.student_answer) : undefined,
      correctAnswer: args.correct_answer != null ? String(args.correct_answer) : undefined,
      analysis: args.analysis != null ? String(args.analysis) : undefined,
      source: args.source != null ? String(args.source) : undefined,
    })
    return {
      output: `错题已登记: ${m.id}（${m.subject}·${m.topic}）\n下次复习: ${fmtDate(m.nextReviewAt)}。复习时用 tutor_mistake_list 取题、学生重做后用 tutor_mistake_review 汇报结果。`,
      data: { id: m.id, subject: m.subject, topic: m.topic, nextReviewAt: m.nextReviewAt },
    }
  },
}

function mistakeLine(m: MistakeRecord, now: number): string {
  const progress = m.status === "mastered" ? "已掌握" : `已通过 ${m.reviewCount}/5 次`
  const schedule = m.status === "mastered" ? "" : m.nextReviewAt <= now ? "，已到期" : `，下次复习 ${fmtDate(m.nextReviewAt)}`
  return `- ${m.id}（${m.subject}·${m.topic}）${progress}${schedule}，累计复习 ${m.reviewTotal} 次`
}

const mistakeList: Tool = {
  name: "mistake_list",
  description:
    "查看错题清单（默认列复习中的，按下次复习时间升序；可按学科/知识点过滤，due=true 只列到期该复习的）。默认只含题目摘要；要完整题目/答案/解析（复习出题前）传 full=true。已掌握的传 status=mastered 翻查。",
  card: { titleParams: ["subject", "topic"] },
  parameters: schema({
    subject: { type: "string", description: "学科精确过滤（如 数学）" },
    topic: { type: "string", description: "知识点子串过滤（如 方程 匹配 一元二次方程）" },
    status: { enum: ["active", "mastered"], description: "active=复习中（默认）/ mastered=已掌握" },
    due: { type: "boolean", description: "true=只列已到期待复习的错题" },
    full: { type: "boolean", description: "true=每题附完整题目/学生答案/正确答案/解析" },
  }),
  async execute(args, ctx): Promise<ToolResult> {
    const now = Date.now()
    const list = await listMistakes(ctx.home, ctx.user, {
      subject: args.subject != null ? String(args.subject) : undefined,
      topic: args.topic != null ? String(args.topic) : undefined,
      status: args.status === "mastered" ? "mastered" : undefined,
      dueBefore: args.due === true ? now : undefined,
    })
    if (!list.length) {
      return { output: "没有符合条件的错题。", data: { items: [] } }
    }
    const dueCount = list.filter((m) => m.status === "active" && m.nextReviewAt <= now).length
    const lines = list.map((m) => {
      let out = mistakeLine(m, now)
      if (args.full === true) {
        out += `\n  题目: ${m.question}`
        if (m.studentAnswer != null) out += `\n  学生答案: ${m.studentAnswer}`
        if (m.correctAnswer != null) out += `\n  正确答案: ${m.correctAnswer}`
        if (m.analysis != null) out += `\n  解析: ${m.analysis}`
        if (m.source != null) out += `\n  来源: ${m.source}`
      } else {
        const excerpt = m.question.length > 80 ? `${m.question.slice(0, 80)}…` : m.question
        out += `\n  题目摘要: ${excerpt}（完整内容传 full=true）`
      }
      return out
    })
    const head = args.status === "mastered" ? `共 ${list.length} 道已掌握错题：` : `共 ${list.length} 道复习中错题（其中 ${dueCount} 道已到期）：`
    return { output: `${head}\n${lines.join("\n")}`, data: { items: list } }
  },
}

const mistakeReview: Tool = {
  name: "mistake_review",
  description:
    "错题复习结果汇报：学生重做该题后按结果汇报，自动排下次复习。pass=这次做对（间隔递增 1/3/7/14/30 天，连续 5 次通过自动掌握）；fail=又错（间隔重置为 1 天）；master=已彻底掌握（直接归档）。复习流程：tutor_mistake_list 取题 → 学生重做 → 本工具汇报。",
  card: { titleParams: ["id", "result"] },
  parameters: schema(
    {
      id: { type: "string", description: "错题 ID（tutor_mistake_list 查看）" },
      result: { enum: ["pass", "fail", "master"], description: "复习结果：pass=做对 / fail=又错 / master=彻底掌握" },
    },
    ["id", "result"],
  ),
  async execute(args, ctx): Promise<ToolResult> {
    const result = String(args.result) as ReviewResult
    const m = await reviewMistake(ctx.home, ctx.user, String(args.id ?? ""), result)
    if (!m) return { output: `错题不存在或 id 非法: ${args.id}（可用 tutor_mistake_list 查看）。` }
    if (m.status === "mastered") {
      return { output: `错题 ${m.id}（${m.subject}·${m.topic}）已掌握归档，不再排期复习。`, data: m }
    }
    return {
      output: `错题 ${m.id}（${m.subject}·${m.topic}）复习结果已记录（${result === "pass" ? "做对" : "又错"}，已通过 ${m.reviewCount}/5 次），下次复习: ${fmtDate(m.nextReviewAt)}。`,
      data: m,
    }
  },
}

const mistakeRemove: Tool = {
  name: "mistake_remove",
  description: "删除错题（按 id，不可恢复，需审批）。仅学生明确要求移除时使用；只是「做对了」不移除——用 tutor_mistake_review 走掌握流程。",
  requiresApproval: true,
  card: { titleParams: ["id"] },
  parameters: schema({ id: { type: "string", description: "要删除的错题 ID" } }, ["id"]),
  async execute(args, ctx): Promise<ToolResult> {
    const removed = await removeMistake(ctx.home, ctx.user, String(args.id ?? ""))
    return removed
      ? { output: `错题已删除: ${args.id}` }
      : { output: `错题不存在或 id 非法: ${args.id}（可用 tutor_mistake_list 查看）。` }
  },
}

export const tools: Record<string, Tool> = {
  profile,
  knowledge,
  mistake_add: mistakeAdd,
  mistake_list: mistakeList,
  mistake_review: mistakeReview,
  mistake_remove: mistakeRemove,
}
export const requiresApproval = { mistake_remove: true }
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
