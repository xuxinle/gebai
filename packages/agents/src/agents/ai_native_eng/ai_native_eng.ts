import type { SubAgentDef } from "@gebai/sdk"
// 系统提示词独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./ai_native_eng.md"

export const name = "ai_native_eng"
export const description =
  "AI 原生软件工程方法论（知识型子Agent，不接项目、不写代码）：把「以智能体为执行者、以人为验收者」的开发形态讲清楚——无头闭环（控制通道 + 截图 + 像素断言）、协同自进化（框架问题当场修/写 BACKLOG、沉淀位置四分流）、观感类改动的七条硬约束与八步循环、智能体工程纪律（记忆落盘/审批留痕/逆向验证/「改了没生效」三变体）、跨平台不可见坑。来自霜天框架的完整实践（含四次返工与 20 类运行期缺陷的实测代价）。需要评审开发流程、沉淀方法论、或给新会话指路时装载；需要动手改代码用 code，改霜天用 shuangtian。输入：要评审/讲解的工程环节或问题。"

export const systemPrompt = systemPromptBase
export const preload = false
export const def: SubAgentDef = { name, description, systemPrompt, preload }
