/**
 * 文件工作台 · 终端面板的纯逻辑：ANSI SGR 解析、控制字符（`\r` / `\b` / `\n` / `\t`）处理、
 * 输出缓冲与命令历史。
 *
 * 为什么与视图分开：终端输出解析是**有状态机**——一次颜色设置可能被轮询分片截断
 * （`"\x1b[3"` + `"1m"` 两片才拼成红色），`\r` 要回到行首原地覆盖而不是新起一行，
 * 无换行的长输出要按列截断（否则一行撑爆横向布局）。这些规则挂在 DOM 上就没法单测
 * （web 包无 jsdom），因此独立成模块：本文件只做文本 → 结构化行/run 的变换，
 * `terminal.ts` 负责把 run 映射成带 class 的 span 并增量追加进 DOM。
 */

/* ------------------------------ ANSI SGR ------------------------------ */

/** SGR 前景色语义名（30-37 普通 / 90-97 亮色）；具体颜色由 CSS 类决定，核心只认语义。 */
export type AnsiColor =
  | "black"
  | "red"
  | "green"
  | "yellow"
  | "blue"
  | "magenta"
  | "cyan"
  | "white"
  | "brightBlack"
  | "brightRed"
  | "brightGreen"
  | "brightYellow"
  | "brightBlue"
  | "brightMagenta"
  | "brightCyan"
  | "brightWhite"

const NORMAL_COLORS: AnsiColor[] = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"]
const BRIGHT_COLORS: AnsiColor[] = ["brightBlack", "brightRed", "brightGreen", "brightYellow", "brightBlue", "brightMagenta", "brightCyan", "brightWhite"]

/** SGR 码 → 前景色（30-37 与 90-97 同构，用下标映射，避免写 16 行分支）。 */
export function fgColorOf(code: number): AnsiColor | null {
  if (code >= 30 && code <= 37) return NORMAL_COLORS[code - 30] ?? null
  if (code >= 90 && code <= 97) return BRIGHT_COLORS[code - 90] ?? null
  return null
}

/** 一段同样式文本（视图层据此生成一个 span）。 */
export interface AnsiRun {
  text: string
  color: AnsiColor | null
  bold: boolean
}

/** 解析状态：颜色/粗体跨调用保持（分片轮询），`pending` 存被分片截断的半截转义序列。 */
export interface AnsiState {
  color: AnsiColor | null
  bold: boolean
  pending: string
}

export function newAnsiState(): AnsiState {
  return { color: null, bold: false, pending: "" }
}

/** 单个转义序列的识别结果；`null` = 序列不完整（被分片截断），调用方需留到下一片。 */
function matchEscape(text: string, i: number): { len: number; sgr: boolean; params: string } | null {
  const next = text[i + 1]
  if (next === undefined) return null
  if (next === "[") {
    // CSI：参数字节 0x30-0x3f、中间字节 0x20-0x2f，最终字节 0x40-0x7e
    for (let j = i + 2; j < text.length; j++) {
      const code = text.charCodeAt(j)
      if (code >= 0x40 && code <= 0x7e) return { len: j - i + 1, sgr: text[j] === "m", params: text.slice(i + 2, j) }
    }
    return null
  }
  if (next === "]") {
    // OSC（改标题/超链接等）：到 BEL 或 ST（ESC \）结束，内容整体丢弃
    for (let j = i + 2; j < text.length; j++) {
      if (text[j] === "\x07") return { len: j - i + 1, sgr: false, params: "" }
      if (text[j] === "\x1b" && text[j + 1] === "\\") return { len: j - i + 2, sgr: false, params: "" }
    }
    return null
  }
  // 双字符序列（如 ESC ( B 之外的 ESC M）：整体丢弃
  return { len: 2, sgr: false, params: "" }
}

/** 应用一条 SGR 参数串（`\x1b[<params>m`），就地推进状态。未知码忽略——不能误判成颜色。 */
function applySgr(params: string, state: AnsiState): void {
  if (params === "" || params === "0") {
    state.color = null
    state.bold = false
    return
  }
  const parts = params.split(";")
  for (let i = 0; i < parts.length; i++) {
    const code = Number(parts[i])
    if (!Number.isFinite(code)) continue
    if (code === 0) {
      state.color = null
      state.bold = false
    } else if (code === 1) state.bold = true
    else if (code === 22) state.bold = false
    else if (code === 39) state.color = null
    else if (code === 38 || code === 48) {
      // 256 色 / 真彩（38;5;n 或 38;2;r;g;b）：不支持但要跳过它自己的参数，
      // 否则 `38;5;31` 里的 31 会被当成普通色号、把后面的文本染红
      const mode = Number(parts[i + 1])
      i += mode === 2 ? 4 : mode === 5 ? 2 : 0
    } else {
      const c = fgColorOf(code)
      if (c) state.color = c
    }
  }
}

