/**
 * 相期子Agent：agent 桌面常驻面板的投递端。
 *
 * 这个文件是**能力模块**（`agent_load` 装载后工具并入当前工具集），
 * 不是独立会话。弹一张卡片不需要起一个 LLM 循环。
 */

import { readFileSync } from "node:fs"

import type { SubAgentDef, ToolSet } from "@gebai/sdk"

import { createTools } from "./tools"

// 提示词以纯 Markdown 维护（便于阅读/评审）；`with { type: "file" }`
// 让 bun build --compile 把资源内联进单文件二进制。
import promptPath from "./xiangqi.md" with { type: "file" }

export const systemPrompt = readFileSync(promptPath, "utf-8").trim()

export const name = "xiangqi"
export const description =
  "相期（Xiāngqī）：在用户桌面上弹常驻卡片——提示他、请他选择、让他待办等待。" +
  "需要用户在任务中途介入（确认、选择、去动手）时装载。工具：card_notice / card_ask / card_list / card_dismiss。"

/** 工具集（依赖注入控制器便于测试）。 */
export const tools = createTools() as unknown as ToolSet

/**
 * 审批：全部免审批。
 *
 * 理由：这些操作的效果是**把信息送到用户眼前**，不是修改任何东西。
 * 加审批会变成「为了提醒用户，先要用户批准提醒」——荒谬，而且会卡住
 * 无人值守场景（那时恰恰最需要把问题推给人）。
 *
 * `card_ask` 本身就是一次「请用户定夺」，它天然带人工介入点，
 * 不需要再加一层审批。
 */
export const requiresApproval: Record<string, boolean> = {
  card_notice: false,
  card_ask: false,
  card_list: false,
  card_dismiss: false,
}

export function toolSchemas() {
  return tools
}

export const envVars = [
  {
    name: "XIANGQI_PORT",
    description: "相期连接器监听端口（缺省 7717；相期用 `--agent ws://127.0.0.1:<port>/xiangqi` 连过来）",
  },
  {
    name: "XIANGQI_HOME",
    description:
      "相期数据根（含 inbox/ outbox/，缺省 `<cwd>/相期`）——必须与相期启动时 `--home` 是同一个，否则文件兼底两边各写各的",
  },
  {
    name: "XIANGQI_WAIT_MS",
    description: "card_ask 缺省等待时限（毫秒，缺省 300000 = 5 分钟，上限 1800000）",
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
