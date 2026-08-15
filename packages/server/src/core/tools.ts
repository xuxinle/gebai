import { dirname, isAbsolute, join, relative } from "node:path"
import { homedir, tmpdir } from "node:os"
import { createServer, connect, type AddressInfo } from "node:net"
import type { ContentBlock, DiagramFormat, TodoItem, ToolSchema } from "@gebai/sdk"
import type { ChoiceOption, Tool, ToolContext, ToolResult } from "./types"
import { truncatedPath, truncatedLogicalPath } from "./paths"
import { randomUUID } from "node:crypto"
import { isBinaryMode } from "./config"
import { diffLines, inferLang, unifiedDiff, splitLines, DIFF_MAX_LINES } from "./diff"
import { applyPatch, parsePatch, PATCH_MAX_FILE_BYTES, PATCH_MAX_HUNKS } from "./patch"
import { hostBlockReason } from "./ip"
import { deleteMiniTool, saveMiniTool } from "./mini-tools"
import { runFlow, scanFlowApprovals } from "./flow"

export const TRUNCATE_THRESHOLD = 12000
/** 截断消息保留的首/尾字符数（DESIGN「常量参考」）。 */
export const TRUNCATE_HEAD_CHARS = 4000
export const TRUNCATE_TAIL_CHARS = 4000
/** grep：单文件读取上限与最大匹配行数。 */
const GREP_MAX_FILE_BYTES = 1024 * 1024
const GREP_MAX_MATCHES = 200
/** fetch_url：响应大小上限与超时。 */
const FETCH_URL_MAX_BYTES = 200 * 1024
const FETCH_URL_TIMEOUT = 15000
/** render_html：显式预览尺寸上限（px），超限忽略回退默认。 */
export const RENDER_HTML_MAX_WIDTH = 4000
export const RENDER_HTML_MAX_HEIGHT = 2000

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

const IMAGE_EXT = /\.(png|jpe?g|gif|webp|svg|bmp)$/i
const DIAGRAM_EXT = /\.(puml|plantuml|mmd|mermaid|d2)$/i
/** 图表文件扩展名 → 图表语言（draw 工具 path 模式与 read/write 产物块推断用）。 */
const DIAGRAM_FORMATS: Record<string, DiagramFormat> = { puml: "plantuml", plantuml: "plantuml", mmd: "mermaid", mermaid: "mermaid", d2: "d2" }
/** 图表语言 → 产物文件扩展名（draw 工具落盘 tmp/ 用）。 */
export const DIAGRAM_EXT_FOR: Record<DiagramFormat, string> = { plantuml: "puml", mermaid: "mmd", d2: "d2" }
/** 图表语言展示名（错误提示/文档用）。 */
const DIAGRAM_LABEL: Record<DiagramFormat, string> = { plantuml: "PlantUML", mermaid: "Mermaid", d2: "D2" }

/** 按文件名扩展名推断图表语言（未命中返回 undefined）。 */
export function diagramFormatFor(path: string): DiagramFormat | undefined {
  const m = path.match(DIAGRAM_EXT)
  return m ? DIAGRAM_FORMATS[m[1].toLowerCase()] : undefined
}

function mimeFor(path: string): string | undefined {
  const ext = path.split(".").pop()?.toLowerCase()
  if (!ext) return undefined
  const mimes: Record<string, string> = { png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", svg: "image/svg+xml", bmp: "image/bmp" }
  return mimes[ext]
}

/** Build artifact content blocks from a logical path + optional content (for diagrams). */
export function artifactBlocks(path: string, content = ""): ContentBlock[] {
  const name = path.split("/").pop() || path
  if (IMAGE_EXT.test(path)) return [{ type: "image", path, name, mime: mimeFor(path) }]
  if (DIAGRAM_EXT.test(path)) return [{ type: "diagram", format: diagramFormatFor(path) ?? "plantuml", code: content, name }]
  return [{ type: "file", path, name, mime: mimeFor(path) }]
}

export async function truncate(content: string, toolName: string, ctx: ToolContext): Promise<ToolResult> {
  if (content.length <= TRUNCATE_THRESHOLD) return { output: content }
  // 截断文件写入会话 tmp/truncated/（会话根内逻辑路径，模型可经 read 读取、UI 文件面板可见）
  const filePath = truncatedLogicalPath(toolName, content)
  const absPath = truncatedPath(ctx.home, ctx.user, ctx.sessionId, toolName, content)
  try {
    const { mkdir, writeFile } = await import("node:fs/promises")
    await mkdir(dirname(absPath), { recursive: true })
    await writeFile(absPath, content)
  } catch {
    /* ignore write failure; still return truncated head/tail */
  }
  // 按行保留完整行（head 行 + tail 行），避免字符级截断切断半行/半条目（PATH 等列表场景）；
  // 单行巨长（minified 等）时该行按字符兜底截断
  const lines = content.split("\n")
  const headLines: string[] = []
  let len = 0
  for (const l of lines) {
    if (headLines.length > 0 && len + l.length + 1 > TRUNCATE_HEAD_CHARS) break
    headLines.push(l)
    len += l.length + 1
  }
  let head = headLines.join("\n")
  if (head.length > TRUNCATE_HEAD_CHARS) head = head.slice(0, TRUNCATE_HEAD_CHARS)
  const tailLines: string[] = []
  len = 0
  for (let i = lines.length - 1; i >= 0; i--) {
    if (tailLines.length > 0 && len + lines[i].length + 1 > TRUNCATE_TAIL_CHARS) break
    tailLines.unshift(lines[i])
    len += lines[i].length + 1
  }
  let tail = tailLines.join("\n")
  if (tail.length > TRUNCATE_TAIL_CHARS) tail = tail.slice(-TRUNCATE_TAIL_CHARS)
  const skipped = Math.max(0, lines.length - headLines.length - tailLines.length)
  const result = `[输出超长，已截断，完整内容见文件: ${filePath}]\n\n${head}\n\n...（省略 ${skipped} 行）...\n\n${tail}`
  return { output: result, truncated: true, filePath }
}

/** 超长用户输入落盘阈值（字符）：超出时全文写入会话 tmp/user_inputs/，消息正文保留头尾 + 文件引用（DESIGN「上下文保护」）。 */
export const USER_INPUT_SPILL_THRESHOLD = 12000
/** 用户输入落盘后消息正文保留的首/尾字符数（与工具截断同值）。 */
export const USER_INPUT_SPILL_HEAD = 4000
export const USER_INPUT_SPILL_TAIL = 4000

/**
 * 超长用户输入落盘（DESIGN「上下文保护」预防策略）：超过阈值时全文写入会话 tmp/user_inputs/{sha256前16位}.txt
 * （原文不丢——会话文件面板可见、模型可经 read 工具读取全文；内容哈希去重，相同输入复用同一文件），
 * 消息正文保留头尾预览 + 文件引用，避免大段粘贴撑爆上下文；未超阈值原样返回。
 * 落盘失败（磁盘异常）时降级为原样返回（不改变优先于瘦身，不阻塞任务）。
 */
export async function spillLongUserInput(content: string, tmpDir: string): Promise<{ content: string; spilled: boolean; filePath?: string }> {
  if (content.length <= USER_INPUT_SPILL_THRESHOLD) return { content, spilled: false }
  const { createHash } = await import("node:crypto")
  const hash = createHash("sha256").update(content).digest("hex").slice(0, 16)
  const filePath = `tmp/user_inputs/${hash}.txt`
  const abs = join(tmpDir, "user_inputs", `${hash}.txt`)
  try {
    const { mkdir, writeFile } = await import("node:fs/promises")
    await mkdir(dirname(abs), { recursive: true })
    await writeFile(abs, content)
  } catch {
    return { content, spilled: false }
  }
  const head = content.slice(0, USER_INPUT_SPILL_HEAD)
  const tail = content.slice(-USER_INPUT_SPILL_TAIL)
  const skipped = Math.max(0, content.length - head.length - tail.length)
  return {
    content: `[用户输入超长，已全文落盘到会话文件 ${filePath}（原文不丢，可用 read 工具读取全文；全文共 ${content.length} 字符）]\n\n${head}\n\n...（省略中间 ${skipped} 字符）...\n\n${tail}`,
    spilled: true,
    filePath,
  }
}

/** 按行切片读取：offset 为 1 起始行号，limit 为正数取 offset 起 N 行、负数取末尾 N 行（忽略 offset）。 */
export function sliceLines(content: string, offset?: number, limit?: number): string {
  if (offset == null && limit == null) return content
  const trailing = content.endsWith("\n")
  const lines = trailing ? content.split("\n").slice(0, -1) : content.split("\n")
  if (limit != null && limit < 0) return lines.slice(limit).join("\n")
  const start = offset != null && offset > 1 ? offset - 1 : 0
  if (start === 0 && (limit == null || limit >= lines.length)) return content
  return lines.slice(start, limit != null && limit > 0 ? start + limit : undefined).join("\n")
}

export const readTool: Tool = {
  name: "read",
  description: "读取文件内容。路径为会话 tmp/ 或用户目录内（服务端部署受沙箱限制）。图片/图表等二进制或结构化文件会返回对应内容块供 UI 展示。offset 为起始行号（1 起始，默认从头）；limit 为读取行数（正数读 offset 起 N 行，负数读末尾 N 行），返回内容不自动带行号。",
  card: { titleParams: ["path"] },
  parameters: schema({
    path: { type: "string", description: "文件路径" },
    offset: { type: "integer", description: "起始行号（1 起始，默认 1）" },
    limit: { type: "integer", description: "读取行数（正数取 offset 起 N 行；负数取末尾 N 行）" },
  }, ["path"]),
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    const content = await ctx.readFile(path)
    const offset = args.offset == null ? undefined : Number(args.offset)
    const limit = args.limit == null ? undefined : Number(args.limit)
    const sliced = sliceLines(content, offset, limit)
    const truncated = await truncate(sliced, "read", ctx)
    const blocks = artifactBlocks(String(args.path), content)
    return { ...truncated, blocks }
  },
}

export const writeTool: Tool = {
  name: "write",
  description: "写入文件（整体覆盖）。路径受沙箱限制。",
  card: { titleParams: ["path"], args: "code", codeField: "content" },
  parameters: schema({ path: { type: "string" }, content: { type: "string" } }, ["path", "content"]),
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    await ctx.writeFile(path, String(args.content ?? ""))
    const blocks = artifactBlocks(String(args.path), String(args.content ?? ""))
    return { output: `已写入 ${args.path}（${String(args.content).length} 字符）`, blocks }
  },
}

export const lsTool: Tool = {
  name: "ls",
  description: "列出目录内容（文件/子目录、大小）。路径默认会话工作目录。",
  card: { titleParams: ["path"] },
  parameters: schema({ path: { type: "string", description: "目录路径（默认 .）" } }),
  outputSchema: schema({
    entries: {
      type: "array",
      description: "目录条目（目录在前，按路径排序）",
      items: schema({ path: { type: "string", description: "相对路径" }, isDir: { type: "boolean" }, size: { type: "integer", description: "字节（目录为 0）" } }, ["path", "isDir", "size"]),
    },
  }, ["entries"]),
  async execute(args, ctx) {
    const path = ctx.resolvePath(args.path ? String(args.path) : ".")
    const entries = await ctx.listDir(path)
    if (!entries.length) return { output: `（空目录）`, data: { entries: [] } }
    const sorted = entries.sort((a, b) => Number(b.isDir) - Number(a.isDir) || a.path.localeCompare(b.path))
    const lines = sorted.map((e) => (e.isDir ? `${e.path}/` : `${e.path}  (${e.size} B)`))
    return {
      output: lines.join("\n"),
      data: { entries: sorted.map((e) => ({ path: e.path, isDir: e.isDir, size: e.size })) },
    }
  },
}

export const deleteFileTool: Tool = {
  name: "delete_file",
  description: "删除文件或目录（递归）。删除不可恢复，谨慎使用。",
  card: { titleParams: ["path"] },
  parameters: schema({ path: { type: "string" } }, ["path"]),
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    await ctx.deleteFile(path)
    return { output: `已删除 ${args.path}` }
  },
}

export const moveFileTool: Tool = {
  name: "move_file",
  description: "移动或重命名文件/目录。",
  card: { titleParams: ["from", "to"] },
  parameters: schema({ from: { type: "string" }, to: { type: "string" } }, ["from", "to"]),
  async execute(args, ctx) {
    const from = ctx.resolvePath(String(args.from))
    const to = ctx.resolvePath(String(args.to))
    await ctx.moveFile(from, to)
    return { output: `已移动 ${args.from} → ${args.to}` }
  },
}

