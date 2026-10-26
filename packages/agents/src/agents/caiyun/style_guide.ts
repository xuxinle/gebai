/**
 * 裁云的**风格模板工具**：把七种叙事模板从提示词里搬出来，按需读取。
 *
 * ## 为什么要做成工具，而不是全塞进提示词
 *
 * 七模板 × 九字段约 15 KB Markdown。写进系统提示词会让每次对话都背着全部
 * 七套方法论，而一次创作只需要其中**一套**。做成按名读取后：
 * 提示词里只留「选型表 + 三条硬门槛」（决定挑哪个），选完再读那一份。
 *
 * ## 为什么放在子 Agent 目录里（`styles/`）
 *
 * 它是**裁云的知识**，不是霜天框架的知识：模板讲的是「用裁云怎么落地」
 * （具体图层与 `layout` 写法），跟着 Agent 走才不会与框架文档各自漂移。
 * 霜天仓库只放**可运行的东西**——七个一镜示例脚本。
 *
 * 机制与系统提示词一致：`.md` 由打包器内联（`with { type: "text" }`），
 * 无需运行时读盘，dist/二进制形态同样可用。
 */
import type { Tool } from "@gebai/sdk"
import { schema } from "@gebai/sdk/node"
import { readStyleDoc, styleDocList } from "./styles"

export const styleGuideTool: Tool = {
  name: "style_guide",
  safeMode: true,
  description:
    "读取裁云风格模板库（SKILL）。不传 name 列出全部可用文档；传 name 返回全文。 "
    + "`overview`（总纲：七模板选型表、三条选型硬门槛、承接判据、跨模板反模式、风格令牌、数字红线）"
    + "——**选题前必读**，它决定挑哪一个。 "
    + "七个模板各给九字段（定位 / 节拍结构 / 视觉语言 / 核心手法 / 承接方式 / 声学 / 何时用 / 何时别用 / "
    + "**用裁云怎么落地**——具体图层与 layout 写法）："
    + "01-capability-list（能力清单）/ 02-single-point（单点身份）/ 03-causal-chain（因果链）/ "
    + "04-scale-dive（尺度一镜）/ 05-contrast-split（对照分裂）/ 06-spine（脊椎串联）/ 07-pointer（在场操作）。 "
    + "**流程**：先 overview 选型，再读选中的那一个；不要一次全读（一次创作只需要一套方法论）。 "
    + "每个模板在 `examples/caiyun/styles/<编号>-<名字>/` 有一镜示例脚本可对照。",
  parameters: schema(
    {
      name: {
        type: "string",
        description:
          "文档名：overview 或模板名（如 03-causal-chain）；缺省列出全部可用文档",
      },
    },
    [],
  ),
  async execute(args) {
    const name = args.name !== undefined ? String(args.name).trim() : ""
    if (!name) {
      const list = styleDocList()
        .map((d) => `- ${d.name}：${d.summary}`)
        .join("\n")
      return {
        output:
          `可用风格模板文档（用 style_guide name=<名> 读取全文）：\n${list}\n\n`
          + `**先读 overview 选型**（选型表 + 三条硬门槛 + 承接判据），再读选中的那一个模板——`
          + `不要一次全读：一次创作只需要一套方法论，读七套只会稀释当前这一套。`,
      }
    }
    const doc = readStyleDoc(name)
    if (!doc) {
      return {
        output:
          `❌ 未知文档 "${name}"；可用：${styleDocList().map((d) => d.name).join("、")}`,
      }
    }
    return { output: doc.content }
  },
}
