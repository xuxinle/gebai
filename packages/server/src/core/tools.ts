import { dirname, isAbsolute, join, relative, sep } from "node:path"
import { homedir, tmpdir } from "node:os"
import { spawn } from "node:child_process"
import { lookup } from "node:dns/promises"
import { createServer, connect, type AddressInfo } from "node:net"
import type { ContentBlock, DiagramFormat, FileEntry, TodoItem, ToolSchema } from "@gebai/sdk"
import type { ChoiceOption, Tool, ToolContext, ToolResult } from "./types"
import { truncatedPath, truncatedLogicalPath, sessionPath } from "./paths"
import { randomUUID, createHash } from "node:crypto"
import { isBinaryMode } from "./config"
import { diffLines, inferLang, unifiedDiff, splitLines, DIFF_MAX_LINES } from "./diff"
import { applyPatch, parsePatch, PATCH_MAX_FILE_BYTES, PATCH_MAX_HUNKS, type AppliedHunk } from "./patch"
import { hostBlockReason } from "./ip"
import { runFlow, scanFlowApprovals } from "./flow"
import { jsTool, jsRuntimeCommand } from "./js-tool"
import { PY_SAFE_BOOTSTRAP, safeModeWriteCheck, shApprovalFreeAllowed, validateShCommandSafeMode } from "./safety"
import { shTaskLifetimeMs, shTaskStatus, type ShTaskRecord } from "./sh-tasks"
import { EXCLUDED_GLOBAL_TOOLS } from "./tools-excluded.generated"

export const TRUNCATE_THRESHOLD = 12000

/** 构建期排除的全局工具名单（`GEBAI_BUILD_EXCLUDE_TOOLS` → `scripts/build-tools.ts` 生成并烘焙，
 *  二进制形态无法改源码，须构建时定死）。**静态导入**（运行时读文件在 bun --compile 单文件形态不可行，
 *  顶层 await 动态导入在 tools↔js-tool/engine 循环依赖图下产生未初始化绑定）；生成文件提交默认空名单，
 *  裁剪构建后为脏属预期（下次常规构建自动恢复全量）。 */
const excludedGlobalTools = new Set<string>(EXCLUDED_GLOBAL_TOOLS)

/** 测试注入：覆写构建期排除名单（测试环境生成文件为默认空名单，无法验证过滤路径）。 */
export function _setExcludedGlobalToolsForTest(names: string[]): void {
  excludedGlobalTools.clear()
  for (const n of names) excludedGlobalTools.add(n)
}

/** 工具是否被构建期排除：index.ts 注册（含 vision）、engine agent_run 新会话内建编排工具（flow/tool_schemas/js）
 *  注入共用——排除 = 不注册不暴露（schema 不可见、调用报未知工具）。 */
export function isGlobalToolExcluded(name: string): boolean {
  return excludedGlobalTools.has(name)
}

/** 截断消息保留的首/尾字符数（DESIGN「常量参考」）。 */
export const TRUNCATE_HEAD_CHARS = 4000
export const TRUNCATE_TAIL_CHARS = 4000
/** read/edit 文件大小上限（内存护栏）：全量读入内存前先 stat 预检——GB 级文件直接读入会 OOM；
 *  超限引导 offset/limit 分段读取（read）或 patch（edit，同 PATCH_MAX_FILE_BYTES 口径）。 */
const READ_MAX_FILE_BYTES = 8 * 1024 * 1024
const EDIT_MAX_FILE_BYTES = PATCH_MAX_FILE_BYTES

/** 读前大小预检：超过上限返回明确错误（throw 由工具框架转为输出）。 */
async function assertReadableSize(path: string, tool: string, maxBytes: number): Promise<void> {
  const { stat } = await import("node:fs/promises")
  const st = await stat(path)
  if (st.size > maxBytes) {
    throw new Error(`${tool}: 文件过大（${st.size} 字节，上限 ${maxBytes}）——请用 offset/limit 分段读取，或 grep/patch 定位修改`)
  }
}
/** grep：单文件读取上限与最大匹配行数。 */
const GREP_MAX_FILE_BYTES = 1024 * 1024
const GREP_MAX_MATCHES = 200
/** grep 匹配子进程超时：模型提供的正则存在灾难性回溯形态（如 (a+)+b 配超长单行），同步执行
 *  会挂死 JS 事件循环且无同步中断手段（服务端全部会话冻结）——匹配在独立子进程执行，超时强杀。 */
const GREP_MATCHER_TIMEOUT_MS = 20_000

/** grep 匹配 runner（独立子进程，bun 直跑）：stdin 收 JSON 请求、stdout 回 JSON 结果。
 *  只做正则匹配（返回每文件命中行号），文件读取与结果渲染留在进程内（ctx.readFile 抽象/上下文块逻辑）。 */
const GREP_MATCHER_SCRIPT = [
  "let input = ''",
  "process.stdin.setEncoding('utf8')",
  "process.stdin.on('data', (d) => { input += d })",
  "process.stdin.on('end', () => {",
  "  try {",
  "    const req = JSON.parse(input)",
  "    const re = new RegExp(req.pattern, req.flags || '')",
  "    const hits = []",
  "    let total = 0, capped = false",
  "    for (const f of req.files) {",
  "      const hitIdx = []",
  "      for (let i = 0; i < f.lines.length; i++) {",
  "        if (!re.test(f.lines[i])) continue",
  "        if (total >= req.maxMatches) { capped = true; break }",
  "        total++; hitIdx.push(i)",
  "      }",
  "      hits.push({ display: f.display, hitIdx })",
  "      if (capped) break",
  "    }",
  "    process.stdout.write(JSON.stringify({ hits, total, capped }))",
  "  } catch (e) {",
  "    process.stdout.write(JSON.stringify({ error: String((e && e.message) || e) }))",
  "  }",
  "})",
].join("\n")

let grepRunnerPath: string | null = null

/** grep 匹配子进程执行：请求 {pattern, flags, maxMatches, files:[{display,lines}]} → {hits, total, capped}。
 *  超时/崩溃/输出非法时返回 error（不回退进程内匹配——回退即重新暴露事件循环挂死面）。 */
async function runGrepMatcher(
  req: Record<string, unknown>,
): Promise<{ error?: string; hits?: Array<{ display: string; hitIdx: number[] }>; total?: number; capped?: boolean }> {
  const { writeFile } = await import("node:fs/promises")
  if (!grepRunnerPath) {
    grepRunnerPath = join(tmpdir(), `gebai-grep-runner-${process.pid}.js`)
    await writeFile(grepRunnerPath, GREP_MATCHER_SCRIPT)
  }
  const cmd = jsRuntimeCommand(grepRunnerPath)
  const isWin = process.platform === "win32"
  const child = spawn(cmd[0], cmd.slice(1), { stdio: ["pipe", "pipe", "pipe"], detached: !isWin })
  let settled = false
  const kill = () => {
    if (settled) return
    try {
      if (isWin) spawn("taskkill", ["/pid", String(child.pid), "/T", "/F"])
      else if (child.pid) process.kill(-child.pid, "SIGKILL")
    } catch {
      try { child.kill("SIGKILL") } catch { /* 已退出 */ }
    }
  }
  const timer = setTimeout(kill, GREP_MATCHER_TIMEOUT_MS)
  try {
    child.stdin!.end(JSON.stringify(req))
    const stdout = await new Promise<string>((resolve, reject) => {
      let buf = ""
      child.stdout!.setEncoding("utf8")
      child.stdout!.on("data", (d: string) => (buf += d))
      child.stdout!.on("end", () => resolve(buf))
      child.stdout!.on("error", reject)
      child.on("error", reject)
    })
    const parsed = JSON.parse(stdout) as Record<string, unknown>
    if (typeof parsed.error === "string") return { error: parsed.error }
    return parsed as { hits: Array<{ display: string; hitIdx: number[] }>; total: number; capped: boolean }
  } catch {
    return { error: `正则匹配超时或子进程异常（> ${Math.round(GREP_MATCHER_TIMEOUT_MS / 1000)}s）——模式可能引发灾难性回溯（如嵌套量词 (a+)+b），请简化 pattern 或缩小 path/include 范围` }
  } finally {
    settled = true
    clearTimeout(timer)
    kill()
  }
}
/** fetch_url：响应大小上限与超时；STREAM 上限为流式读取的内存护栏（超限中止读取）。 */
const FETCH_URL_MAX_BYTES = 200 * 1024
const FETCH_URL_TIMEOUT = 15000
const FETCH_URL_STREAM_MAX_BYTES = 10 * 1024 * 1024
/** show html 分支：显式预览尺寸上限（px），超限忽略回退默认。 */
export const RENDER_HTML_MAX_WIDTH = 4000
export const RENDER_HTML_MAX_HEIGHT = 2000

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

const IMAGE_EXT = /\.(png|jpe?g|gif|webp|svg|bmp)$/i
const DIAGRAM_EXT = /\.(puml|plantuml|mmd|mermaid|d2|echarts)$/i
/** 图表文件扩展名 → 图表语言（show path 模式与 read/write 产物块推断用）。 */
const DIAGRAM_FORMATS: Record<string, DiagramFormat> = { puml: "plantuml", plantuml: "plantuml", mmd: "mermaid", mermaid: "mermaid", d2: "d2", echarts: "echarts" }
/** 图表语言 → 产物文件扩展名（show 图表分支落盘 tmp/ 用）。 */
export const DIAGRAM_EXT_FOR: Record<DiagramFormat, string> = { plantuml: "puml", mermaid: "mmd", d2: "d2", echarts: "echarts" }
/** 图表语言展示名（错误提示/文档用）。 */
const DIAGRAM_LABEL: Record<DiagramFormat, string> = { plantuml: "PlantUML", mermaid: "Mermaid", d2: "D2", echarts: "ECharts" }

/** 按文件名扩展名推断图表语言（未命中返回 undefined）。 */
export function diagramFormatFor(path: string): DiagramFormat | undefined {
  const m = path.match(DIAGRAM_EXT)
  return m ? DIAGRAM_FORMATS[m[1].toLowerCase()] : undefined
}

function mimeFor(path: string): string | undefined {
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

/** cat -n 风格行号前缀：startLine 为首行真实行号（offset/limit 切片后仍对应文件行号），右对齐 + 制表符分隔。 */
function withLineNumbers(text: string, startLine: number): string {
  const lines = text.split("\n")
  if (lines.length && lines[lines.length - 1] === "") lines.pop()
  const width = String(startLine + Math.max(0, lines.length - 1)).length
  return lines.map((l, i) => `${String(startLine + i).padStart(width)}\t${l}`).join("\n")
}

/** 剥离 UTF-8 BOM（文件头 \uFEFF）：read 展示与 edit/patch 匹配用干净正文（BOM 会让文件首行的
 *  oldString 精确匹配静默失败——Windows 工具生成的文件常见）；写回时按原文件有无 BOM 补回（edit/patch/write）。 */
function stripBom(s: string): string {
  return s.charCodeAt(0) === 0xfeff ? s.slice(1) : s
}

export const readTool: Tool = {
  name: "read",
  description:
    "读取文件内容。相对路径以会话工作目录（tmp/）为基准（tmp/ 前缀可省略），本地模式支持绝对路径（服务端部署受沙箱限制）。默认每行前缀真实行号（cat -n 风格「行号→制表符」，定位/引用 文件:行号、构造 patch 补丁用；复制原文给 edit/patch 时须去掉行号前缀，不需要行号可传 lineNumbers:false）。非 UTF-8 编码（file info 探测的 GBK 等）传 encoding 按指定编码解码读取。图片/图表等二进制或结构化文件会返回对应内容块供 UI 展示。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema({
    path: { type: "string", description: "文件路径" },
    offset: { type: "integer", description: "起始行号（1 起始，默认 1）" },
    limit: { type: "integer", description: "读取行数（正数取 offset 起 N 行；负数取末尾 N 行）" },
    lineNumbers: { type: "boolean", description: "每行前缀真实行号（默认 true；offset/limit 切片仍对应文件真实行号）" },
    encoding: { type: "string", description: "可选：按指定编码解码读取（如 gbk——file info 探测为 GBK 时用，缺省 UTF-8；支持 TextDecoder 编码名）。仅解码读取，需转码改写文件用 py 脚本处理" },
  }, ["path"]),
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    await assertReadableSize(path, "read", READ_MAX_FILE_BYTES)
    // 编码指定读取：按原始字节解码（非 UTF-8 文件——file info 探测为 GBK 等场景）；目录在此一并给出可读引导
    let content: string
    try {
      if (args.encoding) {
        const enc = String(args.encoding)
        const bytes = await ctx.readBinaryFile(path)
        try {
          // @types/node 的 Encoding 枚举不含任意标签（运行时支持），断言绕过类型限制（同 sniffGbk 惯例）
          content = new TextDecoder(enc as never, { fatal: true }).decode(bytes)
        } catch {
          return { output: `read 失败：按 ${enc} 解码失败（编码名非法或内容不是该编码）——可先用 file 工具（action=info）探测实际编码。` }
        }
      } else {
        content = await ctx.readFile(path)
      }
    } catch (err) {
      try {
        const { stat } = await import("node:fs/promises")
        if ((await stat(path)).isDirectory()) {
          return { output: `read 拒绝：${args.path} 是目录——请用 ls 列出内容后选择具体文件（或 glob 按文件名模式查找）。` }
        }
      } catch {
        /* 保持原错误 */
      }
      throw err
    }
    content = stripBom(content)
    const offset = args.offset == null ? undefined : Number(args.offset)
    const limit = args.limit == null ? undefined : Number(args.limit)
    const sliced = sliceLines(content, offset, limit)
    let body = sliced
    if (args.lineNumbers !== false) {
      // 切片首行的真实行号：offset 起 → offset；尾部切片（limit<0）按全文行数倒推
      let startLine = 1
      if (limit != null && limit < 0) {
        const total = content.split("\n").length - (content.endsWith("\n") ? 1 : 0)
        const shown = sliced.split("\n").length
        startLine = Math.max(1, total - shown + 1)
      } else if (offset != null && offset > 1) startLine = offset
      body = withLineNumbers(sliced, startLine)
    }
    // 分段读取（offset/limit）附位置注记：模型据此判断是否还有未读内容、推算下一段 offset（尾注不进文件本体）
    if ((offset != null || limit != null) && sliced) {
      const totalLines = content.split("\n").length - (content.endsWith("\n") ? 1 : 0)
      const shownLines = sliced.split("\n").length - (sliced.endsWith("\n") ? 1 : 0)
      const firstLine = limit != null && limit < 0 ? Math.max(1, totalLines - shownLines + 1) : Math.max(1, offset ?? 1)
      body += `\n（第 ${firstLine}–${firstLine + shownLines - 1} 行，共 ${totalLines} 行）`
    }
    const truncated = await truncate(body, "read", ctx)
    // 登记已读（write 防误覆盖守卫依据；读取失败抛错不登记）
    ctx.fileGuard?.markRead(path)
    // 产物块路径用解析后的可预览路径（会话 tmp/ 逻辑路径或绝对路径——原始参数路径在项目工具下无法由 files 接口解析）
    const blocks = artifactBlocks(previewLogicalPath(path, ctx), content)
    return { ...truncated, blocks }
  },
}

export const writeTool: Tool = {
  name: "write",
  description:
    "写入文件。相对路径以会话工作目录（tmp/）为基准（tmp/ 前缀可省略，受沙箱限制）。默认整体覆盖；append:true 追加模式——内容接在文件末尾（文件不存在则新建）。目标文件**已存在且本会话未 read 过**时拒绝写入（防盲覆盖：先 read 掌握现有内容，确认整体覆盖后再 write；新建文件不受限）。read/edit/patch/write 成功过的文件视为已读；只改局部优先 edit/patch。**大文件（约 300 行以上）分段写入**：先 write 首段，再以 append:true 续写后续段（每段 200~300 行），避免单次输出过长被模型输出上限截断或接口超时。",
  card: { titleParams: ["path"], args: "code", codeField: "content", file: "path" },
  parameters: schema({
    path: { type: "string" },
    content: { type: "string" },
    append: { type: "boolean", description: "追加模式：内容接在文件末尾（不存在则新建）；大文件分段续写用" },
  }, ["path", "content"]),
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    // 安全模式：写入限定用户目录内（降级而非禁用）
    const safeMsg = ctx.safeMode ? safeModeWriteCheck([path], ctx) : null
    if (safeMsg) return { output: safeMsg }
    // 写范围守卫（子Agent 声明，引擎注入）：拒绝则作为工具结果返回（不落盘）
    const guardMsg = await ctx.writeGuard?.([path])
    if (guardMsg) return { output: guardMsg }
    const append = args.append === true
    let existing: string | null = null
    try {
      existing = await ctx.readFile(path)
    } catch {
      existing = null
    }
    // 防误覆盖守卫（ZCode Write 语义，覆盖/追加同规则）：已存在但未读过 → 拒绝并引导先 read（模型下一轮自纠，一次往返）
    if (existing !== null && ctx.fileGuard && !ctx.fileGuard.hasRead(path)) {
      return {
        output: `write 拒绝：${args.path} 已存在，但本会话尚未读取过其内容（防盲覆盖）。请先 read 该文件确认现有内容，确实要整体覆盖时再 write；只改局部用 edit（定点替换）或 patch（unified diff）。新建文件不受此限制。`,
      }
    }
    const content = stripBom(String(args.content ?? ""))
    // 覆盖写保留原文件的 UTF-8 BOM（read 展示的是去 BOM 正文，模型意图即正文；BOM 丢失会改变文件字节内容）；
    // 追加模式接在 existing 之后不动文件头
    const bom = existing !== null && existing.startsWith("\uFEFF") ? "\uFEFF" : ""
    const final = append && existing !== null ? existing + content : bom + content
    await ctx.writeFile(path, final)
    ctx.fileGuard?.markRead(path)
    const blocks = artifactBlocks(previewLogicalPath(path, ctx), final)
    return {
      output: append && existing !== null
        ? `已追加 ${content.length} 字符至 ${args.path}（现共 ${final.length} 字符）`
        : `已写入 ${args.path}（${content.length} 字符）`,
      blocks,
    }
  },
}