export const grepTool: Tool = {
  name: "grep",
  description: "在会话目录中按正则表达式递归搜索文本内容，返回 文件:行号: 匹配行。path 可限定子目录。",
  card: { titleParams: ["pattern"] },
  parameters: schema({ pattern: { type: "string" }, path: { type: "string", description: "搜索起点（默认 .）" }, ignoreCase: { type: "boolean" } }, ["pattern"]),
  outputSchema: schema({
    matches: {
      type: "array",
      description: "匹配列表（按文件与行号顺序，上限 200）",
      items: schema({ file: { type: "string" }, line: { type: "integer", description: "行号（1 起始）" }, text: { type: "string", description: "匹配行（去除首尾空白，截取前 200 字符）" } }, ["file", "line", "text"]),
    },
  }, ["matches"]),
  async execute(args, ctx) {
    let re: RegExp
    try {
      re = new RegExp(String(args.pattern), args.ignoreCase ? "i" : "")
    } catch {
      return { output: `grep: 无效正则: ${args.pattern}` }
    }
    const path = args.path ? String(args.path) : ""
    const prefix = path ? `${path.replace(/\/+$/, "")}/` : ""
    const files = (await ctx.listFiles()).filter((f) => !f.isDir && f.size <= GREP_MAX_FILE_BYTES && (prefix ? f.path.startsWith(prefix) : true))
    if (!files.length) return { output: "（无匹配文件）", data: { matches: [] } }
    const matches: Array<{ file: string; line: number; text: string }> = []
    for (const f of files) {
      let content: string
      try {
        content = await ctx.readFile(ctx.resolvePath(f.path))
      } catch {
        continue // 二进制等不可读文件跳过
      }
      for (const [i, line] of content.split("\n").entries()) {
        if (matches.length >= GREP_MAX_MATCHES) break
        if (re.test(line)) matches.push({ file: f.path, line: i + 1, text: line.trim().slice(0, 200) })
      }
      if (matches.length >= GREP_MAX_MATCHES) break
    }
    if (!matches.length) return { output: "（无匹配）", data: { matches: [] } }
    const result = matches.map((m) => `${m.file}:${m.line}: ${m.text}`).join("\n")
    const truncated = matches.length >= GREP_MAX_MATCHES
      ? await truncate(`${result}\n…（已达匹配上限）`, "grep", ctx)
      : await truncate(result, "grep", ctx)
    return { ...truncated, data: { matches } }
  },
}

/** glob 模式转正则：`*`/`**` → 任意字符（跨目录层级，递归查找），`?` → 单字符。 */
function globToRegExp(pattern: string): RegExp {
  let out = ""
  for (let i = 0; i < pattern.length; i++) {
    const c = pattern[i]
    if (c === "*") {
      out += ".*"
      if (pattern[i + 1] === "*") i++
    } else if (c === "?") out += "."
    else out += c.replace(/[.+^${}()|[\]\\]/g, "\\$&")
  }
  return new RegExp(`^${out}$`)
}

export const searchFilesTool: Tool = {
  name: "search_files",
  description: "按文件名模式（glob，如 *.ts、**/test/*.js）在会话目录中递归查找文件，返回相对路径。path 可限定子目录。",
  card: { titleParams: ["pattern"] },
  parameters: schema({ pattern: { type: "string" }, path: { type: "string", description: "搜索起点（默认 .）" } }, ["pattern"]),
  outputSchema: schema({
    files: { type: "array", items: { type: "string" }, description: "匹配文件相对路径（最多 200 个）" },
    total: { type: "integer", description: "匹配总数（可能大于 files 长度）" },
  }, ["files", "total"]),
  async execute(args, ctx) {
    const re = globToRegExp(String(args.pattern ?? ""))
    const path = args.path ? String(args.path) : ""
    // path 统一经 resolvePath 解析（与 read/write/ls 一致）：支持相对路径与绝对路径，
    // 解析回会话根相对逻辑路径后做前缀匹配（沙箱模式拒绝越界路径）
    let prefix = ""
    if (path) {
      const root = ctx.resolvePath(".")
      const rel = relative(root, ctx.resolvePath(path)).replace(/\\/g, "/")
      // listFiles 仅覆盖会话目录：path 落在会话外（本地模式放开沙箱时可能）则无可列文件
      if (rel === ".." || rel.startsWith("../") || isAbsolute(rel)) {
        return { output: "（无匹配文件）" }
      }
      prefix = !rel || rel === "." ? "" : `${rel.replace(/\/+$/, "")}/`
    }
    const files = (await ctx.listFiles())
      .filter((f) => !f.isDir)
      .map((f) => f.path)
      .filter((p) => (prefix ? p.startsWith(prefix) : true) && re.test(prefix ? p.slice(prefix.length) : p))
    if (!files.length) return { output: "（无匹配文件）", data: { files: [] } }
    const listed = files.slice(0, 200)
    return { output: listed.join("\n"), data: { files: listed, total: files.length } }
  },
}

export const fetchUrlTool: Tool = {
  name: "fetch_url",
  description: "抓取 URL 内容（网页/API/文档）。服务端部署模式限制公网地址（防 SSRF，含重定向逐跳校验）；响应超过阈值截断。",
  card: { titleParams: ["url"], args: "none" },
  parameters: schema({ url: { type: "string" } }, ["url"]),
  outputSchema: schema({
    ok: { type: "boolean", description: "是否抓取成功（输出文本可用）" },
    status: { type: "integer", description: "HTTP 状态码（发起请求并有响应时）" },
    contentType: { type: "string", description: "响应 Content-Type" },
    error: { type: "string", description: "失败原因（ok=false 时）" },
  }, ["ok"]),
  async execute(args, ctx) {
    const url = String(args.url)
    let res: Response
    try {
      // 沙箱模式：重定向守卫逐跳校验目标地址（初始 URL 与每跳 Location 均需公网），
      // 防「初始公网 → 302 内网」跳板绕过；本地模式正常跟随重定向
      res = ctx.sandboxed
        ? await fetchWithRedirectGuard(url, { signal: AbortSignal.timeout(FETCH_URL_TIMEOUT) }, assertPublicHttpUrl)
        : await fetch(url, { signal: AbortSignal.timeout(FETCH_URL_TIMEOUT) })
    } catch (err) {
      return { output: `fetch_url 失败: ${(err as Error).message}`, data: { ok: false, error: (err as Error).message } }
    }
    if (!res.ok) return { output: `fetch_url 失败: HTTP ${res.status} ${res.statusText}`, data: { ok: false, status: res.status } }
    const ct = res.headers.get("content-type") || ""
    if (!/text|json|xml|html|markdown|javascript|css/i.test(ct)) {
      return { output: `非文本内容（${ct || "未知类型"}），已跳过内容抓取。`, data: { ok: false, status: res.status, contentType: ct } }
    }
    const buf = new Uint8Array(await res.arrayBuffer())
    const text = new TextDecoder().decode(buf)
    return { ...(await truncate(text.slice(0, FETCH_URL_MAX_BYTES), "fetch_url", ctx)), data: { ok: true, status: res.status, contentType: ct } }
  },
}

/** 重定向跳数上限（防重定向循环与超长跳板链）。 */
const REDIRECT_MAX_HOPS = 5

/**
 * 带逐跳校验的 fetch（服务端部署模式 SSRF 防护）：`redirect: "manual"` 拿到每跳
 * 3xx 响应后重新校验 Location 目标（相对地址按当前 URL 解析），全部通过才继续；
 * 跳数超限或 Location 非法时报错。`guard` 由调用方按沙箱开关注入（本地模式放行）。
 */
export async function fetchWithRedirectGuard(
  rawUrl: string,
  init: RequestInit,
  guard: (url: string) => void,
): Promise<Response> {
  let url = rawUrl
  for (let hop = 0; ; hop++) {
    guard(url)
    const res = await fetch(url, { ...init, redirect: "manual" })
    const status = res.status
    if (status >= 300 && status < 400) {
      const loc = res.headers.get("location")
      if (!loc) return res
      if (hop >= REDIRECT_MAX_HOPS) {
        res.body?.cancel().catch(() => {})
        throw new Error(`重定向次数超限（>${REDIRECT_MAX_HOPS}）: ${rawUrl}`)
      }
      try {
        url = new URL(loc, url).toString()
      } catch {
        res.body?.cancel().catch(() => {})
        throw new Error(`无效的重定向地址: ${loc}`)
      }
      res.body?.cancel().catch(() => {})
      continue
    }
    return res
  }
}

/** 服务端部署模式的 URL 安全校验：拒绝回环/链路本地/私网（防 SSRF）。
 * 主机名判定统一走 `core/ip.ts`（覆盖 IPv4-mapped IPv6、ULA、尾点 FQDN、规范化的
 * 整数/十六进制 IPv4 等绕过形式）；DNS 重绑定由 fetchWithRedirectGuard 的逐跳校验兜底。 */
