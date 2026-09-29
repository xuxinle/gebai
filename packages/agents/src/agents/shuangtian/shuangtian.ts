import type { SubAgentDef } from "@gebai/sdk"
// 系统提示词独立 md 维护（目录形式约定：{dir}/{dir}.md）。
import systemPromptBase from "./shuangtian.md"
import { requiresApproval as toolApprovals, tools as toolSet } from "./shuangtian_tools"

export const name = "shuangtian"
export const description =
  "构建、运行与操控霜天（Shuangtian）原生桌面应用：无头模式开发闭环（构建/启动/停止/测试/禁令扫描）、" +
  "组件树与选择器查询（tree/find）、属性读写与动作触发（get/set/invoke）、真实鼠标键盘注入（click/type/key，" +
  "逻辑像素坐标）、截图 PNG（无头唯一视觉通道，回归直接可见）、视觉树（visual）、条件等待（wait）、运行指标" +
  "（metrics：后端/无头/DPI/帧耗时）。坐标一律逻辑像素；TCP 控制通道协议 st-control/1，无任意代码执行入口。" +
  "适用于用智能体开发与验证原生界面；真实宿主桌面操作用 desktop 子Agent。"
export const systemPrompt = systemPromptBase

export const tools = toolSet
export const requiresApproval = toolApprovals
export const preload = false

export const envVars = [
  {
    name: "SHUANGTIAN_PROJECT",
    description:
      "霜天框架工程根（含 st.pkg 的目录）。缺省为仓库根 shuangtian/，二进制形态回退 {GEBAI_HOME}/vendor/shuangtian。",
  },
  {
    name: "SHUANGTIAN_TARGET",
    description:
      '默认控制目标：`host:port`、纯端口或控制文件路径。缺省自动读取会话目录 .shuangtian/<app>-control.json（由 shuangtian_run action=start 写入）。',
  },
  {
    name: "SHUANGTIAN_FRAMEWORK",
    description: "随包分发的框架源码位置（离线环境用；与 SHUANGTIAN_PROJECT 同义，取其一）。",
  },
]

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars }
