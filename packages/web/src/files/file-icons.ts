/**
 * 文件类型图标：按文件名/扩展名给出对应图形（图标来自 `material-icon-theme`，VSCode 同名主题）。
 *
 * 表与 SVG 在 `file-icons.generated.ts`（由 `scripts/build-file-icons.ts` 生成），本文件只负责
 * **判定顺序**（与 VSCode 一致：完整文件名 → 扩展名 → 兜底）与 **元素缓存**。
 *
 * 图标自带配色（上游 SVG 里写死 hex），因此不再经 CSS 类上色：同一份图在任意主题下都可辨认，
 * 也免了「新增类型要同时补一条颜色类」的隐性维护。
 */
import { extOfPath } from "@gebai/sdk"
import { FILE_ICON_BY_EXT, FILE_ICON_BY_NAME, FILE_ICON_SVG } from "./file-icons.generated"

/** 兜底图标（未知类型 / 无扩展名）。 */
const FALLBACK = "file"

/** 文件名 → 图标 id：完整文件名（小写，`Dockerfile`/`package.json` 这类）优先，其次扩展名。 */
export function fileIconId(name: string): string {
  const lower = name.toLowerCase()
  const byName = FILE_ICON_BY_NAME[lower]
  if (byName) return byName
  /* 点开头的文件（`.env`、`.gitignore`）扩展名与直觉相反：`extOfPath(".env")` 会给出空串或 `env`，
   * 与 VSCode 一致按「去掉前导点后的整名」再试一次（上游 fileNames 里就登记了 `.env` 这类字面量名）。 */
  const ext = extOfPath(lower)
  if (ext) {
    const byExt = FILE_ICON_BY_EXT[ext]
    if (byExt) return byExt
  }
  return FALLBACK
}

/** 图标元素缓存：树行每次重画都要建节点，逐次 `innerHTML` 解析 SVG 是纯浪费（克隆已有节点即可）。 */
const cache = new Map<string, SVGSVGElement>()

/** 生成文件图标元素（默认 16px）。返回的总是新节点。 */
export function fileIcon(name: string, size = 16): SVGSVGElement {
  const id = fileIconId(name)
  const key = `${id}|${size}`
  const cached = cache.get(key)
  if (cached) return cached.cloneNode(true) as SVGSVGElement
  const glyph = FILE_ICON_SVG[id] ?? FILE_ICON_SVG[FALLBACK]!
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg")
  svg.setAttribute("viewBox", glyph.viewBox)
  svg.setAttribute("width", String(size))
  svg.setAttribute("height", String(size))
  svg.setAttribute("class", "fw-file-glyph")
  svg.setAttribute("aria-hidden", "true")
  svg.innerHTML = glyph.body
  cache.set(key, svg)
  return svg.cloneNode(true) as SVGSVGElement
}