export function assertPublicHttpUrl(raw: string): void {
  let url: URL
  try {
    url = new URL(raw)
  } catch {
    throw new Error(`无效的 URL: ${raw}`)
  }
  if (url.protocol !== "http:" && url.protocol !== "https:") throw new Error(`无效的 URL（仅支持 http/https）: ${raw}`)
  const reason = hostBlockReason(url.hostname, { blockPrivate: true })
  if (reason === "私网地址") throw new Error(`URL 不允许（私网地址）: ${raw}`)
  if (reason) throw new Error(`URL 不允许（回环/链路本地地址）: ${raw}`)
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

export const drawTool: Tool = {
  name: "draw",
  // 需至少多轮交互（前端实时渲染或飞书后端渲染 PNG 两种通道），无交互模式禁用
  interaction: "multi_turn",
  description:
    "创建/更新图表。支持三种图表语言，按需求选最合适的（选择指南见 format 参数）：Mermaid（流程图/时序图/状态图/甘特图/用户旅程等通用场景首选，语法最简洁、文档嵌入最佳）；PlantUML（类图/组件图/部署图/用例图/活动图/ER 图等标准 UML 严谨建模首选，功能最全面）；D2（系统架构图/云架构/网络拓扑等对外展示首选，默认布局最美观）。code 与 path 二选一：code 直接给源码；path 指定会话内已存在的图表文件（.mmd/.puml/.plantuml/.d2，已生成过图表文件时直接渲染文件，避免重发源码；文件名派生图表名；format 未传时按扩展名推断）。渲染成功才返回成功，失败返回错误信息供修正；PlantUML 勿手动添加 @startuml/@enduml（自动补全）。产物保存到会话 tmp/ 并返回图表内容块。render 参数选择渲染通道：**首选 frontend**（默认，浏览器本地渲染 SVG 可交互缩放，零服务端开销）/ backend（服务端直接渲染成 PNG 图片落盘 tmp/，仅导出/分享图片等确需 PNG 文件时使用；前端渲染不可用时改用 backend 重试）。PlantUML 布局规范（保证图不杂乱）：① 流程/时序类图表显式声明方向——横向流程用 `left to right direction`，分层架构保持默认纵向，勿靠逐条连线上写 -down->/-right-> 硬控全局布局；② 密集图表（节点多/跨层连线多）用 `skinparam ranksep 80` 与 `skinparam nodesep 40` 拉开间距（未设置时系统自动注入该默认值）；③ 关系紧密的节点用 `together { … }` 保持相邻；④ 控制规模：单图节点 ≤20 个，架构图按层拆包（package），跨层连线过多时拆成多个图表分别展示。",
  card: { args: "block" },
  parameters: schema(
    {
      code: { type: "string", description: "图表源码（与 path 二选一；与 format 参数一致——Mermaid 直接给图定义；PlantUML 无需 @startuml/@enduml 包裹，自动补全，请勿手动包裹；D2 直接给声明式文本）。PlantUML 布局建议：流程类显式声明 `left to right direction` 或保持默认纵向；密集图设置 `skinparam ranksep 80`/`skinparam nodesep 40` 防节点拥挤；节点 ≤20 个，大图按层拆包" },
      path: { type: "string", description: "会话内已存在的图表文件路径（.mmd/.mermaid/.puml/.plantuml/.d2，与 code 二选一：读取文件内容直接渲染，适合已生成图表文件仅需重新渲染/换通道/再次展示的场景；format 未传时按扩展名推断）" },
      name: { type: "string", description: "图表文件名（不含扩展名；未传时默认 diagram，path 模式默认取文件主名）" },
      format: {
        enum: ["mermaid", "plantuml", "d2"],
        description:
          "图表语言（必选，按需求选择）：\n" +
          "【mermaid】通用场景首选：流程图/时序图/状态图/甘特图/用户旅程、Markdown 文档嵌入（README/Wiki/博客）、简单架构，图表复杂度中等且需求明确（触发词：流程图、时序图、状态图、甘特图、文档）。语法最简洁。\n" +
          "【plantuml】UML 与严谨建模首选：类图/组件图/部署图/用例图/活动图/ER 图等标准 UML，表达复杂继承/依赖/关联关系，语义严谨性要求高（触发词：类图、UML、组件图、部署图、用例图、ER图、继承关系、软件设计）。支持全部 14 种 UML 图，功能最全面。\n" +
          "【d2】美观架构图与对外展示首选：系统架构图/云架构/网络拓扑/微服务，对外展示/PPT/汇报/技术分享等视觉美观场景，希望图表默认好看不愿调布局（触发词：架构图、系统架构、云架构、微服务、汇报、美观）。默认布局最现代化。\n" +
          "组合场景：系统设计文档=plantuml 类图/组件图 + mermaid 流程图；架构汇报=d2 全景架构图 + plantuml 详细组件图。",
      },
      render: { enum: ["frontend", "backend"], default: "frontend", description: "渲染通道（默认 frontend，首选前端渲染降低服务端负载）：frontend（浏览器本地渲染 SVG，可交互缩放、零服务端开销）/ backend（服务端渲染成 PNG 图片落盘 tmp/，仅导出/分享图片等确需 PNG 文件时使用，三语言均支持；前端渲染不可用（收到「画图能力受限」）时改用 backend 重试）" },
    },
    ["format"],
  ),
  async execute(args, ctx) {
    const pathArg = args.path ? String(args.path) : ""
    const formatArg = String(args.format ?? "")
    let format: DiagramFormat = formatArg === "mermaid" || formatArg === "d2" ? formatArg : "plantuml"
    let raw: string
    let base: string
    if (pathArg) {
      // 文件渲染：读取已有图表文件（会话根相对路径），避免重新生成源码；format 未传时按扩展名推断
      try {
        raw = await ctx.readFile(ctx.resolvePath(pathArg))
      } catch (err) {
        return { output: `画图失败：无法读取文件 ${pathArg}（${err instanceof Error ? err.message : String(err)}）。请确认文件存在（可用 ls 查看会话文件）。` }
      }
      const inferred = diagramFormatFor(pathArg)
      if (!formatArg && inferred) format = inferred
      const fileBase = baseName(pathArg).replace(DIAGRAM_EXT, "")
      base = (args.name ? String(args.name) : fileBase || "diagram").replace(DIAGRAM_EXT, "")
    } else {
      raw = String(args.code ?? "")
      base = (args.name ? String(args.name) : "diagram").replace(DIAGRAM_EXT, "")
    }
    const label = DIAGRAM_LABEL[format]
    const code = format === "plantuml" ? injectPlantUmlLayout(normalizePlantUml(raw)) : raw
    // 产物写入会话 tmp/（与 read/write/truncate 的 tmp/ 约定一致，UI 文件面板可见）
    const rel = `tmp/${base}.${DIAGRAM_EXT_FOR[format]}`
    // render=backend：服务端直接渲染 PNG 图片（不经前端/飞书通道），落盘 tmp/{name}.png 并返回 image 内容块（三语言均支持）
    if (args.render === "backend") {
      if (!ctx.renderDiagram || !ctx.writeBinaryFile) {
        return { output: "画图失败：后端渲染不可用（当前环境未启用服务端图表渲染）。" }
      }
      let png: Uint8Array
      try {
        png = await ctx.renderDiagram(code, { format })
      } catch (err) {
        return { output: `画图失败（后端渲染错误）：${err instanceof Error ? err.message : String(err)}。请修正 ${label} 源码后重试。` }
      }
      const pngRel = `tmp/${base}.png`
      await ctx.writeBinaryFile(ctx.resolvePath(pngRel), png)
      await ctx.writeFile(ctx.resolvePath(rel), code)
      return {
        output: pathArg
          ? `图表已渲染为图片: ${pngRel}（源文件 ${pathArg}，${raw.length} 字符）`
          : `图表已生成并渲染为图片: ${pngRel}（${raw.length} 字符）`,
        blocks: [{ type: "image", path: pngRel, name: `${base}.png`, mime: "image/png" }],
      }
    }
    // 实时渲染（Web 前端按 format 本地渲染或飞书后端渲染，通道实现不同但结果一致）：成功才返回成功，渲染错误回传模型，5 秒超时判定画图能力受限
    const rendered = await ctx.waitForDraw({ code, name: base, format })
    if (rendered === null) {
      return { output: "画图能力受限：未能在 5 秒内完成渲染（渲染端离线或超时），请稍后重试，或用其他方式表达图表内容。" }
    }
    if (!rendered.ok) {
      return { output: `画图失败（渲染错误）：${rendered.error ?? "未知错误"}。请修正 ${label} 源码后重试。` }
    }
    const path = ctx.resolvePath(rel)
    await ctx.writeFile(path, code)
    return {
      output: pathArg
        ? `图表已渲染成功: ${rel}（源文件 ${pathArg}，${raw.length} 字符）`
        : `图表已生成并渲染成功: ${rel}（${raw.length} 字符）`,
      blocks: [{ type: "diagram", format, code: raw, name: `${base}.${DIAGRAM_EXT_FOR[format]}`, version: 1 }],
    }
  },
}

/** 前端捕获 html 截断长度（WS 传输与落盘上限；完整 DOM 通常远超此，超出截取首部）。 */
export const PAGE_CAPTURE_HTML_LIMIT = 300 * 1024

export const pageCaptureTool: Tool = {
  name: "page_capture",
  // 仅实时前端可用（请求当前页面捕获并由前端回传），多轮交互/无交互模式禁用
  interaction: "realtime",
  description:
    "请求前端（当前浏览器页面）捕获实际渲染结果：读取渲染后的 DOM html 与页面截图，产物落盘会话 tmp/capture/。适合验证 Web UI 修改后的真实效果——页面即当前打开的 歌白界面（dev 模式修改后自动热更新，捕获前可提示用户刷新页面）；html 用 read 读取完整内容，截图用 vision 工具分析视觉效果。fullPage=true 截整页（长页面 UI 整体效果），缺省截视口（首屏）；delay 为捕获前等待毫秒数（UI 操作/渲染完成后截图，如点击后等动画结束，上限 10 秒）。前端离线或 30 秒未响应时返回失败。无需审批。",
  parameters: schema({
    fullPage: { type: "boolean", description: "是否截整页（默认 false 截视口首屏；整页含全部滚动内容，大页面截图较慢）" },
    delay: { type: "number", description: "捕获前等待毫秒数（默认 0；UI 操作/动画/异步渲染完成后截图，上限 10000）" },
  }),
  async execute(args, ctx) {
    const delayMs = Math.max(0, Math.min(10000, Number(args.delay) || 0))
    const cap = await ctx.waitForCapture({ fullPage: args.fullPage === true, delayMs })
    if (!cap) return { output: "页面捕获失败：前端未能在限定时间内完成捕获（前端离线或捕获超时）。请确认浏览器页面已打开后重试。" }
    if (cap.error) return { output: `页面捕获失败: ${cap.error}` }
    const ts = Date.now()
    const htmlRel = `tmp/capture/page-${ts}.html`
    await ctx.writeFile(ctx.resolvePath(htmlRel), cap.html)
    const blocks: ContentBlock[] = [{ type: "file", path: htmlRel, name: `page-${ts}.html`, mime: "text/html" }]
    let imgRel = ""
    if (cap.imageBase64) {
      // data URL 与裸 base64 均接受（png/jpeg）；非法字符集/解码为空按无截图处理
      const m = cap.imageBase64.match(/^data:(image\/(?:png|jpeg));base64,/)
      const isJpeg = m?.[1] === "image/jpeg"
      const b64 = (m ? cap.imageBase64.slice(m[0].length) : cap.imageBase64).replace(/\s/g, "")
      const buf = /^[A-Za-z0-9+/=]+$/.test(b64) ? Buffer.from(b64, "base64") : Buffer.alloc(0)
      if (buf.byteLength > 0) {
        imgRel = `tmp/capture/page-${ts}.${isJpeg ? "jpg" : "png"}`
        const abs = ctx.resolvePath(imgRel)
        const { mkdir, writeFile } = await import("node:fs/promises")
        await mkdir(dirname(abs), { recursive: true })
        await writeFile(abs, buf)
        blocks.push({ type: "image", path: imgRel, name: imgRel.split("/").pop()!, mime: isJpeg ? "image/jpeg" : "image/png" })
      }
    }
    return {
      output: `已捕获当前页面: ${htmlRel}（${cap.html.length} 字符，可用 read 读取完整内容）${imgRel ? `；截图 ${imgRel}（可用 vision 工具分析图片内容）` : "；前端未返回截图"}`,
      blocks,
    }
  },
}

export const renderHtmlTool: Tool = {
  name: "render_html",
  // 仅实时前端可用（聊天界面内渲染展示），多轮交互/无交互模式禁用
  interaction: "realtime",
  description:
    "生成 HTML 页面并直接在聊天界面内渲染展示（沙箱 iframe 域隔离预览：脚本可执行，但运行在隔离源内，无法访问宿主页面 DOM/存储/顶层导航）。适合网页原型、数据报表、卡片/徽章、可视化组件、带交互脚本的小页面。html 与 path 二选一：html 直接给源码；path 指定会话内已存在的 .html 文件（已生成过页面时直接渲染文件，避免重发源码；文件名派生页面名）。样式用内联 CSS，图片可用 data: URI 或外部 URL，脚本内联或外部均可。可按内容设计指定预览尺寸 width/height（px，如窄高的移动端页面传小宽大高、宽表格页面传大宽），不传则默认固定铺满消息流宽度。产物保存到会话 tmp/ 并返回 html 内容块，无需审批。",
  card: { args: "block" },
  parameters: schema(
    {
      html: { type: "string", description: "HTML 源码（与 path 二选一；完整文档或片段均可，自动补全为完整页面）" },
      path: { type: "string", description: "会话内已存在的 .html 文件路径（与 html 二选一：读取文件内容直接渲染，适合已生成页面文件仅需重新展示/调整预览尺寸的场景）" },
      name: { type: "string", description: "页面标题/文件名（不含扩展名；未传时默认 page，path 模式默认取文件主名）" },
      width: { type: "number", description: "预览宽度（px，可选，默认铺满消息流宽度）" },
      height: { type: "number", description: "预览高度（px，可选，默认 480）" },
    },
  ),
  async execute(args, ctx) {
    const pathArg = args.path ? String(args.path) : ""
    let html: string
    let base: string
    if (pathArg) {
      // 文件渲染：读取已有 .html 文件（会话根相对路径），避免重新生成源码
      try {
        html = await ctx.readFile(ctx.resolvePath(pathArg))
      } catch (err) {
        return { output: `HTML 渲染失败：无法读取文件 ${pathArg}（${err instanceof Error ? err.message : String(err)}）。请确认文件存在（可用 ls 查看会话文件）。` }
      }
      const fileBase = baseName(pathArg).replace(/\.html?$/, "")
      base = (args.name ? String(args.name) : fileBase || "page").replace(/\.html?$/, "")
    } else {
      html = String(args.html ?? "")
      base = (args.name ? String(args.name) : "page").replace(/\.html?$/, "")
    }
    // 显式尺寸：正整数且不超上限（其余值忽略，回退默认）
    const width = typeof args.width === "number" && Number.isFinite(args.width) ? Math.round(args.width) : undefined
    const height = typeof args.height === "number" && Number.isFinite(args.height) ? Math.round(args.height) : undefined
    // 产物写入会话 tmp/（与 draw 的 tmp/ 约定一致，UI 文件面板可见）
    const rel = `tmp/${base}.html`
    await ctx.writeFile(ctx.resolvePath(rel), html)
    const block: Record<string, unknown> = { type: "html", html, name: `${base}.html` }
    if (width !== undefined && width > 0 && width <= RENDER_HTML_MAX_WIDTH) block.width = width
    if (height !== undefined && height > 0 && height <= RENDER_HTML_MAX_HEIGHT) block.height = height
    return {
      output: pathArg
        ? `HTML 页面已渲染: ${rel}（源文件 ${pathArg}，${html.length} 字符）`
        : `HTML 页面已生成并渲染: ${rel}（${html.length} 字符）`,
      blocks: [block as { type: "html"; html: string; name: string }],
    }
  },
}

export const saveTool: Tool = {
  name: "save_tool",
  // 仅实时前端可用（供标题栏「小工具」弹窗加载，依赖前端 UI），多轮交互/无交互模式禁用
  interaction: "realtime",
  description:
    "保存 HTML 小工具到服务端小工具库（供标题栏「小工具」弹窗随时加载使用）。工作流：先用 render_html 在聊天中调试预览，满意后调用本工具保存。scope=public 公用（所有用户可见）/ private 用户私有（默认）。同名工具覆盖更新。工具名仅限字母/数字/下划线/中文（不含 . / 等分隔符）。无需审批。",
  card: { titleParams: ["name"] },
  parameters: schema(
    {
      name: { type: "string", description: "工具名（1-40 字符，字母/数字/下划线/中文；列表与加载均按此名）" },
      html: { type: "string", description: "HTML 源码（完整文档或片段，加载时经沙箱 iframe 渲染，脚本可执行但隔离于宿主页面）" },
      scope: { enum: ["private", "public"], description: "可见范围：private=用户私有（默认）/ public=公用（所有用户可用）" },
    },
    ["name", "html"],
  ),
  async execute(args, ctx) {
    const scope = args.scope === "public" ? "public" : "private"
    const info = await saveMiniTool(ctx.home, ctx.user, {
      name: String(args.name ?? ""),
      html: String(args.html ?? ""),
      scope,
    }, { mode: ctx.authMode ?? "local", role: ctx.userRole })
    return {
      output: `小工具已保存: ${info.name}（${scope === "public" ? "公用" : "用户私有"}，${info.html.length} 字符）\n可在标题栏「小工具」弹窗中加载使用${scope === "public" ? "，所有用户可见" : ""}。`,
    }
  },
}

export const deleteTool: Tool = {
  name: "delete_tool",
  // 仅实时前端可用（删除的是前端小工具库条目），多轮交互/无交互模式禁用
  interaction: "realtime",
  description: "删除已保存的 HTML 小工具（按名称 + 范围，不可恢复）。私有工具仅本人可删；公用工具删除影响所有用户，需审批。",
  requiresApproval: true,
  card: { titleParams: ["name"] },
  parameters: schema(
    {
      name: { type: "string", description: "要删除的工具名" },
      scope: { enum: ["private", "public"], description: "删除的范围：private（默认）/ public" },
    },
    ["name"],
  ),
  async execute(args, ctx) {
    const scope = args.scope === "public" ? "public" : "private"
    const removed = await deleteMiniTool(ctx.home, ctx.user, String(args.name ?? ""), scope, { mode: ctx.authMode ?? "local", role: ctx.userRole })
    if (!removed) return { output: `小工具不存在或名称非法: ${args.name}（scope=${scope}）` }
    return { output: `小工具已删除: ${args.name}（${scope === "public" ? "公用" : "用户私有"}）` }
  },
}

export const editTool: Tool = {
  name: "edit",
  description:
    "精确修改文件：基于 oldString → newString 定点替换，可一次多处，适合小范围改动。原文不匹配则整体失败不落盘；改动较多或行号容易偏移时改用 apply_patch 应用 unified diff（一次多 hunk、容错更强）。",
  card: { titleParams: ["path"], args: "edits", codeField: "edits" },
  parameters: schema(
    {
      path: { type: "string" },
      edits: { type: "array", items: { type: "object", properties: { oldString: { type: "string" }, newString: { type: "string" } }, required: ["oldString", "newString"] } },
    },
    ["path", "edits"],
  ),
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    let content = await ctx.readFile(path)
    const edits = (args.edits as Array<{ oldString: string; newString: string }>) || []
    for (const e of edits) {
      if (!content.includes(e.oldString)) {
        throw new Error(`修改失败: oldString 未在文件中精确匹配: ${e.oldString.slice(0, 60)}`)
      }
      content = content.replace(e.oldString, e.newString)
    }
    await ctx.writeFile(path, content)
    return { output: `已对 ${args.path} 应用 ${edits.length} 处修改` }
  },
}