/**
 * 解析一段终端文本：剥离转义序列、按颜色/粗体切分 run。
 *
 * 不处理 `\r`/`\b`/`\n`/`\t`（那是 `TermBuffer` 的职责，它会先按控制字符切段再调本函数），
 * 这里的 C0 控制字符一律丢弃（含泄漏进来的回车换行），避免把不可见字符渲染成豆腐块。
 */
export function applyAnsi(text: string, state: AnsiState = newAnsiState()): { runs: AnsiRun[]; state: AnsiState } {
  const runs: AnsiRun[] = []
  let buf = ""
  let src = text
  if (state.pending) {
    src = state.pending + src
    state.pending = ""
  }
  const flush = (): void => {
    if (!buf) return
    runs.push({ text: buf, color: state.color, bold: state.bold })
    buf = ""
  }
  let i = 0
  while (i < src.length) {
    const ch = src[i]!
    if (ch === "\x1b") {
      flush()
      const seq = matchEscape(src, i)
      if (!seq) {
        state.pending = src.slice(i) // 序列被分片截断：留到下一片再拼
        break
      }
      if (seq.sgr) applySgr(seq.params, state)
      i += seq.len
      continue
    }
    const code = src.charCodeAt(i)
    if (code < 0x20 || code === 0x7f) {
      i++ // 控制字符（含 \r\n\t\b，正常路径下由 TermBuffer 拦掉）
      continue
    }
    buf += ch
    i++
  }
  flush()
  return { runs, state }
}

/* ------------------------------ 输出缓冲 ------------------------------ */

/** 单元格：终端是「按列覆盖」的模型，`\r` 回写要求能按列改字，所以行内先按字符存。 */
interface Cell {
  ch: string
  color: AnsiColor | null
  bold: boolean
}

/** 渲染单位：一行的纯文本 + 同样式切片 + 是否被截断。 */
export interface TermLine {
  text: string
  runs: AnsiRun[]
  truncated: boolean
}

export interface TermBufferOptions {
  /** 单行最大列数：超出即截断（无换行的日志/超长 JSON 会撑爆横向布局） */
  maxCols?: number
  /** 回溯行数上限：超出的最旧行丢弃，视图按 `trimmedLines` 同步裁 DOM */
  maxLines?: number
}

export const TERM_MAX_COLS = 2000
export const TERM_MAX_LINES = 1000

/**
 * 终端输出缓冲：把任意分片的原始输出（含 ANSI 与 `\r`/`\b`）累积成可渲染的行。
 *
 * 行模型：已结束的行冻结成 `TermLine`（同一行只切一次 run），**正在写的那一行**按单元格保存
 * ——只有它需要随机回写（`\r` 覆盖、`\b` 退格），冻结后就没有随机访问需求，
 * 这样既准确又不会为一个 2000 列的行常驻上千个对象。
 */
export class TermBuffer {
  private frozen: TermLine[] = []
  private cells: Cell[] = []
  private col = 0
  private trunc = false
  private dropped = 0
  private ansi: AnsiState = newAnsiState()
  private readonly maxCols: number
  private readonly maxLines: number

  constructor(opts: TermBufferOptions = {}) {
    this.maxCols = Math.max(2, opts.maxCols ?? TERM_MAX_COLS)
    this.maxLines = Math.max(2, opts.maxLines ?? TERM_MAX_LINES)
  }

  /** 已被丢弃的最旧行数：视图靠它判断「DOM 顶部要同步裁掉几行」。 */
  get trimmedLines(): number {
    return this.dropped
  }