export const lsTool: Tool = {
  name: "ls",
  description: "列出目录内容（文件/子目录、大小）。路径默认会话工作目录（tmp/，前缀可省略）。",
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

/** file info 类型猜测（扩展名 → 描述；目录与未知扩展名另行处理）。 */
const FILE_TYPE_BY_EXT: Record<string, string> = {
  ts: "TypeScript 源码", tsx: "TypeScript JSX 源码", js: "JavaScript 源码", mjs: "JavaScript 模块", jsx: "JSX 源码",
  py: "Python 源码", go: "Go 源码", rs: "Rust 源码", java: "Java 源码", c: "C 源码", h: "C 头文件", cpp: "C++ 源码",
  cs: "C# 源码", rb: "Ruby 源码", php: "PHP 源码", sh: "Shell 脚本", bat: "Windows 批处理", ps1: "PowerShell 脚本",
  json: "JSON 数据", yaml: "YAML 配置", yml: "YAML 配置", toml: "TOML 配置", ini: "INI 配置", env: "环境变量文件",
  md: "Markdown 文档", txt: "纯文本", html: "HTML 文档", htm: "HTML 文档", css: "样式表", csv: "CSV 数据", sql: "SQL 脚本",
  png: "PNG 图片", jpg: "JPEG 图片", jpeg: "JPEG 图片", gif: "GIF 图片", webp: "WebP 图片", svg: "SVG 矢量图", ico: "图标文件",
  pdf: "PDF 文档", docx: "Word 文档", xlsx: "Excel 表格", pptx: "PPT 演示", log: "日志文件", lock: "锁文件",
  zip: "ZIP 压缩包", gz: "Gzip 压缩", tar: "TAR 归档", "7z": "7z 压缩包", rar: "RAR 压缩包",
  exe: "可执行文件", msi: "Windows 安装包", dll: "动态链接库", so: "共享库", dylib: "共享库", bin: "二进制文件",
  mp3: "音频", wav: "音频", flac: "音频", mp4: "视频", mov: "视频", mkv: "视频", webm: "视频",
  mmd: "Mermaid 图表", puml: "PlantUML 图表", plantuml: "PlantUML 图表", d2: "D2 图表",
}

/** 人类可读大小（B/KB/MB/GB，保留 1 位小数）。 */
function humanSize(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`
  return `${(n / 1024 / 1024 / 1024).toFixed(1)} GB`
}

/** 扩展名隐含的粗分类（info 扩展名/内容不符判定用；文本类参与——内容探测为 text 时一致不报，为二进制/图片等时报不符）。 */
const EXT_KIND: Record<string, string> = {
  txt: "text", md: "text", json: "text", html: "text", htm: "text", xml: "text", yaml: "text", yml: "text",
  toml: "text", ini: "text", env: "text", csv: "text", css: "text", log: "text", sql: "text",
  ts: "text", tsx: "text", js: "text", mjs: "text", jsx: "text", py: "text", go: "text", rs: "text",
  java: "text", c: "text", h: "text", cpp: "text", cs: "text", rb: "text", php: "text",
  sh: "text", bat: "text", ps1: "text", mmd: "text", puml: "text", plantuml: "text", d2: "text",
  png: "image", jpg: "image", jpeg: "image", gif: "image", webp: "image", bmp: "image", ico: "image", svg: "image",
  pdf: "document", docx: "document", xlsx: "document", pptx: "document",
  zip: "archive", gz: "archive", tgz: "archive", "7z": "archive", rar: "archive", tar: "archive",
  exe: "executable", msi: "executable", dll: "executable", so: "executable", dylib: "executable", bin: "executable",
  mp3: "audio", wav: "audio", flac: "audio", mp4: "video", mov: "video", mkv: "video", webm: "video",
}

/** 二进制魔数探测（类似 file 命令）：命中返回 { type, kind, encoding? }，未命中返回 null（继续按文本处理）。 */
function sniffMagic(buf: Buffer, ext: string): { type: string; kind: string; encoding?: string } | null {
  const a = (n: number, s = 0) => buf.toString("latin1", s, s + n)
  if (buf[0] === 0x89 && buf[1] === 0x50) return { type: "PNG 图片", kind: "image" }
  if (buf[0] === 0xff && buf[1] === 0xd8 && buf[2] === 0xff) return { type: "JPEG 图片", kind: "image" }
  if (a(6) === "GIF87a" || a(6) === "GIF89a") return { type: "GIF 图片", kind: "image" }
  if (a(4) === "RIFF" && a(4, 8) === "WEBP") return { type: "WebP 图片", kind: "image" }
  if (a(2) === "BM") return { type: "BMP 图片", kind: "image" }
  if (buf[0] === 0 && buf[1] === 0 && buf[2] === 1 && buf[3] === 0) return { type: "ICO 图标", kind: "image" }
  if (a(5) === "%PDF-") return { type: "PDF 文档", kind: "document" }
  if (buf[0] === 0x50 && buf[1] === 0x4b && (buf[2] === 3 || buf[2] === 5 || buf[2] === 7)) {
    // ZIP 容器：Office Open XML/EPUB/JAR 等按扩展名细分，否则泛称 ZIP
    const z: Record<string, string> = {
      docx: "Word 文档（Office Open XML，ZIP 容器）", xlsx: "Excel 表格（Office Open XML，ZIP 容器）", pptx: "PPT 演示（Office Open XML，ZIP 容器）",
      epub: "EPUB 电子书（ZIP 容器）", jar: "JAR 包（ZIP 容器）", apk: "APK 安装包（ZIP 容器）",
    }
    return { type: z[ext] ?? "ZIP 压缩包（或 Office/EPUB 等 ZIP 容器）", kind: "archive" }
  }
  if (buf[0] === 0x1f && buf[1] === 0x8b) return { type: "Gzip 压缩", kind: "archive" }
  if (a(6) === "7z\xbc\xaf\x27\x1c") return { type: "7z 压缩包", kind: "archive" }
  if (a(4) === "Rar!") return { type: "RAR 压缩包", kind: "archive" }
  if (a(5, 257) === "ustar") return { type: "TAR 归档", kind: "archive" }
  if (a(15) === "SQLite format ") return { type: "SQLite 数据库", kind: "data" }
  if (buf[0] === 0x7f && a(4) === "\x7fELF") return { type: "ELF 可执行/共享库", kind: "executable" }
  if (a(2) === "MZ") return { type: "PE 可执行（exe/dll）", kind: "executable" }
  if (buf[0] === 0xca && buf[1] === 0xfe && buf[2] === 0xba && buf[3] === 0xbe) return { type: "Java class 文件", kind: "executable" }
  // UTF-16 BOM（文本族，带编码）
  if (buf[0] === 0xff && buf[1] === 0xfe) return { type: "UTF-16 LE 文本", kind: "text", encoding: "utf-16le" }
  if (buf[0] === 0xfe && buf[1] === 0xff) return { type: "UTF-16 BE 文本", kind: "text", encoding: "utf-16be" }
  return null
}

/** UTF-8 文本内容判定：shebang 解释器 / 高置信格式（SVG/HTML/XML/JSON）→ 具体类型，否则纯 UTF-8 文本。 */
function sniffText(buf: Buffer, bytesRead: number, wholeFile: boolean): { type: string; encoding: string } | null {
  try {
    const text = new TextDecoder("utf-8", { fatal: true }).decode(buf.subarray(0, bytesRead))
    const hasBom = buf[0] === 0xef && buf[1] === 0xbb && buf[2] === 0xbf
    const encoding = hasBom ? "utf-8-bom" : "utf-8"
    const m = /^#!/.test(text)
    if (m) {
      // shebang 解析：取首行去掉 #! 后按空白切分——env 形式取其后解释器（env python3 → python3），
      // 路径形式取最后一段（/usr/bin/python3 → python3，/bin/bash -e → bash）
      const nl = text.indexOf("\n")
      const line = (nl === -1 ? text : text.slice(0, nl)).slice(2).trim()
      const parts = line.split(/\s+/).filter(Boolean)
      const interp = parts[0] === "env" ? (parts[1] ?? "env") : parts[parts.length - 1] ?? ""
      return { type: `脚本（shebang: ${interp}）`, encoding }
    }
    const head = text.trimStart().slice(0, 32).toLowerCase()
    if (head.startsWith("<svg")) return { type: "SVG 矢量图（XML 文本）", encoding }
    if (head.startsWith("<!doctype html") || head.startsWith("<html")) return { type: "HTML 文档（文本）", encoding }
    if (head.startsWith("<?xml")) return { type: "XML 文档（文本）", encoding }
    const t = text.trim()
    if (wholeFile && (t.startsWith("{") || t.startsWith("["))) {
      try {
        JSON.parse(t)
        return { type: "JSON 数据（文本）", encoding }
      } catch {
        // 形似 JSON 但解析失败，按普通文本
      }
    }
    return { type: "UTF-8 文本", encoding }
  } catch {
    return null
  }
}

/** 非 UTF-8 且无魔数：疑似 GBK/ANSI 编码文本（合法 GBK 解码 + 含 CJK + 无替换符；NUL 字节按二进制处理）。 */
function sniffGbk(buf: Buffer, bytesRead: number): boolean {
  const slice = buf.subarray(0, bytesRead)
  if (slice.includes(0)) return false
  try {
    // @types/node 的 Encoding 枚举未含 gbk（Node/Bun 运行时均支持），断言绕过类型限制
    const s = new TextDecoder("gbk" as never).decode(slice)
    return !s.includes("\uFFFD") && /[\u4e00-\u9fff]/.test(s)
  } catch {
    return false
  }
}

/** 未知二进制判定：NUL 字节或控制字符占比过高（\t\n\r 除外）。 */
function looksBinary(buf: Buffer, bytesRead: number): boolean {
  const slice = buf.subarray(0, bytesRead)
  if (slice.includes(0)) return true
  let ctrl = 0
  for (const b of slice) if (b < 0x20 && b !== 9 && b !== 10 && b !== 13) ctrl++
  return ctrl / Math.max(1, bytesRead) > 0.3
}

/** file copy 动作大小上限（二进制整读整写，防巨文件拖垮内存/磁盘；超限引导 sh 复制）。 */
const FILE_COPY_MAX_BYTES = 100 * 1024 * 1024

export const fileTool: Tool = {
  name: "file",
  description:
    "文件管理（单工具多动作）：copy 复制文件（支持二进制，to 为目标路径含文件名，父目录自动创建）/ rename 重命名 / move 移动或跨目录改名 / mkdir 新建目录（递归）/ delete 删除文件或目录（递归，不可恢复，谨慎）/ info 查看文件信息——**按内容探测**（类似 file 命令）：魔数识别实际类型、文本/二进制判定（二进制勿盲 read）、编码（UTF-8/BOM/UTF-16/疑似 GBK——GBK 用 read 的 encoding=gbk 读取）、**扩展名与实际内容不符时显式提示**、大小与修改时间（目录附直接子条目数）。路径与 read/write 同一解析规则。",
  card: { titleParams: ["action", "path"] },
  // delete 递归且不可恢复（能力上甚于一次 sh rm，sh 一律审批）：与审批矩阵对齐，delete 动态需审批
  requiresApproval: (args) => args.action === "delete",
  parameters: schema(
    {
      action: { enum: ["copy", "rename", "move", "mkdir", "delete", "info"], description: "操作类型" },
      path: { type: "string", description: "目标文件/目录路径（copy/rename/move 的源路径，mkdir/delete/info 的目标路径）" },
      to: { type: "string", description: "copy/move 目标路径（含目标文件名；父目录不存在时自动创建）" },
      newName: { type: "string", description: "rename 新名字（仅名字、不含路径分隔符；跨目录改名用 move）" },
    },
    ["action", "path"],
  ),
  outputSchema: schema({
    path: { type: "string", description: "info：逻辑路径" },
    type: { type: "string", description: "info：类型描述（内容探测结果：魔数/文本格式/编码，目录/空文件）" },
    size: { type: "integer", description: "info：字节数（目录为 0）" },
    isDir: { type: "boolean", description: "info：是否目录" },
    modifiedAt: { type: "string", description: "info：修改时间（ISO 8601）" },
    entries: { type: "integer", description: "info：目录直接子条目数（仅目录）" },
    encoding: { type: "string", description: "info：文本编码（utf-8/utf-8-bom/utf-16le/utf-16be/gbk，仅文本）" },
    text: { type: "boolean", description: "info：是否文本内容（true 时可安全 read）" },
    extMismatch: { type: "boolean", description: "info：扩展名隐含分类与实际内容不符" },
  }),
  async execute(args, ctx) {
    const action = String(args.action ?? "")
    const path = ctx.resolvePath(String(args.path))
    // 安全模式：变更动作（copy/rename/move/mkdir/delete）限定用户目录内；info 只读不限
    if (ctx.safeMode && action !== "info") {
      const targets = action === "copy" || action === "move" ? [path, ctx.resolvePath(String(args.to ?? path))] : [path]
      const safeMsg = safeModeWriteCheck(targets, ctx)
      if (safeMsg) return { output: safeMsg }
    }
    if (action === "copy") {
      if (!args.to) return { output: "file 拒绝：copy 需要 to（目标路径，含目标文件名）。" }
      const to = ctx.resolvePath(String(args.to))
      const guardMsg = await ctx.writeGuard?.([path, to])
      if (guardMsg) return { output: guardMsg }
      try {
        if (ctx.readBinaryFile && ctx.writeBinaryFile) {
          const data = await ctx.readBinaryFile(path)
          if (data.byteLength > FILE_COPY_MAX_BYTES) {
            return { output: `file 拒绝：源文件过大（${data.byteLength} 字节，复制上限 ${FILE_COPY_MAX_BYTES}），请用 sh 命令复制。` }
          }
          await ctx.writeBinaryFile(to, data)
        } else {
          await ctx.writeFile(to, await ctx.readFile(path))
        }
      } catch (err) {
        return { output: `file copy 失败：${err instanceof Error ? err.message : String(err)}（请确认源文件存在且可读）` }
      }
      return { output: `已复制 ${args.path} → ${args.to}` }
    }
    if (action === "mkdir") {
      const guardMsg = await ctx.writeGuard?.([path])
      if (guardMsg) return { output: guardMsg }
      const { mkdir } = await import("node:fs/promises")
      await mkdir(path, { recursive: true })
      return { output: `已创建目录 ${args.path}（已存在时不报错、不改变现有内容）` }
    }
    if (action === "rename") {
      const newName = String(args.newName ?? "").trim()
      if (!newName || newName.includes("/") || newName.includes("\\") || newName === "." || newName === "..") {
        return { output: "file 拒绝：rename 需要合法的 newName（仅新名字、不含路径分隔符）；跨目录移动请用 action=move。" }
      }
      const to = join(dirname(path), newName)
      const guardMsg = await ctx.writeGuard?.([path, to])
      if (guardMsg) return { output: guardMsg }
      await ctx.moveFile(path, to)
      return { output: `已重命名 ${args.path} → ${newName}` }
    }
    if (action === "move") {
      if (!args.to) return { output: "file 拒绝：move 需要 to（目标路径，含目标文件名）。" }
      const to = ctx.resolvePath(String(args.to))
      const guardMsg = await ctx.writeGuard?.([path, to])
      if (guardMsg) return { output: guardMsg }
      await ctx.moveFile(path, to)
      return { output: `已移动 ${args.path} → ${args.to}` }
    }
    if (action === "delete") {
      const guardMsg = await ctx.writeGuard?.([path])
      if (guardMsg) return { output: guardMsg }
      await ctx.deleteFile(path)
      return { output: `已删除 ${args.path}` }
    }
    if (action === "info") {
      const { stat, readdir, open } = await import("node:fs/promises")
      let st
      try {
        st = await stat(path)
      } catch (err) {
        return { output: `file: 无法访问 ${args.path}（${err instanceof Error ? err.message : String(err)}）——不存在或无权限，可用 ls/glob 确认。` }
      }
      const modifiedAt = new Date(st.mtimeMs).toISOString()
      if (st.isDirectory()) {
        let entries = -1
        try {
          entries = (await readdir(path)).length
        } catch {
          // 无权限列举时保留 -1（输出省略条目数）
        }
        return {
          output: `${args.path}: 目录${entries >= 0 ? `，${entries} 个直接子条目` : ""}，修改时间 ${modifiedAt}`,
          data: { path: String(args.path), type: "目录", size: 0, isDir: true, modifiedAt, ...(entries >= 0 ? { entries } : {}) },
        }
      }
      const base = path.replace(/[\\/]/g, "/").split("/").pop() ?? ""
      const dot = base.lastIndexOf(".")
      const ext = dot > 0 ? base.slice(dot + 1).toLowerCase() : ""
      const extLabel = FILE_TYPE_BY_EXT[ext] ?? ""
      // 内容探测（类似 file 命令，读头部 1KB）：魔数 → UTF-8 文本（shebang/格式/编码）→ 疑似 GBK → 未知二进制
      if (st.size === 0) {
        return {
          output: `${args.path}: 空文件，修改时间 ${modifiedAt}`,
          data: { path: String(args.path), type: "空文件", size: 0, isDir: false, modifiedAt },
        }
      }
      const fh = await open(path, "r")
      const buf = Buffer.alloc(1024)
      const { bytesRead } = await fh.read(buf, 0, 1024, 0)
      await fh.close()
      const wholeFile = st.size <= 1024
      let type: string
      let kind: string
      let encoding: string | undefined
      const magic = sniffMagic(buf, ext)
      if (magic) {
        type = magic.type
        kind = magic.kind
        encoding = magic.encoding
      } else {
        const text = sniffText(buf, bytesRead, wholeFile)
        if (text) {
          kind = "text"
          encoding = text.encoding
          // 内容有更具体的判定（shebang/SVG/HTML/XML/JSON）用它；否则扩展名标签 + 编码组合，无扩展名泛称
          const specific = !/^(UTF-8 文本)$/.test(text.type)
          const encNote = text.encoding === "utf-8-bom" ? "UTF-8（BOM）" : "UTF-8"
          type = specific ? text.type : extLabel ? `${extLabel}（${encNote} 文本）` : `${encNote} 文本`
        } else if (sniffGbk(buf, bytesRead)) {
          kind = "text"
          encoding = "gbk"
          type = extLabel ? `${extLabel}（疑似 GBK/ANSI 编码，非 UTF-8——read 传 encoding=gbk 读取）` : "文本（疑似 GBK/ANSI 编码，非 UTF-8——read 传 encoding=gbk 读取）"
        } else {
          kind = "binary"
          type = extLabel ? `${extLabel}（二进制内容）` : looksBinary(buf, bytesRead) ? "二进制文件（未知格式）" : "未知格式（非 UTF-8，且无法识别魔数）"
        }
      }
      // 扩展名隐含分类与实际内容不符时显式提示（防误读/误传：模型据此不再盲 read 二进制）
      const extKind = EXT_KIND[ext]
      const mismatch = extKind && extKind !== kind && !(extKind === "image" && ext === "svg" && kind === "text")
      const mismatchNote = mismatch ? `（扩展名 .${ext} 与实际内容不符）` : ""
      return {
        output: `${args.path}: ${type}${mismatchNote}，${humanSize(st.size)}（${st.size} B），修改时间 ${modifiedAt}`,
        data: {
          path: String(args.path), type, size: st.size, isDir: false, modifiedAt,
          ...(encoding ? { encoding } : {}), ...(kind === "text" ? { text: true } : {}), ...(mismatch ? { extMismatch: true } : {}),
        },
      }
    }
    return { output: `file: 未知 action「${action}」，支持 copy/rename/move/mkdir/delete/info。` }
  },
}

/** 列表逻辑路径坐标候选：会话列表带 `tmp/` 前缀（UI/REST 契约），剥离前缀后的裸坐标一并匹配，
 *  使 path/include 过滤在「带/不带 tmp/ 前缀」两种写法下等价（统一路径基准后无需感知前缀）；
 *  项目上下文列表无前缀，仅原样候选。 */
function listPathCandidates(p: string): string[] {
  return p.startsWith("tmp/") ? [p, p.slice(4)] : [p]
}

/** 目录递归遍历时跳过的大型/生成目录（grep 范围外路径与 code/explore 项目根遍历共用，防全量扫描拖慢）。 */
export const WALK_SKIP_DIRS = new Set([".git", "node_modules", "dist", "build", ".next", ".cache", "__pycache__", ".venv", "venv", "target", ".idea", ".vscode", "coverage", ".turbo"])
export const WALK_MAX_DEPTH = 10

/** 目录递归遍历（跳过大型/生成目录、深度上限；root 为单文件时直接返回单条）。pathBase 传入时输出路径带该前缀
 *  （tmp/项目根外搜索的结果路径可直接用于 read 等文件工具），缺省相对 root；root 不存在/不可读返回空。 */
export async function walkDirFiles(root: string, pathBase = ""): Promise<FileEntry[]> {
  const { readdir, stat } = await import("node:fs/promises")
  const st = await stat(root).catch(() => null)
  if (!st) return []
  if (st.isFile()) return [{ path: pathBase || root.replace(/\\/g, "/"), size: st.size, modifiedAt: st.mtimeMs, isDir: false }]
  const out: FileEntry[] = []
  const walk = async (dir: string, rel: string, depth: number): Promise<void> => {
    if (depth > WALK_MAX_DEPTH) return
    let entries
    try {
      entries = await readdir(dir, { withFileTypes: true })
    } catch {
      return
    }
    for (const e of entries) {
      if (e.isDirectory()) {
        if (WALK_SKIP_DIRS.has(e.name)) continue
        await walk(`${dir}/${e.name}`, rel ? `${rel}/${e.name}` : e.name, depth + 1)
      } else if (e.isFile()) {
        let size = 0
        try {
          size = (await stat(`${dir}/${e.name}`)).size
        } catch {
          /* stat 失败按 0 处理 */
        }
        const relPath = rel ? `${rel}/${e.name}` : e.name
        out.push({ path: pathBase ? `${pathBase}/${relPath}` : relPath, size, modifiedAt: 0, isDir: false })
      }
    }
  }
  await walk(root, "", 0)
  return out
}

export const grepTool: Tool = {
  name: "grep",
  description:
    "按正则表达式在会话工作目录（tmp/）中递归搜索文本内容，返回 文件:行号: 匹配行（路径带 tmp/ 前缀，可直接用于 read 等文件工具；本地模式 path 可传 tmp/ 外绝对/相对路径，实际遍历搜索）。宽泛摸底优先 output=files。node_modules/.git/dist 等大型目录默认跳过（显式 include 点名除外）。搜索含正则元字符的代码片段（如 foo.bar(）传 literal:true 按字面匹配。include/exclude 支持逗号分隔多模式与花括号（如 *.{ts,tsx}、tests/**,*.md）。匹配上限 200 处（head_limit 可压低先看一部分）。",
  card: { titleParams: ["pattern"] },
  parameters: schema(
    {
      pattern: { type: "string" },
      path: { type: "string", description: "搜索起点：目录（递归）或单个文件（直接内搜）（默认 .，相对会话工作目录，tmp/ 前缀可省略）" },
      ignoreCase: { type: "boolean" },
      literal: { type: "boolean", description: "true 时 pattern 按字面字符串匹配（正则元字符自动转义），适合搜索含 .()[]* 等字符的代码片段（默认 false 正则）" },
      output: { enum: ["content", "files", "count"], description: "结果形态（默认 content；大范围定位优先 files，只看命中文件不刷内容）" },
      context: { type: "integer", description: "匹配行前后各附上下文行数（0-10，默认 0；仅 content 模式）：匹配行前缀 文件:行号:、上下文行前缀 文件-行号-，组间 -- 分隔（同 grep -n -C）；contextBefore/contextAfter 指定时覆盖对应侧" },
      contextBefore: { type: "integer", description: "匹配行**前**附上下文行数（0-10，仅 content 模式；与 context 独立指定非对称上下文，如同 grep -B）" },
      contextAfter: { type: "integer", description: "匹配行**后**附上下文行数（0-10，仅 content 模式；与 context 独立指定非对称上下文，如同 grep -A——看定义后的实现体常用）" },
      include: { type: "string", description: "文件路径 glob 过滤（如 *.ts、src/**、*.{ts,tsx}，逗号分隔多模式；** 跨目录、* 任意、? 单字符、{a,b} 交替）" },
      exclude: { type: "string", description: "排除的路径 glob（与 include 同语法，命中即排除；如 tests/**,*.{json,md}、dist——无 / 的模式按目录/文件名匹配任意层级）" },
      head_limit: { type: "integer", description: "匹配上限（默认 200；先只看前面一部分时压低，达上限结果标记 truncated——files/count 模式的计数同口径截断）" },
    },
    ["pattern"],
  ),
  outputSchema: schema(
    {
      mode: { type: "string", enum: ["content", "files", "count"], description: "本次结果形态" },
      matches: {
        type: "array",
        description: "匹配列表（mode=content；按文件与行号顺序，上限 head_limit/200）",
        items: schema({ file: { type: "string" }, line: { type: "integer", description: "行号（1 起始）" }, text: { type: "string", description: "匹配行（去除首尾空白，截取前 200 字符）" } }, ["file", "line", "text"]),
      },
      files: { type: "array", description: "命中文件列表（mode=files）", items: { type: "string" } },
      counts: { type: "array", description: "每文件命中行数（mode=count，按命中数降序）", items: schema({ file: { type: "string" }, count: { type: "integer" } }, ["file", "count"]) },
      truncated: { type: "boolean", description: "是否达到匹配上限（结果可能不完整）" },
    },
    ["mode"],
  ),
  async execute(args, ctx) {
    // literal 固定字符串模式：正则元字符转义后按字面匹配（默认正则）
    const pattern = args.literal === true ? String(args.pattern).replace(/[.*+?^${}()|[\]\\]/g, "\\$&") : String(args.pattern)
    // 正则合法性预检（实际匹配在子进程，见 runGrepMatcher）
    try {
      new RegExp(pattern, args.ignoreCase ? "i" : "")
    } catch {
      return { output: `grep: 无效正则: ${args.pattern}` }
    }
    const mode = args.output === "files" || args.output === "count" ? args.output : "content"
    const ctxLine = (v: unknown): number | null => (v == null ? null : Math.max(0, Math.min(10, Math.floor(Number(v) || 0))))
    const context = ctxLine(args.context) ?? 0
    // 非对称上下文（-B/-A）：指定时覆盖 context 的对应侧
    const before = ctxLine(args.contextBefore) ?? context
    const after = ctxLine(args.contextAfter) ?? context
    const maxMatches = Math.max(1, Math.min(GREP_MAX_MATCHES, Math.floor(Number(args.head_limit) || GREP_MAX_MATCHES)))
    const includeRes = globFilters(args.include)
    const includeRaw = args.include ? String(args.include) : ""
    const excludeRes = globFilters(args.exclude)
    const path = args.path ? String(args.path).replace(/\\/g, "/") : ""
    // path 统一经 resolvePath 解析归一化（同 glob：显式传 "." 与省略等价、tmp/ 前缀可省略）
    let relPath = ""
    let outside: FileEntry[] | undefined
    if (path) {
      const root = ctx.resolvePath(".")
      const resolved = ctx.resolvePath(path)
      const rel = relative(root, resolved).replace(/\\/g, "/")
      if (rel === ".." || rel.startsWith("../") || isAbsolute(rel)) {
        // tmp/项目根外路径：本地模式与 read 同款自由度，按给定 path 前缀实际遍历搜索；沙箱部署模式仍拒绝越界
        if (ctx.sandboxed) return { output: "（无匹配文件）", data: { mode, matches: [], files: [], counts: [] } }
        outside = await walkDirFiles(resolved, path.replace(/\/+$/, ""))
        if (!outside.length) return { output: `grep: 路径不存在或无可搜文件: ${path}`, data: { mode, matches: [], files: [], counts: [] } }
      } else relPath = rel === "." ? "" : rel.replace(/\/+$/, "")
    }
    const prefix = relPath ? `${relPath}/` : ""
    const listing = outside ?? (await ctx.listFiles())
    // path 精确命中单文件时直接内搜该文件（grep 传文件语义），否则按目录前缀过滤（范围外已按给定 path 定界）
    const exact = !outside && relPath ? listing.find((f) => !f.isDir && listPathCandidates(f.path).includes(relPath)) : undefined
    const files = (exact ? [exact] : listing).filter((f) => {
      if (f.isDir || f.size > GREP_MAX_FILE_BYTES) return false
      const cs = listPathCandidates(f.path)
      if (prefix && !exact && !cs.some((c) => c.startsWith(prefix))) return false
      if (includeRes && !globMatchAny(includeRes, cs)) return false
      if (excludeRes && globMatchAny(excludeRes, cs)) return false
      // 默认排除大型/生成目录（node_modules/.git/dist 等；include 原文显式点名该目录时不排除，单文件内搜不受限）
      if (!exact && isDefaultExcluded(cs, includeRaw)) return false
      return true
    })
    if (!files.length) return { output: "（无匹配文件）", data: { mode, matches: [], files: [], counts: [] } }
    // 分批读文件 → 行数据送子进程匹配（灾难性回溯防护，见 runGrepMatcher）→ 命中行号回父进程渲染。
    // 批大小上限控制 stdin 载荷与瞬时内存；命中文件的行保留用于上下文渲染（受匹配上限约束）
    const BATCH_BYTES = 4 * 1024 * 1024
    const hitFiles: Array<{ display: string; lines: string[]; hitIdx: number[] }> = []
    let capped = false
    let matcherError: string | null = null
    let batch: Array<{ display: string; lines: string[] }> = []
    let batchBytes = 0
    let stop = false
    const flushBatch = async () => {
      if (!batch.length) return
      const sent = batch
      const r = await runGrepMatcher({ pattern, flags: args.ignoreCase ? "i" : "", maxMatches, files: sent })
      batch = []
      batchBytes = 0
      if (r.error) {
        matcherError = r.error
        stop = true
        return
      }
      for (const [i, h] of (r.hits ?? []).entries()) {
        if (h.hitIdx.length) hitFiles.push({ display: h.display, lines: sent[i]?.lines ?? [], hitIdx: h.hitIdx })
      }
      if (r.capped) {
        capped = true
        stop = true
      }
    }
    for (const f of files) {
      if (stop) break
      let content: string
      try {
        content = await ctx.readFile(ctx.resolvePath(f.path))
      } catch {
        continue // 二进制等不可读文件跳过
      }
      // 二进制内容（NUL 字节）跳过：防乱码匹配行刷进上下文（同 grep 二进制检测语义）
      if (content.includes("\0")) continue
      const lines = content.split("\n")
      if (batchBytes + content.length > BATCH_BYTES) {
        await flushBatch()
        if (stop) break
      }
      batch.push({ display: f.path, lines })
      batchBytes += content.length
    }
    await flushBatch()
    if (matcherError) return { output: `grep: ${matcherError}`, data: { mode, matches: [], files: [], counts: [] } }
    const matches: Array<{ file: string; line: number; text: string }> = []
    // content 模式渲染行（含 context 组）；非 content 模式按文件聚合命中数
    const blocks: string[] = []
    const fileCounts: Array<{ file: string; count: number }> = []
    for (const f of hitFiles) {
      if (mode === "content") {
        for (const i of f.hitIdx) matches.push({ file: f.display, line: i + 1, text: f.lines[i]?.trim().slice(0, 200) ?? "" })
      } else {
        fileCounts.push({ file: f.display, count: f.hitIdx.length })
        continue
      }
      if (before > 0 || after > 0) {
        // 上下文模式：重叠区间合并后整块渲染（匹配行 : 前缀、上下文行 - 前缀，组间 -- 分隔；before/after 可非对称）
        const ranges: Array<[number, number]> = []
        for (const i of f.hitIdx) {
          const s = Math.max(0, i - before)
          const e = Math.min(f.lines.length - 1, i + after)
          const last = ranges[ranges.length - 1]
          if (last && s <= last[1] + 1) last[1] = Math.max(last[1], e)
          else ranges.push([s, e])
        }
        const hitSet = new Set(f.hitIdx)
        for (const [s, e] of ranges) {
          for (let i = s; i <= e; i++) {
            const text = f.lines[i]?.trim().slice(0, 200) ?? ""
            blocks.push(hitSet.has(i) ? `${f.display}:${i + 1}: ${text}` : `${f.display}-${i + 1}- ${text}`)
          }
          blocks.push("--")
        }
      } else {
        blocks.push(...f.hitIdx.map((i) => `${f.display}:${i + 1}: ${f.lines[i]?.trim().slice(0, 200) ?? ""}`))
      }
    }
    const capNote = capped ? "\n…（已达匹配上限，结果可能不完整；可缩小 pattern/path/include 范围）" : ""
    if (mode === "files") {
      const fl = fileCounts.map((c) => c.file)
      if (!fl.length) return { output: "（无匹配）", data: { mode, files: [], counts: [] } }
      return { ...(await truncate(fl.join("\n") + capNote, "grep", ctx)), data: { mode, files: fl, truncated: capped } }
    }
    if (mode === "count") {
      if (!fileCounts.length) return { output: "（无匹配）", data: { mode, counts: [], files: [] } }
      const sorted = [...fileCounts].sort((a, b) => b.count - a.count || a.file.localeCompare(b.file))
      return { ...(await truncate(sorted.map((c) => `${c.file}: ${c.count}`).join("\n") + capNote, "grep", ctx)), data: { mode, counts: sorted, truncated: capped } }
    }
    if (!matches.length) return { output: "（无匹配）", data: { mode, matches: [], files: [], counts: [] } }
    const truncated = await truncate(blocks.join("\n") + capNote, "grep", ctx)
    return { ...truncated, data: { mode, matches, truncated: capped } }
  },
}

/** 花括号展开（支持嵌套）：`*.{ts,tsx}` → [`*.ts`, `*.tsx`]；无花括号/不闭合原样返回。 */
function expandBraces(pattern: string): string[] {
  const start = pattern.indexOf("{")
  if (start < 0) return [pattern]
  let depth = 0
  let end = -1
  for (let i = start; i < pattern.length; i++) {
    if (pattern[i] === "{") depth++
    else if (pattern[i] === "}") {
      if (--depth === 0) {
        end = i
        break
      }
    }
  }
  if (end < 0) return [pattern]
  // 顶层逗号分割（嵌套花括号内的逗号不分割）
  const parts: string[] = []
  let d = 0
  let cur = ""
  for (const ch of pattern.slice(start + 1, end)) {
    if (ch === "{") d++
    else if (ch === "}") d--
    if (ch === "," && d === 0) {
      parts.push(cur)
      cur = ""
      continue
    }
    cur += ch
  }
  parts.push(cur)
  const out: string[] = []
  for (const p of parts) out.push(...expandBraces(pattern.slice(0, start) + p + pattern.slice(end + 1)))
  return out
}

/** glob 模式转正则：`*`/`**` → 任意字符（跨目录层级，递归查找），`?` → 单字符，`{a,b}` 花括号交替展开为多候选。 */
function globToRegExp(pattern: string): RegExp {
  const parts = expandBraces(pattern).map((v) => {
    let out = ""
    for (let i = 0; i < v.length; i++) {
      const c = v[i]
      if (c === "*") {
        out += ".*"
        if (v[i + 1] === "*") i++
      } else if (c === "?") out += "."
      else out += c.replace(/[.+^${}()|[\]\\]/g, "\\$&")
    }
    return out
  })
  return new RegExp(`^(?:${parts.join("|")})$`)
}

/** include/exclude 过滤器列表（逗号分隔多模式 + 各自花括号展开，任一命中即匹配）；空/未传返回 null。 */
function globFilters(v: unknown): RegExp[] | null {
  const s = v == null ? "" : String(v).trim()
  if (!s) return null
  // 顶层逗号分割（花括号内的逗号是交替项不分割），再各自花括号展开
  const pats: string[] = []
  let d = 0
  let cur = ""
  for (const ch of s) {
    if (ch === "{") d++
    else if (ch === "}") d = Math.max(0, d - 1)
    if (ch === "," && d === 0) {
      pats.push(cur)
      cur = ""
      continue
    }
    cur += ch
  }
  pats.push(cur)
  const out: RegExp[] = []
  for (const p of pats.map((x) => x.trim()).filter(Boolean)) out.push(...expandBraces(p).map(globToRegExp))
  return out.length ? out : null
}

/** glob 过滤匹配（include/exclude 共用）：候选相对路径整条或任一路径段（含文件名）命中任一模式即真
 *  ——含 `/` 的模式只能整条命中（锚定正则含 / 不可能匹配单段），无 `/` 的模式（如 dist、*.log）按段命中（同 rg --glob 语义）。 */
function globMatchAny(filters: RegExp[], candidates: string[]): boolean {
  return candidates.some((c) => filters.some((re) => re.test(c) || c.split("/").some((seg) => re.test(seg))))
}

/** 默认排除判定：路径任一段命中 WALK_SKIP_DIRS（node_modules/.git/dist 等大型目录）且 include/pattern 原文未显式点名该目录。 */
function isDefaultExcluded(candidates: string[], rawPattern: string): boolean {
  return candidates.some((c) => c.split("/").some((seg) => WALK_SKIP_DIRS.has(seg) && !rawPattern.includes(seg)))
}

export const globTool: Tool = {
  name: "glob",
  description:
    "按文件名模式（glob，如 *.ts、**/test/*.js、*.{ts,tsx}——花括号交替）在会话工作目录（tmp/）中递归查找文件，返回相对路径（带 tmp/ 前缀，可直接用于 read 等文件工具）。node_modules/.git/dist 等大型目录默认跳过（模式显式点名除外）。exclude 可排除路径模式（如 tests/**,*.md）。",
  card: { titleParams: ["pattern"] },
  parameters: schema(
    {
      pattern: { type: "string", description: "文件名 glob 模式（** 跨目录、* 任意、? 单字符、{a,b} 交替）" },
      path: { type: "string", description: "搜索起点（默认 .，相对会话工作目录，tmp/ 前缀可省略）" },
      exclude: { type: "string", description: "排除的路径 glob（逗号分隔多模式；与 grep exclude 同语法——无 / 的模式按目录/文件名匹配任意层级）" },
    },
    ["pattern"],
  ),
  outputSchema: schema({
    files: { type: "array", items: { type: "string" }, description: "匹配文件相对路径（最多 200 个）" },
    total: { type: "integer", description: "匹配总数（可能大于 files 长度）" },
  }, ["files", "total"]),
  async execute(args, ctx) {
    const rawPattern = String(args.pattern ?? "")
    const re = globToRegExp(rawPattern)
    const excludeRes = globFilters(args.exclude)
    const path = args.path ? String(args.path).replace(/\\/g, "/") : ""
    // path 统一经 resolvePath 解析（与 read/write/ls 一致：相对路径基于会话工作目录 tmp/，tmp/ 前缀可省略；
    // 项目上下文基于项目根），解析回基准相对逻辑路径后与列表坐标做前缀匹配（沙箱模式拒绝越界路径）
    let prefix = ""
    if (path) {
      const root = ctx.resolvePath(".")
      const rel = relative(root, ctx.resolvePath(path)).replace(/\\/g, "/")
      // listFiles 仅覆盖列表范围（会话 tmp/ 子树或项目根）：path 落在范围外（本地模式放开沙箱时可能）则无可列文件
      if (rel === ".." || rel.startsWith("../") || isAbsolute(rel)) {
        return { output: "（无匹配文件）" }
      }
      prefix = !rel || rel === "." ? "" : `${rel.replace(/\/+$/, "")}/`
    }
    const files = (await ctx.listFiles())
      .filter((f) => !f.isDir)
      .map((f) => f.path)
      .filter((p) => {
        const cs = listPathCandidates(p)
        return cs.some((c) => {
          const rel = prefix ? c.slice(prefix.length) : c
          return (prefix ? c.startsWith(prefix) : true) && re.test(rel)
        }) && !(excludeRes && globMatchAny(excludeRes, cs)) && !isDefaultExcluded(cs, rawPattern)
      })
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
    // 流式限量读取（先下载后截断防不住内存：超大/无限流响应在截断前就已全量入内存）
    const reader = res.body?.getReader()
    let text = ""
    if (reader) {
      const chunks: Uint8Array[] = []
      let received = 0
      let oversized = false
      const dec = new TextDecoder()
      for (;;) {
        const { done, value } = await reader.read()
        if (done) break
        if (value) {
          received += value.byteLength
          if (received > FETCH_URL_STREAM_MAX_BYTES) {
            oversized = true
            void reader.cancel().catch(() => {})
            break
          }
          chunks.push(value)
          text += dec.decode(value, { stream: true })
        }
      }
      text += dec.decode()
      if (oversized) text += `
…（响应超过 ${Math.round(FETCH_URL_STREAM_MAX_BYTES / 1024 / 1024)}MB 读取上限，已截断）`
    } else {
      text = await res.text()
    }
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
  guard: (url: string) => void | Promise<void>,
): Promise<Response> {
  let url = rawUrl
  for (let hop = 0; ; hop++) {
    await guard(url)
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
 * 整数/十六进制 IPv4 等绕过形式）；**域名做 DNS 解析复查**——字面量之外，「解析到私网/回环的域名」
 * （内网 DNS 名 kubernetes.default.svc、A 记录指向内网的攻击域名）同样拒绝，解析失败放行
 * （fetch 自会失败）；逐跳校验防重定向跳板（解析时刻与 fetch 时刻仍有窗口，纵深依赖出口网络一致性）。 */
export async function assertPublicHttpUrl(raw: string): Promise<void> {
  let url: URL
  try {
    url = new URL(raw)
  } catch {
    throw new Error(`无效的 URL: ${raw}`)
  }
  if (url.protocol !== "http:" && url.protocol !== "https:") throw new Error(`无效的 URL（仅支持 http/https）: ${raw}`)
  const hostname = url.hostname.replace(/\.$/, "") // 尾点 FQDN 归一（ip.ts 同款）
  const reason = hostBlockReason(hostname, { blockPrivate: true })
  if (reason === "私网地址") throw new Error(`URL 不允许（私网地址）: ${raw}`)
  if (reason) throw new Error(`URL 不允许（回环/链路本地地址）: ${raw}`)
  // DNS 解析复查（3s 超时放行——挂起的解析器不应额外阻断，fetch 层自有超时）
  let addrs: Array<{ address: string }>
  try {
    addrs = await Promise.race([
      lookup(hostname, { all: true }),
      new Promise<never>((_, reject) => setTimeout(() => reject(new Error("dns-timeout")), 3000)),
    ])
  } catch {
    return
  }
  for (const a of addrs) {
    const r = hostBlockReason(a.address, { blockPrivate: true })
    if (r) throw new Error(`URL 不允许（域名解析到${r === "私网地址" ? "私网" : "回环/链路本地"}地址 ${a.address}）: ${raw}`)
  }
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

/** show：非会话 tmp/ 文件复制进会话文件区的尺寸上限（超出提示改为告知路径，防大文件拖垮磁盘/传输）。 */
export const SHOW_MAX_BYTES = 100 * 1024 * 1024
/** show：文本直读展示的读取上限（字节，超出不再内联文本，仅给查看/下载卡片）。 */
export const SHOW_TEXT_DIRECT_BYTES = 512 * 1024
/** show：内联文本/代码块字符上限（超出截断展示 + 附 file 卡片取全文）。 */
export const SHOW_TEXT_MAX_CHARS = 40_000

/** show：可内联直显的文本/代码扩展名（命中则内容直接进 code 块展示）。 */
const SHOW_TEXT_EXT = new Set(
  "txt md markdown csv tsv json jsonl xml yaml yml log toml ini env properties conf cfg ts tsx js jsx mjs cjs py pyw rb go rs java kt kts c h cpp hpp cc cs php sh bash zsh bat cmd ps1 ps1m1 sql lua dart swift scala r pl vue svelte css scss less sass".split(
    " ",
  ),
)

/** show 文件归属处理：会话 tmp/ 内文件直接引用（零复制）；会话外（本地模式工作区/绝对路径）复制一份到
 * tmp/shown/（内容哈希命名，重复展示复用同一副本）后引用——前端文件接口只服务会话 tmp/，复制保证
 * 可见性与文件面板留存。归属判定用会话 tmp 真实绝对路径（项目绑定子Agent 的 resolvePath 基准是项目根，
 * 不能作判定依据）；复制目标写会话 tmp 绝对路径（同样绕开项目根基准，保证落在真实会话文件区）。 */
async function stageShowFile(
  ctx: ToolContext,
  rawPath: string,
  display: string,
): Promise<{ error: string } | { logical: string; abs: string; size: number; inSessionTmp: boolean; copiedBytes: Uint8Array | null }> {
  let abs: string
  try {
    abs = ctx.resolvePath(rawPath)
  } catch (err) {
    return { error: `show 失败：路径被拒绝（${err instanceof Error ? err.message : String(err)}）。请确认路径在允许范围内。` }
  }
  const { stat } = await import("node:fs/promises")
  let size: number
  try {
    const st = await stat(abs)
    if (!st.isFile()) return { error: `show 失败：${rawPath} 不是文件（目录请用 ls 列出后选择具体文件）。` }
    size = st.size
  } catch {
    return { error: `show 失败：文件不存在（${rawPath}）。请确认路径（可用 ls 查看）。` }
  }
  const sessionTmp = join(sessionPath(ctx.home, ctx.user, ctx.sessionId), "tmp")
  const inSessionTmp = abs === sessionTmp || abs.startsWith(sessionTmp + sep)
  // 非会话文件先复制（所有分支统一：内联展示的同时产物留存会话文件区，文件面板可见/可下载）
  let logical: string
  let copiedBytes: Uint8Array | null = null
  if (inSessionTmp) {
    logical = `tmp/${relative(sessionTmp, abs).split(sep).join("/")}`
  } else {
    if (size > SHOW_MAX_BYTES) {
      return { error: `show 失败：文件过大（${size} 字节，复制上限 ${SHOW_MAX_BYTES} 字节）。请改为在回复中告知文件路径。` }
    }
    if (!ctx.readBinaryFile || !ctx.writeBinaryFile) {
      return { error: "show 失败：当前环境不支持二进制复制（readBinaryFile/writeBinaryFile 未启用），仅支持会话 tmp/ 内文件。" }
    }
    try {
      copiedBytes = await ctx.readBinaryFile(abs)
    } catch (err) {
      return { error: `show 失败：无法读取 ${rawPath}（${err instanceof Error ? err.message : String(err)}）。` }
    }
    const base = display.replace(/\.[^.]+$/, "") || "file"
    const ext = display.match(/\.[^.]+$/)?.[0] ?? ""
    const hash = createHash("sha256").update(copiedBytes).digest("hex").slice(0, 8)
    logical = `tmp/shown/${base}-${hash}${ext}`
    // 复制目标写会话 tmp 绝对路径（项目绑定子Agent 的 resolvePath 基准是项目根，直接写会落错位置）
    await ctx.writeBinaryFile(join(sessionTmp, "shown", `${base}-${hash}${ext}`), copiedBytes)
  }
  return { logical, abs, size, inSessionTmp, copiedBytes }
}

/** show 图表分支渲染管线（code 源码与 path 图表文件共用）：PlantUML 自动包装+布局注入、ECharts JSON
 * 预校验、frontend 实时渲染验证闭环（渲染成功才返回成功）、backend 服务端 PNG、产物落盘 tmp/。
 * fromPath 非空 = path 模式（源文件已留存，文案注明来源）。 */
async function showDiagram(
  ctx: ToolContext,
  raw: string,
  format: DiagramFormat,
  base: string,
  fromPath: string,
  render: unknown,
): Promise<ToolResult> {
  const label = DIAGRAM_LABEL[format]
  const code = format === "plantuml" ? injectPlantUmlLayout(normalizePlantUml(raw)) : raw
  // 产物写入会话 tmp/（与 read/write/truncate 的 tmp/ 约定一致，UI 文件面板可见）
  const rel = `tmp/${base}.${DIAGRAM_EXT_FOR[format]}`
  // render=backend：服务端直接渲染 PNG 图片（不经前端/飞书通道），落盘 tmp/{name}.png 并返回 image 内容块（四语言均支持）
  if (render === "backend") {
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
      output: fromPath
        ? `图表已渲染为图片: ${pngRel}（源文件 ${fromPath}，${raw.length} 字符）`
        : `图表已生成并渲染为图片: ${pngRel}（${raw.length} 字符）`,
      blocks: [{ type: "image", path: pngRel, name: `${base}.png`, mime: "image/png" }],
    }
  }
  // echarts：服务端预校验 JSON（纯解析零渲染开销）——无效 JSON 立即精确报错，不白跑前端一轮；
  // 旧版前端会把未知语言当 PlantUML 渲染出误导性错误，预校验先拦掉真正的源码问题
  if (format === "echarts") {
    const { parseEchartsInput } = await import("./diagram-render")
    try {
      parseEchartsInput(code)
    } catch (err) {
      return { output: `画图失败：${err instanceof Error ? err.message : String(err)}` }
    }
  }
  // 无交互模式（REST 单次调用）无前端渲染通道：不空等 5 秒超时，直接引导后端渲染
  if (ctx.interactionMode === "none") {
    return { output: "画图能力受限：当前通道无前端渲染（无交互调用）。请改用 render=backend（服务端渲染 PNG 图片）后重试。" }
  }
  // 实时渲染（Web 前端按 format 本地渲染或飞书后端渲染，通道实现不同但结果一致）：成功才返回成功，渲染错误回传模型，5 秒超时判定画图能力受限
  const rendered = await ctx.waitForDraw({ code, name: base, format })
  if (rendered === null) {
    return { output: "画图能力受限：未能在 5 秒内完成渲染（渲染端离线或超时），请稍后重试，或用其他方式表达图表内容。" }
  }
  if (!rendered.ok) {
    // 报错引擎与请求语言不符（如请求 echarts 却由 PlantUML 引擎报错）= 前端为旧版本（旧代码把未知
    // 语言静默当 PlantUML 渲染）：明确诊断引导刷新页面——前端渲染是默认通道，不自动换通道旁路
    const engine = (rendered.error ?? "").match(/^(PlantUML|Mermaid|D2|ECharts) 渲染/)?.[1]
    if (engine && engine !== label) {
      return { output: `画图失败：前端渲染器版本过旧（请求 ${label} 图表却由 ${engine} 引擎渲染报错）。请提示用户刷新页面（加载新前端版本）后重试；确需本次立即出图可改用 render=backend。` }
    }
    return { output: `画图失败（渲染错误）：${rendered.error ?? "未知错误"}。请修正 ${label} 源码后重试。` }
  }
  await ctx.writeFile(ctx.resolvePath(rel), code)
  return {
    output: fromPath
      ? `图表已渲染成功: ${rel}（源文件 ${fromPath}，${raw.length} 字符）`
      : `图表已生成并渲染成功: ${rel}（${raw.length} 字符）`,
    blocks: [{ type: "diagram", format, code: raw, name: `${base}.${DIAGRAM_EXT_FOR[format]}`, version: 1 }],
  }
}

/** html 内容块构造：显式尺寸为正整数且不超上限时附加（其余值忽略，回退默认）。 */
function htmlBlock(html: string, name: string, width: unknown, height: unknown): ContentBlock {
  const block: Record<string, unknown> = { type: "html", html, name }
  const w = typeof width === "number" && Number.isFinite(width) ? Math.round(width) : undefined
  const h = typeof height === "number" && Number.isFinite(height) ? Math.round(height) : undefined
  if (w !== undefined && w > 0 && w <= RENDER_HTML_MAX_WIDTH) block.width = w
  if (h !== undefined && h > 0 && h <= RENDER_HTML_MAX_HEIGHT) block.height = h
  return block as ContentBlock
}

/** show HTML 分支（html 源码）：沙箱 iframe 域隔离预览（仅实时前端通道），产物落盘 tmp/ 并返回 html 内容块。 */
async function showHtml(ctx: ToolContext, html: string, base: string, width: unknown, height: unknown): Promise<ToolResult> {
  if (ctx.interactionMode && ctx.interactionMode !== "realtime") {
    return { output: "show 失败：当前通道不支持 HTML 页面预览（仅 Web 前端实时会话可用）。请改为产出 .html 文件（write）后用 path 交付文件，或在回复中描述内容。" }
  }
  // 产物写入会话 tmp/（与图表分支的 tmp/ 约定一致，UI 文件面板可见）
  const rel = `tmp/${base}.html`
  await ctx.writeFile(ctx.resolvePath(rel), html)
  return {
    output: `HTML 页面已生成并渲染: ${rel}（${html.length} 字符）`,
    blocks: [htmlBlock(html, `${base}.html`, width, height)],
  }
}

/**
 * show：把内容**直接展示给用户**——统一入口，内容源三选一（code / html / path），内容在消息流内联呈现：
 * - `code`+`format`（图表创作）：四语言实时渲染验证（成功才返回成功，失败返回错误供修正），
 *   `render=backend` 服务端渲染 PNG；产物落盘 tmp/ 并返回 diagram 块。
 * - `html`（HTML 页面）：沙箱 iframe 域隔离预览，返回 html 块；仅实时前端通道（分支内校验）。
 * - `path`（已有文件直显）：图片 → image 块；图表源文件 → 渲染验证 + diagram 块（与 code 同一管线）；
 *   `.html` → html 块；文本/代码 → code 块内联（超长截断 + 附 file 卡片）；其余 → file 卡片。
 */
export const showTool: Tool = {
  name: "show",
  description:
    "向用户展示内容（聊天界面内联呈现），内容源三选一：①图表——code 传源码 + format 指定语言（Mermaid 通用首选 / PlantUML 标准 UML 建模 / D2 美观架构图 / ECharts 数据图表，选型指南见 format 参数），前端实时渲染验证，渲染成功才返回成功、失败返回错误信息供修正；②HTML 页面——html 传源码，沙箱 iframe 域隔离预览，仅 Web 前端通道；③已有文件——path 传路径按类型直显：图片内联显示、图表源文件（.puml/.mmd/.d2/.echarts）渲染成图表、.html 页面预览、文本/代码内联展示、无法内联的类型（PDF/压缩包/Office/音视频等）给查看/下载卡片。产物保存到会话 tmp/ 并返回对应内容块。",
  card: { args: "block" },
  parameters: schema(
    {
      code: { type: "string", description: "图表源码（与 html/path 三选一，需同时传 format）。PlantUML 布局：流程类显式 `left to right direction` 或保持默认纵向，勿逐条连线硬控方向；关系紧密的节点用 `together { … }` 保持相邻；节点 ≤20 个，大图按层拆包。PlantUML 勿手动添加 @startuml/@enduml（自动补全）。ECharts：传 option 的严格 JSON（键名与字符串一律双引号，不支持单引号/裸键名/…省略号缩写；容错 //注释 与尾逗号），格式化用字符串模板如 \"{b}: {c}\"" },
      html: { type: "string", description: "HTML 页面源码（与 code/path 三选一；完整文档或片段均可，自动补全为完整页面）。沙箱 iframe 域隔离预览：脚本可执行但运行在隔离源内，无法访问宿主页面 DOM/存储/顶层导航。适合网页原型、数据报表、卡片/徽章、可视化组件、带交互脚本的小页面。样式用内联 CSS，图片可用 data: URI 或外部 URL，脚本内联或外部均可" },
      path: { type: "string", description: "已有文件路径（与 code/html 三选一），按类型直显：图片内联、图表源文件（.mmd/.puml/.plantuml/.d2/.echarts）渲染成图表、.html 页面预览、文本/代码内联、其余查看/下载卡片——适合交付产物或需要用户过目的文件。会话内路径（tmp/ 前缀可省略）；本地模式也可给工作区/绝对路径，不在会话文件区内的文件会复制一份（≤100MB）到会话文件区再展示；显式传 format 可按指定图表语言渲染任意文本文件" },
      name: { type: "string", description: "展示名/产物主名（不含扩展名；未传时图表默认 diagram、HTML 默认 page、path 模式默认取文件主名）" },
      format: {
        enum: ["mermaid", "plantuml", "d2", "echarts"],
        description:
          "图表语言（code 模式必选；path 模式可选，未传时按文件扩展名推断）：\n" +
          "【mermaid】流程图/时序图/状态图/甘特图/用户旅程、Markdown 文档嵌入、简单架构；语法最简。\n" +
          "【plantuml】类图/组件图/部署图/用例图/活动图/ER 图等标准 UML 与严谨建模，功能最全。\n" +
          "【d2】系统架构/云架构/网络拓扑/微服务等对外展示场景（PPT/汇报），默认布局最现代。\n" +
          "【echarts】柱状/折线/饼图/散点/雷达/仪表盘/热力图/地图等数据可视化与统计图表；code 传 option 的严格 JSON（键名与字符串一律双引号，值禁止函数），可选信封 {\"option\": {...}, \"width\": 960, \"height\": 600} 指定画布尺寸（默认 960×600）。\n" +
          "组合场景：设计文档=plantuml 类图/组件图 + mermaid 流程图；架构汇报=d2 全景架构图 + plantuml 详细组件图；数据分析=echarts 统计图表。",
      },
      render: { enum: ["frontend", "backend"], default: "frontend", description: "渲染通道（默认 frontend，首选前端渲染降低服务端负载）：frontend（浏览器本地渲染 SVG，可交互缩放、零服务端开销）/ backend（服务端渲染成 PNG 图片落盘 tmp/，仅导出/分享图片等确需 PNG 文件时使用，四语言均支持；前端渲染不可用（收到「画图能力受限」）时改用 backend 重试）" },
      width: { type: "number", description: "HTML 预览宽度（px，可选，默认铺满消息流宽度）" },
      height: { type: "number", description: "HTML 预览高度（px，可选，不传默认取会话区域高度的 2/3）" },
    },
    [],
  ),
  async execute(args, ctx) {
    const codeArg = args.code != null ? String(args.code) : ""
    const htmlArg = args.html != null ? String(args.html) : ""
    const pathArg = args.path != null ? String(args.path) : ""
    const formatArg = String(args.format ?? "")
    // format 校验前置：非法值立即报错而非静默回退 plantuml——实测复盘：漏传 format 时
    // ECharts JSON 被当 PlantUML 渲染，报「PlantUML 渲染错误」误导模型连续多轮失败
    if (formatArg && formatArg !== "mermaid" && formatArg !== "plantuml" && formatArg !== "d2" && formatArg !== "echarts") {
      return { output: `show 失败：format 参数无效（"${formatArg}"）。可选值：mermaid / plantuml / d2 / echarts。` }
    }
    if (!codeArg && !htmlArg && !pathArg.trim()) {
      return { output: "show 失败：缺少内容源——code（图表源码）/ html（HTML 源码）/ path（已有文件）三选一。" }
    }
    // ① 图表分支：code 源码（format 必选）
    if (codeArg) {
      if (!formatArg) return { output: "show 失败：code（图表源码）必须同时传 format（mermaid / plantuml / d2 / echarts）。" }
      const base = (args.name ? String(args.name) : "diagram").replace(DIAGRAM_EXT, "")
      return showDiagram(ctx, codeArg, formatArg as DiagramFormat, base, "", args.render)
    }
    // ② HTML 分支：html 源码（沙箱 iframe 预览，仅实时前端通道）
    if (htmlArg) {
      const base = (args.name ? String(args.name) : "page").replace(/\.html?$/, "")
      return showHtml(ctx, htmlArg, base, args.width, args.height)
    }
    // ③ 文件分支：path 按类型直显（归属处理 → 图片/图表/HTML/文本/文件卡片）
    const rawPath = pathArg.trim()
    const display = args.name ? String(args.name) : rawPath.replace(/[\\/]+$/, "").split(/[\\/]/).pop() || "file"
    const staged = await stageShowFile(ctx, rawPath, display)
    if ("error" in staged) return { output: staged.error }
    const { logical, abs, size, inSessionTmp, copiedBytes } = staged
    const readText = async (): Promise<string> => {
      if (copiedBytes) return new TextDecoder().decode(copiedBytes)
      return ctx.readFile(abs)
    }
    const copiedNote = inSessionTmp ? "" : `（源 ${rawPath}，已复制到会话文件区 ${logical}）`
    // path+format：按指定图表语言渲染该文件内容（不限扩展名，与 code 模式同一管线）
    if (formatArg) {
      return showDiagram(ctx, await readText(), formatArg as DiagramFormat, display.replace(/\.[^.]+$/, "") || "diagram", rawPath, args.render)
    }
    if (IMAGE_EXT.test(abs)) {
      return {
        output: `已向用户展示文件 ${display}（${logical}，${size} 字节，图片内联展示，点击可全屏查看）${copiedNote}。`,
        blocks: [{ type: "image", path: logical, name: display, mime: mimeFor(abs) }],
      }
    }
    if (DIAGRAM_EXT.test(abs)) {
      // 图表源文件：走渲染验证管线（渲染成功才返回成功；重新渲染/换通道 base 取文件主名）
      return showDiagram(ctx, await readText(), diagramFormatFor(abs) ?? "plantuml", display.replace(/\.[^.]+$/, "") || "diagram", rawPath, args.render)
    }
    if (/\.html?$/i.test(abs)) {
      if (ctx.interactionMode && ctx.interactionMode !== "realtime") {
        return { output: `show 失败：当前通道不支持 HTML 页面预览（仅 Web 前端实时会话可用）。文件本体已留存 ${logical}，可告知用户路径。` }
      }
      return {
        output: `已向用户展示文件 ${display}（${logical}，${size} 字符，HTML 页面预览展示）${copiedNote}。`,
        blocks: [htmlBlock(await readText(), display, args.width, args.height)],
      }
    }
    const blocks: ContentBlock[] = []
    let how = ""
    if (SHOW_TEXT_EXT.has(abs.split(".").pop()?.toLowerCase() ?? "") && size <= SHOW_TEXT_DIRECT_BYTES) {
      const text = await readText()
      if (text.length <= SHOW_TEXT_MAX_CHARS) {
        blocks.push({ type: "code", text, language: inferLang(display) || undefined, path: logical, name: display })
        how = `内容内联展示（${text.length} 字符）`
      } else {
        // 超长文本截断展示；全文经文件卡工具栏下载/「原文件」查看获取（files/content 按需加载），不再附独立 file 卡片
        blocks.push({
          type: "code",
          text: `${text.slice(0, SHOW_TEXT_MAX_CHARS)}\n…（文本过长已截断，全文 ${text.length} 字符可下载或点「原文件」查看）`,
          language: inferLang(display) || undefined,
          path: logical,
          name: display,
        })
        how = `内容截断内联展示（前 ${SHOW_TEXT_MAX_CHARS} 字符）`
      }
    } else {
      blocks.push({ type: "file", path: logical, name: display, mime: mimeFor(abs) })
      how = "该类型无法内联展示，已提供查看/下载卡片（点击时才加载内容）"
    }
    return { output: `已向用户展示文件 ${display}（${logical}，${size} 字节，${how}）${copiedNote}。`, blocks }
  }
}

/** 前端捕获 html 截断长度（WS 传输与落盘上限；完整 DOM 通常远超此，超出截取首部）。 */
export const PAGE_CAPTURE_HTML_LIMIT = 300 * 1024

export const pageCaptureTool: Tool = {
  name: "page_capture",
  // 仅实时前端可用（请求当前页面捕获并由前端回传），多轮交互/无交互模式禁用
  interaction: "realtime",
  description:
    "请求前端（当前浏览器页面）捕获实际渲染结果：读取渲染后的 DOM html 与页面截图，产物落盘会话 tmp/capture/。适合验证 Web UI 修改后的真实效果——页面即当前打开的 歌白界面（dev 模式修改后自动热更新，捕获前可提示用户刷新页面）；html 用 read 读取完整内容，截图用 vision 工具分析视觉效果。前端离线或 30 秒未响应时返回失败。",
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

// save_tool/delete_tool（HTML 小工具库）已下沉 widgets 子Agent：core/mini-tools.ts 提供存储实现，
// widgets_save/widgets_list/widgets_get/widgets_delete 以子Agent 命名空间暴露（与模型工具语义区分）
// draw/render_html/show_file 三工具已合并为 show（内容统一展示入口）：图表/HTML/文件三分支

/** 统计 needle 在 content 中的非重叠出现位置（起始索引），limit 后截断（防巨量匹配耗尽内存）。 */
function findOccurrences(content: string, needle: string, limit = 1000): number[] {
  const out: number[] = []
  let i = content.indexOf(needle)
  while (i >= 0 && out.length < limit) {
    out.push(i)
    i = content.indexOf(needle, i + needle.length)
  }
  return out
}

/** 索引位置对应的行号（1 起始）。 */
function lineOfIndex(content: string, index: number): number {
  let line = 1
  for (let i = 0; i < index; i++) if (content.charCodeAt(i) === 10 /* \n */) line++
  return line
}

/** 空白归一化（近似匹配提示用）：所有空白折叠为单空格并去首尾。 */
function collapseWhitespace(s: string): string {
  return s.replace(/\s+/g, " ").trim()
}

export const editTool: Tool = {
  name: "edit",
  description:
    "精确修改文件：基于 oldString → newString 定点替换，可一次多处，适合小范围改动；任一编辑项校验失败（不唯一/不匹配）则整体不落盘。目标文件已存在但本会话未 read 过时拒绝（防盲改，同 write 守卫；read/edit/patch/write 成功过的文件视为已读）。oldString 须从文件当前内容精确复制（不含 read 输出的行号前缀）。改动较多或行号容易偏移时改用 patch。修改前先 read 目标区域。",
  card: { titleParams: ["path"], args: "edits", codeField: "edits", file: "path" },
  parameters: schema(
    {
      path: { type: "string" },
      edits: {
        type: "array",
        items: {
          type: "object",
          properties: {
            oldString: { type: "string", description: "原文片段（非空，从文件当前内容精确复制，含缩进；须在文件中唯一，否则补充上下文或用 replaceAll）" },
            newString: { type: "string", description: "替换后的内容" },
            replaceAll: { type: "boolean", description: "true 时替换全部匹配（默认 false：多处匹配报错不落盘）" },
          },
          required: ["oldString", "newString"],
        },
      },
    },
    ["path", "edits"],
  ),
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    // 安全模式：修改限定用户目录内（降级而非禁用）
    const safeMsg = ctx.safeMode ? safeModeWriteCheck([path], ctx) : null
    if (safeMsg) return { output: safeMsg }
    const guardMsg = await ctx.writeGuard?.([path])
    if (guardMsg) return { output: guardMsg }
    await assertReadableSize(path, "edit", EDIT_MAX_FILE_BYTES)
    let content = await ctx.readFile(path)
    // BOM 感知：匹配用去 BOM 正文（BOM 会让首行 oldString 精确匹配失败），写回按原文件补回
    const hadBom = content.startsWith("\uFEFF")
    if (hadBom) content = content.slice(1)
    // 防盲改守卫（与 write 防盲覆盖同规则）：已存在但本会话未 read 过 → 拒绝，防凭记忆/假设内容盲改（oldString 匹配失败白跑一轮）
    if (ctx.fileGuard && !ctx.fileGuard.hasRead(path)) {
      return {
        output: `edit 拒绝：${args.path} 已存在，但本会话尚未读取过其内容（防盲改）。请先 read 该文件（或目标区域）确认当前内容后再 edit；read/edit/patch/write 成功过的文件视为已读。`,
      }
    }
    const edits = (args.edits as Array<{ oldString: string; newString: string; replaceAll?: boolean }>) || []
    const applied: string[] = []
    for (const [idx, e] of edits.entries()) {
      const oldString = String(e.oldString ?? "")
      const newString = String(e.newString ?? "")
      const nth = `第 ${idx + 1} 项`
      if (!oldString) {
        throw new Error(`修改失败: ${nth} oldString 为空（oldString 必须是文件中的非空原文片段；新建文件用 write）`)
      }
      const occ = findOccurrences(content, oldString)
      if (!occ.length) {
        // 空白近似提示：归一化后可命中 → 大概率是缩进/空白复制不精确；
        // 行号泄漏提示：oldString 携带 read 输出的「行号→制表符」前缀 → 去掉前缀后可精确命中
        const near = collapseWhitespace(content).includes(collapseWhitespace(oldString))
        const stripped = oldString.split("\n").map((l) => l.replace(/^\s*\d+\t/, "")).join("\n")
        const lineNoLeak = stripped !== oldString && findOccurrences(content, stripped).length > 0
        const hint = lineNoLeak
          ? "（检测到 oldString 携带 read 输出的行号前缀——请去掉每行行号后重试）"
          : near
            ? "（检测到空白/缩进不一致的近似原文——请 read 后从原文逐字符复制 oldString）"
            : "（请先 read 当前文件核对最新内容）"
        throw new Error(`修改失败: ${nth} oldString 未在文件中精确匹配: ${oldString.slice(0, 60)}${hint}`)
      }
      if (occ.length > 1 && e.replaceAll !== true) {
        const lines = occ.slice(0, 8).map((i) => lineOfIndex(content, i)).join("、")
        throw new Error(`修改失败: ${nth} oldString 匹配 ${occ.length} 处（行 ${lines}${occ.length > 8 ? "…" : ""}）——请扩大 oldString 上下文使其唯一，或确认全部替换时该项传 replaceAll: true`)
      }
      const lineNos = e.replaceAll === true ? occ : [occ[0]]
      // 行号在替换前的内容上计算（替换会移动后续文本位置）
      const shown = lineNos.slice(0, 8).map((i) => lineOfIndex(content, i))
      content = e.replaceAll === true ? content.split(oldString).join(newString) : content.replace(oldString, newString)
      applied.push(`${idx + 1}) 行 ${shown.join("、")}${lineNos.length > 8 ? `（共 ${lineNos.length} 处）` : ""}`)
    }
    await ctx.writeFile(path, (hadBom ? "\uFEFF" : "") + content)
    // 修改后内容即已掌握（模型无需重读验证），登记已读
    ctx.fileGuard?.markRead(path)
    // 产物块（与 read/write 同款）：修改后的文件内容卡（弹窗查看模式收敛为文件链接）
    const blocks = artifactBlocks(previewLogicalPath(path, ctx), content)
    return { output: `已对 ${args.path} 应用 ${edits.length} 处修改：${applied.join("；")}`, blocks }
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

/** patch 应用结果摘要（hunk 位置与净变化）。 */
function describeAppliedPatch(applied: Array<{ line: number; delta: number }>): string {
  const add = applied.reduce((s, a) => s + Math.max(0, a.delta), 0)
  const del = applied.reduce((s, a) => s + Math.max(0, -a.delta), 0)
  return `已应用 ${applied.length} 处 hunk（净 +${add}/-${del} 行，首处位于行 ${applied[0]?.line ?? 0}）`
}

export const patchTool: Tool = {
  name: "patch",
  description:
    "应用 unified diff 补丁（一次多 hunk，行号模糊容错）。patch 参数为 unified diff 文本（可基于 diff 工具输出构造）：@@ -旧起行,旧行数 +新起行,新行数 @@ 后接行内容——空格前缀=上下文行、-前缀=删除行、+前缀=新增行（如 @@ -2,1 +2,1 @@\\n-旧行\\n+新行）。**多文件补丁**：带 ---/+++ 文件头的段落按各文件头定位目标（a/、b/ 前缀自动剥离）逐文件应用；单文件补丁文件头可省略、以 path 参数定位（传了 path 时优先 path）。全部文件全部 hunk 校验通过才整体落盘（原子），任一不匹配整体失败不修改。目标文件已存在但本会话未 read 过时拒绝（防盲改，同 write/edit 守卫）。",
  card: { titleParams: ["path"], args: "code", codeField: "patch", codeLang: "diff", file: "path" },
  parameters: schema(
    {
      path: { type: "string", description: "目标文件路径（单文件补丁定位用；多文件补丁按文件头定位，可省略）" },
      patch: { type: "string", description: "unified diff 补丁文本" },
      dryRun: { type: "boolean", description: "true 时仅预演（校验并报告将应用的位置），不写入" },
    },
    ["patch"],
  ),
  async execute(args, ctx) {
    const sections = parsePatch(String(args.patch ?? ""))
    if (sections.length === 0) return { output: "patch: 补丁未解析到任何 hunk（请提供含 @@ 头的 unified diff 文本，格式见 diff 工具输出）" }
    const argPath = args.path ? String(args.path) : ""
    /** 文件头路径规范化：剥离 git 风格 a// b/ 前缀（/dev/null 与空值返回 undefined）。 */
    const headerPath = (p?: string): string | undefined => (p && p !== "/dev/null" ? p.replace(/^[ab]\//, "") : undefined)
    // 段落 → 目标路径：单文件补丁优先 path 参数（既有行为），多文件按各段文件头定位（无头段落回退 path）
    const targets: string[] = []
    for (const [i, s] of sections.entries()) {
      const header = headerPath(s.newPath) ?? headerPath(s.oldPath)
      const t = sections.length === 1 && argPath ? argPath : header ?? argPath
      if (!t) return { output: `patch: 第 ${i + 1} 段无 ---/+++ 文件头且未传 path 参数，无法定位目标文件（多文件补丁须带文件头）` }
      targets.push(t)
    }
    // 同一目标的多个段落合并按序应用（去重保持顺序）
    const order: string[] = []
    const byTarget = new Map<string, typeof sections>()
    for (const [i, t] of targets.entries()) {
      if (!byTarget.has(t)) {
        byTarget.set(t, [])
        order.push(t)
      }
      byTarget.get(t)!.push(sections[i])
    }
    const absList = order.map((t) => ctx.resolvePath(t))
    // 安全模式：修改限定用户目录内；写范围守卫：全部目标一次校验（任一命中整体拒绝）
    const safeMsg = ctx.safeMode ? safeModeWriteCheck(absList, ctx) : null
    if (safeMsg) return { output: safeMsg }
    const guardMsg = await ctx.writeGuard?.(absList)
    if (guardMsg) return { output: guardMsg }
    // 预检 + 应用（内存中逐文件完成，全部通过才落盘——跨文件原子）
    const planned: Array<{ target: string; abs: string; result: string; applied: AppliedHunk[]; bom: boolean }> = []
    for (const [ti, target] of order.entries()) {
      const abs = absList[ti]
      const parts = byTarget.get(target)!
      if (parts.reduce((s, p) => s + p.hunks.length, 0) > PATCH_MAX_HUNKS) {
        return { output: `patch: ${target} 的 hunk 数超上限（> ${PATCH_MAX_HUNKS}），请拆分补丁` }
      }
      let content = ""
      let exists = true
      try {
        content = await ctx.readFile(abs)
      } catch {
        exists = false
      }
      // BOM 感知：匹配用去 BOM 正文（同 edit），写回按原文件补回
      const bom = exists && content.startsWith("\uFEFF")
      if (bom) content = content.slice(1)
      if (exists && content.length > PATCH_MAX_FILE_BYTES) {
        return { output: `patch: 文件过大（${target} ${content.length} 字符，上限 ${PATCH_MAX_FILE_BYTES}），请改用 edit 分段修改` }
      }
      // 防盲改守卫（与 write/edit 同规则）：已存在但未读过 → 拒绝，防凭记忆构造补丁盲改
      if (exists && ctx.fileGuard && !ctx.fileGuard.hasRead(abs)) {
        return { output: `patch 拒绝：${target} 已存在，但本会话尚未读取过其内容（防盲改）。请先 read 该文件确认当前内容后再打补丁；read/edit/patch/write 成功过的文件视为已读。` }
      }
      let applied: AppliedHunk[] = []
      for (const [pi, part] of parts.entries()) {
        const r = applyPatch(content, part)
        if (!r.ok) {
          return {
            output: `patch: ${order.length > 1 ? `${target} ` : ""}第 ${r.hunkIndex + 1} 处 hunk 未匹配：${r.error}（请先 read 当前文件内容核对，或改用 edit 定点替换；dryRun=true 可预演）${parts.length > 1 ? `（${target} 第 ${pi + 1} 段）` : ""}`,
          }
        }
        content = r.result
        applied = applied.concat(r.applied)
      }
      planned.push({ target, abs, result: content, applied, bom })
    }
    if (args.dryRun === true) {
      const lines = planned.map((p) => `${p.target}：${describeAppliedPatch(p.applied)}`)
      return { output: `patch 预演通过（${planned.length} 个文件，dryRun，未写入）：\n${lines.join("\n")}` }
    }
    // 落盘 + 登记已读 + 产物块
    const blocks: ContentBlock[] = []
    for (const p of planned) {
      await ctx.writeFile(p.abs, (p.bom ? "\uFEFF" : "") + p.result)
      ctx.fileGuard?.markRead(p.abs)
      blocks.push(...artifactBlocks(previewLogicalPath(p.abs, ctx), p.result))
    }
    if (planned.length === 1) {
      const p = planned[0]
      return { output: `patch 已写入 ${p.target}：${describeAppliedPatch(p.applied)}`, blocks }
    }
    const lines = planned.map((p) => `- ${p.target}：${describeAppliedPatch(p.applied)}`)
    return { output: `patch 已写入 ${planned.length} 个文件：\n${lines.join("\n")}`, blocks }
  },
}

/** git 只读工具：log 条数默认与上限。 */
const GIT_DEFAULT_LOG = 10
const GIT_MAX_LOG = 50

/** git 命令内嵌参数（ref/path）安全字符校验：拒绝 shell/cmd 元字符（引号拼接注入、& | 管道、% 变量展开、^ 转义等）。 */
function safeGitArg(v: string): boolean {
  return v.trim() !== "" && !/["&|<>^%`\r\n;]/.test(v)
}

