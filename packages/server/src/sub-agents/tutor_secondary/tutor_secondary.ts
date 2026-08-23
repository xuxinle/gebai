import type { SubAgentDef } from "../../core/types"
import { createTutorTools, SECONDARY_STAGE } from "./tools"
// 系统提示词拆为独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./tutor_secondary.md"

export const name = "tutor_secondary"
export const description =
  "中学（初中+高中）九科学习辅导：引导式解题（先诊断卡点、四级提示逐步放手）、知识点讲解、出题练习与批改、知识点掌握度评估（0-4 级诊断图，按用户持久保存）、错题本（自动 1/3/7/14/30 天间隔复习）、学习档案与备考复习规划。中学生本人或家长的学习辅导需求装载本子Agent（小学辅导用 tutor_primary）。输入：问题/题目/学习需求；输出：引导讲解、练习与批改、掌握度与错题管理。"
export const systemPrompt = systemPromptBase
const { tools, requiresApproval } = createTutorTools(SECONDARY_STAGE)
export { tools, requiresApproval }
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
