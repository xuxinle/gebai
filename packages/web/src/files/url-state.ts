/**
 * 工作台地址栏同步（`/files` 的 URL = 界面状态）。
 *
 * 为什么需要：文件工作台是独立页面，进目录/开文件若不写进地址栏，**刷新就回落到默认根**、
 * 也**没法用浏览器后退**回到上一个目录（IDE 里这相当于"布局状态不持久"）。把 root / 打开的
 * 目录 / 当前文件 / 行号同步到 query，就能：刷新恢复原处、后退前进在目录间穿行、复制链接分享位置。
 *
 * 与 `deeplink.ts` 的分工：那边负责**解析并落位**（含根推断规则），这边只负责**写回**与
 * 从已有参数恢复；两边共用同一套参数名（root/path/line/session），互不重复实现匹配逻辑。
 *
 * 写入策略：目录切换用 pushState（后退能回到上一个目录），同目录内打开文件用 replaceState
 * （否则 Ctrl+P 连开几个文件后要按很多次后退才回得去）。节流合并同期内的连续变更。
 */

export interface UrlState {
  root: string
  /** 当前定位的路径（文件或目录；相对当前根） */
  path: string
  /** 打开文件时定位的行号（1 起） */
  line?: number
  session?: string
}

/**
 * 构造新的 query：保留与本页无关但需要透传的参数（如 session / diffRoot / gb_style），
 * 只更新 root / path / line 三件套。
 */
export function buildQuery(current: string, st: UrlState): string {
  const p = new URLSearchParams(current)
  p.delete("gb_root") // 旧参数名，避免与新 root 并存产生歧义
  if (st.root) p.set("root", st.root)
  else p.delete("root")
  if (st.path) p.set("path", st.path)
  else p.delete("path")
  if (st.path && st.line && st.line > 1) p.set("line", String(st.line))
  else p.delete("line")
  return p.toString()
}

/** 从当前 query 解析出待恢复的状态（无则空）。 */
export function parseUrlState(search: string): Partial<UrlState> {
  const p = new URLSearchParams(search)
  const line = Number(p.get("line"))
  return {
    root: p.get("root") ?? undefined,
    path: p.get("path") ?? undefined,
    line: Number.isFinite(line) && line > 1 ? line : undefined,
    session: p.get("session") ?? undefined,
  }
}

/**
 * URL 同步器：合并同期内的多次变更（一次提交一个历史条目），
 * 并在 popstate 时把外部状态交回调用方。
 */
export function createUrlSync(opts: {
  /** 取当前界面状态（以主区标签 + 资源管理器选中项为准） */
  read: () => UrlState
  /** 浏览器前进/后退时恢复 */
  onPop: (st: Partial<UrlState>) => void | Promise<void>
  /** 地址栏基准（默认当前 pathname） */
  pathname?: string
}): {
  /** 记一条历史（目录切换等"值得后退"的动作） */
  push: () => void
  /** 就地替换（同目录内打开文件） */
  replace: () => void
  dispose: () => void
} {
  let timer: number | null = null
  let mode: "push" | "replace" = "replace"

  function commit(): void {
    timer = null
    const st = opts.read()
    const q = buildQuery(location.search, st)
    const url = `${opts.pathname ?? location.pathname}${q ? `?${q}` : ""}`
    if (`${location.pathname}${location.search}` === url) return // 无变化不写历史
    if (mode === "push") history.pushState({ fw: 1 }, "", url)
    else history.replaceState({ fw: 1 }, "", url)
  }

  const schedule = (m: "push" | "replace") => {
    // push 优先级更高：同期内既有目录切换又有文件打开时，只留一条可后退记录
    if (m === "push" || timer === null) mode = m
    if (timer !== null) window.clearTimeout(timer)
    timer = window.setTimeout(commit, 60)
  }

  const onPopState = (): void => {
    const st = parseUrlState(location.search)
    void opts.onPop(st)
  }
  window.addEventListener("popstate", onPopState)

  return {
    push: () => schedule("push"),
    replace: () => schedule("replace"),
    dispose: () => {
      window.removeEventListener("popstate", onPopState)
      if (timer !== null) window.clearTimeout(timer)
    },
  }
}