/** 路径取文件名（兼容 `/` 与 `\` 分隔符）。 */
function baseName(p: string): string {
  const s = p.replace(/\\/g, "/")
  return s.slice(s.lastIndexOf("/") + 1)
}

export const diffTool: Tool = {
  name: "diff",
  description: "对比两段文本或两个文件（旧 → 新），返回行级差异：unified diff 文本 + diff 内容块（UI 并排高亮对比）。oldText/newText 与 oldPath/newPath 任选一种。",
  card: { args: "block" },
  parameters: schema(
    {
      oldText: { type: "string", description: "旧文本内容（与 oldPath 二选一）" },
      newText: { type: "string", description: "新文本内容（与 newPath 二选一）" },
      oldPath: { type: "string", description: "旧文件路径（与 oldText 二选一）" },
      newPath: { type: "string", description: "新文件路径（与 newText 二选一）" },
      language: { type: "string", description: "语法高亮语言（typescript/json/python/bash 等，默认按文件名推断）" },
      name: { type: "string", description: "对比标题（如「重构前后对比」，推荐传入有意义的标题；不传则默认取文件名）" },
      oldName: { type: "string", description: "旧侧面板标题（如「重构前」「v1」，不传默认「旧」）" },
      newName: { type: "string", description: "新侧面板标题（如「重构后」「v2」，不传默认「新」）" },
    },
  ),
  async execute(args, ctx) {
    let oldText: string
    let newText: string
    const oldPath = args.oldPath ? String(args.oldPath) : ""
    const newPath = args.newPath ? String(args.newPath) : ""
    if (args.oldText != null || args.newText != null) {
      if (oldPath || newPath) return { output: "diff: oldPath/newPath 不能与 oldText/newText 混用" }
      oldText = String(args.oldText ?? "")
      newText = String(args.newText ?? "")
    } else {
      if (!oldPath || !newPath) return { output: "diff: 需要提供 oldText/newText 或 oldPath/newPath" }
      oldText = await ctx.readFile(ctx.resolvePath(oldPath))
      newText = await ctx.readFile(ctx.resolvePath(newPath))
    }
    const oldCount = splitLines(oldText).length
    const newCount = splitLines(newText).length
    if (oldCount > DIFF_MAX_LINES || newCount > DIFF_MAX_LINES) {
      return { output: `diff: 文本过大（${oldCount} / ${newCount} 行，上限 ${DIFF_MAX_LINES} 行），请分段对比` }
    }
    const name = args.name ? String(args.name) : baseName(newPath) || baseName(oldPath) || "diff"
    const language = args.language
      ? String(args.language)
      : inferLang(oldPath) || inferLang(newPath) || (args.name ? inferLang(String(args.name)) : "")
    const lines = diffLines(oldText, newText)
    const unified = unifiedDiff(oldText, newText, oldPath || "old", newPath || "new")
    const truncated = await truncate(unified, "diff", ctx)
    return {
      ...truncated,
      blocks: [{ type: "diff", oldText, newText, language, name, oldName: args.oldName ? String(args.oldName) : undefined, newName: args.newName ? String(args.newName) : undefined, lines }],
    }
  },
}

/** apply_patch 应用结果摘要（hunk 位置与净变化）。 */
function describeAppliedPatch(applied: Array<{ line: number; delta: number }>): string {
  const add = applied.reduce((s, a) => s + Math.max(0, a.delta), 0)
  const del = applied.reduce((s, a) => s + Math.max(0, -a.delta), 0)
  return `已应用 ${applied.length} 处 hunk（净 +${add}/-${del} 行，首处位于行 ${applied[0]?.line ?? 0}）`
}

export const applyPatchTool: Tool = {
  name: "apply_patch",
  description:
    "应用 unified diff 补丁到文件（一次多 hunk，行号模糊容错）。patch 参数为 unified diff 文本（可基于 diff 工具输出构造，---/+++ 文件头可省略），格式：@@ -旧起行,旧行数 +新起行,新行数 @@ 后接行内容——空格前缀=上下文行、-前缀=删除行、+前缀=新增行（如 @@ -2,1 +2,1 @@\n-旧行\n+新行）；全部 hunk 校验通过才整体落盘（原子），任一 hunk 不匹配整体失败不修改；dryRun=true 仅预演不落盘。单次仅处理一个文件（多文件补丁请分文件调用）。改动较多或行号容易偏移时优先于 edit 使用。",
  card: { titleParams: ["path"], args: "code", codeField: "patch", codeLang: "diff" },
  parameters: schema(
    {
      path: { type: "string", description: "目标文件路径" },
      patch: { type: "string", description: "unified diff 补丁文本" },
      dryRun: { type: "boolean", description: "true 时仅预演（校验并报告将应用的位置），不写入" },
    },
    ["path", "patch"],
  ),
  async execute(args, ctx) {
    const path = String(args.path)
    const files = parsePatch(String(args.patch ?? ""))
    if (files.length === 0) return { output: "apply_patch: 补丁未解析到任何 hunk（请提供含 @@ 头的 unified diff 文本，格式见 diff 工具输出）" }
    if (files.length > 1) return { output: `apply_patch: 补丁含 ${files.length} 个文件，请分文件调用（本次仅处理 ${path}）` }
    const patch = files[0]
    if (patch.hunks.length > PATCH_MAX_HUNKS) return { output: `apply_patch: hunk 数超上限（${patch.hunks.length} > ${PATCH_MAX_HUNKS}），请拆分补丁` }
    let content = ""
    let exists = true
    try {
      content = await ctx.readFile(ctx.resolvePath(path))
    } catch {
      exists = false
    }
    if (exists && content.length > PATCH_MAX_FILE_BYTES) {
      return { output: `apply_patch: 文件过大（${content.length} 字符，上限 ${PATCH_MAX_FILE_BYTES}），请改用 edit 分段修改` }
    }
    const r = applyPatch(content, patch)
    if (!r.ok) {
      return { output: `apply_patch: 第 ${r.hunkIndex + 1} 处 hunk 未匹配：${r.error}（请先 read 当前文件内容核对，或改用 edit 定点替换；dryRun=true 可预演）` }
    }
    if (args.dryRun === true) return { output: `apply_patch 预演通过：${describeAppliedPatch(r.applied)}（dryRun，未写入）` }
    await ctx.writeFile(ctx.resolvePath(path), r.result)
    return { output: `apply_patch 已写入 ${path}：${describeAppliedPatch(r.applied)}` }
  },
}

/** git 只读工具：log 条数默认与上限。 */
const GIT_DEFAULT_LOG = 10
const GIT_MAX_LOG = 50

export const gitTool: Tool = {
  name: "git",
  description:
    "只读 Git 检查（仅 status/diff/log 三操作，不修改仓库、无需审批）：status 工作区状态 / diff 未暂存或暂存区变更 / log 最近提交。写操作（add/commit 等）请用 sh（需审批）。",
  card: { args: "none" },
  parameters: schema(
    {
      action: { type: "string", enum: ["status", "diff", "log"], description: "status 工作区状态 / diff 变更内容 / log 提交历史" },
      dir: { type: "string", description: "Git 仓库目录（默认会话工作目录）" },
      staged: { type: "boolean", description: "diff 是否查看暂存区（--staged），默认否" },
      maxEntries: { type: "integer", description: "log 条数（默认 10，上限 50）" },
    },
    ["action"],
  ),
  outputSchema: schema({
    action: { type: "string", enum: ["status", "diff", "log"] },
    branch: { type: "string", description: "当前分支（仅 status）" },
    ahead: { type: "integer", description: "领先远端提交数（仅 status，无则省略）" },
    behind: { type: "integer", description: "落后远端提交数（仅 status，无则省略）" },
    changes: { type: "array", description: "变更文件（仅 status）", items: schema({ status: { type: "string", description: "git 状态码（如 M/A/??）" }, path: { type: "string" } }, ["status", "path"]) },
    commits: { type: "array", description: "提交历史（仅 log）", items: schema({ hash: { type: "string" }, subject: { type: "string" } }, ["hash", "subject"]) },
  }, ["action"]),
  async execute(args, ctx) {
    const action = String(args.action)
    const dir = ctx.resolvePath(args.dir ? String(args.dir) : ".")
    let cmd = ""
    if (action === "status") cmd = "git status --short --branch"
    else if (action === "diff") cmd = args.staged === true ? "git diff --staged --no-color" : "git diff --no-color"
    else if (action === "log") {
      const n = Math.min(Math.max(Number(args.maxEntries ?? GIT_DEFAULT_LOG) || GIT_DEFAULT_LOG, 1), GIT_MAX_LOG)
      cmd = `git log --oneline -n ${n}`
    } else return { output: `git: 未知操作: ${action}（status/diff/log）` }
    const { stdout, stderr, code } = await ctx.runCommand(cmd, { workdir: dir })
    if (code !== 0) return { output: `git ${action} 失败（exit ${code}，目录 ${args.dir || "."} 可能不是 Git 仓库）:\n${stderr || stdout}` }
    if (!stdout.trim()) {
      const empty = action === "diff" ? "（工作区无变更）" : action === "status" ? "（工作区干净）" : "（无提交记录）"
      return { output: empty, data: { action, ...(action === "status" ? { changes: [] } : action === "log" ? { commits: [] } : {}) } }
    }
    if (action === "status") {
      const lines = stdout.split("\n").filter(Boolean)
      const branchLine = lines[0]?.startsWith("##") ? lines[0].slice(2).trim() : ""
      const m = branchLine.match(/^(\S+?)(?:\.\.\.)?(?:\s+\[ahead (\d+)(?:, behind (\d+))?\])?/)
      const rest = lines.filter((l) => !l.startsWith("##"))
      return {
        ...(await truncate(stdout, "git", ctx)),
        data: {
          action,
          branch: m?.[1] ?? branchLine,
          ...(m?.[2] ? { ahead: Number(m[2]) } : {}),
          ...(m?.[3] ? { behind: Number(m[3]) } : {}),
          changes: rest.map((l) => ({ status: l.slice(0, 2).trim(), path: l.slice(3).trim() })),
        },
      }
    }
    if (action === "log") {
      const commits = stdout.split("\n").filter(Boolean).map((l) => {
        const i = l.indexOf(" ")
        return i < 0 ? { hash: l, subject: "" } : { hash: l.slice(0, i), subject: l.slice(i + 1) }
      })
      return { ...(await truncate(stdout, "git", ctx)), data: { action, commits } }
    }
    return { ...(await truncate(stdout, "git", ctx)), data: { action } }
  },
}

/** 脚本执行超时参数（秒）：默认 300（5 分钟，与引擎脚本超时一致），上限 540（即引擎 9 分钟工具兜底值，脚本级超时不会晚于引擎兜底触发）。 */
const SCRIPT_TIMEOUT_DEFAULT_S = 300
const SCRIPT_TIMEOUT_MAX_S = 540

/** 脚本超时参数解析（秒 → 毫秒）：非正数/非法回退默认值，超上限截断。 */
function scriptTimeoutMs(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SCRIPT_TIMEOUT_DEFAULT_S * 1000
  return Math.min(n, SCRIPT_TIMEOUT_MAX_S) * 1000
}

/** sh/py 结构化 data 中 stdout/stderr 字符上限：data 供编排引用（分支判定/字段映射），超长截断防映射膨胀；完整文本以 output（及截断文件）为准。 */
const SCRIPT_DATA_TEXT_CAP = 100000

/** sh/py 共用结构化输出：{ stdout, stderr, exitCode }（编排可按 exitCode 分支、按 stdout 映射）。 */
const scriptOutputSchema = schema({
  stdout: { type: "string", description: "标准输出（超长截断至 100k 字符）" },
  stderr: { type: "string", description: "标准错误（超长截断至 100k 字符）" },
  exitCode: { type: "integer", description: "退出码（0=成功）" },
}, ["stdout", "stderr", "exitCode"])

function scriptData(stdout: string, stderr: string, exitCode: number): Record<string, unknown> {
  return {
    stdout: stdout.length > SCRIPT_DATA_TEXT_CAP ? stdout.slice(0, SCRIPT_DATA_TEXT_CAP) : stdout,
    stderr: stderr.length > SCRIPT_DATA_TEXT_CAP ? stderr.slice(0, SCRIPT_DATA_TEXT_CAP) : stderr,
    exitCode,
  }
}

export const shTool: Tool = {
  name: "sh",
  description: "执行 Shell 命令。输出以 stdout 为准，需审批。可选 input 参数作为命令 stdin（flow 管道数据传递）；可选 timeout 参数设置执行超时秒数（默认 300，上限 540，超时按进程树终止并返回超时结果）。",
  requiresApproval: true,
  card: { args: "code", codeField: "command", codeLang: "bash" },
  parameters: schema(
    {
      command: { type: "string" },
      input: { type: "string", description: "可选：作为命令 stdin 的输入数据" },
      timeout: { type: "number", description: "可选：执行超时秒数（默认 300，上限 540；超时进程被终止并返回超时结果）" },
    },
    ["command"],
  ),
  outputSchema: scriptOutputSchema,
  async execute(args, ctx) {
    const input = args.input != null ? String(args.input) : undefined
    const { stdout, stderr, code } = await ctx.runCommand(String(args.command), { workdir: ctx.workdir, env: ctx.env, input, timeoutMs: scriptTimeoutMs(args.timeout) })
    const out = code === 0 ? stdout : `${stdout}\n${stderr}\n[exit ${code}]`
    // 成功但无输出：明确提示（区分「命令成功无输出」与「输出捕获失败/静默吞掉」）
    const final = code === 0 && !stdout.trim() ? "（命令执行成功，无输出）" : out
    return { ...(await truncate(final, "sh", ctx)), data: scriptData(stdout, stderr, code) }
  },
}

/** python 可执行文件探测缓存：undefined=未探测，null=已探测但未命中候选。 */
let pythonCmdCache: string | null | undefined

/** 测试用：重置探测缓存。 */
export function _resetPythonCmdCache(): void {
  pythonCmdCache = undefined
}

/** 探测可用的 python 命令（跨平台：Linux/macOS 多为 python3，Windows 多为 python/py），结果缓存。 */
export async function resolvePythonCmd(ctx: ToolContext): Promise<string> {
  if (pythonCmdCache != null) return pythonCmdCache
  for (const cand of ["python3", "python", "py"]) {
    const r = await ctx.runCommand(`${cand} --version`).catch(() => ({ stdout: "", stderr: "", code: 1 }))
    if (r.code === 0) {
      pythonCmdCache = cand
      return cand
    }
  }
  pythonCmdCache = "python"
  return "python"
}

export const pyTool: Tool = {
  name: "py",
  description: "执行 Python 代码，需审批。代码经临时文件执行，stdin（input 参数，flow 管道数据）供程序读取，stdout 为输出；可选 timeout 参数设置执行超时秒数（默认 300，上限 540，超时进程被终止并返回超时结果）。",
  requiresApproval: true,
  card: { args: "code", codeField: "code", codeLang: "python" },
  parameters: schema(
    {
      code: { type: "string", description: "Python 程序源码" },
      input: { type: "string", description: "可选：作为程序 stdin 的输入数据" },
      timeout: { type: "number", description: "可选：执行超时秒数（默认 300，上限 540；超时进程被终止并返回超时结果）" },
    },
    ["code"],
  ),
  outputSchema: scriptOutputSchema,
  async execute(args, ctx) {
    const code = String(args.code ?? "")
    const input = args.input != null ? String(args.input) : undefined
    // 代码写临时文件执行：stdin 留给管道数据（原实现 code 走 stdin，无法同时传输入）
    const { writeFile, rm } = await import("node:fs/promises")
    const scriptPath = `${ctx.workdir}/.gebai_py_${randomUUID().replace(/-/g, "")}.py`
    await writeFile(scriptPath, code)
    try {
      // -X utf8 / PYTHONUTF8=1：强制 UTF-8 输出（Windows 默认 GBK 会造成乱码）
      const py = await resolvePythonCmd(ctx)
      const { stdout, stderr, code: exit } = await ctx.runCommand(`${py} -X utf8 "${scriptPath}"`, { workdir: ctx.workdir, env: { ...ctx.env, PYTHONUTF8: "1" }, input, timeoutMs: scriptTimeoutMs(args.timeout) })
      const out = exit === 0 ? stdout : `${stdout}\n${stderr}\n[exit ${exit}]`
      // 成功但无输出：明确提示（区分「程序成功无输出」与「stdout 捕获失败」）
      const final = exit === 0 && !stdout.trim() ? "（程序执行成功，无输出）" : out
      return { ...(await truncate(final, "py", ctx)), data: scriptData(stdout, stderr, exit) }
    } finally {
      await rm(scriptPath, { force: true }).catch(() => {})
    }
  },
}

/** 用户反馈查询工具（反馈闭环）：self_optimize 等子Agent 经本工具读取用户反馈，
 *  反馈数据才真正进入 Agent 上下文（此前仅有 REST/WS 通道，Agent 无法消费）。 */
export const readFeedbackTool: Tool = {
  name: "read_feedback",
  description:
    "读取用户提交的反馈（本用户，按时间倒序，最近 N 条）。用于自我优化等场景了解用户对既往输出的评价（点赞/点踩/文字反馈/建议）与改进点。limit 默认 10，上限 50；可按 sessionId 过滤某会话的反馈。",
  parameters: schema(
    {
      limit: { type: "integer", description: "返回条数（默认 10，上限 50）" },
      sessionId: { type: "string", description: "可选：仅返回该会话的反馈" },
    },
  ),
  outputSchema: schema({
    items: {
      type: "array",
      description: "反馈列表（按时间倒序）",
      items: schema({
        type: { type: "string", description: "thumbs_up/thumbs_down/suggestion/text" },
        createdAt: { type: "integer", description: "毫秒时间戳" },
        sessionId: { type: "string" },
        messageId: { type: "string" },
        label: { type: "string" },
        subAgent: { type: "string" },
        text: { type: "string" },
      }, ["type", "createdAt"]),
    },
  }, ["items"]),
  async execute(args, ctx) {
    const { readFeedback } = await import("../feedback")
    const list = await readFeedback(ctx.home, ctx.user)
    const filtered = args.sessionId ? list.filter((f) => f.sessionId === String(args.sessionId)) : list
    const n = Math.min(Math.max(Number(args.limit ?? 10) || 10, 1), 50)
    const items = filtered.slice(0, n)
    if (!items.length) return { output: args.sessionId ? `该会话暂无反馈记录。` : "暂无反馈记录。", data: { items: [] } }
    const label = (t: string) => (t === "thumbs_up" ? "👍" : t === "thumbs_down" ? "👎" : t === "suggestion" ? "建议" : "文字")
    return {
      output:
        `用户反馈（最近 ${items.length} 条${args.sessionId ? `，会话 ${String(args.sessionId)}` : ""}）：\n` +
        items
          .map((f) => {
            const parts = [
              `- [${new Date(f.createdAt).toISOString().slice(0, 19).replace("T", " ")}] ${label(f.type)}`,
              f.sessionId ? `会话 ${f.sessionId}` : "",
              f.messageId ? `消息 ${f.messageId}` : "",
              f.label ? `标签 ${f.label}` : "",
              f.subAgent ? `子Agent ${f.subAgent}` : "",
            ].filter(Boolean)
            const body = f.text ? `\n  ${f.text.slice(0, 500)}` : ""
            return `${parts.join("，")}${body}`
          })
          .join("\n"),
      data: { items },
    }
  },
}

/** 时区偏移文本（如 +08:00 / -05:30），供本地时间标注。 */
function tzOffsetText(d: Date): string {
  const off = -d.getTimezoneOffset()
  const sign = off >= 0 ? "+" : "-"
  const abs = Math.abs(off)
  const pad = (n: number) => String(n).padStart(2, "0")
  return `${sign}${pad(Math.floor(abs / 60))}:${pad(abs % 60)}`
}

const WEEKDAY_CN = ["日", "一", "二", "三", "四", "五", "六"]

