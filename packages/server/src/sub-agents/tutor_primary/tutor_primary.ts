import type { SubAgentDef } from "../../core/types"
// 工具工厂与中学段共享（逻辑一致，学段差异仅学科清单/年级说明/文案前缀）。
import { createTutorTools, PRIMARY_STAGE } from "../tutor_secondary/tools"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./tutor_primary.md"

export const name = "tutor_primary"
export const description =
  "小学（1-6 年级）全科学习辅导：具象化引导解题（实物类比/画图/生活情境，小步引导逐级放手）、口算与应用题训练、看图写话与阅读理解、错题本（自动 1/3/7/14/30 天间隔复习）与知识点掌握度评估（按用户持久保存）、学习习惯培养与家长陪学建议；支持学生/家长/教师三种身份（家长=陪学顾问与进度汇报、教师=备课与课堂演示，档案 role 持久切换），做对后主动引导知识深入/发散/关联并配可动手的动态演示（分数/竖式动画/几何拖动等）。小学生、家长或教师（低年级常由家长代述）的学习辅导需求装载本子Agent（中学辅导用 tutor_secondary）。输入：问题/题目/学习需求；输出：具象化引导讲解、趣味练习与批改、掌握度与错题管理。"
export const systemPrompt = systemPromptBase
const { tools, requiresApproval } = createTutorTools(PRIMARY_STAGE)
export { tools, requiresApproval }
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