/** git grep pattern 安全校验：双引号内 cmd/shell 活动元字符黑名单（`"` 断引号、`%` 变量展开、`$`/反引号 sh 展开）；
 *  `&`/`|`/`<`/`>` 在双引号内为字面量**放行**——正则交替 `a|b` 等语法需要。 */
function safeGitPattern(v: string): boolean {
  return v.trim() !== "" && !/["%$`\r\n]/.test(v)
}

export const gitTool: Tool = {
  name: "git",
  description:
    "只读 Git 检查（status/diff/log/show/branch/ls-files/grep 七操作，不修改仓库；各操作说明见 action 参数）。写操作（add/commit 等）请用 sh。",
  card: { args: "none" },
  parameters: schema(
    {
      action: { type: "string", enum: ["status", "diff", "log", "show", "branch", "ls-files", "grep"], description: "status 工作区状态 / diff 变更内容 / log 提交历史 / show 查看某提交或文件的完整内容（ref 默认 HEAD）/ branch 本地与远程分支列表 / ls-files 已跟踪文件清单（自动尊重 .gitignore，摸底项目结构快于 glob）/ grep 在**已跟踪文件**中内容搜索（自动尊重 .gitignore——grep 工具不读 .gitignore 的补口；basic 正则语法；未 add 的新文件不在结果）" },
      dir: { type: "string", description: "Git 仓库目录（默认会话工作目录）" },
      staged: { type: "boolean", description: "diff 是否查看暂存区（--staged），默认否" },
      maxEntries: { type: "integer", description: "log 条数（默认 10，上限 50）" },
      ref: { type: "string", description: "show 的目标：提交哈希/分支/tag/HEAD~n 等（默认 HEAD）" },
      path: { type: "string", description: "ls-files 的路径过滤（前缀或 glob，如 src/、*.test.ts）；grep 的搜索范围限定（可选）" },
      pattern: { type: "string", description: "grep 的搜索模式（basic 正则，如 foo\\.bar、error|warn）" },
    },
    ["action"],
  ),
  outputSchema: schema({
    action: { type: "string", enum: ["status", "diff", "log", "show", "branch", "ls-files", "grep"] },
    branch: { type: "string", description: "当前分支（仅 status）" },
    ahead: { type: "integer", description: "领先远端提交数（仅 status，无则省略）" },
    behind: { type: "integer", description: "落后远端提交数（仅 status，无则省略）" },
    changes: { type: "array", description: "变更文件（仅 status）", items: schema({ status: { type: "string", description: "git 状态码（如 M/A/??）" }, path: { type: "string" } }, ["status", "path"]) },
    commits: { type: "array", description: "提交历史（仅 log）", items: schema({ hash: { type: "string" }, subject: { type: "string" } }, ["hash", "subject"]) },
    files: { type: "array", description: "文件清单（仅 ls-files）", items: { type: "string" } },
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
    } else if (action === "show") {
      const ref = args.ref ? String(args.ref) : "HEAD"
      if (!safeGitArg(ref)) return { output: `git: 非法 ref（含命令元字符）: ${ref}` }
      cmd = `git show --no-color "${ref}"`
    } else if (action === "branch") {
      cmd = "git branch -a -v --no-color"
    } else if (action === "ls-files") {
      const path = args.path ? String(args.path) : ""
      if (path && !safeGitArg(path)) return { output: `git: 非法 path（含命令元字符）: ${path}` }
      cmd = path ? `git ls-files -- "${path}"` : "git ls-files"
    } else if (action === "grep") {
      const pattern = args.pattern ? String(args.pattern) : ""
      if (!pattern.trim()) return { output: "git: grep 需要 pattern（basic 正则搜索模式）。" }
      if (!safeGitPattern(pattern)) return { output: `git: 非法 pattern（含引号内活动元字符 " % $ 反引号——请改写模式或用 grep 工具）: ${pattern.slice(0, 60)}` }
      const path = args.path ? String(args.path) : ""
      if (path && !safeGitArg(path)) return { output: `git: 非法 path（含命令元字符）: ${path}` }
      cmd = `git grep -n -I --no-color -e "${pattern}"${path ? ` -- "${path}"` : ""}`
    } else return { output: `git: 未知操作: ${action}（status/diff/log/show/branch/ls-files/grep）` }
    const { stdout, stderr, code } = await ctx.runCommand(cmd, { workdir: dir })
    if (code !== 0) return { output: `git ${action} 失败（exit ${code}，目录 ${args.dir || "."} 可能不是 Git 仓库）:\n${stderr || stdout}` }
    if (!stdout.trim()) {
      const empty: Record<string, string> = { diff: "（工作区无变更）", status: "（工作区干净）", log: "（无提交记录）", show: "（无内容）", branch: "（无分支）", "ls-files": "（无跟踪文件）", grep: "（无匹配）" }
      const emptyData: Record<string, Record<string, unknown>> = { status: { changes: [] }, log: { commits: [] }, "ls-files": { files: [] } }
      return { output: empty[action] ?? "（无输出）", data: { action, ...(emptyData[action] ?? {}) } }
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
    if (action === "ls-files") {
      const files = stdout.split("\n").map((l) => l.trim()).filter(Boolean)
      return { ...(await truncate(files.join("\n"), "git", ctx)), data: { action, files: files.slice(0, 2000) } }
    }
    return { ...(await truncate(stdout, "git", ctx)), data: { action } }
  },
}

/** 脚本执行超时参数（秒）：默认 300（5 分钟，与引擎脚本超时一致），上限 540（即引擎 9 分钟工具兜底值，脚本级超时不会晚于引擎兜底触发）。 */
const SCRIPT_TIMEOUT_DEFAULT_S = 300
const SCRIPT_TIMEOUT_MAX_S = 540

/** 脚本超时参数解析（秒 → 毫秒）：非正数/非法回退默认值，超上限截断。（js 脚本工具复用，导出） */
export function scriptTimeoutMs(v: unknown): number {
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

/** sh/py 免审参数（approval:false 跳过本次审批）的动态审批判定：缺省/true 需审批；显式 false 时
 *  **强制白名单校验**——仅只读（validateShCommandSafeMode）或测试/静态检查类命令（shApprovalFreeAllowed）
 *  放行免审，其余仍需审批（防提示词注入借免审标记执行任意命令）；py 的 code 为任意代码、无法静态判定，
 *  免审标记不生效。 */
function scriptRequiresApproval(args: Record<string, unknown>, ctx?: ToolContext): boolean {
  if (args.approval !== false) return true
  // 安全模式：sh 在 execute 内按只读白名单降级（非白名单命令直接被拒并回提示），风险已由白名单
  // 约束——审批层不再重复拦截，免审标记直接生效（降级语义与审批语义分层）
  if (ctx?.safeMode) return false
  if (ctx && typeof args.command === "string" && shApprovalFreeAllowed(args.command, { sandboxed: ctx.sandboxed, home: ctx.home, user: ctx.user, workdir: ctx.workdir })) return false
  return true
}

const SCRIPT_APPROVAL_PARAM = {
  approval: { type: "boolean", description: "可选：本次调用是否需要用户审批（默认 true 需审批）；仅对明确安全的只读命令（cat/ls/git status 等）或测试/静态检查类（bun test、pytest、tsc、eslint 等）可设 false 跳过审批（服务端强制白名单校验，不满足仍会弹审批）；风险命令勿关闭" },
}

/** 脚本 stdin 序列化：对象/数组转 JSON 文本（双引号，Python json.loads 可直接解析），其余按字符串。 */
function scriptInput(v: unknown): string | undefined {
  if (v == null) return undefined
  if (typeof v === "object") return JSON.stringify(v)
  return String(v)
}

export const shTool: Tool = {
  name: "sh",
  description: "执行 Shell 命令，输出以 stdout 为准。Windows 下经 cmd.exe 执行：命令串联用 &&/||/换行（; 非分隔符会被并入参数），引号语义以 cmd 为准；指定工作目录用 workdir 参数（免 cd X && cmd 串联）。安全模式下降级为只读命令白名单（cat/grep/find/git 读类等），输出重定向限定用户目录内。长耗时命令（构建/测试/安装等）可传 async:true 后台执行——立即返回 taskId，先做其他事再用 sh_task 回头查询/等待/终止。",
  requiresApproval: scriptRequiresApproval,
  card: { args: "code", codeField: "command", codeLang: "bash" },
  parameters: schema(
    {
      command: { type: "string" },
      workdir: { type: "string", description: "可选：命令工作目录（相对路径基于会话工作目录/项目根解析，绝对路径本地模式可用）——替代 cd X && cmd 串联（Windows cmd 下引号语义更稳），不传用默认工作目录" },
      input: { type: "string", description: "可选：作为命令 stdin 的输入数据" },
      timeout: { type: "number", description: "可选：执行超时秒数（同步默认 300、上限 540，超时进程被终止并返回超时结果；async:true 时为任务生命周期上限，默认 1800、上限 3600）" },
      strict: { type: "boolean", description: "可选：true 时退出码非 0 抛工具级错误（flow 编排「非 0 即中断」；配合 optional 容错）；默认 false 非 0 退出作为正常结果返回" },
      async: { type: "boolean", description: "可选：true 后台异步执行——立即返回 taskId 不等待完成（适合构建/测试等长命令，期间可处理其他任务）；后续用 sh_task（action=status/wait/kill/list）查询输出、等待完成或终止" },
      ...SCRIPT_APPROVAL_PARAM,
    },
    ["command"],
  ),
  outputSchema: scriptOutputSchema,
  async execute(args, ctx) {
    const input = scriptInput(args.input)
    const workdir = args.workdir ? ctx.resolvePath(String(args.workdir)) : ctx.workdir
    // 安全模式：只读命令白名单 + 输出重定向限用户目录（降级而非禁用；解析 fail-closed）
    if (ctx.safeMode) {
      const deny = validateShCommandSafeMode(String(args.command), ctx)
      if (deny) return { output: deny }
    }
    // 异步后台执行（DESIGN「sh 异步执行」）：spawn 进后台 + 落盘会话 tmp/sh-tasks/，立即返回 taskId
    if (args.async === true) {
      if (!ctx.shTasks) return { output: "当前环境不支持后台任务执行（shTasks 服务未注入）。" }
      const rec = await ctx.shTasks.start(String(args.command), { cwd: workdir, env: ctx.env, input, maxMs: shTaskLifetimeMs(args.timeout) })
      return {
        output: `[后台任务已启动] taskId: ${rec.id}\n命令: ${args.command}\n（后台执行中不阻塞会话——可先处理其他任务，之后用 sh_task action=status id=${rec.id} 查询输出，action=wait 阻塞等待完成，action=kill 终止；输出日志 tmp/sh-tasks/${rec.id}.log）`,
        data: { taskId: rec.id, pid: rec.pid },
      }
    }
    const { stdout, stderr, code } = await ctx.runCommand(String(args.command), { workdir, env: ctx.env, input, timeoutMs: scriptTimeoutMs(args.timeout) })
    // strict：非 0 退出码转工具级异常（flow 中未声明 optional 时中断整个编排，声明则容错继续）
    if (args.strict === true && code !== 0) {
      throw new Error(`命令执行失败（exit ${code}）${stderr ? `：\n${stderr.slice(0, 2000)}` : ""}`)
    }
    const out = code === 0 ? stdout : `${stdout}\n${stderr}\n[exit ${code}]`
    // 成功但无输出：明确提示（区分「命令成功无输出」与「输出捕获失败/静默吞掉」）
    const final = code === 0 && !stdout.trim() ? "（命令执行成功，无输出）" : out
    return { ...(await truncate(final, "sh", ctx)), data: scriptData(stdout, stderr, code) }
  },
}

/** sh_task 输出尾部默认/上限（字符）：后台任务输出可能持续增长，status/wait 仅取尾部。 */
const SH_TASK_TAIL_DEFAULT = 4000
const SH_TASK_TAIL_MAX = 20000
/** wait 默认等待秒数（上限对齐脚本超时上限 540，保证不晚于引擎 9 分钟兜底）。 */
const SH_TASK_WAIT_DEFAULT_S = 60
const SH_TASK_WAIT_MAX_S = 540

function shTaskTailChars(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SH_TASK_TAIL_DEFAULT
  return Math.min(Math.floor(n), SH_TASK_TAIL_MAX)
}

function shTaskWaitMs(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n) || n <= 0) return SH_TASK_WAIT_DEFAULT_S * 1000
  return Math.min(n, SH_TASK_WAIT_MAX_S) * 1000
}

function shTaskElapsed(r: { startedAt: number; endedAt?: number }, now = Date.now()): number {
  return Math.round(((r.endedAt ?? now) - r.startedAt) / 1000)
}

/** sh 异步后台任务状态行（status/wait/kill 单任务 + list 每行共用）。 */
function shTaskLine(r: ShTaskRecord, label?: string): string {
  const status = shTaskStatus(r)
  const head = `${label ?? ""}taskId ${r.id} [${status}] ${shTaskElapsed(r)}s`
  if (status === "running") return `${head} — ${r.command}`
  const exit = r.exitCode === undefined ? "（退出码未知）" : `（exit ${r.exitCode}）`
  const suffix = r.timedOut ? " [生命周期超时已终止]" : r.killed ? " [已手动终止]" : r.lost ? " [进程已结束，服务可能重启过]" : r.spawnError ? ` [启动失败: ${r.spawnError.slice(0, 200)}]` : ""
  return `${head}${exit}${suffix} — ${r.command}`
}

/** sh 异步后台任务管理：查询（status）/等待（wait，阻塞至完成或超时）/终止（kill，进程树）/清单（list）。
 *  任务由 sh async:true 启动（返回 taskId）；输出为 stdout+stderr 合并日志尾部（完整日志在 tmp/sh-tasks/{id}.log）。 */
export const shTaskTool: Tool = {
  name: "sh_task",
  description: "管理 sh 异步后台任务（sh async:true 启动并返回 taskId）。action=status 立即返回任务状态与输出尾部；action=wait 阻塞等待完成（timeout 秒内未完成返回当前状态，适合「先做别的再回头等结果」）；action=kill 终止任务进程树；action=list 列出本会话全部后台任务。",
  card: { titleParams: ["action", "id"] },
  parameters: schema(
    {
      action: { type: "string", enum: ["status", "wait", "kill", "list"], description: "操作（必填）" },
      id: { type: "string", description: "任务 taskId（action=list 可省略）" },
      timeout: { type: "number", description: "wait 操作等待秒数（默认 60，上限 540）" },
      tail: { type: "number", description: "返回输出尾部字符数（默认 4000，上限 20000）" },
    },
    ["action"],
  ),
  outputSchema: schema(
    {
      id: { type: "string", description: "任务 id（list 为空）" },
      status: { type: "string", description: "running/done/failed/killed/timed_out/lost" },
      exitCode: { type: "integer", description: "退出码（未知为 null）" },
      output: { type: "string", description: "输出尾部（stdout+stderr 合并）" },
      tasks: { type: "array", description: "list 的任务概要", items: schema({ id: { type: "string" }, status: { type: "string" }, command: { type: "string" } }, ["id", "status"]) },
    },
    [],
  ),
  async execute(args, ctx) {
    if (!ctx.shTasks) return { output: "当前环境不支持后台任务（shTasks 服务未注入）。" }
    const action = String(args.action ?? "status")
    const tail = shTaskTailChars(args.tail)
    if (action === "list") {
      const tasks = await ctx.shTasks.list()
      if (!tasks.length) return { output: "本会话暂无后台任务（用 sh async:true 启动）。", data: { tasks: [] } }
      const lines = tasks.map((r) => shTaskLine(r))
      return { output: `本会话后台任务（${tasks.length} 个，按启动顺序）:\n${lines.join("\n")}`, data: { tasks: tasks.map((r) => ({ id: r.id, status: shTaskStatus(r), command: r.command })) } }
    }
    const id = String(args.id ?? "")
    if (!id) return { output: "缺少任务 id（status/wait/kill 需要传 sh async:true 返回的 taskId；列清单用 action=list）。" }
    let rec = action === "wait" ? await ctx.shTasks.wait(id, shTaskWaitMs(args.timeout)) : action === "kill" ? await ctx.shTasks.kill(id) : await ctx.shTasks.refresh(id)
    if (!rec) return { output: `未找到后台任务: ${id}（taskId 以 sh async:true 的返回为准；查现有任务用 action=list）。` }
    if (action === "wait" && !rec.endedAt) {
      // 等待超时仍在运行：返回当前状态与已有输出，模型可再次 wait 或继续其他工作
      const out = await ctx.shTasks.readLog(id, tail)
      const text = `${shTaskLine(rec, "")}\n（等待超时仍在运行；可再次 wait、用 status 查询，或 kill 终止）\n已产出输出（尾部 ${Math.min(out.length, tail)} 字符）:\n${out || "（暂无输出）"}`
      return { ...(await truncate(text, "sh_task", ctx)), data: { id, status: shTaskStatus(rec), exitCode: null, output: out } }
    }
    const out = await ctx.shTasks.readLog(id, tail)
    const text = `${shTaskLine(rec)}${out ? `\n输出（尾部 ${Math.min(out.length, tail)} 字符，完整日志 tmp/sh-tasks/${id}.log）:\n${out}` : "\n（无输出）"}`
    return { ...(await truncate(text, "sh_task", ctx)), data: { id, status: shTaskStatus(rec), exitCode: rec.exitCode ?? null, output: out } }
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
  description: "执行 Python 代码（经临时文件），stdout 为输出。安全模式下审计钩子屏蔽写文件/进程/网络（仅保留文件读取）。",
  requiresApproval: scriptRequiresApproval,
  card: { args: "code", codeField: "code", codeLang: "python" },
  parameters: schema(
    {
      code: { type: "string", description: "Python 程序源码" },
      input: { type: "string", description: "可选：作为程序 stdin 的输入数据" },
      timeout: { type: "number", description: "可选：执行超时秒数（默认 300，上限 540；超时进程被终止并返回超时结果）" },
      strict: { type: "boolean", description: "可选：true 时退出码非 0 抛工具级错误（flow 编排「非 0 即中断」；配合 optional 容错）；默认 false 非 0 退出作为正常结果返回" },
      approval: { type: "boolean", description: "兼容参数：py 的 code 为任意代码、无法静态判定安全性，免审标记不生效（默认且恒需审批）" },
    },
    ["code"],
  ),
  outputSchema: scriptOutputSchema,
  async execute(args, ctx) {
    const code = String(args.code ?? "")
    const input = scriptInput(args.input)
    // 代码写临时文件执行：stdin 留给管道数据（原实现 code 走 stdin，无法同时传输入）；
    // 安全模式：前置审计钩子引导段（sys.addaudithook 拦写模式 open 与进程/网络/文件变更系统调用，仅保留文件读取）
    const finalCode = ctx.safeMode ? `${PY_SAFE_BOOTSTRAP}\n${code}` : code
    const { writeFile, rm } = await import("node:fs/promises")
    const scriptPath = `${ctx.workdir}/.gebai_py_${randomUUID().replace(/-/g, "")}.py`
    await writeFile(scriptPath, finalCode)
    try {
      // -X utf8 / PYTHONUTF8=1：强制 UTF-8 输出（Windows 默认 GBK 会造成乱码）
      const py = await resolvePythonCmd(ctx)
      const { stdout, stderr, code: exit } = await ctx.runCommand(`${py} -X utf8 "${scriptPath}"`, { workdir: ctx.workdir, env: { ...ctx.env, PYTHONUTF8: "1" }, input, timeoutMs: scriptTimeoutMs(args.timeout) })
      // strict：非 0 退出码转工具级异常（flow 中未声明 optional 时中断整个编排，声明则容错继续）
      if (args.strict === true && exit !== 0) {
        throw new Error(`程序执行失败（exit ${exit}）${stderr ? `：\n${stderr.slice(0, 2000)}` : ""}`)
      }
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
    "读取用户提交的反馈（本用户，按时间倒序）。用于自我优化等场景了解用户对既往输出的评价（点赞/点踩/文字反馈/建议）与改进点。",
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
    "探测当前运行环境：平台/架构、PATH（去重）、关键工具链版本（node/bun/python/git/cargo/rustc/go/docker 等，缺失标记不可用）、Windows 下 VS Build Tools（MSVC，cargo/rustc 依赖）与 WebView2 运行时状态。用于一次判断工具链可用性、指导安装缺失组件，避免逐命令探测。",
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
      "待办管理（统一入口）：entries 为操作列表，每项 op=add/update/delete。省略 entries 或传空数组 = 查询（清单含 id）。返回操作摘要与当前全部待办状态。",
    parameters: schema({
      entries: {
        type: "array",
        description: "操作列表（空/省略=查询）。每项：{ op: add|update|delete, 及对应字段 }",
        items: {
          type: "object",
          properties: {
            op: { enum: ["add", "update", "delete"], description: "操作类型" },
            title: { type: "string", description: "add：标题；update/delete：定位目标（无 id 时按标题精确或唯一包含匹配，多条同名用 id；update 有 id 时作为新标题）" },
            id: { type: "string", description: "update/delete：待办 id（清单返回，优先于标题定位）" },
            status: { enum: ["pending", "in_progress", "completed", "failed", "cancelled"], description: "update：目标状态" },
            progress: { type: "number", description: "update：目标进度（0-100）" },
            priority: { enum: ["low", "medium", "high"], description: "add：优先级" },
            note: { type: "string", description: "add：备注" },
            eta: { type: "number", description: "add：预计耗时（分钟）" },
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

/** 规范化 ask 选项询问分支的选项：纯文本 → { title }，复杂选项原样。 */
function normalizeChoiceOption(o: unknown): ChoiceOption {
  if (o && typeof o === "object") {
    const t = String((o as { title?: unknown }).title ?? "")
    if (t) return { title: t, description: (o as { description?: unknown }).description != null ? String((o as { description?: unknown }).description) : undefined }
  }
  return String(o ?? "")
}

/** 计划文档存储目录（会话 tmp/plans/，与其它文件工具同一路径基准，随会话文件面板可见）。 */
export const PLAN_DIR = "plans"

/** 计划文件名清洗：仅保留字母/数字/下划线/连字符，其余替换为 `-`，空标题回退 `plan`（直接作文件名，防路径穿越）。 */
export function planFileName(title: string): string {
  const slug = String(title ?? "")
    .replace(/[^\p{L}\p{N}_-]+/gu, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 60)
  return `${slug || "plan"}.md`
}

/** 组装计划 Markdown：content 提供时原样使用，否则由 title + steps 拼装（勾选清单；前端展示与落盘同构，双端同规则）。 */
export function buildPlanMarkdown(title: string, steps: string[], content?: string): string {
  const body = content && String(content).trim() ? String(content).trim() : ["# " + title, "", "## 执行计划", ""].concat(steps.map((s) => `- [ ] ${s}`)).join("\n")
  return body
}

/**
 * ask：向用户询问并**阻塞等待回应**——统一入口（原 ask_user/ask_env/plan 三工具合并），分支按专属参数三选一：
 * - 选项询问（options+prompt，原 ask_user）：用户点选/自定义文本/拒绝；multi=true 多选。
 * - 环境变量填值（name，原 ask_env）：前端弹窗填值后注入本次任务环境。
 * - 计划审批（title+steps/content，原 plan）：计划写入会话 tmp/plans/ 展示全文，批准/拒绝（附意见）。
 * 结果文案前缀（「用户选择：」「计划已批准」等）与「请审核计划」prompt 前缀是前端卡片识别契约，勿改。
 */
export const askTool: Tool = {
  name: "ask",
  description:
    "向用户询问并**阻塞等待回应**（统一入口，按参数三选一）：①选项询问——prompt + options（multi=true 可多选），用户点选/输入自定义文本/拒绝，结果返回后据此继续；适合方案确认、方向决策。②环境变量填值——name（+description 用途说明、secret 敏感掩码），前端弹窗填值后注入本次任务环境并保存浏览器本地；适合工具缺少必需凭证（API 密钥/Token 等）时向用户索取。③计划审批——title + steps（或 content 完整 Markdown），计划写入会话文件并在聊天界面展示全文，批准后严格按计划执行、拒绝可附修改意见修订重提；适合多步骤、有风险、需用户把关的任务（简单任务用 todo 跟踪即可）。",
  card: { args: "none" },
  parameters: schema(
    {
      prompt: { type: "string", description: "选项询问的问题文本（与 options 搭配，用户按此作答）" },
      options: {
        type: "array",
        description: "选项询问的选项清单（触发选项分支）：每项可为纯文本字符串，或复杂选项 { title, description }（UI 按标题+说明展示，返回值为 title）",
        items: {
          anyOf: [
            { type: "string" },
            { type: "object", properties: { title: { type: "string" }, description: { type: "string" } }, required: ["title"] },
          ],
        },
      },
      multi: { type: "boolean", description: "选项询问是否允许多选（默认单选）" },
      name: { type: "string", description: "环境变量填值分支（触发填值分支）：要请求的环境变量名（如 FEISHU_DOCS_APP_ID，仅限字母/数字/下划线）" },
      description: { type: "string", description: "（填值分支）变量用途说明（展示给用户，帮助其填写正确的值）" },
      secret: { type: "boolean", description: "（填值分支）是否敏感值（密钥/Token 等，输入框掩码显示，默认 false）" },
      title: { type: "string", description: "计划审批分支（触发计划分支）：计划标题（简明概括任务目标，如「重构订单模块」）" },
      steps: { type: "array", items: { type: "string" }, description: "（计划分支）执行步骤清单（按顺序，每步一句可执行动作；与 content 二选一）" },
      content: { type: "string", description: "（计划分支）可选：完整计划 Markdown 正文（提供时覆盖 steps 的自动拼装，用于复杂嵌套/表格结构）" },
    },
    [],
  ),
  outputSchema: schema(
    {
      status: { type: "string", description: "（仅计划分支返回）审批结果：approved/rejected/cancelled/timeout" },
      title: { type: "string", description: "（仅计划分支返回）计划标题" },
      path: { type: "string", description: "（仅计划分支返回）计划文档逻辑路径（tmp/plans/ 下，模型可经 read 读取）" },
      feedback: { type: "string", description: "（仅计划分支返回）拒绝时的用户修改意见（无则空）" },
    },
    ["status", "title", "path"],
  ),
  async execute(args, ctx) {
    // 分支分派（专属参数）：options/multi → 选项询问；name → 填值；title/steps/content → 计划审批
    if (args.options != null || args.multi === true) {
      const options = (Array.isArray(args.options) ? args.options : []).map(normalizeChoiceOption)
      if (!options.length) throw new Error("ask 选项询问需要至少一个选项（options）")
      if (!String(args.prompt ?? "").trim()) return { output: "ask 失败：选项询问（options）需同时提供 prompt（问题文本）。" }
      // 无交互模式无人可答：明确报错引导自行决策（不空等 5 分钟超时）
      if (ctx.interactionMode === "none") {
        return { output: "ask 失败：当前通道无交互能力（无交互调用），无法向用户询问。请基于现有信息自行决策，或在回复中说明选项供用户下次指示。" }
      }
      const prompt = String(args.prompt ?? "")
      // 阻塞等待用户回应：事件推送由引擎 waitForChoice 发布（含 choiceId/multi），
      // 用户经 UI/REST/WS 提交选项/自定义文本/拒绝后本工具才返回，模型据此继续
      const choice = await ctx.waitForChoice(prompt, options, args.multi === true)
      if (!choice) return { output: "用户未在时限内做出选择，已取消本次询问。请基于现有信息自行决策或换一种方式征询。" }
      if (choice.kind === "refuse") {
        return { output: "用户拒绝了本次询问。请停止继续询问，基于现有信息自行决策；如信息不足，说明所需信息并请用户另行补充。" }
      }
      if (choice.kind === "multi") return { output: `用户选择：${choice.values.join("、")}` }
      return { output: `用户选择：${choice.value}` }
    }
    if (args.name != null) {
      const name = String(args.name).trim()
      if (!name) return { output: "ask 失败：填值分支需要指定环境变量名（name）。" }
      if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) return { output: `环境变量名非法: ${name}（仅允许字母/数字/下划线，字母或下划线开头）。` }
      // 填值弹窗仅 Web 前端实时会话可用（飞书/无交互无弹窗通道）
      if (ctx.interactionMode && ctx.interactionMode !== "realtime") {
        return { output: "ask 失败：当前通道不支持填值弹窗（仅 Web 前端实时会话可用）。请说明所需配置引导用户在设置面板配置环境变量。" }
      }
      const ok = await ctx.waitForEnv(name, String(args.description ?? ""), args.secret === true)
      if (!ok) return { output: `用户未提供环境变量 ${name}（拒绝或超时）。请说明所需配置，或基于现有信息改用其他方式。` }
      return { output: `环境变量 ${name} 已由用户设置并注入本次任务（后续工具读取立即生效，并已保存到浏览器本地，后续任务自动生效）。` }
    }
    if (args.title != null || args.steps != null || args.content != null) {
      const title = String(args.title ?? "").trim()
      const steps = (Array.isArray(args.steps) ? args.steps : []).map(String).filter(Boolean)
      const content = args.content != null ? String(args.content) : ""
      if (!title) return { output: "ask 失败：计划审批分支需要指定计划标题（title）。" }
      if (!steps.length && !content.trim()) return { output: "ask 失败：计划审批分支需要至少一个执行步骤（steps）或完整计划内容（content）。" }
      if (ctx.interactionMode === "none") {
        return { output: "ask 失败：当前通道无交互能力（无交互调用），无法提交计划审批。请基于现有信息直接执行，并在回复中说明计划要点。" }
      }
      const md = buildPlanMarkdown(title, steps, content || undefined)
      const logical = `tmp/${PLAN_DIR}/${planFileName(title)}`
      const abs = ctx.resolvePath(logical)
      // 写范围守卫（子Agent 声明，引擎注入）：命中则拒绝落盘（计划文档属会话产物，常规不命中）
      const guardMsg = await ctx.writeGuard?.([abs])
      if (guardMsg) return { output: `计划文档未落盘：${guardMsg}` }
      try {
        await ctx.writeFile(abs, md)
      } catch (err) {
        return { output: `计划文档保存失败：${(err as Error).message}。请检查会话目录权限后重试。` }
      }
      // 阻塞等待用户审批：批准 → 模型继续按计划执行；拒绝（含自定义修改意见）→ 修订后重新提交
      const choice = await ctx.waitForChoice(
        `请审核计划「${title}」（已保存到会话文件 ${logical}）：批准后将按计划逐步执行；拒绝将返回模型修改；也可直接输入修改意见（视为拒绝）。`,
        ["批准执行", "拒绝执行"],
      )
      const data = { status: "", title, path: logical }
      if (!choice) {
        return { output: `计划审批超时：「${title}」（5 分钟未响应）。请先向用户确认计划是否可执行；若继续执行，说明计划已提交过审批。`, data: { ...data, status: "timeout" } }
      }
      if (choice.kind === "refuse") {
        return { output: `用户拒绝审核计划「${title}」。请停止计划相关操作，基于现有信息直接回答用户或询问其需求。`, data: { ...data, status: "cancelled" } }
      }
      const value = choice.kind === "multi" ? choice.values.join("、") : choice.value
      if (value === "批准执行") {
        return { output: `计划已批准：「${title}」。请严格按计划逐步执行（计划文档：${logical}，可用 read 读取），每完成一步用 todo 更新状态，关键节点向用户汇报。`, data: { ...data, status: "approved" } }
      }
      const feedback = value === "拒绝执行" ? "" : value
      const reviseNote =
        feedback === ""
          ? "用户未附具体修改意见，请自行分析计划可能存在的不足（目标不清/步骤缺失/风险未评估等），修订后重新提交新版本。"
          : `用户修改意见：${feedback}。请按意见修订计划后重新提交新版本。`
      return { output: `计划已拒绝：「${title}」。${reviseNote}`, data: { ...data, status: "rejected", feedback } }
    }
    return { output: "ask 失败：缺少询问内容——prompt+options（选项询问）/ name（环境变量填值）/ title+steps 或 content（计划审批）三选一。" }
  },
}

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
      "数据流编排：一次调用执行多步工具链（把工具视为函数做动态编程），减少与模型的往返与词元消耗。**适用边界**：本工具是声明式管道，只做稳定的固定流程编排（引用映射/条件分支/循环）；超出表达式语言能力的高阶逻辑（复杂字符串/数据变换、参数需动态计算、错误捕获与分支恢复、条件重试策略、跨步骤聚合统计）**一律改用 js 工具**（完整 JS 语言能力，工具同样像内置函数直接调用），不要在 flow 里硬凑复杂表达式。支持：\n" +
      "- **步骤**：steps 为工具步骤或循环分组列表（分组须声明 foreach 或 while）。\n" +
      "- **引用**：`{{s1.data.xxx}}` 引用步骤结构化输出（各工具 data 结构先用 tool_schemas 批量查询），`{{s1.output}}` 引用文本输出。params 值恰为一个 `{{表达式}}` 时保留原始类型（数字/数组/对象原样传递），混排字符串按文本拼接——**字符串值混排进脚本源码须自带引号**（写 `'item:{{item}}'`，裸拼 `'item:' + {{item}}` 会把值当标识符、执行报错）；**多行字段（如 data.stdout）混排会带入换行**，拼进脚本源码字符串会语法错误：多行文本建议走 stdin（input 参数）或三引号包裹，引用单值字段（exitCode/length）不受影响；**对象/数组直接映射给 sh/py 的 input 参数时以 JSON 文本（双引号）传入 stdin，脚本 `json.loads` 可直接解析**（不会出现 Python repr 单引号形态）。根名：步骤 id（缺省自动编号 s1/s2…）、`prev`（上一实际执行步骤）、`item`/`index`（foreach 当前项/序号）、`iteration`（while 轮次）、`input`（flow 参数 input）。路径访问 `.字段`/`[下标]`/`.length`。\n" +
      "- **input 显式映射**：`{ 目标参数名: \"{{源}}\" }`，解析后覆盖 params 同名字段并抑制自动注入——字段改名、多对一汇聚（多个步骤输出映射进同一工具的不同参数）都用它表达；**非对象形式**（如 `input: \"{{item}}\"`）等价于 `{ input: \"{{item}}\" }`，直接给工具的 input 参数传值（脚本 stdin）。\n" +
      "- **when 条件分支**：表达式为假时跳过该步（不中断）。支持两种写法（等价）：裸表达式 `gen.data.ok == true` 或 `{{表达式}}` 包裹/混排 `{{gen.data.ok}} == true`。运算：`==`/`!=`/`>`/`>=`/`<`/`<=`、`&&`/`||`/`!`、括号、函数 `len()`/`contains()`/`exists()`；空数组视为假；引用不存在的路径解析为 undefined/空串（不报错，需判定存在性用 `exists()`）。\n" +
      "- **foreach 数据循环**（一对多扇出）：值为数组（逐项——**可直接写 JSON 数组文本如 `\"[1,2,3]\"`**，或表达式/`{{引用}}` 求值为数组，脚本 stdout 的 JSON 数组文本同样自动解析）或正整数（按次），体内经 `{{item}}`/`{{index}}` 引用；**快照语义**——迭代次数固定为求值时的长度，循环体修改源数组不影响遍历；**嵌套时内层 `{{item}}`/`{{index}}` 遮蔽外层同名引用**（外层值需提前映射到中间步骤）；组结果 data = 每轮末步 data 的数组。\n" +
      "- **while 条件循环**（do-while：先执行一轮再判断，条件可引用本组最新结果如 `{{g.data.exitCode}}`，适合重试直到成功）：为真继续下一轮，达上限停止；`maxLoops` 默认 10、上限 50；需前置判断时配 when。**组 data = 最后一轮末步的 data（单轮结果，非数组，无 `.length`）**——`iteration` 引用轮次，`{{g.data.xxx}}` 始终取最新轮结果。\n" +
      "- **失败语义**：**工具级异常才中断** flow（调用不存在的工具、strict 脚本非 0 退出等），错误信息含失败位置、原因与**已执行步骤清单**（判断副作用、安全续接重试用）；sh/py **非 0 退出码默认是正常结果**（exitCode 在 data，可用 when 判定，或给该步传 strict: true 转为中断）；`optional: true` 的步骤任何失败（执行失败/参数模板错误等）都不中断（记录错误继续）。\n" +
      "- **自动注入**（未显式 input 映射时保留旧版语义）：当前步为脚本工具（sh/py）时上一步输出经 stdin（input 参数）传入；其余工具按参数名映射注入上一步 JSON 输出，兜底注入 input 参数。注入物恒为上一步**文本输出**——上一步为 js 时即日志 + `[返回值]` 前缀的混排文本（非干净 JSON），下游要干净结果请显式映射 `{{id.data.result}}`。\n" +
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
  description: "装载指定子Agent 能力模块（类比 import 子模块）：其工具立即并入当前工具集（以 {agent}_ 前缀调用）、完整系统提示词注入当前上下文。不创建新上下文、无独立执行——装载后直接调用其工具，全程在当前会话内完成；重复装载幂等跳过。",
  card: { titleParams: ["name"], args: "none" },
  parameters: schema({ name: { type: "string" } }, ["name"]),
  async execute(args, ctx) {
    const name = String(args.name)
    await ctx.loadSubAgent(name)
    // 装载反馈枚举实际并入的工具全名（模型无需猜测 {agent}_* 的具体形态，直接调用即可）
    const tools = ctx.listSubAgentDefs().find((d) => d.name === name)?.tools ?? []
    const names = tools.map((t) => `${name}_${t}`)
    const toolNote = names.length
      ? `已并入工具（${names.length} 个）: ${names.join("、")}，直接调用即可`
      : `其能力以系统提示词形式注入（无自有工具）`
    return { output: `子Agent ${name} 已装载（${toolNote}）。`, data: { loaded: name, tools: names } }
  },
}

export const agentRunTool: Tool = {
  name: "agent_run",
  description: "执行新会话：派生临时新会话（独立上下文，与主会话完全隔离），预加载指定子Agent 列表（可多个，其完整系统提示词与工具进入新会话），阻塞执行任务直到结束，只返回最终结果文本；执行过程全程存档供历史回放。",
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

/** 切换完整模式（DESIGN「极简模式」）：仅极简会话可见可用（引擎 schema 过滤），需用户批准后执行。 */
export const fullModeTool: Tool = {
  name: "full_mode",
  description: "切换到完整模式（仅在极简模式会话中可用）：解锁全部工具（读取文件、编排、子Agent 等）。当前会话处于极简模式（仅 sh/edit）而任务确需其他工具能力时调用本工具，用户批准后本任务后续轮次与后续任务均启用完整工具集与完整说明；被拒绝则继续用 sh/edit 完成。",
  requiresApproval: true,
  card: { titleParams: ["reason"], args: "none" },
  parameters: schema({
    reason: { type: "string", description: "可选：需要完整模式的原因（展示给用户作审批参考）" },
  }, []),
  async execute(_args, ctx) {
    if (!ctx.exitMinimalMode) return { output: "当前环境不支持切换完整模式。" }
    await ctx.exitMinimalMode()
    return { output: "已切换到完整模式：全部工具已解锁、完整说明已生效（本任务后续轮次立即可用）。请继续执行任务。" }
  },
}

/** 全量全局工具表（不经构建期排除过滤）：构建脚本校验清单用（`scripts/build-tools.ts` 须对全量名单校验，
 *  否则连续两次不同排除清单的构建会误拒上次被排除的名字）。 */
export function createAllGlobalTools(): Record<string, Tool> {
  const todoTool = makeTodoTool()
  return {
    read: readTool,
    write: writeTool,
    ls: lsTool,
    grep: grepTool,
    glob: globTool,
    file: fileTool,
    edit: editTool,
    patch: patchTool,
    diff: diffTool,
    flow: makeFlowTool(),
    tool_schemas: toolSchemasTool,
    sh: shTool,
    sh_task: shTaskTool,
    py: pyTool,
    js: jsTool,
    show: showTool,
    // save_tool/delete_tool（HTML 小工具库）不注册为全局工具：由 widgets 子Agent 命名空间暴露（增删改查补齐）
    fetch_url: fetchUrlTool,
    todo: todoTool,
    ask: askTool,
    // current_time 已移除：时间获取用 sh/py/js 脚本（如 sh date）按需完成，不占全局工具位；
    // 刻意不注入时间相关提示词引导——模型自身知道如何用现有工具取时间
    // read_feedback 不注册进总Agent 全局工具集（读取用户反馈是自我优化专属输入通道，
    // 由 self_optimize 子Agent 以 self_optimize_read_feedback 命名空间暴露；def 声明保证新会话模式可用）
    // git 不注册进总Agent 全局工具集（只读 git 属编码工作流，由 code/explore 子Agent 以
    // code_git/explore_git 命名空间暴露；self_optimize 经连带装载 code 一并获得）
    // preview_server/env_detect/system_info 不注册进总Agent 全局工具集（开发验证/环境探测属编码工作流，
    // 由 code 子Agent 以 code_ 命名空间暴露；self_optimize 经连带装载 code 一并获得）
    // agent_list 不注册进总Agent 全局工具集：未装载子Agent 清单已由 systemPromptInjection 注入提示词
    // （模型上下文已有，工具调用冗余且干扰工具选择）；agent_list 仅在新会话执行（组合子Agent 编排环境）
    // 注入——runNewSession 对纯 md 组合式子Agent 自动注入 agent_list/agent_load/agent_run
    agent_load: agentLoadTool,
    agent_run: agentRunTool,
    // full_mode（极简模式切换完整模式）注册进全局工具集，但仅极简会话可见可用（引擎按任务白名单过滤 schema，
    // 完整模式会话从 schema 移除，防冗余工具干扰选择）
    full_mode: fullModeTool,
  }
}

/** 全局工具表（构建期排除过滤后）：被 GEBAI_BUILD_EXCLUDE_TOOLS 排除的工具不注册不暴露。 */
export function createGlobalTools(): Record<string, Tool> {
  const all = createAllGlobalTools()
  return Object.fromEntries(Object.entries(all).filter(([name]) => !excludedGlobalTools.has(name)))
}

export type ToolSet = Record<string, Tool>
