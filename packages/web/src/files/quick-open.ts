/**
 * 「快速打开」（VSCode Quick Open 同款）：Ctrl+P 弹出输入框，边打边模糊筛文件名，↑↓ 选、Enter 开。
 *
 * 组成：本条（面板与交互）+ `quick-open-core.ts`（匹配与排序，纯函数、有单测）+ `recents.ts`
 * （最近打开的文件，空查询时显示）。
 *
 * 索引：一次 `GET /api/v1/fs/files` 拿回 root 下全部文件路径，之后**全部在前端筛**
 * （逐键请求服务端既不现实也不快）；按根缓存，TTL 到期或收到文件变更事件时重取。
 *
 * 键位：面板自己管 ↑↓/Enter/Esc（焦点在它的输入框里，走不了 document 分发——那里输入框内的
 * 按键默认归输入框自己）；键位表里只登记（`owned: false`）以免帮助 UI 漏掉。
 */
import { popKeyScope, pushEscScope } from "../keymap"
import type { FsApi } from "./api"
import { rankPaths, type QuickOpenItem } from "./quick-open-core"
import { recentFiles } from "./recents"
import { h, icon, toast } from "./ui"
import { fileIcon } from "./file-icons"

/** 索引缓存：按根存，TTL 内直接复用（打开面板不等待网络）。 */
const indexCache = new Map<string, { at: number; files: string[]; truncated: boolean }>()
const INDEX_TTL_MS = 60_000

/** 让缓存失效（文件变更事件、保存后调用）：下次打开面板重新取索引。 */
export function invalidateQuickOpenIndex(root?: string): void {
  if (root) indexCache.delete(root)
  else indexCache.clear()
}

export interface QuickOpenDeps {
  api: FsApi
  /** 当前根（索引与「最近打开」都按根隔离）。 */
  root: () => string
  /** 打开文件；`preview` 为 true 时复用预览标签（VSCode 单击/Enter 的语义）。 */
  open: (root: string, path: string, opts: { preview: boolean; line?: number }) => void | Promise<void>
}

/** 已打开的面板（VSCode 里连按 Ctrl+P 是在面板内往上选，不是再开一层）。 */
let active: { focus: () => void } | null = null

/** 面板是否已打开——键位表用它决定 Ctrl+P 要不要让给面板自己处理（VSCode：Ctrl+P 在面板内 = 上一项）。 */
export function isQuickOpenOpen(): boolean {
  return active !== null
}

