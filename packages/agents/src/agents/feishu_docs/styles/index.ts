/**
 * 飞书文档排版 SKILL 资源（体裁契约与语法参考）。
 *
 * 与系统提示词同机制：`.md` 文本模块在 dev 与 dist/二进制形态下均由打包器内联，无需构建脚本或运行时读盘；
 * **显式带 `with { type: "text" }` 导入属性**——Bun 默认对 .md 走 html loader（会包 `<p>` 标签），
 * 依赖 bunfig 的 loader 配置又只在部分包生效；技能文档必须拿到原始 Markdown，故逐文件声明 text。
 * 新增体裁 = 在 `genres/` 放文件并登记到下方 `GENRE_DOCS`（`styles.test.ts` 断言目录与登记表一致，防漏登记）。
 */
import styleDoc from "./style.md" with { type: "text" }
import xmlDoc from "./xml.md" with { type: "text" }
import genreBusinessAnalysis from "./genres/business-analysis.md" with { type: "text" }
import genreDataReport from "./genres/data-report.md" with { type: "text" }
import genreExecutionPlan from "./genres/execution-plan.md" with { type: "text" }
import genreFormalDoc from "./genres/formal-doc.md" with { type: "text" }
import genreMeetingMinutes from "./genres/meeting-minutes.md" with { type: "text" }
import genreMemoBrief from "./genres/memo-brief.md" with { type: "text" }
import genreOfficialRedhead from "./genres/official-redhead.md" with { type: "text" }
import genrePrd from "./genres/prd.md" with { type: "text" }
import genreProposal from "./genres/proposal.md" with { type: "text" }
import genreResearchReport from "./genres/research-report.md" with { type: "text" }
import genreRetrospective from "./genres/retrospective.md" with { type: "text" }
import genreSopTutorial from "./genres/sop-tutorial.md" with { type: "text" }
import genreTechnicalDoc from "./genres/technical-doc.md" with { type: "text" }
import genreWeeklyReport from "./genres/weekly-report.md" with { type: "text" }
import genreWhitePaper from "./genres/white-paper.md" with { type: "text" }

export interface StyleDoc {
  /** 读取名（`style`、`xml` 或体裁短名如 `weekly-report`）。 */
  name: string
  /** 一句话说明适用场景（style_guide 列表展示）。 */
  summary: string
  content: string
}

/** 总纲与语法：写文档前读 `style`；用 XML 排版前读 `xml`。 */
export const STYLE_DOCS: StyleDoc[] = [
  { name: "style", summary: "排版总纲：判定原则、视觉策略档位、组件选择表、颜色规范、硬性排版规格、两阶段工作流与交付自检", content: styleDoc },
  { name: "xml", summary: "XML 排版语法：标签清单、颜色取值、暂不支持项、与 Markdown 的分工", content: xmlDoc },
]

/** 体裁契约：动笔前按文档类型读取对应骨架与硬约束（选择规则见 style.md「体裁选择表」）。 */
export const GENRE_DOCS: StyleDoc[] = [
  { name: "memo-brief", summary: "备忘 / 简报：快速知悉、决策封面或会前准备（信息/决策/会前三模式）", content: genreMemoBrief },
  { name: "weekly-report", summary: "周报 / 进展报告：状态判据、delta 产出、风险与下一里程碑", content: genreWeeklyReport },
  { name: "proposal", summary: "方案 / 提案：备选方案、同口径取舍与决策点（要批准）", content: genreProposal },
  { name: "execution-plan", summary: "执行计划：交付物、依赖与关键路径、带退出条件的里程碑与验收", content: genreExecutionPlan },
  { name: "prd", summary: "PRD / 产品需求：用户问题到可验收行为需求", content: genrePrd },
  { name: "technical-doc", summary: "技术文档：设计 RFC / API 参考 / 故障诊断三选一主模式", content: genreTechnicalDoc },
  { name: "sop-tutorial", summary: "SOP / 操作教程：前置条件、分步操作、验证与回滚", content: genreSopTutorial },
  { name: "retrospective", summary: "复盘：为何如此、下一轮怎么改（可执行、可验证）", content: genreRetrospective },
  { name: "meeting-minutes", summary: "会议纪要：决定、行动项（人/时点/验收）与未决问题", content: genreMeetingMinutes },
  { name: "research-report", summary: "研究报告 / 调研：结论先行、证据强度与不确定性", content: genreResearchReport },
  { name: "data-report", summary: "数据报告：口径、基线、拆解与归因", content: genreDataReport },
  { name: "business-analysis", summary: "商业分析：现状基准、同口径选项比较与翻转条件（不要求批准）", content: genreBusinessAnalysis },
  { name: "white-paper", summary: "白皮书：框架、证据与反例、边界与失败条件（面向专业读者）", content: genreWhitePaper },
  { name: "formal-doc", summary: "正式文档：已授权的内部规则 / 通知 / 整改记录（formal 视觉档）", content: genreFormalDoc },
  { name: "official-redhead", summary: "法定公文（红头）：文种、主送抄送、中文手写编号体系", content: genreOfficialRedhead },
]

const ALL: StyleDoc[] = [...STYLE_DOCS, ...GENRE_DOCS]

/** 全部可读文档（列表用）。 */
export function styleDocList(): StyleDoc[] {
  return ALL
}

/** 按名读取；未知名字返回 undefined（调用方据此给出可读清单）。 */
export function readStyleDoc(name: string): StyleDoc | undefined {
  const key = name.trim().toLowerCase().replace(/\.md$/, "")
  return ALL.find((d) => d.name === key)
}