export const currentTimeTool: Tool = {
  name: "current_time",
  description: "获取当前时间（一次性给出多种格式，直接取用无需转换）：ISO 8601 / Unix 秒与毫秒 / 本地日期时间（含星期与时区偏移）。",
  card: { args: "none" },
  parameters: schema({}),
  outputSchema: schema({
    iso: { type: "string", description: "ISO 8601（UTC）" },
    unix: { type: "integer", description: "Unix 秒" },
    unixMs: { type: "integer", description: "Unix 毫秒" },
    local: { type: "string", description: "本地日期时间（含星期与时区偏移）" },
  }, ["iso", "unix", "unixMs", "local"]),
  async execute() {
    const now = new Date()
    const pad = (n: number) => String(n).padStart(2, "0")
    const localDateTime = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} ${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())} 星期${WEEKDAY_CN[now.getDay()]}（${tzOffsetText(now)}）`
    return {
      output:
        `当前时间（多种格式，按需取用）:\n` +
        `- ISO 8601: ${now.toISOString()}\n` +
        `- Unix 秒: ${Math.floor(now.getTime() / 1000)}\n` +
        `- Unix 毫秒: ${now.getTime()}\n` +
        `- 本地日期时间: ${localDateTime}`,
      data: { iso: now.toISOString(), unix: Math.floor(now.getTime() / 1000), unixMs: now.getTime(), local: localDateTime },
    }
  },
}

export const systemInfoTool: Tool = {
  name: "system_info",
  description: "获取系统信息（平台、架构、Node 版本、当前工作目录）。",
  card: { args: "none" },
  parameters: schema({}),
  outputSchema: schema({
    platform: { type: "string" }, arch: { type: "string" }, runtime: { type: "string" }, cwd: { type: "string" }, pid: { type: "integer" },
  }, ["platform", "arch", "runtime", "cwd", "pid"]),
  async execute() {
    const info = {
      platform: process.platform,
      arch: process.arch,
      runtime: `bun ${Bun.version}`,
      cwd: process.cwd(),
      pid: process.pid,
    }
    return { output: JSON.stringify(info, null, 2), data: info }
  },
}

/** 环境探测：一次性输出平台/PATH（去重）与关键工具链版本；Windows 下附 VS Build Tools 与 WebView2 状态。只读，无需审批。 */
export const envDetectTool: Tool = {
  name: "env_detect",
  description:
    "探测当前运行环境：平台/架构、PATH（去重）、关键工具链版本（node/bun/python/git/cargo/rustc/go/docker 等，缺失标记不可用）、Windows 下 VS Build Tools（MSVC，cargo/rustc 依赖）与 WebView2 运行时状态。用于一次判断工具链可用性、指导安装缺失组件，避免逐命令探测。无需审批。",
  card: { args: "none" },
  parameters: schema({}),
  async execute(_args, ctx) {
    const probe = async (label: string, cmd: string): Promise<string> => {
      try {
        const r = await ctx.runCommand(cmd, { timeoutMs: 8000 })
        if (r.code !== 0) return `${label}: 不可用（exit ${r.code}）`
        const v = r.stdout.trim().split(/\r?\n/)[0].trim()
        return `${label}: ${v || "（成功但无版本输出）"}`
      } catch {
        return `${label}: 探测失败`
      }
    }
    const lines = [`环境探测（${process.platform} ${process.arch}）`]
    lines.push(
      ...(await Promise.all([
        probe("node", "node --version"),
        probe("bun", "bun --version"),
        probe("python", "python --version"),
        probe("git", "git --version"),
        probe("cargo", "cargo --version"),
        probe("rustc", "rustc --version"),
        probe("go", "go version"),
        probe("docker", "docker --version"),
      ])),
    )
    if (process.platform === "win32") {
      const vswhere = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe"
      try {
        const { existsSync } = await import("node:fs")
        if (existsSync(vswhere)) {
          const r = await ctx.runCommand(
            `"${vswhere}" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`,
            { timeoutMs: 8000 },
          )
          lines.push(`VS Build Tools: ${r.stdout.trim() || "未找到 MSVC 工具链（cargo/rustc 编译需要，建议安装 Visual Studio Build Tools 并勾选 C++ 工作负载）"}`)
        } else {
          lines.push("VS Build Tools: vswhere 不存在（未安装 Visual Studio / Build Tools，cargo/rustc 编译需要 MSVC）")
        }
      } catch {
        lines.push("VS Build Tools: 探测失败")
      }
      try {
        const r = await ctx.runCommand(
          'reg query "HKLM\\SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" /v pv',
          { timeoutMs: 8000 },
        )
        lines.push(`WebView2 运行时: ${(r.stdout.trim().split(/\s+/).pop() ?? "").trim() || "已安装（版本未知）"}`)
      } catch {
        lines.push("WebView2 运行时: 未检测到")
      }
    }
    // PATH 去重（Windows 风格路径大小写不敏感——按路径形态判定而非宿主平台，Linux 下模拟/探测 Windows PATH 同样生效；空项剔除）
    const pathVar = (ctx.env.PATH || process.env.PATH || "").split(";")
    const looksWindows = (p: string) => p.includes("\\") || /^[A-Za-z]:/.test(p)
    const seen = new Set<string>()
    const uniq = pathVar.filter((p) => {
      if (!p) return false
      const k = looksWindows(p) ? p.toLowerCase() : p
      if (seen.has(k)) return false
      seen.add(k)
      return true
    })
    lines.push(`PATH（去重 ${uniq.length}/${pathVar.filter(Boolean).length} 项）：`)
    lines.push(...uniq.map((p) => `  - ${p}`))
    return truncate(lines.join("\n"), "env_detect", ctx)
  },
}

export function makeTodoTool(): Tool {
  /** 单行待办：状态/标题/进度/预计耗时/id（id 供 update/delete 精确定位，同名待办区分用）。 */
  const line = (t: TodoItem): string => {
    const progress = t.progress !== undefined ? ` (${t.progress}%)` : ""
    const eta = t.etaMin !== undefined ? `（预计 ${t.etaMin} 分钟）` : ""
    return `[${t.status}] ${t.title}${progress}${eta}（id: ${t.id}）`
  }
  /** 完整清单文本（操作后随结果返回，让模型一次掌握最新状态，无需再查）。 */
  const snapshot = (todos: TodoItem[]): string => {
    if (!todos.length) return "当前全部待办（0 项）：（无）"
    return `当前全部待办（${todos.length} 项）：\n${todos.map(line).join("\n")}`
  }
  /**
   * 定位待办：id 优先（精确）；无 id 时按 title 定位——先精确匹配，唯一时命中；
   * 精确多匹配或无精确时唯一包含匹配兜底；仍不唯一/无匹配返回 undefined。
   */
  const matchTodo = (e: Record<string, unknown>, todos: TodoItem[]): TodoItem | undefined => {
    const id = String(e.id ?? "").trim()
    if (id) return todos.find((x) => x.id === id)
    const title = String(e.title ?? "").trim()
    if (!title) return undefined
    const exact = todos.filter((x) => x.title === title)
    if (exact.length === 1) return exact[0]
    const subs = todos.filter((x) => x.title.includes(title))
    if (subs.length === 1) return subs[0]
    return undefined
  }
  const tool: Tool = {
    name: "todo",
    description:
      "待办管理（统一入口）：entries 为操作列表，每项 op=add/update/delete——add 新建（需 title，可带 priority/note/eta），update 按 id 或 title 修改（status/progress；改标题需用 id 定位），delete 按 id 或 title 删除。省略 entries 或传空数组 = 查询（清单含 id）。返回操作摘要与当前全部待办状态。",
    parameters: schema({
      entries: {
        type: "array",
        description: "操作列表（空/省略=查询）。每项：{ op: add|update|delete, 及对应字段 }",
        items: {
          type: "object",
          properties: {
            op: { enum: ["add", "update", "delete"], description: "操作类型" },
            title: { type: "string", description: "add 必填：标题；update 可选：传 id 时作为新标题，无 id 时用作定位（精确/唯一包含匹配，多个同名请用 id）；delete 可选：用作定位（同 update 无 id 时）" },
            id: { type: "string", description: "update/delete 可选：待办 id（todo 返回清单中的 id，优先于 title 定位）" },
            status: { enum: ["pending", "in_progress", "completed", "failed", "cancelled"], description: "update 可选：目标状态" },
            progress: { type: "number", description: "update 可选：目标进度（0-100）" },
            priority: { enum: ["low", "medium", "high"], description: "add 可选：优先级" },
            note: { type: "string", description: "add 可选：备注" },
            eta: { type: "number", description: "add 可选：预计耗时（分钟）" },
          },
          required: ["op"],
        },
      },
    }),
    outputSchema: schema({
      todos: {
        type: "array",
        description: "操作后的全部待办（查询时即当前清单）",
        items: schema({
          id: { type: "string" }, title: { type: "string" }, status: { type: "string", description: "pending/in_progress/completed/failed/cancelled" },
          priority: { type: "string", description: "low/medium/high" }, progress: { type: "number" }, etaMin: { type: "number", description: "预计耗时（分钟）" }, note: { type: "string" },
        }, ["id", "title", "status", "priority"]),
      },
    }, ["todos"]),
    async execute(args, ctx) {
      const todos = await ctx.getTodos()
      const entries = Array.isArray(args.entries) ? (args.entries as Array<Record<string, unknown>>) : []
      // 空列表 = 查询：不落盘不发布事件
      if (!entries.length) return { output: `查询待办：\n${snapshot(todos)}`, data: { todos } }
      const results: string[] = []
      const failures: string[] = []
      for (const raw of entries) {
        const e = raw ?? {}
        const op = String(e.op ?? "")
        if (op === "add") {
          const title = String(e.title ?? "").trim()
          if (!title) throw new Error("add 操作缺少 title")
          todos.push({
            id: randomUUID().replace(/-/g, ""),
            title,
            status: "pending",
            priority: (e.priority as TodoItem["priority"]) || "medium",
            etaMin: e.eta !== undefined ? Number(e.eta) : undefined,
            note: e.note ? String(e.note) : undefined,
          })
          results.push(`新增: ${title}`)
        } else if (op === "update" || op === "delete") {
          const t = matchTodo(e, todos)
          if (!t) {
            const byId = String(e.id ?? "").trim()
            const byTitle = String(e.title ?? "").trim()
            const key = byId ? `id: ${byId}` : byTitle ? `标题「${byTitle}」` : "（未指定 id/title）"
            const tip = byTitle && todos.filter((x) => x.title === byTitle).length > 1 ? "（标题匹配多个待办，请用 id 指定）" : ""
            failures.push(`${op === "update" ? "更新" : "删除"}未匹配 ${key}${tip}`)
            continue
          }
          if (op === "update") {
            // 有 id 时 title 表示新标题；无 id（按 title 定位）时 title 仅用于定位，不改标题
            if (String(e.id ?? "").trim()) {
              if (e.title !== undefined) t.title = String(e.title)
            } else {
              if (e.status === undefined && e.progress === undefined) {
                failures.push(`更新未变更任何字段（无 id 时 title 仅用于定位，改标题请用 id）: ${t.title}`)
                continue
              }
            }
            if (e.status) t.status = e.status as TodoItem["status"]
            if (e.progress !== undefined) t.progress = Number(e.progress)
            results.push(`更新: ${t.title}`)
          } else {
            const idx = todos.indexOf(t)
            todos.splice(idx, 1)
            results.push(`删除: ${t.title}`)
          }
        } else {
          throw new Error(`未知操作: ${op || "(空)"}（应为 add/update/delete）`)
        }
      }
      await ctx.setTodos(todos)
      ctx.publish("event.todo.update", { todos })
      const head =
        `待办操作完成（${results.length} 成功${failures.length ? `，${failures.length} 失败` : ""}）：\n` +
        results.join("\n") +
        (failures.length ? `\n失败：\n${failures.join("\n")}` : "")
      return { output: `${head}\n${snapshot(todos)}`, data: { todos } }
    },
  }
  return tool
}

const askUserTool: Tool = {
  name: "ask_user",
  // 需至少多轮交互（前端选择卡片或飞书按钮作答），无交互模式禁用
  interaction: "multi_turn",
  description:
    "向用户提出一组选项并**阻塞等待用户回应**（方案确认、方向决策等场景）。用户可点选选项（multi=true 时可多选）、输入自定义文本或拒绝回答，结果会作为本工具结果返回，据此继续执行。options 每项可为纯文本字符串，或复杂选项 { title, description }（UI 按标题+说明展示，返回值为 title）。",
  parameters: schema(
    {
      prompt: { type: "string" },
      options: {
        type: "array",
        items: {
          anyOf: [
            { type: "string" },
            { type: "object", properties: { title: { type: "string" }, description: { type: "string" } }, required: ["title"] },
          ],
        },
      },
      multi: { type: "boolean" },
    },
    ["prompt", "options"],
  ),
  async execute(args, ctx) {
    const prompt = String(args.prompt ?? "")
    const multi = args.multi === true
    const options = (Array.isArray(args.options) ? args.options : []).map(normalizeChoiceOption)
    if (!options.length) throw new Error("ask_user 需要至少一个选项")
    // 阻塞等待用户回应：事件推送由引擎 waitForChoice 发布（含 choiceId/multi），
    // 用户经 UI/REST/WS 提交选项/自定义文本/拒绝后本工具才返回，模型据此继续
    const choice = await ctx.waitForChoice(prompt, options, multi)
    if (!choice) return { output: "用户未在时限内做出选择，已取消本次询问。请基于现有信息自行决策或换一种方式征询。" }
    if (choice.kind === "refuse") {
      return { output: "用户拒绝了本次询问。请停止继续询问，基于现有信息自行决策；如信息不足，说明所需信息并请用户另行补充。" }
    }
    if (choice.kind === "multi") return { output: `用户选择：${choice.values.join("、")}` }
    return { output: `用户选择：${choice.value}` }
  },
}

export const askEnvTool: Tool = {
  name: "ask_env",
  description:
    "向用户请求设置一个环境变量并**阻塞等待**：前端弹出填值窗口（展示变量名与用途说明），用户填写提交后值即注入本次任务环境（后续工具读取立即生效），同时保存到浏览器本地（后续任务自动生效）。用于工具缺少必需环境变量（API 密钥/Token/应用凭证等）时向用户索取——如 feishu_docs 缺少 FEISHU_DOCS_APP_ID 时可调用本工具。secret=true 时输入框掩码显示。用户拒绝/超时返回失败，请改用其他方式或说明所需配置。",
  // 需至少多轮交互（前端填值弹窗），无交互模式禁用
  interaction: "realtime",
  card: { titleParams: ["name"], args: "none" },
  parameters: schema(
    {
      name: { type: "string", description: "要请求的环境变量名（如 FEISHU_DOCS_APP_ID，仅限字母/数字/下划线）" },
      description: { type: "string", description: "变量用途说明（展示给用户，帮助其填写正确的值）" },
      secret: { type: "boolean", description: "是否敏感值（密钥/Token 等，输入框掩码显示，默认 false）" },
    },
    ["name"],
  ),
  async execute(args, ctx) {
    const name = String(args.name ?? "").trim()
    if (!name) return { output: "ask_env 需要指定环境变量名（name）。" }
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) return { output: `环境变量名非法: ${name}（仅允许字母/数字/下划线，字母或下划线开头）。` }
    const ok = await ctx.waitForEnv(name, String(args.description ?? ""), args.secret === true)
    if (!ok) return { output: `用户未提供环境变量 ${name}（拒绝或超时）。请说明所需配置，或基于现有信息改用其他方式。` }
    return { output: `环境变量 ${name} 已由用户设置并注入本次任务（后续工具读取立即生效，并已保存到浏览器本地，后续任务自动生效）。` }
  },
}

/** 规范化 ask_user 选项：纯文本 → { title }，复杂选项原样。 */
function normalizeChoiceOption(o: unknown): ChoiceOption {
  if (o && typeof o === "object") {
    const t = String((o as { title?: unknown }).title ?? "")
    if (t) return { title: t, description: (o as { description?: unknown }).description != null ? String((o as { description?: unknown }).description) : undefined }
  }
  return String(o ?? "")
}
export { askUserTool }

const PREVIEW_START_TIMEOUT_MS = 15000
const PREVIEW_POLL_INTERVAL_MS = 300
const PREVIEW_STATE_FILE = "gebai-preview.json"

/** 预览服务状态记录（写入 os.tmpdir()/gebai-preview.json，跨进程可见）。 */
export interface PreviewServerEntry {
  port: number
  pid: number
  url: string
  log: string
  startedAt: number
}

interface PreviewServerDeps {
  host: string
  /** 脚本模式服务端入口（默认 packages/server/src/index.ts）。 */
  entry: string
  /** 二进制模式：spawn 自身可执行文件（环境变量覆盖 GEBAI_PORT）。 */
  binary: boolean
  /** 状态文件与日志目录（默认系统临时目录）。 */
  tmpDir: string
  timeoutMs: number
  intervalMs: number
}

function previewServerDeps(overrides: Partial<PreviewServerDeps> = {}): PreviewServerDeps {
  return {
    host: process.env.GEBAI_HOST || "127.0.0.1",
    entry: join(import.meta.dirname, "..", "index.ts"),
    binary: isBinaryMode(),
    tmpDir: tmpdir(),
    timeoutMs: PREVIEW_START_TIMEOUT_MS,
    intervalMs: PREVIEW_POLL_INTERVAL_MS,
    ...overrides,
  }
}

function pidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0)
    return true
  } catch {
    return false
  }
}

/** TCP 探测端口是否可连接（就绪探测与占用检查统一走 127.0.0.1，不受绑定地址影响）。 */
function isPortOpen(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const sock = connect({ host: "127.0.0.1", port })
    sock.setTimeout(1000)
    sock.once("connect", () => {
      sock.destroy()
      resolve(true)
    })
    sock.once("error", () => {
      sock.destroy()
      resolve(false)
    })
    sock.once("timeout", () => {
      sock.destroy()
      resolve(false)
    })
  })
}

function findFreePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const srv = createServer()
    srv.unref()
    srv.once("error", reject)
    srv.listen(0, "127.0.0.1", () => {
      const { port } = srv.address() as AddressInfo
      srv.close(() => resolve(port))
    })
  })
}

async function waitPortOpen(port: number, timeoutMs: number, intervalMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (await isPortOpen(port)) return true
    await new Promise((r) => setTimeout(r, intervalMs))
  }
  return false
}

/** 读取日志文件尾部（失败/未就绪时的诊断信息）。 */
async function logTail(log: string, lines = 20): Promise<string> {
  try {
    const content = await Bun.file(log).text()
    return content.split("\n").slice(-lines).join("\n")
  } catch {
    return ""
  }
}

async function loadPreviewState(tmpDir: string): Promise<PreviewServerEntry[]> {
  try {
    const raw = await Bun.file(join(tmpDir, PREVIEW_STATE_FILE)).json()
    if (!Array.isArray(raw)) return []
    return (raw as PreviewServerEntry[]).filter((e) => typeof e.pid === "number" && pidAlive(e.pid))
  } catch {
    return []
  }
}

async function savePreviewState(tmpDir: string, entries: PreviewServerEntry[]): Promise<void> {
  const { writeFile } = await import("node:fs/promises")
  try {
    await writeFile(join(tmpDir, PREVIEW_STATE_FILE), JSON.stringify(entries, null, 2))
  } catch {
    /* 状态文件写失败不影响服务本身 */
  }
}

async function killPid(pid: number): Promise<boolean> {
  try {
    process.kill(pid, "SIGTERM")
  } catch {
    return !pidAlive(pid)
  }
  for (let i = 0; i < 25; i++) {
    if (!pidAlive(pid)) return true
    await new Promise((r) => setTimeout(r, 200))
  }
  try {
    process.kill(pid, "SIGKILL")
  } catch {
    /* 已退出 */
  }
  return !pidAlive(pid)
}

function displayHostFor(host: string): string {
  return host === "0.0.0.0" || host === "::" || host === "::0" ? "127.0.0.1" : host
}

async function startPreview(deps: PreviewServerDeps, rawPort?: unknown): Promise<ToolResult> {
  const port = rawPort === undefined ? await findFreePort() : Number(rawPort)
  if (!Number.isInteger(port) || port < 1 || port > 65535) return { output: `无效端口: ${rawPort}` }
  if (await isPortOpen(port)) return { output: `端口 ${port} 已被占用，请换一个端口或省略 port 自动选取。` }
  const log = join(deps.tmpDir, `gebai-preview-${port}.log`)
  const cmd = deps.binary ? [process.execPath] : [process.execPath, deps.entry]
  const proc = Bun.spawn({
    cmd,
    env: { ...process.env, GEBAI_PORT: String(port) },
    cwd: deps.binary ? homedir() : process.cwd(),
    stdout: Bun.file(log),
    stderr: Bun.file(log),
    detached: true,
  })
  const up = await waitPortOpen(port, deps.timeoutMs, deps.intervalMs)
  if (!up) {
    await killPid(proc.pid)
    const tail = await logTail(log)
    return { output: `验证服务启动失败（端口 ${port}）${tail ? `，日志尾部：\n${tail}` : ""}` }
  }
  const host = displayHostFor(deps.host)
  const url = `http://${host}:${port}`
  const state = await loadPreviewState(deps.tmpDir)
  const entry: PreviewServerEntry = { port, pid: proc.pid, url, log, startedAt: Date.now() }
  await savePreviewState(deps.tmpDir, [...state.filter((e) => e.port !== port), entry])
  return {
    output: `验证服务已启动（独立进程，不中断当前会话）：${url}\nPID: ${proc.pid}\n日志: ${log}\n验证完成后用 preview_server action=stop 停止（pid=${proc.pid}）。`,
  }
}