/** 打开面板（已在打开态时只聚焦，不叠第二层）。 */
export function openQuickOpen(deps: QuickOpenDeps): void {
  if (active) {
    active.focus()
    return
  }
  const root = deps.root()
  const input = h("input", { class: "fw-qo-input", placeholder: "按文件名搜索（支持缩写，如 smain）", spellcheck: "false", autocomplete: "off" })
  const list = h("div", { class: "fw-qo-list" })
  const status = h("div", { class: "fw-qo-status" })
  const card = h("div", { class: "fw-qo" }, [h("div", { class: "fw-qo-head" }, [icon("search", 14), input]), list, status])
  const overlay = h("div", { class: "fw-overlay fw-qo-overlay" }, [card])

  let files: string[] = []
  let items: QuickOpenItem[] = []
  let selected = 0
  let indexReady = false
  let truncated = false

  /** 渲染结果行（命中字符高亮 —— VSCode 靠它让用户一眼确认「匹配到哪几个字」）。 */
  const render = (): void => {
    list.replaceChildren()
    if (!items.length) {
      list.appendChild(h("div", { class: "fw-qo-empty", text: input.value.trim() ? "无匹配文件" : "还没有最近打开的文件" }))
      status.textContent = indexReady ? (truncated ? "文件索引已达上限，结果可能不全" : "") : "正在建立文件索引…"
      return
    }
    items.forEach((it, i) => {
      const slash = it.path.lastIndexOf("/")
      const dir = slash < 0 ? "" : it.path.slice(0, slash + 1)
      const name = slash < 0 ? it.path : it.path.slice(slash + 1)
      // 高亮按**整路径**下标切分：目录段与文件名共用一套下标，切段后各自偏移
      const mark = (text: string, offset: number): DocumentFragment => {
        const frag = document.createDocumentFragment()
        const hit = new Set(it.positions.filter((p) => p >= offset && p < offset + text.length).map((p) => p - offset))
        let buf = ""
        let bold = false
        const flush = (): void => {
          if (!buf) return
          frag.appendChild(bold ? h("b", { class: "fw-qo-hit", text: buf }) : document.createTextNode(buf))
          buf = ""
        }
        for (let ci = 0; ci < text.length; ci++) {
          const isHit = hit.has(ci)
          if (isHit !== bold) {
            flush()
            bold = isHit
          }
          buf += text[ci]
        }
        flush()
        return frag
      }
      const row = h("div", { class: `fw-qo-row${i === selected ? " active" : ""}${it.recent ? " recent" : ""}` })
      row.appendChild(h("span", { class: "fw-file-icon" }, [fileIcon(name, 13)]))
      const pathEl = h("span", { class: "fw-qo-path" })
      if (dir) pathEl.appendChild(mark(dir, 0))
      pathEl.appendChild(mark(name, dir.length))
      row.appendChild(pathEl)
      if (it.recent) row.appendChild(h("span", { class: "fw-qo-tag", text: "最近" }))
      row.onmousedown = (e) => e.preventDefault() // 别把焦点从输入框拿走（否则后续按键落到别处）
      row.onclick = () => {
        selected = i
        void accept(false)
      }
      list.appendChild(row)
    })
    const rowH = 22
    list.scrollTop = Math.max(0, (selected - 4) * rowH)
    status.textContent = `${items.length} / ${files.length || items.length} 个文件${truncated ? "（索引已截断）" : ""}`
  }

  const recompute = (): void => {
    const recent = recentFiles(root)
    items = rankPaths(input.value, files, { limit: 200, recent })
    selected = 0
    render()
  }

  /** 打开选中项（或对齐 VSCode：Enter 开预览标签、Ctrl+Enter 固定为常驻标签）。 */
  const accept = async (pin: boolean): Promise<void> => {
    const it = items[selected]
    if (!it) return
    close()
    await deps.open(root, it.path, { preview: !pin })
  }

  let scopeId = ""
  const close = (): void => {
    overlay.remove()
    popKeyScope(scopeId)
    document.removeEventListener("keydown", onDocKey, true)
    active = null
  }

  /** 焦点跑出输入框时（点了别处）仍要能 Esc 关：在 document 捕获阶段兜一层。 */
  const onDocKey = (e: KeyboardEvent): void => {
    if (e.key === "Escape") {
      e.preventDefault()
      close()
    }
  }

  input.oninput = () => recompute()
  input.onkeydown = (e) => {
    if (e.isComposing) return // 中文候选态的 Enter/↑↓ 属于输入法
    if (e.key === "ArrowDown" || (e.key === "n" && e.ctrlKey)) {      e.preventDefault()
      selected = Math.min(selected + 1, items.length - 1)
      render()
      return
    }
    if (e.key === "ArrowUp" || (e.key === "p" && e.ctrlKey)) {
      e.preventDefault()
      selected = Math.max(selected - 1, 0)
      render()
      return
    }
    if (e.key === "PageDown") {
      e.preventDefault()
      selected = Math.min(selected + 10, Math.max(0, items.length - 1))
      render()
      return
    }
    if (e.key === "PageUp") {
      e.preventDefault()
      selected = Math.max(selected - 10, 0)
      render()
      return
    }
    if (e.key === "Enter") {
      e.preventDefault()
      void accept(e.ctrlKey || e.metaKey)
      return
    }
    if (e.key === "Escape") {
      e.preventDefault()
      close()
    }
  }

  overlay.onclick = (e) => {
    if (e.target === overlay) close()
  }
  document.body.appendChild(overlay)
  scopeId = pushEscScope("wb.quickOpen", "关闭快速打开", close)
  document.addEventListener("keydown", onDocKey, true)
  active = { focus: () => input.focus() }
  input.focus()
  recompute()

  // 索引：缓存命中就直接筛，否则取一次（面板先可用并提示"正在建立索引"）
  const cached = indexCache.get(root)
  if (cached && Date.now() - cached.at < INDEX_TTL_MS) {
    files = cached.files
    truncated = cached.truncated
    indexReady = true
    recompute()
    return
  }
  void deps.api
    .files(root)
    .then((res) => {
      files = res.files
      truncated = res.truncated
      indexCache.set(root, { at: Date.now(), files, truncated })
      indexReady = true
      recompute()
      if (res.truncated) toast("文件索引已达上限，快速打开的结果可能不全", "warn", 4000)
    })
    .catch((err) => {
      indexReady = true
      status.textContent = `文件索引获取失败：${(err as Error).message}`
    })
}
