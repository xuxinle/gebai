import type { SubAgentDef, Tool } from "@gebai/sdk"
// 系统提示词独立 md 维护（目录形式约定：`{dir}/{dir}.md`，见 AGENTS.md「如何新增子 Agent」）。
// 为何不内联成字符串数组：提示词有 400 行且含大量反斜杠转义（`\n`、JSON 片段、代码块），
// 写在 TS 字面量里既难读也难改，而且**改动错了不会报错**（只让智能体读到半句指令）。
import systemPromptBase from "./caiyun.md" with { type: "text" }
// 成片质检：读渲染器量好的画面质量 + 可选的配音 WAV 指标，出可复算的判据。
// 度量能力属于剪辑软件自己（它在渲染时就知道每镜边界与每帧时刻），
// 不是旁挂子代理——见 verify.ts 顶部说明。
import { verifyTool } from "./verify"
// 风格模板库：七种叙事模板的方法论（含「用裁云怎么落地」）。
// 放在本目录的 `styles/`：它是**裁云的知识**（讲具体图层与 layout 写法），
// 跟着 Agent 走才不会与框架文档各自漂移；霜天仓库只放可运行的示例脚本。
import { styleGuideTool } from "./style_guide"

// 工具面全部走依赖（DESIGN「子Agent 依赖与自动装载」）：霜天全套（shuangtian）、图像生成（imagegen）、
// 语音与音效（tts）——本代理零自有工具，三个依赖的工具各以自己的命名空间注册
// （`shuangtian_*` / `imagegen_*` / `tts_*`）。不内联展开：同一能力只允许一个来源，
// 否则会出现两份审批映射、两个命名空间，且提示词里该写哪个名字会分叉。

export const name = "caiyun"

export const description =
  "裁云（Nimbus）：用霜天框架做 AI 视频——写导演脚本、选素材、配音与音效、出片并核验。" +
  "裁云是 AI 驱动视频生成器（真值源 = JSON 导演脚本；自研 C++ 渲染器，纯离线无外部依赖），" +
  "装载后可直接驱动应用出 mp4（含配乐、配音、音效，以及景深/光晕/粒子/运镜等影像特效；" +
  "**支持真三维模型图层**——盒子/球/圆环/圆柱/平面/OBJ，有显卡走显卡、无显卡自动降级）。" +
  "适用于「做一条宣传片 / 产品视频 / 发布会短片 / 动效短片」「改已有片子的分镜与文案」「生成素材并合成」。 " +
  "输入：视频需求（主题/时长/风格/尺寸）或现有脚本路径。 " +
  "内含霜天子代理的全套工具（`shuangtian_*`）+ 依赖连带装载的图像生成（`imagegen_*`）与语音音效（`tts_*`）。"

// ## 为何要 `.replace(/\r\n/g, "\n")`（实测）
//
// bun 的 `with { type: "text" }` 在 Windows 上会把文件的 `\n` 转成 `\r\n`，
// 于是提示词里凭空多出 396 个 `\r`（日志里看不见，但它进了模型上下文），
// 且与 md 源码不再逐字一致——以后拿源码校对会一直假报差异。
// 归一成 `\n`：**提示词的字符内容应当等于文件内容**。
export const systemPrompt = systemPromptBase.replace(/\r\n/g, "\n")

// 自有工具：`verify`（成片质检）+ `style_guide`（风格模板读取）。
// 其余能力仍走依赖（霜天 / imagegen / tts），不内联展开——同一能力只允许一个来源。
//
// `verify` 必须自有：它读的是本应用渲染器在**渲染循环里量好的**数据（逐镜边界、每帧时刻），
// 旁挂子代理只能事后解码 mp4 再抽帧，既过一遍有损压缩又拿不到逐镜区间。
//
// `style_guide` 必须自有：模板讲的是「用裁云怎么落地」——具体图层字段与 `layout`
// 写法，只有裁云自己知道。放成纯文档则会与框架文档各自漂移。
export const tools: Record<string, Tool> = { verify: verifyTool, style_guide: styleGuideTool }
export const preload = false
// 三个依赖名与 caiyun 均不构成前缀，工具各以自身命名空间注册
export const def: SubAgentDef = { name, description, systemPrompt, tools, preload, dependencies: ["shuangtian", "imagegen", "tts"] }
