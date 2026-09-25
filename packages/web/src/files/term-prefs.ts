/**
 * 文件工作台 · 终端偏好（纯函数，带单测）。
 *
 * 存储分两处，理由是**跨实现一致**：字号与「跟随当前根」是 PTY 与降级（管道式）两个终端
 * 共有的偏好，沿用它们原有的键（`gebai.ui.termFontSize` / `gebai.ui.termFollowRoot`），
 * 换实现不该换字号；其余只有 PTY 版式有意义的选项（行高、光标样式/闪烁、选中即复制、
 * Ctrl+滚轮缩放）收在一个 JSON 键里。
 *
 * 解析一律**容错**：单项非法只回落该项默认值，不因为一个脏字段把整套偏好丢掉
 * （旧版本写入的值、手工改过的 localStorage、隐私模式下读不到，都不该让终端开不起来）。
 */

/** 字号键（与降级实现同口径：两版式共用一份字号）。 */
export const FONT_SIZE_KEY = "gebai.ui.termFontSize"
/** 跟随当前根键（同上）。 */
export const FOLLOW_ROOT_KEY = "gebai.ui.termFollowRoot"
/** PTY 专有选项（JSON）。 */
export const PREFS_KEY = "gebai.ui.termPrefs"

export type CursorStyle = "bar" | "block" | "underline"

export interface TermPrefs {
  /** 字号（8–28，与 xterm 的合法区间一致）。 */
  fontSize: number
  /** 行高倍数（1–2）。 */
  lineHeight: number
  cursorStyle: CursorStyle
  cursorBlink: boolean
  /** 选中即复制（VSCode 的 terminal.integrated.copyOnSelection，两端默认关：Linux 外的习惯）。 */
  copyOnSelection: boolean
  /** Ctrl+滚轮缩放字号（浏览器里 Ctrl+滚轮是整页缩放，接管它比放任更合理，故默认开）。 */
  wheelZoom: boolean
  /** 切换工作台根时在当前终端补一条 cd。 */
  followRoot: boolean
}

export const DEFAULT_PREFS: TermPrefs = {
  fontSize: 13,
  lineHeight: 1.25,
  cursorStyle: "bar",
  cursorBlink: true,
  copyOnSelection: false,
  wheelZoom: true,
  followRoot: true,
}

/** 菜单里循环切换的取值表（顺序即切换顺序）。 */
export const LINE_HEIGHT_STEPS = [1, 1.25, 1.5]
export const CURSOR_STYLES: CursorStyle[] = ["bar", "block", "underline"]

export const FONT_SIZE_MIN = 8
export const FONT_SIZE_MAX = 28

export function clampFontSize(n: unknown): number {
  // null/空串是「没有这个值」而不是 0：Number(null) 与 Number("") 都是 0，
  // 不管这一层的话，缺少键（首次打开、刚清过 localStorage）会得到最小字号 8。
  if (n === null || n === undefined || n === "") return DEFAULT_PREFS.fontSize
  const v = Math.round(Number(n))
  if (!Number.isFinite(v)) return DEFAULT_PREFS.fontSize
  return Math.max(FONT_SIZE_MIN, Math.min(FONT_SIZE_MAX, v))
}

export function clampLineHeight(n: unknown): number {
  if (n === null || n === undefined || n === "") return DEFAULT_PREFS.lineHeight
  const v = Number(n)
  if (!Number.isFinite(v)) return DEFAULT_PREFS.lineHeight
  return Math.max(1, Math.min(2, Math.round(v * 100) / 100))
}

/** 取循环表里的下一个值（菜单项「切换」用；当前值不在表里时回到第一个）。 */
export function nextInCycle<T>(list: readonly T[], cur: T): T {
  const i = list.indexOf(cur)
  return list[(i + 1) % list.length]!
}

function bool(v: unknown, fallback: boolean): boolean {
  return typeof v === "boolean" ? v : fallback
}

/** 解析偏好（`raw` 为 JSON 键的原始内容，`legacy` 为两个旧键的原始值）。 */
export function parsePrefs(raw: unknown, legacy: { fontSize?: unknown; followRoot?: unknown } = {}): TermPrefs {
  let obj: Record<string, unknown> = {}
  if (typeof raw === "string" && raw) {
    try {
      const parsed = JSON.parse(raw)
      if (parsed && typeof parsed === "object") obj = parsed as Record<string, unknown>
    } catch {
      /* 脏数据：整套回落默认 */
    }
  } else if (raw && typeof raw === "object") {
    obj = raw as Record<string, unknown>
  }
  // 旧键优先于 JSON 里的同名字段？不：JSON 是当前位置，旧键只在新键缺失时兜底
  const fontSize = obj.fontSize === undefined ? (legacy.fontSize === undefined ? DEFAULT_PREFS.fontSize : clampFontSize(legacy.fontSize)) : clampFontSize(obj.fontSize)
  const rawStyle = typeof obj.cursorStyle === "string" ? obj.cursorStyle : ""
  const cursorStyle = (CURSOR_STYLES as string[]).includes(rawStyle) ? (rawStyle as CursorStyle) : DEFAULT_PREFS.cursorStyle
  const followRaw = legacy.followRoot
  const followRoot =
    obj.followRoot === undefined
      ? followRaw === undefined
        ? DEFAULT_PREFS.followRoot
        : !(followRaw === "0" || followRaw === 0 || followRaw === false)
      : bool(obj.followRoot, DEFAULT_PREFS.followRoot)
  return {
    fontSize,
    lineHeight: obj.lineHeight === undefined ? DEFAULT_PREFS.lineHeight : clampLineHeight(obj.lineHeight),
    cursorStyle,
    cursorBlink: bool(obj.cursorBlink, DEFAULT_PREFS.cursorBlink),
    copyOnSelection: bool(obj.copyOnSelection, DEFAULT_PREFS.copyOnSelection),
    wheelZoom: bool(obj.wheelZoom, DEFAULT_PREFS.wheelZoom),
    followRoot,
  }
}

/** 序列化（全量；字号与跟随根另写各自的键，供降级实现读同一份值）。 */
export function serializePrefs(p: TermPrefs): string {
  return JSON.stringify(p)
}
