/**
 * 文件工作台 · UI 基础件：DOM 构造、图标、格式化、对话框、右键菜单、轻提示。
 *
 * 自包含（不依赖主界面 `state/ui/markdown` 等模块）——`/files` 是独立入口，
 * 避免为一两个工具函数把整个聊天界面的模块图拖进来；样式令牌与主界面共用（见 files.css）。
 */

/* ------------------------------ DOM ------------------------------ */

type Attrs = Record<string, string | number | boolean | undefined | null | ((e: Event) => void)>

/** 创建元素：`h("div", { class: "x", onclick: fn }, [child, "text"])`。 */
export function h<K extends keyof HTMLElementTagNameMap>(tag: K, attrs: Attrs = {}, children: Array<Node | string | null | undefined> = []): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag)
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue
    if (k === "class") node.className = String(v)
    else if (k === "text") node.textContent = String(v)
    else if (k === "html") node.innerHTML = String(v)
    else if (k === "value" && node instanceof HTMLInputElement) node.value = String(v)
    else if (k === "checked" && node instanceof HTMLInputElement) node.checked = v === true || v === "true"
    else if (k.startsWith("on") && typeof v === "function") node.addEventListener(k.slice(2), v as EventListener)
    else if (k === "style") node.setAttribute("style", String(v))
    else if (v === true) node.setAttribute(k, "")
    else node.setAttribute(k, String(v))
  }
  append(node, children)
  return node
}

/** 追加子节点（字符串转文本节点，null/undefined 跳过）。 */
export function append(parent: Node, children: Array<Node | string | null | undefined>): void {
  for (const c of children) {
    if (c === null || c === undefined) continue
    parent.appendChild(typeof c === "string" ? document.createTextNode(c) : c)
  }
}

export function clear(node: Element): void {
  while (node.firstChild) node.removeChild(node.firstChild)
}

export function qs<T extends Element = HTMLElement>(sel: string, root: ParentNode = document): T {
  const el = root.querySelector<T>(sel)
  if (!el) throw new Error(`元素不存在: ${sel}`)
  return el
}

/* ------------------------------ 图标 ------------------------------ */

