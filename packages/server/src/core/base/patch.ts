import { splitLines } from "./diff"

/**
 * patch 工具：unified diff 补丁解析与应用（纯函数，无 fs 依赖，与 diff.ts 同构）。
 * - 解析：`---`/`+++` 文件头（可省略）、`@@ -l,c +l,c @@` hunk 头（容错省略 count 的形式）、
 *   上下文/新增/删除行、`\ No newline` 标记；git 风格元数据行（diff --git/index/mode 等）容忍跳过
 * - 应用：逐 hunk 顺序应用到行数组（偏移累积）；删除行作锚点全文件匹配，上下文不符时
 *   裁剪 hunk 头/尾上下文各至多 PATCH_FUZZ_LINES 行重试（模糊容错）；纯新增 hunk 按头行号插入
 * - 原子性：任一 hunk 不匹配整体失败（返回失败 hunk 索引与原因），调用方保证不落盘
 */

/** 行号模糊容错：上下文裁剪行数上限。 */
export const PATCH_FUZZ_LINES = 3
/** 单次补丁 hunk 数上限。 */
export const PATCH_MAX_HUNKS = 100
/** patch 目标文件大小上限（字符）。 */
export const PATCH_MAX_FILE_BYTES = 5 * 1024 * 1024

/** 补丁行类型：0=上下文、1=新增、-1=删除。 */
export type PatchLineKind = 0 | 1 | -1

export interface PatchLine {
  kind: PatchLineKind
  text: string
}

export interface PatchHunk {
  /** `@@` 头声明的旧侧起始行（1 起始；省略/未知为 0，仅纯新增定位用）。 */
  startA: number
  lines: PatchLine[]
}

export interface PatchFile {
  oldPath?: string
  newPath?: string
  /** 新建文件（`--- /dev/null`）。 */
  isNew: boolean
  hunks: PatchHunk[]
}

export interface AppliedHunk {
  /** 原始 hunk 序号（0 起始，对应补丁文本顺序）。 */
  index: number
  /** 应用后本 hunk 首个变更行在文件中的行号（1 起始）。 */
  line: number
  /** 本 hunk 造成的行数净变化（新增数 − 删除数）。 */
  delta: number
}

export type ApplyPatchResult =
  | { ok: true; result: string; applied: AppliedHunk[] }
  | { ok: false; hunkIndex: number; error: string }

const HUNK_RE = /^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@/

/** 解析 unified diff 文本为文件补丁列表（git 风格元数据行容忍跳过；无 hunk 的文件不返回）。 */
export function parsePatch(text: string): PatchFile[] {
  const out: PatchFile[] = []
  let cur: PatchFile | null = null
  let curHunk: PatchHunk | null = null
  for (const raw of text.split("\n")) {
    if (raw === "") continue // 尾随/间隔空行（hunk 内空上下文行为 " " 前缀，不会被吞）
    const line = raw.length > 0 && raw[raw.length - 1] === "\r" ? raw.slice(0, -1) : raw // 容错 CRLF
    const m = HUNK_RE.exec(line)
    if (m) {
      curHunk = { startA: Number(m[1]), lines: [] }
      if (!cur) {
        // 无文件头直接出现 hunk（头可省略）：匿名文件条目
        cur = { oldPath: undefined, newPath: undefined, isNew: false, hunks: [] }
        out.push(cur)
      }
      cur.hunks.push(curHunk)
      continue
    }
    if (line.startsWith("--- ")) {
      curHunk = null
      cur = { oldPath: line.slice(4), newPath: undefined, isNew: false, hunks: [] }
      out.push(cur)
      continue
    }
    if (line.startsWith("+++ ")) {
      curHunk = null
      if (!cur) {
        cur = { oldPath: undefined, newPath: undefined, isNew: false, hunks: [] }
        out.push(cur)
      }
      cur.newPath = line.slice(4)
      cur.isNew = cur.oldPath === "/dev/null" || cur.oldPath === "a/dev/null"
      continue
    }
    if (!curHunk) continue // 文件头外的元数据（diff --git/index/mode 等）跳过
    if (line.startsWith("\\")) continue // `\ No newline at end of file` 标记（按无尾换行容错）
    if (line.startsWith("+")) curHunk.lines.push({ kind: 1, text: line.slice(1) })
    else if (line.startsWith("-")) curHunk.lines.push({ kind: -1, text: line.slice(1) })
    else curHunk.lines.push({ kind: 0, text: line.startsWith(" ") ? line.slice(1) : line })
  }
  return out.filter((f) => f.hunks.length > 0)
}

/** 在文件行数组中查找补丁行块（上下文+删除）的精确匹配，返回起始下标（-1=未找到）。 */
function findBlock(lines: string[], block: string[], startSearch: number): number {
  if (!block.length || !lines.length) return -1
  const anchor = block[0]
  let found = -1
  let candidates = 0
  for (let i = Math.max(0, startSearch); i <= lines.length - block.length; i++) {
    if (lines[i] !== anchor) continue
    if (++candidates > 50) break // 锚点出现过多（如空行）：放弃避免拖慢
    let ok = true
    for (let k = 1; k < block.length; k++) {
      if (lines[i + k] !== block[k]) {
        ok = false
        break
      }
    }
    if (ok) {
      found = i
      break
    }
  }
  return found
}