async function stopPreview(deps: PreviewServerDeps, rawPid?: unknown, rawPort?: unknown): Promise<ToolResult> {
  const state = await loadPreviewState(deps.tmpDir)
  const pid = rawPid !== undefined ? Number(rawPid) : undefined
  const port = rawPort !== undefined ? Number(rawPort) : undefined
  const target = pid ? state.find((e) => e.pid === pid) : port ? state.find((e) => e.port === port) : undefined
  if (!target) {
    const hint = pid ? `PID ${pid}` : port ? `端口 ${port}` : ""
    return { output: hint ? `未找到 ${hint} 对应的预览服务（可能已停止）。` : "停止预览服务需要提供 pid 或 port。" }
  }
  const dead = await killPid(target.pid)
  await savePreviewState(deps.tmpDir, state.filter((e) => e.pid !== target.pid))
  return { output: dead ? `预览服务已停止：${target.url}（PID ${target.pid}）` : `预览服务停止失败（PID ${target.pid}），请手动 kill。` }
}

/** 预览/验证服务：在临时新端口启动一份独立进程（不中断当前会话），供用户验证改动；依赖注入便于测试。 */
export function makePreviewServerTool(overrides: Partial<PreviewServerDeps> = {}): Tool {
  const deps = previewServerDeps(overrides)
  return {
    name: "preview_server",
    description: "在临时新端口启动/停止一份 歌白验证服务（独立进程，不中断当前会话与主服务），供用户验证代码改动。action=start（默认）启动并返回访问 URL/PID/日志路径；action=stop 停止（pid 或 port 指定）。验证完必须停止，避免残留进程。",
    parameters: schema({
      action: { type: "string", enum: ["start", "stop"], description: "操作（默认 start）" },
      port: { type: "number", description: "启动/停止目标端口（启动时默认自动选取空闲端口）" },
      pid: { type: "number", description: "停止时指定进程 PID（与 port 二选一）" },
    }),
    async execute(args) {
      if (args.action === "stop") return stopPreview(deps, args.pid, args.port)
      return startPreview(deps, args.port)
    },
  }
}

export function makeFlowTool(): Tool {
  const flow: Tool = {
    name: "flow",
    description:
      "数据流编排：一次调用执行多步工具链（把工具视为函数做动态编程），减少与模型的往返与词元消耗。支持引用映射、条件分支与循环：\n" +
      "- **步骤**：steps 每项为工具步骤 `{ id?, tool, params?, input?, when?, optional? }` 或循环分组 `{ id?, foreach | while, maxLoops?, steps: [...] }`（分组须声明 foreach 或 while）。\n" +
      "- **引用**：`{{s1.data.xxx}}` 引用步骤结构化输出（各工具 data 结构先用 tool_schemas 批量查询），`{{s1.output}}` 引用文本输出。params 值恰为一个 `{{表达式}}` 时保留原始类型（数字/数组/对象原样传递），混排字符串按文本拼接。根名：步骤 id（缺省自动编号 s1/s2…）、`prev`（上一实际执行步骤）、`item`/`index`（foreach 当前项/序号）、`iteration`（while 轮次）、`input`（flow 参数 input）。路径访问 `.字段`/`[下标]`/`.length`。\n" +
      "- **input 显式映射**：`{ 目标参数名: \"{{源}}\" }`，解析后覆盖 params 同名字段并抑制自动注入——字段改名、多对一汇聚（多个步骤输出映射进同一工具的不同参数）都用它表达；**非对象形式**（如 `input: \"{{item}}\"`）等价于 `{ input: \"{{item}}\" }`，直接给工具的 input 参数传值（脚本 stdin）。\n" +
      "- **when 条件分支**：表达式为假时跳过该步（不中断）。支持两种写法（等价）：裸表达式 `gen.data.ok == true` 或 `{{表达式}}` 包裹/混排 `{{gen.data.ok}} == true`。运算：`==`/`!=`/`>`/`>=`/`<`/`<=`、`&&`/`||`/`!`、括号、函数 `len()`/`contains()`/`exists()`；空数组视为假；引用不存在的路径解析为 undefined/空串（不报错，需判定存在性用 `exists()`）。\n" +
      "- **foreach 数据循环**（一对多扇出）：表达式求值为数组（逐项，**JSON 数组文本如 `[1,2,3]` 自动解析**）或正整数（按次），体内经 `{{item}}`/`{{index}}` 引用；**快照语义**——迭代次数固定为求值时的长度，循环体修改源数组不影响遍历；**嵌套时内层 `{{item}}`/`{{index}}` 遮蔽外层同名引用**（外层值需提前映射到中间步骤）；组结果 data = 每轮末步 data 的数组。\n" +
      "- **while 条件循环**（do-while：先执行一轮再判断，条件可引用本组最新结果如 `{{g.data.exitCode}}`，适合重试直到成功）：为真继续下一轮，达上限停止；`maxLoops` 默认 10、上限 50；需前置判断时配 when。\n" +
      "- **timeout 超时**（整体预算，秒）：步骤间累计执行时间超限即中止（循环失控保护），返回已执行部分 + 超时说明；缺省不限制。单步执行中无法中止——慢步骤请用各工具自身 timeout 参数。\n" +
      "- **optional 容错**：该步失败不中断（记录错误继续）；未声明时任一步失败中断整个 flow，报告失败位置与原因。\n" +
      "- **自动注入**（未显式 input 映射时保留旧版语义）：脚本工具（sh/py）上一步输出经 stdin（input 参数）传入；其余工具的上一步 JSON 输出按参数名映射注入，兜底注入 input 参数。\n" +
      "- **审批与安全**：内部任一工具需审批则整个 flow 提交一次审批（通过后依次执行）；安全模式下风险工具在 step 层同规则拦截。\n" +
      "- **规模上限**：单次工具调用总数 ≤ 100、foreach ≤ 50 项、分组嵌套 ≤ 4 层；超限请拆分多次调用。",
    requiresApproval: (args, ctx) => scanFlowApprovals(args.steps, ctx),
    parameters: schema(
      {
        steps: {
          type: "array",
          description: "步骤列表：工具步骤 { id?, tool, params?, input?, when?, optional? } 或循环分组 { id?, foreach|while, maxLoops?, steps }",
          items: { type: "object", properties: { tool: { type: "string" }, params: { type: "object" } }, required: ["tool"] },
        },
        input: { description: "初始输入（任意类型），步骤中经 {{input}} 引用" },
        timeout: { type: "number", description: "flow 整体执行超时（秒）：步骤间累计时间超限即中止并返回已执行部分（防循环失控）；缺省不限制；单步慢请用各工具自身 timeout 参数" },
      },
      ["steps"],
    ),
    async execute(args, ctx) {
      return runFlow({ steps: args.steps, input: args.input, timeout: args.timeout }, ctx)
    },
  }
  return flow
}

