/**
 * 子代理与引擎共用的纯工具函数（零引擎内部依赖）。
 * - schema/parseRegion：工具参数 schema 构造与 region 解析（desktop/cv-analysis/playwright 等识别工具复用）；
 * - truncate/spillLongUserInput/sliceLines：输出截断、超长用户输入落盘、按行切片（全仓工具/子Agent 复用）。
 * 依赖仅 node 标准库 + 契约类型（ToolContext 最小字段：home/user/sessionId）。
 */
import { createHash } from "node:crypto"
import { dirname, join } from "node:path"
import type { ToolContext, ToolResult } from "./agent-contract"

/** 构造 object 型工具参数 schema（properties + required 快捷方式）。 */
export function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchemaLike {
  return { type: "object", properties, required }
}

interface ToolSchemaLike {
  type: "object"
  properties: Record<string, unknown>
  required?: string[]
}

/** region 参数校验：'x,y,w,h'，x/y 可为负（副屏在主屏左侧/上方时坐标为负），w/h 非负。
 *  desktop 截图/识别工具与 cv-analysis 共享工厂（及 playwright 识别工具）复用。 */
export function parseRegion(region: string): { x: number; y: number; w: number; h: number } | null {
  const m = /^(-?\d+),(-?\d+),(\d+),(\d+)$/.exec(region.trim())
  if (!m) return null
  const [, x, y, w, h] = m
  return { x: Number(x), y: Number(y), w: Number(w), h: Number(h) }
}

/* ---------------- 会话路径推导（与引擎存储布局契约一致：users/{user}/sessions/{2}/{2}/{id}/tmp/…） ---------------- */

/** 会话 ID 格式白名单：32 位小写 hex。防路径穿越（外部输入的 id 必须先过此校验）。 */
export function isValidSessionId(id: string): boolean {
  return /^[0-9a-f]{32}$/.test(id)
}

/** 会话根目录（分片段 = ID 前 2+2 位）。 */
export function sessionPath(home: string, user: string, sessionId: string): string {
  if (!isValidSessionId(sessionId)) throw new Error(`invalid session id: ${sessionId}`)
  return join(home, "users", user, "sessions", sessionId.slice(0, 2), sessionId.slice(2, 4), sessionId)
}

/** 截断文件逻辑路径（相对会话根，模型/前端感知的逻辑路径；固定正斜杠跨平台）。 */
export function truncatedLogicalPath(toolName: string, content: string): string {
  const hash = createHash("sha256").update(content).digest("hex")
  return `tmp/truncated/${toolName}_${hash}.txt`
}

export function truncatedPath(home: string, user: string, sessionId: string, toolName: string, content: string): string {
  return join(sessionPath(home, user, sessionId), truncatedLogicalPath(toolName, content))
}

/* ---------------- 输出截断（全仓工具/子Agent 复用） ---------------- */

export const TRUNCATE_THRESHOLD = 12000
/** 截断消息保留的首/尾字符数（DESIGN「常量参考」）。 */
export const TRUNCATE_HEAD_CHARS = 4000
export const TRUNCATE_TAIL_CHARS = 4000

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

/** 超长用户输入落盘阈值（字符）：超出时全文写入会话 tmp/user_inputs/，消息正文保留头尾 + 文件引用。 */
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
