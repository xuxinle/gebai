/** 文件类全局工具（read/write/ls/file/grep/glob/edit/patch/diff）——自 core/tools.ts 按域拆分。
 *  注册条目见文件尾 globalTools（聚合器自动扫描，丢文件即注册）。 */
import { spawn } from "node:child_process"
import { tmpdir } from "node:os"
import { dirname, isAbsolute, join, relative } from "node:path"
import type { Tool } from "../base/types"
import { VISION_IMAGE_MIME } from "../llm/llm"
import type { ContentBlock, FileEntry } from "@gebai/sdk"
import { diffLines, inferLang, unifiedDiff, splitLines, DIFF_MAX_LINES } from "../base/diff"
import { applyPatch, parsePatch, PATCH_MAX_FILE_BYTES, PATCH_MAX_HUNKS, type AppliedHunk } from "../base/patch"
import { safeModeWriteCheck } from "../security/safety"
import { truncate, sliceLines } from "../support/truncate"
import { walkDirFiles, WALK_SKIP_DIRS } from "../support/walk"
import { artifactBlocks, baseName, previewLogicalPath } from "../support/artifacts"
import { jsRuntimeCommand } from "../exec/js-tool"
import { schema, type GlobalToolEntry } from "./shared"

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
/** cat -n 风格行号前缀：startLine 为首行真实行号（offset/limit 切片后仍对应文件行号），右对齐 + 制表符分隔。 */
function withLineNumbers(text: string, startLine: number): string {
  const lines = text.split("\n")
  if (lines.length && lines[lines.length - 1] === "") lines.pop()
  const width = String(startLine + Math.max(0, lines.length - 1)).length
  return lines.map((l, i) => `${String(startLine + i).padStart(width)}\t${l}`).join("\n")
}

/** 剥离 UTF-8 BOM（文件头 \uFEFF）：read 展示与 edit/patch 匹配用干净正文（BOM 会让文件首行的
 *  old_string 精确匹配静默失败——Windows 工具生成的文件常见）；写回时按原文件有无 BOM 补回（edit/patch/write）。 */
