/** 工具产物内容块 / MIME 与图表格式推断 / PlantUML 规范化（自 core/tools.ts 抽取的公共展示辅助）。 */
import { isAbsolute, join, relative, sep } from "node:path"
import type { ContentBlock, DiagramFormat } from "@gebai/sdk"
import type { ToolContext } from "../base/types"
import { sessionPath } from "../base/paths"

/** show html 分支：显式预览尺寸上限（px），超限忽略回退默认。 */
export const RENDER_HTML_MAX_WIDTH = 4000
export const RENDER_HTML_MAX_HEIGHT = 2000
export const IMAGE_EXT = /\.(png|jpe?g|gif|webp|svg|bmp)$/i
export const DIAGRAM_EXT = /\.(puml|plantuml|mmd|mermaid|d2|echarts)$/i
/** 图表文件扩展名 → 图表语言（show path 模式与 read/write 产物块推断用）。 */
const DIAGRAM_FORMATS: Record<string, DiagramFormat> = { puml: "plantuml", plantuml: "plantuml", mmd: "mermaid", mermaid: "mermaid", d2: "d2", echarts: "echarts" }
/** 图表语言 → 产物文件扩展名（show 图表分支落盘 tmp/ 用）。 */
export const DIAGRAM_EXT_FOR: Record<DiagramFormat, string> = { plantuml: "puml", mermaid: "mmd", d2: "d2", echarts: "echarts" }
/** 图表语言展示名（错误提示/文档用）。 */
export const DIAGRAM_LABEL: Record<DiagramFormat, string> = { plantuml: "PlantUML", mermaid: "Mermaid", d2: "D2", echarts: "ECharts" }

/** 按文件名扩展名推断图表语言（未命中返回 undefined）。 */
export function diagramFormatFor(path: string): DiagramFormat | undefined {
  const m = path.match(DIAGRAM_EXT)
  return m ? DIAGRAM_FORMATS[m[1].toLowerCase()] : undefined
}
export function mimeFor(path: string): string | undefined {
  const ext = path.split(".").pop()?.toLowerCase()
  if (!ext) return undefined
  const mimes: Record<string, string> = {
    png: "image/png",
    jpg: "image/jpeg",
    jpeg: "image/jpeg",
    gif: "image/gif",
    webp: "image/webp",
    svg: "image/svg+xml",
    bmp: "image/bmp",
    // 文档/数据/音视频（show 文件卡片提示与预览入口用）
    pdf: "application/pdf",
    txt: "text/plain",
    md: "text/markdown",
    csv: "text/csv",
    json: "application/json",
    html: "text/html",
    xml: "application/xml",
    yaml: "application/yaml",
    zip: "application/zip",
    xlsx: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    docx: "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    pptx: "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    mp4: "video/mp4",
    mp3: "audio/mpeg",
    wav: "audio/wav",
  }
  return mimes[ext]
}
/** 解析后绝对路径 → 前端可解析的产物路径（DESIGN「文件链接弹窗查看」）：在真实会话 `tmp/` 内
 *  → `tmp/` 逻辑路径（files 接口直接解析）；其余（code 项目文件/本地绝对路径）→ 绝对路径（files/preview
 *  按用户隔离边界解析）。归属判定用会话 tmp 真实绝对路径（sessionPath 拼接——项目绑定工具的 workdir
 *  是项目根，不能作判定依据）；会话 id 异常时退回 workdir 兜底（路径解析是展示辅助，绝不因基准缺失
 *  让工具执行失败）。 */
export function previewLogicalPath(absPath: string, ctx: ToolContext): string {
  let sessionTmp = ctx.workdir
  try {
    sessionTmp = join(sessionPath(ctx.home, ctx.user, ctx.sessionId), "tmp")
  } catch {
    /* 会话 id 异常：保持 workdir 兜底 */
  }
  const rel = relative(sessionTmp, absPath)
  if (rel && !rel.startsWith("..") && !isAbsolute(rel)) return `tmp/${rel.split(sep).join("/")}`
  return absPath
}
/** Build artifact content blocks from a logical path + optional content (for diagrams). */
export function artifactBlocks(path: string, content = ""): ContentBlock[] {
  const name = baseName(path) || path
  if (IMAGE_EXT.test(path)) return [{ type: "image", path, name, mime: mimeFor(path) }]
  if (DIAGRAM_EXT.test(path)) return [{ type: "diagram", format: diagramFormatFor(path) ?? "plantuml", code: content, name }]
  return [{ type: "file", path, name, mime: mimeFor(path) }]
}
/** 各类图块的 start/end 指令（@startmindmap 等非 @startuml 块同样支持）。 */
const PLANTUML_START_END: Record<string, string> = {
  uml: "@enduml",
  mindmap: "@endmindmap",
  wbs: "@endwbs",
  gantt: "@endgantt",
  salt: "@endsalt",
  json: "@endjson",
  yaml: "@endyaml",
}

/** PlantUML 源码规范化：已含 @start 指令（@startuml/@startmindmap/@startwbs 等）则保留并补全缺失的 @end；否则自动补全 @startuml/@enduml。 */
export function normalizePlantUml(code: string): string {
  const m = code.match(/@start(\w*)/)
  if (m) {
    const end = PLANTUML_START_END[m[1]]
    if (end && !code.includes(end)) return `${code.trimEnd()}\n${end}`
    return code
  }
  return `@startuml\n${code.trimEnd()}\n@enduml`
}
/** 布局兜底默认参数（@startuml 类图）：源码未显式设置节点间距时注入 ranksep/nodesep，
 *  防密集图节点拥挤、连线杂乱（方向/分组仍由模型显式控制，这里只保证最小可读间距）；
 *  其余图型（mindmap/wbs/gantt/salt/json/yaml 等）布局由结构决定，不注入。 */
export function injectPlantUmlLayout(code: string): string {
  const m = code.match(/@start(\w*)/)
  if (!m || m[1] !== "uml") return code
  if (/skinparam\s+(?:ranksep|nodesep)/i.test(code)) return code
  const layout = "skinparam ranksep 80\nskinparam nodesep 40"
  const endIdx = code.search(/@enduml\b/)
  if (endIdx < 0) return `${code.trimEnd()}\n${layout}\n@enduml`
  return `${code.slice(0, endIdx)}${layout}\n${code.slice(endIdx)}`
}
/** 路径取文件名（兼容 `/` 与 `\` 分隔符）。 */
export function baseName(p: string): string {
  const s = p.replace(/\\/g, "/")
  return s.slice(s.lastIndexOf("/") + 1)
}
