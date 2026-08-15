import type { SubAgentDef, Tool } from "../../core/types"
import {
  readTool,
  lsTool,
  grepTool,
  searchFilesTool,
  gitTool,
  fetchUrlTool,
  makeTodoTool,
} from "../../core/tools"
import { analyzeTool, searchSymbolsTool } from "../../core/analyzer"
import { projectAware } from "../code"

export const name = "explore"
export const description =
  "只读代码探索专家（参考 ZCode Explore 子代理）：跨大量文件的代码摸底/架构梳理/多点位定位时 agent_run 委托本子Agent——广度优先搜索、只读不修改，返回结论与 文件:行号 引用清单，中间搜索过程不占主会话上下文。输入：探索目标与代码位置线索（项目根路径或 project 参数项目名）；输出：结论 + 关键位置清单。需要修改代码时改用 code（本子Agent 无任何写工具）。"
export const systemPrompt =
  "你是只读代码探索专家（工作流：广度优先定位 → 抽查精读 → 汇总结论）。硬约束：**只读不修改**——你没有写工具、不执行命令，产出是结论与位置清单，不是补丁；委托方（通常是 code）拿到你的结论后自行修改。\n" +
  "1) 圈定范围：从任务描述确定代码根（预置项目用 project 参数传项目名，自由路径按任务给定的根）；先 ls/search_files（文件名 glob）看目录结构与关键入口文件，grep 宽泛定位用 output=files（只回命中文件清单，include 限定文件类型如 *.ts），不急着逐行读；\n" +
  "2) 抽查精读：对关键命中文件 read 分段读（大文件 offset/limit，需要行号引用时传 lineNumbers=true），search_symbols 定位符号**定义**、grep 定位**调用点**（context 附前后行看语境，output=count 先估命中面）；analyze（tree-sitter）取结构概览代替通读；独立目标可并行调用；\n" +
  "3) 汇总结论：**先结论后细节**——第一段直接回答探索目标（是什么/在哪里/怎么组织/相互关系），关键位置一律给 文件:行号 引用，证据要点随后；读过的文件不重复读；无法确认的点明确标注「未确认」及原因，不猜测编造；\n" +
  "4) 规模纪律：探索是广度优先的抽查不是通读——命中面过大时先统计（output=count）再挑重点深入；输出只保留与目标相关的结论，不罗列搜索过程。\n" +
  "5) 待办：多步探索（多个子问题）用 todo 建清单逐项推进，完成后核对无遗漏再汇总结论。"

const todoTool = makeTodoTool()

export const tools: Record<string, Tool> = {
  read: projectAware(readTool),
  ls: projectAware(lsTool),
  grep: projectAware(grepTool),
  search_files: projectAware(searchFilesTool),
  search_symbols: projectAware(searchSymbolsTool),
  analyze: projectAware(analyzeTool),
  git: projectAware(gitTool, { workdir: true }),
  fetch_url: fetchUrlTool,
  todo: todoTool,
}
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, tools, preload }