function stripBom(s: string): string {
  return s.charCodeAt(0) === 0xfeff ? s.slice(1) : s
}
export const readTool: Tool = {
  name: "read",
  description:
    "读取文件内容。相对路径以会话工作目录（tmp/）为基准（tmp/ 前缀可省略），本地模式支持绝对路径（服务端部署受沙箱限制）。默认每行前缀真实行号（cat -n 风格「行号→制表符」，定位/引用 文件:行号、构造 patch 补丁用；复制原文给 edit/patch 时须去掉行号前缀，不需要行号可传 line_numbers:false）。非 UTF-8 编码（file info 探测的 GBK 等）传 encoding 按指定编码解码读取。图片文件（png/jpg/jpeg/gif/webp）不以文本读取：主模型多模态时图片直接内联进上下文（无需 vision 等其他工具），非多模态返回说明与 vision 指引；svg 为文本按正常读取。图片/图表等二进制或结构化文件另返回对应内容块供 UI 展示。",
  card: { titleParams: ["path"], file: "path" },
  parameters: schema({
    path: { type: "string", description: "文件路径" },
    offset: { type: "integer", description: "起始行号（1 起始，默认 1）" },
    limit: { type: "integer", description: "读取行数（正数取 offset 起 N 行；负数取末尾 N 行）" },
    line_numbers: { type: "boolean", description: "每行前缀真实行号（默认 true；offset/limit 切片仍对应文件真实行号）" },
    encoding: { type: "string", description: "可选：按指定编码解码读取（如 gbk——file info 探测为 GBK 时用，缺省 UTF-8；支持 TextDecoder 编码名）。仅解码读取，需转码改写文件用 py 脚本处理" },
  }, ["path"]),
  async execute(args, ctx) {
    const path = ctx.resolvePath(String(args.path))
    await assertReadableSize(path, "read", READ_MAX_FILE_BYTES)
    // 编码指定读取：按原始字节解码（非 UTF-8 文件——file info 探测为 GBK 等场景）；目录在此一并给出可读引导
    let content: string
    try {
      // 多模态图片读取（DESIGN「多模态支持」）：白名单图片（png/jpg/jpeg/gif/webp）不按文本解码（乱码无意义）——
      // 二进制读入后经 ToolResult.images 交引擎内联进工具结果消息（主模型多模态时模型直接可见，无需视觉工具）；
      // 非多模态（ctx.multimodal 未注入/为 false）返回说明 + 视觉子代理指引。encoding 显式指定时按文本解码（模型明确要原始字节内容）
      const ext = path.split(".").pop()?.toLowerCase() ?? ""
      const imageMime = VISION_IMAGE_MIME[ext]
      if (imageMime && !args.encoding) {
        const buf = await ctx.readBinaryFile(path)
        const size = humanSize(buf.byteLength)
        ctx.fileGuard?.markRead(path)
        return {
          output: ctx.multimodal
            ? `已读取图片文件 ${args.path}（${imageMime}，${size}）——图片已作为多模态内容附加在本结果中，可直接查看分析，无需调用其他工具。`
            : `已读取图片文件 ${args.path}（${imageMime}，${size}）：当前主模型未声明多模态能力（GEBAI_LLM_MULTIMODAL），图片内容未注入上下文。如需查看图片内容，请调用 vision_analyze（vision 子代理，装载 vision 后 image 参数传该路径）。`,
          images: [{ path, display: String(args.path), mime: imageMime, data: Buffer.from(buf).toString("base64") }],
          blocks: artifactBlocks(previewLogicalPath(path, ctx)),
        }
      }
      if (ext === "bmp") {
        return { output: `read 拒绝：${args.path} 是 bmp 图片，不在多模态支持格式内（png/jpg/jpeg/gif/webp）。请先转换为支持格式（如 sh/py 脚本或 draw 工具）后读取。` }
      }
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
    if (args.line_numbers !== false) {
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
    // 登记已读与内容指纹（write 防误覆盖/防陈旧覆盖守卫依据；读取失败抛错不登记）
    ctx.fileGuard?.markRead(path, content)
    // 产物块路径用解析后的可预览路径（会话 tmp/ 逻辑路径或绝对路径——原始参数路径在项目工具下无法由 files 接口解析）
    const blocks = artifactBlocks(previewLogicalPath(path, ctx), content)
    return { ...truncated, blocks }
  },
}

export const writeTool: Tool = {
  name: "write",
  description:
    "写入文件。相对路径以会话工作目录（tmp/）为基准（tmp/ 前缀可省略，受沙箱限制）。默认整体覆盖；append:true 追加模式——内容接在文件末尾（文件不存在则新建）。目标文件**已存在且本会话未 read 过**时拒绝写入（防盲覆盖：先 read 掌握现有内容，确认整体覆盖后再 write；新建文件不受限）；**已存在但内容自上次读取/写入后被修改过**（并行分支、脚本命令、外部编辑）同样拒绝（防陈旧覆盖：重新 read 后再写）。read/edit/patch/write 成功过的文件视为已读；只改局部优先 edit/patch。**大文件（约 300 行以上）分段写入**：先 write 首段，再以 append:true 续写后续段（每段 200~300 行），避免单次输出过长被模型输出上限截断或接口超时。",
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
    // 防陈旧覆盖守卫：内容自本会话上次读取/写入后已漂移（并行分支/主线/脚本命令/外部编辑）→ 拒绝并引导重读，
    // 防基于旧认知静默覆盖他人改动
    if (existing !== null && ctx.fileGuard?.staleSinceRead(path, existing)) {
      return {
        output: `write 拒绝：${args.path} 的内容自本会话上次读取/写入后已被修改（可能是并行分支、主线任务、脚本命令或外部编辑）。请重新 read 最新内容后再写，避免覆盖他人的改动；确认要覆盖时在 read 之后立即 write。`,
      }
    }
    const content = stripBom(String(args.content ?? ""))
    // 覆盖写保留原文件的 UTF-8 BOM（read 展示的是去 BOM 正文，模型意图即正文；BOM 丢失会改变文件字节内容）；
    // 追加模式接在 existing 之后不动文件头
    const bom = existing !== null && existing.startsWith("\uFEFF") ? "\uFEFF" : ""
    const final = append && existing !== null ? existing + content : bom + content
    await ctx.writeFile(path, final)
    ctx.fileGuard?.markRead(path, final)
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
      new_name: { type: "string", description: "rename 新名字（仅名字、不含路径分隔符；跨目录改名用 move）" },
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
      const newName = String(args.new_name ?? "").trim()
      if (!newName || newName.includes("/") || newName.includes("\\") || newName === "." || newName === "..") {
        return { output: "file 拒绝：rename 需要合法的 new_name（仅新名字、不含路径分隔符）；跨目录移动请用 action=move。" }
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
export const grepTool: Tool = {
  name: "grep",
  description:
    "按正则表达式在会话工作目录（tmp/）中递归搜索文本内容，返回 文件:行号: 匹配行（路径带 tmp/ 前缀，可直接用于 read 等文件工具；本地模式 path 可传 tmp/ 外绝对/相对路径，实际遍历搜索）。宽泛摸底优先 output=files。node_modules/.git/dist 等大型目录默认跳过（显式 include 点名除外）。搜索含正则元字符的代码片段（如 foo.bar(）传 literal:true 按字面匹配。include/exclude 支持逗号分隔多模式与花括号（如 *.{ts,tsx}、tests/**,*.md）。匹配上限 200 处（head_limit 可压低先看一部分）。",
  card: { titleParams: ["pattern"] },
  parameters: schema(
    {
      pattern: { type: "string" },
      path: { type: "string", description: "搜索起点：目录（递归）或单个文件（直接内搜）（默认 .，相对会话工作目录，tmp/ 前缀可省略）" },
      ignore_case: { type: "boolean", description: "true 时大小写不敏感" },
      literal: { type: "boolean", description: "true 时 pattern 按字面字符串匹配（正则元字符自动转义），适合搜索含 .()[]* 等字符的代码片段（默认 false 正则）" },
      output: { enum: ["content", "files", "count"], description: "结果形态（默认 content；大范围定位优先 files，只看命中文件不刷内容）" },
      context: { type: "integer", description: "匹配行前后各附上下文行数（0-10，默认 0；仅 content 模式）：匹配行前缀 文件:行号:、上下文行前缀 文件-行号-，组间 -- 分隔（同 grep -n -C）；context_before/context_after 指定时覆盖对应侧" },
      context_before: { type: "integer", description: "匹配行**前**附上下文行数（0-10，仅 content 模式；与 context 独立指定非对称上下文，如同 grep -B）" },
      context_after: { type: "integer", description: "匹配行**后**附上下文行数（0-10，仅 content 模式；与 context 独立指定非对称上下文，如同 grep -A——看定义后的实现体常用）" },
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
      new RegExp(pattern, args.ignore_case ? "i" : "")
    } catch {
      return { output: `grep: 无效正则: ${args.pattern}` }
    }
    const mode = args.output === "files" || args.output === "count" ? args.output : "content"
    const ctxLine = (v: unknown): number | null => (v == null ? null : Math.max(0, Math.min(10, Math.floor(Number(v) || 0))))
    const context = ctxLine(args.context) ?? 0
    // 非对称上下文（-B/-A）：指定时覆盖 context 的对应侧
    const before = ctxLine(args.context_before) ?? context
    const after = ctxLine(args.context_after) ?? context
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
      const r = await runGrepMatcher({ pattern, flags: args.ignore_case ? "i" : "", maxMatches, files: sent })
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
    "精确修改文件：基于 old_string → new_string 定点替换，可一次多处，适合小范围改动；任一编辑项校验失败（不唯一/不匹配）则整体不落盘。目标文件已存在但本会话未 read 过时拒绝（防盲改，同 write 守卫；read/edit/patch/write 成功过的文件视为已读）。old_string 须从文件当前内容精确复制（不含 read 输出的行号前缀）。改动较多或行号容易偏移时改用 patch。修改前先 read 目标区域。",
  card: { titleParams: ["path"], args: "edits", codeField: "edits", file: "path" },
  parameters: schema(
    {
      path: { type: "string" },
      edits: {
        type: "array",
        items: {
          type: "object",
          properties: {
            old_string: { type: "string", description: "原文片段（非空，从文件当前内容精确复制，含缩进；须在文件中唯一，否则补充上下文或用 replace_all）" },
            new_string: { type: "string", description: "替换后的内容" },
            replace_all: { type: "boolean", description: "true 时替换全部匹配（默认 false：多处匹配报错不落盘）" },
          },
          required: ["old_string", "new_string"],
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
    // BOM 感知：匹配用去 BOM 正文（BOM 会让首行 old_string 精确匹配失败），写回按原文件补回
    const hadBom = content.startsWith("\uFEFF")
    if (hadBom) content = content.slice(1)
    // 防盲改守卫（与 write 防盲覆盖同规则）：已存在但本会话未 read 过 → 拒绝，防凭记忆/假设内容盲改（old_string 匹配失败白跑一轮）
    if (ctx.fileGuard && !ctx.fileGuard.hasRead(path)) {
      return {
        output: `edit 拒绝：${args.path} 已存在，但本会话尚未读取过其内容（防盲改）。请先 read 该文件（或目标区域）确认当前内容后再 edit；read/edit/patch/write 成功过的文件视为已读。`,
      }
    }
    // 防陈旧改守卫（与 write 防陈旧覆盖同规则）：内容自本会话上次读取/写入后已漂移 → 拒绝并引导重读——
    // patch 的行号模糊容错可对漂移内容误命中，edit 的唯一性匹配失败提示也远不如陈旧提示直接
    // （指纹边界 BOM 无关，此处传去 BOM 正文即可）
    if (ctx.fileGuard?.staleSinceRead(path, content)) {
      return {
        output: `edit 拒绝：${args.path} 的内容自本会话上次读取/写入后已被修改（可能是并行分支、主线任务、脚本命令或外部编辑）。请重新 read 最新内容后再修改。`,
      }
    }
    const edits = (args.edits as Array<{ old_string: string; new_string: string; replace_all?: boolean }>) || []
    const applied: string[] = []
    for (const [idx, e] of edits.entries()) {
      const old_string = String(e.old_string ?? "")
      const new_string = String(e.new_string ?? "")
      const nth = `第 ${idx + 1} 项`
      if (!old_string) {
        throw new Error(`修改失败: ${nth} old_string 为空（old_string 必须是文件中的非空原文片段；新建文件用 write）`)
      }
      const occ = findOccurrences(content, old_string)
      if (!occ.length) {
        // 空白近似提示：归一化后可命中 → 大概率是缩进/空白复制不精确；
        // 行号泄漏提示：old_string 携带 read 输出的「行号→制表符」前缀 → 去掉前缀后可精确命中
        const near = collapseWhitespace(content).includes(collapseWhitespace(old_string))
        const stripped = old_string.split("\n").map((l) => l.replace(/^\s*\d+\t/, "")).join("\n")
        const lineNoLeak = stripped !== old_string && findOccurrences(content, stripped).length > 0
        const hint = lineNoLeak
          ? "（检测到 old_string 携带 read 输出的行号前缀——请去掉每行行号后重试）"
          : near
            ? "（检测到空白/缩进不一致的近似原文——请 read 后从原文逐字符复制 old_string）"
            : "（请先 read 当前文件核对最新内容）"
        throw new Error(`修改失败: ${nth} old_string 未在文件中精确匹配: ${old_string.slice(0, 60)}${hint}`)
      }
      if (occ.length > 1 && e.replace_all !== true) {
        const lines = occ.slice(0, 8).map((i) => lineOfIndex(content, i)).join("、")
        throw new Error(`修改失败: ${nth} old_string 匹配 ${occ.length} 处（行 ${lines}${occ.length > 8 ? "…" : ""}）——请扩大 old_string 上下文使其唯一，或确认全部替换时该项传 replace_all: true`)
      }
      const lineNos = e.replace_all === true ? occ : [occ[0]]
      // 行号在替换前的内容上计算（替换会移动后续文本位置）
      const shown = lineNos.slice(0, 8).map((i) => lineOfIndex(content, i))
      content = e.replace_all === true ? content.split(old_string).join(new_string) : content.replace(old_string, new_string)
      applied.push(`${idx + 1}) 行 ${shown.join("、")}${lineNos.length > 8 ? `（共 ${lineNos.length} 处）` : ""}`)
    }
    await ctx.writeFile(path, (hadBom ? "\uFEFF" : "") + content)
    // 修改后内容即已掌握（模型无需重读验证），登记已读（指纹边界 BOM 无关，传去 BOM 正文即可）
    ctx.fileGuard?.markRead(path, content)
    // 产物块（与 read/write 同款）：修改后的文件内容卡（弹窗查看模式收敛为文件链接）
    const blocks = artifactBlocks(previewLogicalPath(path, ctx), content)
    return { output: `已对 ${args.path} 应用 ${edits.length} 处修改：${applied.join("；")}`, blocks }
  },
}

export const diffTool: Tool = {
  name: "diff",
  description: "对比两段文本或两个文件（旧 → 新），返回行级差异：unified diff 文本 + diff 内容块（UI 并排高亮对比）。old_text/new_text 与 old_path/new_path 任选一种。",
  card: { args: "block" },
  parameters: schema(
    {
      old_text: { type: "string", description: "旧文本内容（与 old_path 二选一）" },
      new_text: { type: "string", description: "新文本内容（与 new_path 二选一）" },
      old_path: { type: "string", description: "旧文件路径（与 old_text 二选一）" },
      new_path: { type: "string", description: "新文件路径（与 new_text 二选一）" },
      language: { type: "string", description: "语法高亮语言（typescript/json/python/bash 等，默认按文件名推断）" },
      name: { type: "string", description: "对比标题（如「重构前后对比」，推荐传入有意义的标题；不传则默认取文件名）" },
      oldName: { type: "string", description: "旧侧面板标题（如「重构前」「v1」，不传默认「旧」）" },
      newName: { type: "string", description: "新侧面板标题（如「重构后」「v2」，不传默认「新」）" },
    },
  ),
  async execute(args, ctx) {
    let oldText: string
    let newText: string
    const oldPath = args.old_path ? String(args.old_path) : ""
    const newPath = args.new_path ? String(args.new_path) : ""
    if (args.old_text != null || args.new_text != null) {
      if (oldPath || newPath) return { output: "diff: old_path/new_path 不能与 old_text/new_text 混用" }
      oldText = String(args.old_text ?? "")
      newText = String(args.new_text ?? "")
    } else {
      if (!oldPath || !newPath) return { output: "diff: 需要提供 old_text/new_text 或 old_path/new_path" }
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
      blocks: [{ type: "diff", oldText, newText, language, name, oldName: args.old_name ? String(args.old_name) : undefined, newName: args.new_name ? String(args.new_name) : undefined, lines }],
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
      dry_run: { type: "boolean", description: "true 时仅预演（校验并报告将应用的位置），不写入" },
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
      // 防陈旧改守卫（与 write/edit 同规则）：内容自本会话上次读取/写入后已漂移 → 拒绝并引导重读——
      // 行号模糊容错可对漂移内容误命中错位应用（指纹边界 BOM 无关，传去 BOM 正文即可）
      if (exists && ctx.fileGuard?.staleSinceRead(abs, content)) {
        return { output: `patch 拒绝：${target} 的内容自本会话上次读取/写入后已被修改（可能是并行分支、主线任务、脚本命令或外部编辑）。请重新 read 最新内容后再打补丁。` }
      }
      let applied: AppliedHunk[] = []
      for (const [pi, part] of parts.entries()) {
        const r = applyPatch(content, part)
        if (!r.ok) {
          return {
            output: `patch: ${order.length > 1 ? `${target} ` : ""}第 ${r.hunkIndex + 1} 处 hunk 未匹配：${r.error}（请先 read 当前文件内容核对，或改用 edit 定点替换；dry_run=true 可预演）${parts.length > 1 ? `（${target} 第 ${pi + 1} 段）` : ""}`,
          }
        }
        content = r.result
        applied = applied.concat(r.applied)
      }
      planned.push({ target, abs, result: content, applied, bom })
    }
    if (args.dry_run === true) {
      const lines = planned.map((p) => `${p.target}：${describeAppliedPatch(p.applied)}`)
      return { output: `patch 预演通过（${planned.length} 个文件，dry_run，未写入）：\n${lines.join("\n")}` }
    }
    // 落盘 + 登记已读 + 产物块
    const blocks: ContentBlock[] = []
    for (const p of planned) {
      await ctx.writeFile(p.abs, (p.bom ? "\uFEFF" : "") + p.result)
      ctx.fileGuard?.markRead(p.abs, p.result)
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

/** 本文件的全局工具注册条目（projectAware 默认包装：会话相对路径 + project 参数切换解析基准）。 */
export const globalTools: GlobalToolEntry[] = [
  { name: "read", tool: readTool, project: true },
  { name: "write", tool: writeTool, project: true },
  { name: "ls", tool: lsTool, project: true },
  { name: "grep", tool: grepTool, project: true },
  { name: "glob", tool: globTool, project: true },
  { name: "file", tool: fileTool, project: true },
  { name: "edit", tool: editTool, project: true },
  { name: "patch", tool: patchTool, project: true },
  { name: "diff", tool: diffTool, project: true },
]
