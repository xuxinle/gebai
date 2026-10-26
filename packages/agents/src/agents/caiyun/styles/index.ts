/**
 * 裁云风格模板 SKILL 资源（七种叙事模板的方法论）。
 *
 * 与系统提示词同机制：`.md` 文本模块在 dev 与 dist/二进制形态下均由打包器内联，
 * **显式带 `with { type: "text" }` 导入属性**——Bun 默认对 .md 走 html loader
 * （会包 `<p>` 标签），依赖 bunfig 的 loader 配置又只在部分包生效；技能文档必须
 * 拿到原始 Markdown，故逐文件声明 text。
 *
 * 新增模板 = 在 `templates/` 放文件并登记到下方 `TEMPLATE_DOCS`
 * （`styles.test.ts` 断言目录与登记表一致，防漏登记与死登记）。
 */
import overviewDoc from "./caiyun_styles.md" with { type: "text" }
import tplCapabilityList from "./templates/01-capability-list.md" with { type: "text" }
import tplSinglePoint from "./templates/02-single-point.md" with { type: "text" }
import tplCausalChain from "./templates/03-causal-chain.md" with { type: "text" }
import tplScaleDive from "./templates/04-scale-dive.md" with { type: "text" }
import tplContrastSplit from "./templates/05-contrast-split.md" with { type: "text" }
import tplSpine from "./templates/06-spine.md" with { type: "text" }
import tplPointer from "./templates/07-pointer.md" with { type: "text" }

export interface StyleDoc {
  /** 读取名（`overview` 或模板短名如 `03-causal-chain`）。 */
  name: string
  /** 一句话说明适用场景（`style_guide` 列表展示）。 */
  summary: string
  content: string
}

/** 总纲：选型表、三条硬门槛、承接判据。**选型前只读这一份**。 */
export const STYLE_DOCS: StyleDoc[] = [
  {
    name: "overview",
    summary:
      "总纲：七模板选型表、三条选型硬门槛、承接判据（怎么算「上一镜活下来了」）、跨模板反模式、风格令牌、数字红线",
    content: overviewDoc,
  },
]

/** 单个模板的九字段：定位 / 节拍结构 / 视觉语言 / 核心手法 / 承接方式 / 声学 / 何时用 / 何时别用 / 用裁云怎么落地。 */
export const TEMPLATE_DOCS: StyleDoc[] = [
  {
    name: "01-capability-list",
    summary: "能力清单：并列报出多项能力，每项一个物理反馈。适合有真实素材、需展示人在场、15s 内报 6~8 项",
    content: tplCapabilityList,
  },
  {
    name: "02-single-point",
    summary: "单点身份：一个元素连续变形演完全片。零素材、零镜头，讲「一个东西如何成为一切」",
    content: tplSinglePoint,
  },
  {
    name: "03-causal-chain",
    summary: "因果链：每次碰撞启动下一次。讲连锁/蔓延/一个错误引发下游一切（失败链路、故障传播、依赖关系）",
    content: tplCausalChain,
  },
  {
    name: "04-scale-dive",
    summary: "尺度一镜：一次连续运动穿越所有尺度、中间不剪断。讲同一件事在不同粒度上重复（分辨率、递归、复利）",
    content: tplScaleDive,
  },
  {
    name: "05-contrast-split",
    summary: "对照分裂：两态并置、让观众自己看出差异。讲有它/没它、旧/新这类可视对照（差异必须做进图形，不能贴标注）",
    content: tplContrastSplit,
  },
  {
    name: "06-spine",
    summary: "脊椎串联：一条主线往复产出多个产物。一个输入能产出多个不同结果时用，主线每次必须可见地回来",
    content: tplSpine,
  },
  {
    name: "07-pointer",
    summary: "在场操作：角色身体驱动每个界面反应。产品有拟人化形象、想展示「谁在做这件事」时用",
    content: tplPointer,
  },
]

const ALL: StyleDoc[] = [...STYLE_DOCS, ...TEMPLATE_DOCS]

/** 全部可读文档（列表用）。 */
export function styleDocList(): StyleDoc[] {
  return ALL
}

/** 按名读取；未知名字返回 undefined（调用方据此给出可读清单）。 */
export function readStyleDoc(name: string): StyleDoc | undefined {
  const key = name.trim().toLowerCase().replace(/\.md$/, "")
  return ALL.find((d) => d.name === key)
}