const ICONS: Record<string, string> = {
  chevronRight: '<path d="M6 4l4 4-4 4" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>',
  chevronDown: '<path d="M4 6l4 4 4-4" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>',
  folder: '<path d="M2 4.5A1.5 1.5 0 013.5 3h3l1.5 2h5A1.5 1.5 0 0114.5 6.5v6A1.5 1.5 0 0113 14H3.5A1.5 1.5 0 012 12.5v-8z" fill="currentColor" opacity=".85"/>',
  folderOpen: '<path d="M2 12.5V4.5A1.5 1.5 0 013.5 3h3L8 5h4.5A1.5 1.5 0 0114 6.5V8H5.6a1.5 1.5 0 00-1.45 1.1L2 12.5z" fill="currentColor" opacity=".85"/><path d="M4.6 9.1A1 1 0 015.55 8.5h8.2a1 1 0 01.96 1.28l-1.1 3.7A1.5 1.5 0 0112.2 14.5H3.2a1 1 0 01-.96-1.28l2.36-4.12z" fill="currentColor" opacity=".5"/>',
  file: '<path d="M4 2.5A1.5 1.5 0 015.5 1H9l4 4v9.5A1.5 1.5 0 0111.5 16h-6A1.5 1.5 0 014 14.5v-12z" fill="currentColor" opacity=".3"/><path d="M9 1v4h4" fill="none" stroke="currentColor" stroke-width="1.3"/>',
  save: '<path d="M3 3h8.5L14 5.5V15a1 1 0 01-1 1H3a1 1 0 01-1-1V4a1 1 0 011-1z" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M5 3v4h6V3M5 16v-5h6v5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  edit: '<path d="M11.5 2.5l2 2L6 12l-2.5.5L4 10l7.5-7.5z" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/>',
  eye: '<path d="M1 8s2.5-4.5 7-4.5S15 8 15 8s-2.5 4.5-7 4.5S1 8 1 8z" fill="none" stroke="currentColor" stroke-width="1.3"/><circle cx="8" cy="8" r="1.9" fill="none" stroke="currentColor" stroke-width="1.3"/>',
  refresh: '<path d="M13.5 8a5.5 5.5 0 11-1.7-4M13.5 2v4h-4" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  search: '<circle cx="7" cy="7" r="4.2" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M10.2 10.2L14 14" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  git: '<circle cx="4.5" cy="4" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="4.5" cy="12.5" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="12" cy="7.5" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M4.5 6v4.5M6.4 6.4C8 7 10 7 12 7.5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  download: '<path d="M8 2v8m0 0l3-3m-3 3L5 7" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><path d="M2.5 12.5v1A1.5 1.5 0 004 15h8a1.5 1.5 0 001.5-1.5v-1" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  upload: '<path d="M8 14V6m0 0L5 9m3-3l3 3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><path d="M2.5 4.5v-1A1.5 1.5 0 014 2h8a1.5 1.5 0 011.5 1.5v1" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  plus: '<path d="M8 3v10M3 8h10" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  minus: '<path d="M3 8h10" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  close: '<path d="M4 4l8 8M12 4l-8 8" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/>',
  trash: '<path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.7 9.2a1 1 0 001 .8h5.6a1 1 0 001-.8l.7-9.2" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  check: '<path d="M3 8.5l3.2 3.2L13 5" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>',
  branch: '<circle cx="4.5" cy="4" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="4.5" cy="12.5" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="12" cy="7.5" r="2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M4.5 6v4.5M6.4 6.4C8 7 10 7 12 7.5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  tag: '<path d="M8.5 1.5H14v5.5L7 14.5 1.5 9 8.5 1.5z" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/><circle cx="11.4" cy="4.6" r="1.1" fill="currentColor"/>',
  history: '<circle cx="8" cy="8" r="6" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M8 4.6V8l2.4 1.6" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  sync: '<path d="M13.5 8a5.5 5.5 0 01-9.4 3.9M2.5 8a5.5 5.5 0 019.4-3.9" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/><path d="M13.8 2.6v3.2h-3.2M2.2 13.4v-3.2h3.2" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  diff: '<path d="M4 2v12M12 2v12M2 5h4M10 11h4" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  settings: '<circle cx="8" cy="8" r="2.2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M8 1.5l1 1.8 2-.4.4 2 1.8 1-1 1.8 1 1.8-1.8 1-.4 2-2-.4L8 14.5l-1-1.8-2 .4-.4-2-1.8-1 1-1.8-1-1.8 1.8-1 .4-2 2 .4L8 1.5z" fill="none" stroke="currentColor" stroke-width="1.2"/>',
  back: '<path d="M10 3L5 8l5 5" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>',
  expand: '<path d="M6 2h8v8M6 14H2V6" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  play: '<path d="M5 3l8 5-8 5V3z" fill="currentColor"/>',
  zoomIn: '<circle cx="7" cy="7" r="4.5" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M10.5 10.5L14 14M7 5v4M5 7h4" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  zoomOut: '<circle cx="7" cy="7" r="4.5" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M10.5 10.5L14 14M5 7h4" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  rotate: '<path d="M13.5 8a5.5 5.5 0 11-2-4.2M13.5 2.2v3.6h-3.6" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  archive: '<path d="M2 3.5h12v3H2zM3 6.5h10V14a1 1 0 01-1 1H4a1 1 0 01-1-1V6.5z" fill="none" stroke="currentColor" stroke-width="1.3"/><path d="M6.5 9h3" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/>',
  info: '<circle cx="8" cy="8" r="6.2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M8 7.2v4.2M8 4.9v.1" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  warning: '<path d="M8 2l6 11H2L8 2z" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/><path d="M8 6.5v3.2M8 11.4v.1" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>',
  terminal: '<path d="M3 3h10a1 1 0 011 1v8a1 1 0 01-1 1H3a1 1 0 01-1-1V4a1 1 0 011-1z" fill="none" stroke="currentColor" stroke-width="1.3"/><path d="M4.6 6.2L7 8.2l-2.4 2M8.4 10.6h3" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/>',
  wheel: '<circle cx="8" cy="8" r="6" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="8" cy="8" r="2" fill="currentColor"/>',
  copy: '<rect x="5" y="5" width="8" height="9" rx="1.2" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M3 10.5V3.8A1.3 1.3 0 014.3 2.5h5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  undo: '<path d="M3.5 8a5 5 0 115 5H6" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/><path d="M6 4.5L3 7.6l3 3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>',
}