/**
 * 应用补丁（单个文件）。oldText 为当前文件内容（新文件传 ""，或 isNew 且内容为空时按新建处理）。
 * 失败时返回失败 hunk 序号与原因（整体不应用，调用方不得落盘）。
 */
export function applyPatch(oldText: string, patch: PatchFile): ApplyPatchResult {
  const newFile = patch.isNew && oldText === ""
  const trailingNL = oldText.endsWith("\n")
  const fileLines = splitLines(oldText)
  let lines = fileLines
  const applied: AppliedHunk[] = []
  let cumOffset = 0 // 先前 hunk 造成的行数偏移（纯新增定位用）

  for (let hi = 0; hi < patch.hunks.length; hi++) {
    const hunk = patch.hunks[hi]
    const ctxAndDel = hunk.lines.filter((l) => l.kind !== 1) // 上下文 + 删除（待匹配块）
    const dels = ctxAndDel.filter((l) => l.kind === -1)
    const adds = hunk.lines.filter((l) => l.kind === 1)

    if (newFile) {
      if (dels.length > 0 || ctxAndDel.some((l) => l.kind === 0)) {
        return { ok: false, hunkIndex: hi, error: "新建文件补丁仅允许新增行（+），不应包含删除/上下文行" }
      }
      lines = [...lines, ...adds.map((l) => l.text)]
      applied.push({ index: hi, line: lines.length - adds.length + 1, delta: adds.length })
      cumOffset += adds.length
      continue
    }

    if (dels.length === 0) {
      // 纯新增：按头行号（叠加先前偏移）定位，带上下文则校验
      let pos = hunk.startA >= 1 ? hunk.startA - 1 + cumOffset : lines.length
      pos = Math.max(0, Math.min(pos, lines.length))
      const ctxLines = ctxAndDel.map((l) => l.text)
      let insertAt = pos
      if (ctxLines.length > 0) {
        const found = findBlock(lines, ctxLines, Math.max(0, pos - PATCH_FUZZ_LINES))
        if (found < 0) {
          return { ok: false, hunkIndex: hi, error: "新增行上下文未匹配（定位行号附近无对应内容）" }
        }
        insertAt = found + ctxLines.length
      }
      const newLines = [...lines.slice(0, insertAt), ...adds.map((l) => l.text), ...lines.slice(insertAt)]
      lines = newLines
      applied.push({ index: hi, line: insertAt + 1, delta: adds.length })
      cumOffset += adds.length
      continue
    }

    // 含删除行：以块为整体匹配，上下文不符时裁剪头/尾上下文重试（fuzz 只裁上下文，删除行必在块内）
    const firstDel = ctxAndDel.findIndex((l) => l.kind === -1)
    const lastDel = ctxAndDel.length - 1 - [...ctxAndDel].reverse().findIndex((l) => l.kind === -1)
    const maxFuzzHead = Math.min(PATCH_FUZZ_LINES, firstDel)
    const maxFuzzTail = Math.min(PATCH_FUZZ_LINES, ctxAndDel.length - 1 - lastDel)
    let matched: { at: number; fuzzHead: number; fuzzTail: number } | null = null
    outer: for (let fuzzHead = 0; fuzzHead <= maxFuzzHead; fuzzHead++) {
      for (let fuzzTail = 0; fuzzTail <= maxFuzzTail; fuzzTail++) {
        const block = ctxAndDel.slice(fuzzHead, ctxAndDel.length - fuzzTail)
        const found = findBlock(lines, block.map((l) => l.text), 0)
        if (found >= 0) {
          matched = { at: found, fuzzHead, fuzzTail }
          break outer
        }
      }
    }
    if (!matched) {
      return {
        ok: false,
        hunkIndex: hi,
        error: `删除行「${dels[0].text.slice(0, 60)}」附近未匹配到上下文（内容可能已变化，请先 read 当前文件）`,
      }
    }

    // 应用：块内按补丁顺序重放——上下文保留、删除跳过、新增插入原位（顺序保持）
    const { at, fuzzHead } = matched
    const out = lines.slice(0, at)
    let cursor = 0
    for (const l of hunk.lines) {
      if (l.kind === 0) {
        if (cursor >= fuzzHead && cursor < ctxAndDel.length - matched.fuzzTail) out.push(l.text)
        cursor++
      } else if (l.kind === -1) {
        cursor++
      } else {
        out.push(l.text)
      }
    }
    out.push(...lines.slice(at + (ctxAndDel.length - fuzzHead - matched.fuzzTail)))
    // 首个变更行号：块起点 + 首个非上下文行在 hunk 中的偏移 − 已裁头上下文
    const firstChange = hunk.lines.findIndex((l) => l.kind !== 0)
    const changeAt = firstChange >= 0 ? at + Math.max(0, firstChange - fuzzHead) : at
    lines = out
    applied.push({ index: hi, line: changeAt + 1, delta: adds.length - dels.length })
    cumOffset += adds.length - dels.length
  }

  const result = lines.length === 0 ? "" : lines.join("\n") + (trailingNL || newFile ? "\n" : "")
  return { ok: true, result, applied }
}
