/**
 * 相期子Agent：agent 桌面常驻面板的投递端。
 *
 * 这个文件是**能力模块**（`agent_load` 装载后工具并入当前工具集），
 * 不是独立会话。弹一张卡片不需要起一个 LLM 循环。
 */

import type { SubAgentDef, ToolSet } from "@gebai/sdk"

import { createTools } from "./tools"

// 提示词以纯 Markdown 维护（便于阅读/评审）；用 `with { type: "text" }` 直接取原文——
// 不能靠 Bun 对 .md 的缺省 loader（会包 <p> 把提示词变成 HTML 串）。
import systemPromptRaw from "./xiangqi.md" with { type: "text" }

export const systemPrompt = String(systemPromptRaw).trim()

export const name = "xiangqi"
export const description =
  "相期（Xiāngqī）：在用户桌面上弹常驻卡片——提示他、请他选择、让他待办等待、送他一个去授权的链接。" +
  "需要用户在任务中途介入（确认、选择、去动手、去授权）时装载。" +
  "工具：card_notice / card_ask / card_authorize / card_list / card_dismiss。"

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
  card_authorize: false,
  card_list: false,
  card_dismiss: false,
}

export function toolSchemas() {
  return tools
}

export const envVars = [
  {
    name: "XIANGQI_PORT",
    description:
      "相期控制器自己 listen() 时的端口（缺省 7717）。方案 B 下窗口连的是**主服务端口**，只有「无主服务的独立使用」场景才需要它",
  },
  {
    name: "XIANGQI_HOME",
    description:
      "相期数据根（含 inbox/ outbox/，缺省 `{GEBAI_HOME}/xiangqi`）——必须与相期启动时 `--home` 是同一个，否则文件兼底两边各写各的",
  },
  {
    name: "XIANGQI_BIN",
    description: "拉起相期窗口用的 exe 路径（设了只用它，不再按部署副本/仓库标记探测；指错会如实报未找到）",
  },
  {
    name: "XIANGQI_AUTO",
    description: "`off` = 拟人模式启动时不自动拉起相期窗口；其余值或未设 = 自动拉起",
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
