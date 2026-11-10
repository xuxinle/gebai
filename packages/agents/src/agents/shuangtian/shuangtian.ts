import type { SubAgentDef } from "@gebai/sdk"
// 系统提示词独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./shuangtian.md" with { type: "text" }
import { requiresApproval as toolApprovals, tools as toolSet } from "./shuangtian_tools"

export const name = "shuangtian"
export const description =
  "构建、运行与操控霜天（Shuangtian）原生桌面应用：先 shuangtian_run(action=shape) 探形态（源码树 / 只有分发的二进制 exe）、无头模式开发闭环（构建/启动/停止/测试/禁令扫描）、组件树与选择器查询（tree/find）、属性读写与动作触发（get/set/invoke）、真实鼠标键盘注入（click/type/key）、截图 PNG（无头唯一视觉通道）、视觉树、条件等待、运行指标（后端/无头/DPI/帧耗时）。**另有 shuangtian_doc 按路径读开发文档树**（入口 → 概览/组件族页 → 组件页逐层下钻，单次有字符预算并按行切块，不会被输出护标静默截断）。坐标一律逻辑像素；无任意代码执行入口。**支持无源码运行**：只有构建好的应用 exe 时也能启动/操控/截图（仅 build/test/lint 不可用）。适用于用智能体开发与验证原生界面；真实宿主桌面操作用 desktop 子Agent。内含 code 子代理（`code_*`）负责源码探索与定点修改。"
export const systemPrompt = systemPromptBase

export const tools = toolSet
export const requiresApproval = toolApprovals
export const preload = false

export const envVars = [
  {
    name: "SHUANGTIAN_PROJECT",
    description:
      "霜天框架/分发根。源码形态：含 st.pkg 的仓库目录；按需分发形态：放着应用 exe 的目录。缺省为仓库根 shuangtian/。",
  },
  {
    name: "SHUANGTIAN_TARGET",
    description:
      '默认控制目标：`host:port`、纯端口或控制文件路径。缺省自动读取会话目录 .shuangtian/<app>-control.json（由 shuangtian_run action=start 写入）。',
  },
]

export const def: SubAgentDef = {
  name,
  description,
  systemPrompt,
  tools,
  requiresApproval,
  preload,
  envVars,
  // 依赖连带装载：`code` 的工具以 `code_*` 命名空间并入。
  //
  // 为何必须依赖它（而不是“需要改代码时用户再装载 code”）：
  // 用霜天写应用时，“改源码”不是可选项而是**主循环的一部分**——
  // 一个典型回合是“读现有实现 → 改框架/应用代码 → 构建 → 截图核验”
  // （§2 工作循环与 §6.5「框架缺能力就补齐」都要求动手改源码）。
  // 没有读/改文件的能力，这个循环走不下去：只能看到症状、改不了原因。
  // 依赖表就是把这个**必然会被要求的能力**提前备好，不靠调用方每次记得。
  dependencies: ["code"],
}
