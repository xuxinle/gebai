/**
 * 文件工作台 · 终端搜索选项（纯函数，带单测）。
 *
 * 三件套（区分大小写 / 全词 / 正则）与「第 n/m 项」计数对齐 VSCode 的终端查找：
 * 只会「找下一个」的查找框在长输出里几乎不可用——不知道自己找的是第几处、也不知道有没有匹配。
 *
 * **高亮必须显式给 decoration 取值**：xterm 的 search addon 只在传入 `decorations` 时才建
 * 命中装饰（不传就只移动选区，滚动缓冲里看不到任何标记），且未给的字段是"不画"而不是"用默认色"，
 * 于是「搜索没有高亮」正是漏传该参数的必然结果。
 */

export interface TermSearchOptions {
  caseSensitive: boolean
  regex: boolean
  wholeWord: boolean
}

export type SearchToggle = keyof TermSearchOptions

export const DEFAULT_SEARCH_OPTIONS: TermSearchOptions = { caseSensitive: false, regex: false, wholeWord: false }

/** 查找框内三个开关（顺序与 VSCode 一致：Aa → ab → .*）。 */
export const SEARCH_TOGGLES: Array<{ key: SearchToggle; label: string; title: string }> = [
  { key: "caseSensitive", label: "Aa", title: "区分大小写" },
  { key: "wholeWord", label: "ab", title: "全词匹配" },
  { key: "regex", label: ".*", title: "正则表达式" },
]

export function toggleSearchOption(cur: TermSearchOptions, key: SearchToggle): TermSearchOptions {
  return { ...cur, [key]: !cur[key] }
}

/** 解析持久化的开关（脏数据回落默认）。 */
export function parseSearchOptions(raw: unknown): TermSearchOptions {
  let obj: Record<string, unknown> = {}
  if (typeof raw === "string" && raw) {
    try {
      const parsed = JSON.parse(raw)
      if (parsed && typeof parsed === "object") obj = parsed as Record<string, unknown>
    } catch {
      /* 脏数据：整套回落 */
    }
  } else if (raw && typeof raw === "object") {
    obj = raw as Record<string, unknown>
  }
  const bool = (v: unknown, d: boolean) => (typeof v === "boolean" ? v : d)
  return {
    caseSensitive: bool(obj.caseSensitive, DEFAULT_SEARCH_OPTIONS.caseSensitive),
    regex: bool(obj.regex, DEFAULT_SEARCH_OPTIONS.regex),
    wholeWord: bool(obj.wholeWord, DEFAULT_SEARCH_OPTIONS.wholeWord),
  }
}

export interface SearchDecorations {
  matchBackground: string
  matchOverviewRuler: string
  activeMatchBackground: string
  activeMatchBorder: string
  activeMatchColorOverviewRuler: string
}

/** 由配色模块给出的三个色（见 `term-theme.searchMatchColors`）组装 decoration 取值。 */
export function searchDecorations(colors: { matchBackground: string; activeMatchBackground: string; activeMatchBorder: string }): SearchDecorations {
  return {
    matchBackground: colors.matchBackground,
    matchOverviewRuler: colors.activeMatchBorder,
    activeMatchBackground: colors.activeMatchBackground,
    activeMatchBorder: colors.activeMatchBorder,
    activeMatchColorOverviewRuler: colors.activeMatchBorder,
  }
}

/**
 * 查找框右侧的状态文案：空关键词不占位、有命中显示「第 n/m 项」、
 * 有命中但当前未选中（xterm 回 -1）显示总处数、零命中显示「无匹配」。
 */
export function searchStatusText(result: { resultIndex: number; resultCount: number } | null, query: string): string {
  if (!query) return ""
  if (!result || result.resultCount <= 0) return "无匹配"
  if (result.resultIndex >= 0) return `${result.resultIndex + 1}/${result.resultCount}`
  return `${result.resultCount} 处`
}