  /** 追加输出分片（`\n` 结行、`\r` 回行首、`\b` 退格、`\t` 按 8 列制表）。 */
  write(chunk: string): void {
    if (!chunk) return
    let pending = ""
    const flush = (): void => {
      if (!pending) return
      this.emit(applyAnsi(pending, this.ansi).runs)
      pending = ""
    }
    // 按码点迭代（不按 UTF-16 单元）：emoji 等代理对不会被 \b/\r 从中间劈开
    for (const ch of chunk) {
      if (ch === "\n") {
        flush()
        this.freeze()
      } else if (ch === "\r") {
        flush()
        this.col = 0
      } else if (ch === "\b") {
        flush()
        this.col = Math.max(0, this.col - 1)
      } else if (ch === "\t") {
        flush()
        const stop = Math.min(this.maxCols, (Math.floor(this.col / 8) + 1) * 8)
        this.emit([{ text: " ".repeat(stop - this.col), color: this.ansi.color, bold: this.ansi.bold }])
      } else {
        pending += ch
      }
    }
    flush()
  }

  /** 全部行（含正在写的最后一行）；始终至少一行，视图按此增量渲染。 */
  lines(): TermLine[] {
    return [...this.frozen, this.snapshot()]
  }

  /** 纯文本快照（测试 / 复制用）。 */
  text(): string {
    return this.lines()
      .map((l) => l.text)
      .join("\n")
  }

  clear(): void {
    this.frozen = []
    this.cells = []
    this.col = 0
    this.trunc = false
    this.dropped = 0
    this.ansi = newAnsiState()
  }

  /** 按列写入 run（已存在的列覆盖，超出的列追加）——`\r` 后的回写即走这里。 */
  private emit(runs: AnsiRun[]): void {
    for (const r of runs) {
      for (const ch of r.text) {
        if (this.col >= this.maxCols) {
          this.trunc = true // 该行已满：后续字符丢弃，行上标记截断
          return
        }
        const cell: Cell = { ch, color: r.color, bold: r.bold }
        if (this.col < this.cells.length) this.cells[this.col] = cell
        else this.cells.push(cell)
        this.col++
      }
    }
  }

  private freeze(): void {
    this.frozen.push(this.snapshot())
    this.cells = []
    this.col = 0
    this.trunc = false
    const over = this.frozen.length - this.maxLines
    if (over > 0) {
      this.frozen.splice(0, over)
      this.dropped += over
    }
  }

  /** 单元格 → 行（相邻同样式合并成 run，渲染时 span 数量与颜色变化次数同阶）。 */
  private snapshot(): TermLine {
    const runs: AnsiRun[] = []
    let text = ""
    for (const c of this.cells) {
      text += c.ch
      const last = runs[runs.length - 1]
      if (last && last.color === c.color && last.bold === c.bold) last.text += c.ch
      else runs.push({ text: c.ch, color: c.color, bold: c.bold })
    }
    return { text, runs, truncated: this.trunc }
  }
}

/* ------------------------------ 命令历史 ------------------------------ */

/** 历史条数上限（localStorage `gebai.ui.termHistory` 同此口径）。 */
export const TERM_HISTORY_MAX = 100

/**
 * 追加一条命令历史（不可变返回）。
 * 规则同 shell：空白提交不入历史、重复命令提到末尾（不产生第二条），超出上限丢最旧。
 */
export function pushHistory(list: readonly string[], raw: string, max = TERM_HISTORY_MAX): string[] {
  const cmd = raw.trim()
  const trimmed = list.slice(-max)
  if (!cmd) return trimmed
  const out = trimmed.filter((c) => c !== cmd)
  out.push(cmd)
  return out.length > max ? out.slice(out.length - max) : out
}

/* ------------------------------ 路径显示 ------------------------------ */

/** 路径尾段（标题栏与提示符只显示尾段，完整路径放 `title`）。 */
export function pathTail(p: string): string {
  const t = p.replace(/[\\/]+$/, "")
  const seg = t.split(/[\\/]/).pop() ?? ""
  return seg || p || "."
}

/** 是否 Windows 风格绝对路径（盘符开头）——大小写不敏感比较只对这种路径成立。 */
function isWinLike(p: string): boolean {
  return /^[a-z]:\//i.test(p)
}

/**
 * 路径等价比较：判断「终端当前是否已在跟随的目标目录」。
 * 分隔符统一；Windows 盘符路径额外忽略大小写（`C:\A` 与 `c:\a` 是同一目录），POSIX 保持大小写敏感。
 */
export function samePath(a: string, b: string): boolean {
  const norm = (p: string) => p.replace(/\\/g, "/").replace(/\/+$/, "")
  const na = norm(a)
  const nb = norm(b)
  if (na === nb) return true
  return isWinLike(na) && isWinLike(nb) && na.toLowerCase() === nb.toLowerCase()
}
