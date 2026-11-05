import type { SubAgentDef, Tool } from "@gebai/sdk"
// 系统提示词独立 md 维护（目录形式约定：`{dir}/{dir}.md`，见 AGENTS.md「如何新增子 Agent」）。
// 不内联成字符串：提示词里有大量代码块与反斜杠转义，写成 TS 字面量既难读也难改。
import systemPromptBase from "./jianmu.md" with { type: "text" }
import { modelTools } from "./model_tools"

/**
 * 建木（Jianmu）：用霜天框架的三维工作台 `jianmu` 做建模与演示。
 *
 * ## 为什么单独一个子代理（而不是让 shuangtian 顺便干）
 *
 * 三维的**失败模式几乎都不报错**：相机钻进物体、物体全在取景框外、光方向反了、
 * 环境反射缺失导致金属死黑——画面看上去都"像是画了点什么"。要判定就得知道
 * 这套视口的状态面（`objects`/`picked`/`error`/`camera`）和两种脚本各自的语义差异
 * （three.js Y-up vs bpy Z-up、色调映射默认值、哪个表达层擅长什么）。
 * 这些是**建木自己的知识**，塞进通用霜天子代理会让后者每遇三维就重新推猜一遍。
 *
 * ## 工具面
 *
 * - 霜天全套（`shuangtian_*`，依赖连带装载）：构建/启动/截图/取属性/注入输入
 * - 本代理自有三个工具（`model_tools.ts`）：场景清单、脚本校验与出图、静态检查
 */
export const name = "jianmu"

export const description =
  "建木（Jianmu）：用霜天框架的三维建模与演示工作台做模型与演示——写 three.js 场景脚本、写 bpy 建模脚本、" +
  "热重载实时预览、轨道相机观察、出单帧图；支持两种表达层的混合工作流（bpy 产资产 → three.js 组装）。 " +
  "适用于「做一个三维场景 / 产品展示 / 结构演示」「参数化建模（齿轮、机械件、程序化几何）」" +
  "「检查模型为什么画不出来 / 为什么看着假」「给裁云或镂月准备三维素材」。 " +
  "输入：三维需求（要做什么、什么风格、要不要动画）或现有脚本路径。 " +
  "内含霜天子代理的全套工具（`shuangtian_*`）。"

// ## 为何要 `.replace(/\r\n/g, "\n")`（实测）
//
// bun 的 `with { type: "text" }` 在 Windows 上会把 `\n` 转成 `\r\n`，提示词里凭空
// 多出几百个 `\r`（日志看不见但进了模型上下文），且与 md 源码不再逐字一致。
export const systemPrompt = systemPromptBase.replace(/\r\n/g, "\n")

export const tools: Record<string, Tool> = { ...modelTools }
export const preload = false

// 零依赖冲突：`jianmu` 与 `shuangtian` 不构成前缀关系，工具各以自身命名空间注册
export const def: SubAgentDef = {
  name,
  description,
  systemPrompt,
  tools,
  preload,
  dependencies: ["shuangtian"],
}
