/**
 * 冲突合并：`<<<<<<< / ||||||| / ======= / >>>>>>>` 标记的解析与解决（**纯函数层**）。
 *
 * 纯逻辑单独成文件的原因：合并是「改文件内容」的操作，解析与替换一旦出错就是静默损坏文件——
 * 必须能脱离 DOM/Monaco 单测（含畸形标记、CRLF、diff3 三段式、多块倒序替换等边界）。
 * 渲染与交互在 `merge-view.ts`。
 */

/** 一个冲突块（行号 1 起始，含标记行本身）。 */
export interface ConflictBlock {
  /** `<<<<<<<` 行号 */
  startLine: number
  /** `>>>>>>>` 行号 */
  endLine: number
  /** 我方内容行（不含标记行） */
  ours: string[]
  /** 对方内容行 */
  theirs: string[]
  /** 共同祖先内容行（diff3 风格 `|||||||` 段；无此段时 undefined） */
  base?: string[]
  /** 标记行上的标签（分支/提交说明，用于界面提示） */
  oursLabel: string
  theirsLabel: string
}

const MARK_OURS = "<<<<<<<"
const MARK_BASE = "|||||||"
const MARK_SEP = "======="
const MARK_THEIRS = ">>>>>>>"

/** 整行判定（允许行尾 `\r`；标签文本任意）。 */
function isMark(line: string, mark: string): boolean {
  const t = line.replace(/\r$/, "")
  return t === mark || t.startsWith(`${mark} `)
}

function label(line: string, mark: string): string {
  return line.replace(/\r$/, "").slice(mark.length).trim()
}

/**
 * 解析冲突标记块（按出现顺序）。
 *
 * 容错取向：**只把完整闭合的块算作冲突**（`<<<<<<<` 必须有配对的 `>>>>>>>`），
 * 畸形/未闭合的片段原样保留、不参与解决——宁可不识别，也不乱改用户内容。
 * 嵌套的 `<<<<<<<`（git 不该产出）视为内容行，不递归。
 */
export function parseConflictBlocks(text: string): ConflictBlock[] {
  const lines = text.split("\n")
  const out: ConflictBlock[] = []
  let i = 0
  while (i < lines.length) {
    if (!isMark(lines[i], MARK_OURS)) {
      i++
      continue
    }
    const start = i
    const oursLabel = label(lines[i], MARK_OURS)
    const ours: string[] = []
    let base: string[] | undefined
    const theirs: string[] = []
    let theirsLabel = ""
    let j = i + 1
    let section: "ours" | "base" | "theirs" = "ours"
    let closed = false
    for (; j < lines.length; j++) {
      const line = lines[j]
      if (section === "ours" && isMark(line, MARK_BASE)) {
        section = "base"
        base = []
        continue
      }
      if (isMark(line, MARK_SEP) && section !== "theirs") {
        section = "theirs"
        continue
      }
      if (isMark(line, MARK_THEIRS)) {
        theirsLabel = label(line, MARK_THEIRS)
        closed = true
        break
      }
      // 后续 `<<<<<<<` 视为内容（不嵌套），避免畸形输入吞掉后面的真冲突
      if (section === "ours") ours.push(line)
      else if (section === "base") base?.push(line)
      else theirs.push(line)
    }
    if (!closed) {
      // 未闭合：不作为冲突块（保留原文），继续扫描后续行
      i = start + 1
      continue
    }
    out.push({ startLine: start + 1, endLine: j + 1, ours, theirs, base, oursLabel, theirsLabel })
    i = j + 1
  }
  return out
}

export type Resolution = "ours" | "theirs" | "both" | "base"

/** 单个冲突块的替换文本（行数组，保持调用方换行风格）。 */
export function resolutionLines(block: ConflictBlock, choice: Resolution): string[] {
  if (choice === "ours") return block.ours
  if (choice === "theirs") return block.theirs
  if (choice === "base") return block.base ?? []
  return [...block.ours, ...block.theirs]
}

/**
 * 解决指定冲突块（也可一次解决全部同选择），返回新文本。
 * 从后往前替换，避免行号偏移——批量解决时后续块的行号不被前面的替换影响。
 */
export function applyResolution(text: string, blocks: ConflictBlock[], choice: Resolution): string {
  if (!blocks.length) return text
  const lines = text.split("\n")
  const ordered = [...blocks].sort((a, b) => b.startLine - a.startLine)
  for (const b of ordered) {
    const content = resolutionLines(b, choice).map((l) => l.replace(/\r$/, ""))
    // 原块首行带 \r（CRLF 文件）：替换内容的每一行同样带 \r，保持整文件换行风格不变
    const keepCr = lines[b.startLine - 1]?.endsWith("\r") === true
    const replaced = keepCr ? content.map((l) => `${l}\r`) : content
    lines.splice(b.startLine - 1, b.endLine - b.startLine + 1, ...replaced)
  }
  return lines.join("\n")
}

/** 是否仍有未解决的冲突标记（保存前提示用；与 parseConflictBlocks 判定口径一致）。 */
export function hasConflictMarkers(text: string): boolean {
  return parseConflictBlocks(text).length > 0
}

/** 冲突块摘要（导航条显示：第几块 / 共几块 / 各段行数）。 */
export function blockSummary(block: ConflictBlock): string {
  const parts = [`我方 ${block.ours.length} 行`, `对方 ${block.theirs.length} 行`]
  if (block.base) parts.push(`祖先 ${block.base.length} 行`)
  return parts.join(" · ")
}