/** 生成图标元素（14px，currentColor）。 */
export function icon(name: keyof typeof ICONS | string, size = 14): SVGSVGElement {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg")
  svg.setAttribute("viewBox", "0 0 16 16")
  svg.setAttribute("width", String(size))
  svg.setAttribute("height", String(size))
  svg.setAttribute("class", "fw-icon")
  svg.setAttribute("aria-hidden", "true")
  svg.innerHTML = ICONS[name] ?? ICONS.file
  return svg
}

export { ICONS }

/* ------------------------------ 格式化 ------------------------------ */

export function formatSize(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return "-"
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`
}

export function formatTime(ms: number): string {
  if (!ms) return "-"
  const d = new Date(ms)
  const p = (n: number) => String(n).padStart(2, "0")
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}

export function timeAgo(ms: number): string {
  if (!ms) return ""
  const diff = Date.now() - ms
  const m = Math.round(diff / 60000)
  if (m < 1) return "刚刚"
  if (m < 60) return `${m} 分钟前`
  const hour = Math.round(m / 60)
  if (hour < 24) return `${hour} 小时前`
  const day = Math.round(hour / 24)
  if (day < 30) return `${day} 天前`
  const mon = Math.round(day / 30)
  if (mon < 12) return `${mon} 个月前`
  return `${Math.round(mon / 12)} 年前`
}

/** 文件扩展名（小写，不含点）。 */
export function extOf(name: string): string {
  const i = name.lastIndexOf(".")
  if (i <= 0) return ""
  return name.slice(i + 1).toLowerCase()
}

/** 文件类型 → 图标颜色类（IDE 风格的语义色，不依赖具体主题）。 */
const ICON_COLOR: Record<string, string> = {
  ts: "blue", tsx: "blue", mts: "blue", cts: "blue",
  js: "yellow", jsx: "yellow", mjs: "yellow", cjs: "yellow",
  json: "yellow", jsonc: "yellow", json5: "yellow", jsonl: "yellow",
  md: "cyan", markdown: "cyan", mdx: "cyan", rst: "cyan",
  py: "green", rb: "red", go: "cyan", rs: "orange", java: "red", kt: "purple", kts: "purple",
  c: "blue", h: "blue", cpp: "blue", cc: "blue", hpp: "blue", cs: "purple", swift: "orange", dart: "cyan",
  html: "orange", htm: "orange", vue: "green", svelte: "orange", css: "blue", scss: "pink", sass: "pink", less: "blue",
  yml: "purple", yaml: "purple", toml: "orange", ini: "gray", conf: "gray", env: "yellow",
  sh: "green", bash: "green", zsh: "green", ps1: "blue", bat: "gray", cmd: "gray",
  png: "purple", jpg: "purple", jpeg: "purple", gif: "purple", webp: "purple", svg: "orange", ico: "purple", bmp: "purple", avif: "purple",
  pdf: "red", doc: "blue", docx: "blue", xls: "green", xlsx: "green", ppt: "orange", pptx: "orange",
  zip: "yellow", tar: "yellow", gz: "yellow", "7z": "yellow", rar: "yellow", jar: "yellow",
  mp4: "pink", mov: "pink", webm: "pink", mkv: "pink", avi: "pink", mp3: "pink", wav: "pink", flac: "pink", ogg: "pink",
  sql: "cyan", graphql: "pink", gql: "pink", proto: "cyan", lock: "gray", sum: "gray",
}

export function iconColorFor(name: string, type: string): string {
  if (type === "dir") return "folder"
  if (type === "symlink") return "cyan"
  return ICON_COLOR[extOf(name)] ?? "gray"
}

/* ------------------------------ 轻提示 ------------------------------ */

let toastHost: HTMLElement | null = null

export function toast(message: string, kind: "info" | "success" | "error" | "warn" = "info", ms = 3200): void {
  if (!toastHost) {
    toastHost = h("div", { class: "fw-toasts" })
    document.body.appendChild(toastHost)
  }
  const el = h("div", { class: `fw-toast ${kind}` }, [icon(kind === "error" ? "warning" : kind === "success" ? "check" : "info"), h("span", { text: message })])
  toastHost.appendChild(el)
  setTimeout(() => {
    el.classList.add("out")
    setTimeout(() => el.remove(), 260)
  }, ms)
  el.onclick = () => el.remove()
}

/* ------------------------------ 对话框 ------------------------------ */

interface PromptOpts {
  title: string
  label?: string
  value?: string
  placeholder?: string
  okText?: string
  /** 多行输入（提交信息用） */
  multiline?: boolean
  /** 附加说明（如「将覆盖同名文件」） */
  hint?: string
}

/** 通用输入对话框（Promise 化；取消返回 null）。 */
export function promptDialog(opts: PromptOpts): Promise<string | null> {
  return new Promise((resolve) => {
    const input = opts.multiline
      ? h("textarea", { class: "fw-input fw-textarea", placeholder: opts.placeholder ?? "", rows: 5 })
      : h("input", { class: "fw-input", placeholder: opts.placeholder ?? "" })
    input.value = opts.value ?? ""
    const okBtn = h("button", { class: "fw-btn primary", text: opts.okText ?? "确定" })
    const cancelBtn = h("button", { class: "fw-btn", text: "取消" })
    const body = h("div", { class: "fw-dialog-body" }, [
      opts.label ? h("label", { class: "fw-label", text: opts.label }) : null,
      input,
      opts.hint ? h("div", { class: "fw-hint", text: opts.hint }) : null,
    ])
    const dialog = h("div", { class: "fw-dialog" }, [h("div", { class: "fw-dialog-title", text: opts.title }), body, h("div", { class: "fw-dialog-actions" }, [cancelBtn, okBtn])])
    const overlay = h("div", { class: "fw-overlay" }, [dialog])
    const done = (v: string | null) => {
      overlay.remove()
      document.removeEventListener("keydown", onKey, true)
      resolve(v)
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.stopPropagation()
        done(null)
      } else if (e.key === "Enter" && (!opts.multiline || e.ctrlKey || e.metaKey)) {
        e.stopPropagation()
        e.preventDefault()
        done(input.value)
      }
    }
    okBtn.onclick = () => done(input.value)
    cancelBtn.onclick = () => done(null)
    overlay.onclick = (e) => {
      if (e.target === overlay) done(null)
    }
    document.addEventListener("keydown", onKey, true)
    document.body.appendChild(overlay)
    setTimeout(() => {
      input.focus()
      if (input instanceof HTMLInputElement) input.select()
    }, 20)
  })
}

/** 确认对话框（Promise 化）。 */
export function confirmDialog(opts: { title: string; message: string; okText?: string; danger?: boolean; hint?: string }): Promise<boolean> {
  return new Promise((resolve) => {
    const okBtn = h("button", { class: `fw-btn ${opts.danger ? "danger" : "primary"}`, text: opts.okText ?? "确定" })
    const cancelBtn = h("button", { class: "fw-btn", text: "取消" })
    const dialog = h("div", { class: "fw-dialog" }, [
      h("div", { class: "fw-dialog-title" }, [opts.danger ? icon("warning") : icon("info"), h("span", { text: opts.title })]),
      h("div", { class: "fw-dialog-body" }, [h("div", { class: "fw-dialog-msg", text: opts.message }), opts.hint ? h("div", { class: "fw-hint", text: opts.hint }) : null]),
      h("div", { class: "fw-dialog-actions" }, [cancelBtn, okBtn]),
    ])
    const overlay = h("div", { class: "fw-overlay" }, [dialog])
    const done = (v: boolean) => {
      overlay.remove()
      document.removeEventListener("keydown", onKey, true)
      resolve(v)
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.stopPropagation()
        done(false)
      } else if (e.key === "Enter") {
        e.stopPropagation()
        done(true)
      }
    }
    okBtn.onclick = () => done(true)
    cancelBtn.onclick = () => done(false)
    overlay.onclick = (e) => {
      if (e.target === overlay) done(false)
    }
    document.addEventListener("keydown", onKey, true)
    document.body.appendChild(overlay)
    setTimeout(() => okBtn.focus(), 20)
  })
}

/* ------------------------------ 右键菜单 ------------------------------ */

export interface MenuItem {
  label?: string
  icon?: string
  shortcut?: string
  danger?: boolean
  disabled?: boolean
  separator?: boolean
  onClick?: () => void
  submenu?: MenuItem[]
}

let menuHost: HTMLElement | null = null

export function closeMenu(): void {
  menuHost?.remove()
  menuHost = null
}

/** 弹出右键菜单 / 下拉菜单（点击外部或 Escape 关闭；屏幕边缘自动回折）。 */
export function showMenu(x: number, y: number, items: MenuItem[]): void {
  closeMenu()
  const host = h("div", { class: "fw-menu-pop" })
  const build = (list: MenuItem[], container: HTMLElement) => {
    for (const it of list) {
      if (it.separator) {
        container.appendChild(h("div", { class: "fw-menu-sep" }))
        continue
      }
      const btn = h("button", { class: `fw-menu-item${it.danger ? " danger" : ""}${it.disabled ? " disabled" : ""}` }, [
        it.icon ? icon(it.icon) : h("span", { class: "fw-menu-icon-gap" }),
        h("span", { class: "fw-menu-label", text: it.label ?? "" }),
        it.shortcut ? h("span", { class: "fw-menu-shortcut", text: it.shortcut }) : null,
        it.submenu ? icon("chevronRight") : null,
      ])
      if (it.disabled) btn.setAttribute("disabled", "")
      else
        btn.onclick = (e) => {
          e.stopPropagation()
          if (it.submenu) return
          closeMenu()
          it.onClick?.()
        }
      if (it.submenu) {
        let sub: HTMLElement | null = null
        btn.onmouseenter = () => {
          sub?.remove()
          sub = h("div", { class: "fw-menu-pop sub" })
          build(it.submenu ?? [], sub)
          host.appendChild(sub)
          const r = btn.getBoundingClientRect()
          sub.style.left = `${Math.min(r.right + 2, window.innerWidth - 200)}px`
          sub.style.top = `${Math.min(r.top, window.innerHeight - sub.offsetHeight - 8)}px`
        }
        btn.onmouseleave = () => setTimeout(() => sub?.remove(), 260)
      }
      container.appendChild(btn)
    }
  }
  build(items, host)
  host.style.left = `${x}px`
  host.style.top = `${y}px`
  document.body.appendChild(host)
  const rect = host.getBoundingClientRect()
  if (rect.right > window.innerWidth - 6) host.style.left = `${Math.max(4, window.innerWidth - rect.width - 6)}px`
  if (rect.bottom > window.innerHeight - 6) host.style.top = `${Math.max(4, window.innerHeight - rect.height - 6)}px`
  menuHost = host
  const onDown = (e: MouseEvent) => {
    if (!host.contains(e.target as Node)) closeMenu()
  }
  const onKey = (e: KeyboardEvent) => {
    if (e.key === "Escape") closeMenu()
  }
  setTimeout(() => {
    document.addEventListener("mousedown", onDown)
    document.addEventListener("keydown", onKey)
  }, 0)
  const cleanup = () => {
    document.removeEventListener("mousedown", onDown)
    document.removeEventListener("keydown", onKey)
    if (menuHost === host) menuHost = null
    observer.disconnect()
  }
  const observer = new MutationObserver(() => {
    if (!document.body.contains(host)) cleanup()
  })
  observer.observe(document.body, { childList: true })
}

/** 下拉菜单（按钮锚定；用于工具栏菜单）。 */
export function dropdown(anchor: HTMLElement, items: MenuItem[]): void {
  const r = anchor.getBoundingClientRect()
  showMenu(r.left, r.bottom + 4, items)
}