/** 批量获取工具的输入参数与结构化输出 schema（DESIGN「工具双输出」）：编排（flow）前理解输出结构，避免逐个试调。 */
export const toolSchemasTool: Tool = {
  name: "tool_schemas",
  description:
    "批量获取工具的输入参数 schema 与结构化输出（data）schema。编写 flow 数据流编排前先用本工具了解相关工具的输出结构（引用 {{步骤id.data.字段}} 的前提）。tools 传工具名列表（可含子Agent 命名空间工具，如 code_read）；省略时返回全部已启用工具的输出结构概要（不含输入参数，紧凑一行一个）。无 outputSchema 的工具仅有文本 output（无结构化 data 可引用）。",
  parameters: schema({
    tools: { type: "array", items: { type: "string" }, description: "工具名列表（省略 = 全部已启用工具的输出概要）" },
  }),
  async execute(args, ctx) {
    const all = ctx.registry.schemas()
    const names = Array.isArray(args.tools) ? args.tools.map(String).filter(Boolean) : []
    if (!names.length) {
      const lines = all.map((s) => {
        const os = ctx.registry.resolve(s.name)?.tool.outputSchema
        return `- ${s.name}: ${os ? JSON.stringify(os) : "（仅文本 output，无结构化 data）"}`
      })
      return { output: `已启用工具（${all.length} 个）的输出结构：\n${lines.join("\n")}`, data: { tools: all.map((s) => ({ name: s.name, outputSchema: ctx.registry.resolve(s.name)?.tool.outputSchema ?? null })) } }
    }
    const entries = names.map((name) => {
      const s = all.find((x) => x.name === name)
      if (!s) return { name, error: "未知或未启用的工具" }
      return { name, description: s.description, parameters: s.parameters, outputSchema: ctx.registry.resolve(name)?.tool.outputSchema ?? null }
    })
    return { output: JSON.stringify(entries, null, 2), data: { tools: entries } }
  },
}

export const agentListTool: Tool = {
  name: "agent_list",
  description: "列出可用子Agent（名称、描述、是否已装载）。工具名以已注册的工具集为准，不在此列出。",
  parameters: schema({}),
  outputSchema: schema({
    agents: {
      type: "array",
      items: schema({ name: { type: "string" }, description: { type: "string" }, loaded: { type: "boolean", description: "是否已装载" } }, ["name", "description", "loaded"]),
    },
  }, ["agents"]),
  async execute(_args, ctx) {
    const defs = ctx.listSubAgentDefs()
    if (!defs.length) return { output: "无可用子Agent。", data: { agents: [] } }
    return {
      output: defs
        .map((d) => `- ${d.name}${d.loaded ? " [已装载]" : " [未装载]"}: ${d.description}`)
        .join("\n"),
      data: { agents: defs.map((d) => ({ name: d.name, description: d.description, loaded: d.loaded })) },
    }
  },
}

export const agentLoadTool: Tool = {
  name: "agent_load",
  description: "装载指定子Agent 能力模块（类比 import 子模块）：其工具立即并入当前工具集（以 {agent}_ 前缀调用）、完整系统提示词（工作流与行为约束）注入当前上下文，获得该子Agent 的完整能力。装载不创建新上下文、不启动独立执行——装载后直接调用其工具，全程在当前会话上下文内完成；重复装载幂等跳过。默认使用方式：先装载后直接用其工具；仅在需要干净上下文（结果隔离）或防止上下文膨胀时才改用 agent_run（执行新会话，预加载子Agent 后独立执行，无需装载）。",
  card: { titleParams: ["name"], args: "none" },
  parameters: schema({ name: { type: "string" } }, ["name"]),
  async execute(args, ctx) {
    await ctx.loadSubAgent(String(args.name))
    return { output: `子Agent ${args.name} 已装载（其工具已并入当前工具集，可直接调用 ${args.name}_* 工具）。` }
  },
}

export const agentRunTool: Tool = {
  name: "agent_run",
  description: "执行新会话：派生一个临时新会话（独立上下文，与主会话完全隔离），预加载指定子Agent 列表（一个或多个，其完整系统提示词与工具进入新会话上下文），然后阻塞执行任务直到结束，只返回最终结果文本；新会话执行过程全程存档供历史回放。默认优先 agent_load 装载后直接用其工具；仅在需要干净上下文（子任务结果隔离、不污染主上下文）或防止上下文膨胀（子任务中间过程多、输出大）时使用。",
  card: { titleParams: ["agents"] },
  parameters: schema(
    {
      agents: { type: "array", items: { type: "string" }, description: "预加载进新会话的子Agent 名称列表（一个或多个，如 [\"code\", \"playwright\"]）" },
      input: { type: "string", description: "任务指令（新会话的初始消息）" },
    },
    ["agents", "input"],
  ),
  async execute(args, ctx) {
    const agents = Array.isArray(args.agents) ? args.agents.map(String) : []
    if (!agents.length) return { output: "参数 agents 必须为非空子Agent 名称列表。" }
    const result = await ctx.runNewSession(agents, String(args.input))
    // 最终返回超长时截断（与其余工具一致）；新会话完整存档原样挂到调用记录（截断只影响主上下文可见的结果文本）
    const safe = !result.output || result.output.length <= TRUNCATE_THRESHOLD ? { output: result.output } : await truncate(result.output, `session_${agents[0]}`, ctx)
    return { output: safe.output, sessionRun: result.archive }
  },
}

/** 定时任务工具（cron_*）：服务端启用 GEBAI_CRON_ENABLED=true 时才注册，关闭时完全不可见。 */
export function makeCronTools(): Record<string, Tool> {
  const add: Tool = {
    name: "cron_add",
    description:
      "创建定时任务，到点自动执行。类型二选一：script（脚本运行——shell 命令在会话 tmp/ 目录以会话环境执行，执行结果写入会话消息）；prompt（提示词运行 agent——以指定提示词触发一次完整 Agent 会话，过程与结果出现在会话消息流）。schedule 支持 5 段 cron（分 时 日 月 周，本地时区，如 0 9 * * * 每天 9:00）或 @every 30m（每 30 分钟）/ @daily / @hourly / @weekly / @monthly。仅服务端开启定时任务能力（GEBAI_CRON_ENABLED=true）时可用。",
    requiresApproval: true,
    parameters: schema(
      {
        name: { type: "string", description: "任务名称（可选，便于识别与管理）" },
        schedule: { type: "string", description: "定时表达式：5 段 cron（分 时 日 月 周）或 @every 30m / @daily / @hourly / @weekly / @monthly" },
        type: { enum: ["script", "prompt"], description: "任务类型：script=脚本运行（shell 命令）；prompt=提示词运行 agent" },
        script: { type: "string", description: "type=script 时必填：要执行的 shell 命令" },
        prompt: { type: "string", description: "type=prompt 时必填：触发 agent 运行的提示词" },
        enabled: { type: "boolean", description: "是否启用（默认 true）" },
      },
      ["schedule", "type"],
    ),
    async execute(args, ctx) {
      if (!ctx.cron) return { output: "定时任务能力未启用（服务端未配置 GEBAI_CRON_ENABLED=true）。" }
      const task = await ctx.cron.add({
        name: args.name != null ? String(args.name) : undefined,
        type: String(args.type) as "script" | "prompt",
        schedule: String(args.schedule ?? ""),
        script: args.script != null ? String(args.script) : undefined,
        prompt: args.prompt != null ? String(args.prompt) : undefined,
        enabled: args.enabled === undefined ? undefined : Boolean(args.enabled),
      })
      return {
        output: `定时任务已创建: ${task.id}${task.name ? `（${task.name}）` : ""}\n类型: ${task.type === "script" ? "脚本运行" : "提示词运行 agent"}\n周期: ${task.schedule}\n下次执行: ${new Date(task.nextRunAt).toString()}\n可用 cron_list 查看、cron_update 修改、cron_remove 删除。`,
      }
    },
  }
  const list: Tool = {
    name: "cron_list",
    description: "查看本会话的定时任务列表（ID/名称/类型/周期/启用状态/上次与下次执行时间/执行次数）。",
    parameters: schema({}),
    async execute(_args, ctx) {
      if (!ctx.cron) return { output: "定时任务能力未启用（服务端未配置 GEBAI_CRON_ENABLED=true）。" }
      const tasks = await ctx.cron.list()
      if (!tasks.length) return { output: "本会话暂无定时任务。" }
      return {
        output: tasks
          .map((t) => {
            const body = t.type === "script" ? `命令: ${t.script}` : `提示词: ${t.prompt}`
            const last = t.lastRunAt ? new Date(t.lastRunAt).toString() : "未执行"
            const next = t.enabled ? new Date(t.nextRunAt).toString() : "-（已停用）"
            return `- ${t.id}${t.name ? `（${t.name}）` : ""} [${t.enabled ? "启用" : "停用"}] 类型: ${t.type === "script" ? "脚本运行" : "提示词运行 agent"} 周期: ${t.schedule}\n  ${body}\n  上次: ${last}（${t.lastStatus ?? "-"}） 下次: ${next} 次数: ${t.runCount}${t.lastError ? `\n  最近错误: ${t.lastError}` : ""}`
          })
          .join("\n"),
      }
    },
  }
  const update: Tool = {
    name: "cron_update",
    description: "修改定时任务（按 id：改启用状态/周期/类型/内容），修改后下次执行时间自动重算。",
    requiresApproval: true,
    parameters: schema(
      {
        id: { type: "string", description: "任务 ID（cron_list 查看）" },
        enabled: { type: "boolean", description: "启用/停用" },
        schedule: { type: "string", description: "新的定时表达式" },
        type: { enum: ["script", "prompt"], description: "新的任务类型" },
        script: { type: "string", description: "type=script：新的 shell 命令" },
        prompt: { type: "string", description: "type=prompt：新的提示词" },
      },
      ["id"],
    ),
    async execute(args, ctx) {
      if (!ctx.cron) return { output: "定时任务能力未启用（服务端未配置 GEBAI_CRON_ENABLED=true）。" }
      const task = await ctx.cron.update(String(args.id), {
        enabled: args.enabled === undefined ? undefined : Boolean(args.enabled),
        schedule: args.schedule != null ? String(args.schedule) : undefined,
        type: args.type != null ? String(args.type) as "script" | "prompt" : undefined,
        script: args.script != null ? String(args.script) : undefined,
        prompt: args.prompt != null ? String(args.prompt) : undefined,
      })
      if (!task) return { output: `定时任务不存在: ${args.id}` }
      return { output: `定时任务已更新: ${task.id}${task.name ? `（${task.name}）` : ""} [${task.enabled ? "启用" : "停用"}] 周期: ${task.schedule}\n下次执行: ${task.enabled ? new Date(task.nextRunAt).toString() : "-（已停用）"}` }
    },
  }
  const remove: Tool = {
    name: "cron_remove",
    description: "删除定时任务（按 id，不可恢复）。",
    requiresApproval: true,
    parameters: schema({ id: { type: "string" } }, ["id"]),
    async execute(args, ctx) {
      if (!ctx.cron) return { output: "定时任务能力未启用（服务端未配置 GEBAI_CRON_ENABLED=true）。" }
      const removed = await ctx.cron.remove(String(args.id))
      return removed ? { output: `定时任务已删除: ${args.id}` } : { output: `定时任务不存在: ${args.id}` }
    },
  }
  return { cron_add: add, cron_list: list, cron_remove: remove, cron_update: update }
}

export function createGlobalTools(): Record<string, Tool> {
  const todoTool = makeTodoTool()
  return {
    read: readTool,
    write: writeTool,
    ls: lsTool,
    grep: grepTool,
    search_files: searchFilesTool,
    delete_file: deleteFileTool,
    move_file: moveFileTool,
    edit: editTool,
    apply_patch: applyPatchTool,
    diff: diffTool,
    git: gitTool,
    flow: makeFlowTool(),
    tool_schemas: toolSchemasTool,
    sh: shTool,
    py: pyTool,
    draw: drawTool,
    render_html: renderHtmlTool,
    save_tool: saveTool,
    delete_tool: deleteTool,
    fetch_url: fetchUrlTool,
    todo: todoTool,
    ask_user: askUserTool,
    ask_env: askEnvTool,
    read_feedback: readFeedbackTool,
    preview_server: makePreviewServerTool(),
    current_time: currentTimeTool,
    system_info: systemInfoTool,
    env_detect: envDetectTool,
    // agent_list 不注册进总Agent 全局工具集：未装载子Agent 清单已由 systemPromptInjection 注入提示词
    // （模型上下文已有，工具调用冗余且干扰工具选择）；agent_list 仅在新会话执行（组合子Agent 编排环境）
    // 注入——runNewSession 对纯 md 组合式子Agent 自动注入 agent_list/agent_load/agent_run
    agent_load: agentLoadTool,
    agent_run: agentRunTool,
  }
}

export type ToolSet = Record<string, Tool>
