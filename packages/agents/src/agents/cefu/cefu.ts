/**
 * 册府子Agent：结构化知识库（TS 直接控制霜天册府应用的无头形态）。
 *
 * 能力模块（`agent_load` 装载后工具并入当前工具集）——查证类工具不建独立 LLM 循环；
 * 与人格的 `agents: ["cefu"]` 装载共用同一工具面（`createTools` 每次调用现取控制器单例，
 * 见 tools.ts 的「为何 ctl 是每次取」注释）。
 */

import type { SubAgentDef, ToolSet } from "@gebai/sdk"

import { createTools } from "./tools"

import systemPromptRaw from "./cefu.md" with { type: "text" }

export const systemPrompt = String(systemPromptRaw).trim()

export const name = "cefu"
export const description =
  "册府（结构化知识库）子代理（C++ 边车常驻进程）：把「既成事实」存成能翻、能查、能整理的工具书条目。cefu_search 中文自然语言检索（1+2-gram 分词、字段加权 BM25、别名短语面、返回为何命中）、cefu_get 取全文（现象/有序步骤/命令/硬事实/出处）、cefu_put 结构化写入（强制 title/category/source + 近似查重拒绝）、cefu_list 按分类成册浏览、cefu_review 整理巡检（重复/过期/冷条目/分类漂移）、cefu_confirm 标为已确认、cefu_stats 概览。用于回答运维/环境/「找谁」类问题前先查证，以及把用户纠正与实测结论沉淀下来"

export const tools = createTools() as unknown as ToolSet

/**
 * 审批：检索/读取类免审（查询是只读）；写入类需审批（知识库是长期资产，
 * 写错条目会误导后续所有依赖它的判断——与相期「提醒免审」的取向相反）。
 */
export const requiresApproval: Record<string, boolean> = {
  cefu_search: false,
  cefu_get: false,
  cefu_list: false,
  cefu_stats: false,
  cefu_put: true,
  cefu_review: false,
  cefu_confirm: false,
}

export function toolSchemas() {
  return tools
}

export const envVars = [
  {
    name: "CEFU_BIN",
    description: "册府 exe 路径（设了只用它；缺省按 GEBAI_HOME/shuangtian build/仓库树/PATH 探测）",
  },
  {
    name: "CEFU_HOME",
    description: "册府数据根（含 cefu.db 与 ctl.json，缺省 {GEBAI_HOME}/data/cefu）",
  },
]

export const def: SubAgentDef = {
  name,
  description,
  systemPrompt,
  tools,
  requiresApproval,
  envVars,
}
