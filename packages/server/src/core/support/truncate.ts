/** 工具输出截断与超长用户输入落盘（自 core/tools.ts 抽取的公共文本护栏，全仓工具/子Agent 复用）。 */
import { dirname, join } from "node:path"
import type { ToolContext, ToolResult } from "../base/types"
import { truncatedLogicalPath, truncatedPath } from "../base/paths"

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
