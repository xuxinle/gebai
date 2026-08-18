/**
 * 自研交互组件：替换浏览器原生交互（alert/confirm 对话框、title tooltip、select/checkbox/输入建议/表单校验气泡）。
 * 全部基于主题 CSS 变量自绘，样式见 overlays.css。
 */

import { el } from "./state"

/* ---------- 剪贴板（复制） ---------- */

/** 复制文本到剪贴板：优先 Clipboard API（仅 HTTPS/localhost 安全上下文可用），
 *  失败回退 execCommand（远程 HTTP 等非安全上下文仍可复制）。 */
export async function copyText(text: string): Promise<void> {
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text)
      return
    } catch {
      /* 回退 execCommand */
    }
  }
  const ta = el("textarea")
  ta.value = text
  ta.style.position = "fixed"
  ta.style.opacity = "0"
  document.body.appendChild(ta)
  ta.select()
  const ok = document.execCommand("copy")
  ta.remove()
  if (!ok) throw new Error("复制失败")
}

/* ---------- Toast（替换 alert） ---------- */

let toastTimer: number | null = null
let toastEl: HTMLElement | null = null

export function toast(text: string, kind: "error" | "ok" = "error"): void {
  if (toastTimer) clearTimeout(toastTimer)
  if (!toastEl || !toastEl.isConnected) {
    toastEl = el("div", "toast")
    document.body.appendChild(toastEl)
  }
  toastEl.className = `toast ${kind}`
  toastEl.textContent = text
  // 强制重排以触发进场动画
  void toastEl.offsetWidth
  toastEl.classList.add("show")
  toastTimer = window.setTimeout(() => {
    toastEl?.classList.remove("show")
    toastTimer = null
  }, 3200)
}

/* ---------- 确认对话框（替换 confirm） ---------- */

export interface ConfirmOptions {
  title: string
  text?: string
  /** 确认按钮文案 */
  okLabel?: string
  /** 确认按钮危险样式（默认 true） */
  danger?: boolean
  /** 附加条目列表（如待删会话名） */
  list?: string[]
}

/** 模态确认框：返回 Promise<boolean>，Esc / 遮罩 / 取消 = false。 */
export function confirmDialog(opts: ConfirmOptions): Promise<boolean> {
  return new Promise((resolve) => {
    const overlay = el("div", "preview-overlay")
    const card = el("div", "confirm-card")
    card.append(el("div", "confirm-title", opts.title))
    if (opts.text) card.append(el("div", "confirm-text", opts.text))
    if (opts.list?.length) {
      const list = el("div", "confirm-list")
      const shown = opts.list.slice(0, 5)
      for (const n of shown) list.append(el("div", "confirm-list-item", `「${n}」`))
      if (opts.list.length > shown.length) list.append(el("div", "confirm-list-item", `…等 ${opts.list.length - shown.length} 个`))
      card.appendChild(list)
    }
    const actions = el("div", "confirm-actions")
    const cancel = el("button", "confirm-cancel", "取消")
    const ok = el("button", "confirm-ok", opts.okLabel ?? "确定")
    if (opts.danger === false) ok.classList.remove("confirm-ok")
    actions.append(cancel, ok)
    card.appendChild(actions)
    overlay.appendChild(card)
    document.body.appendChild(overlay)

    const close = (result: boolean) => {
      overlay.remove()
      document.removeEventListener("keydown", onKey)
      resolve(result)
    }
    const onKey = (ev: KeyboardEvent) => {
      if (ev.key === "Escape") close(false)
    }
    cancel.onclick = () => close(false)
    overlay.onclick = (ev) => {
      if (ev.target === overlay) close(false)
    }
    ok.onclick = () => close(true)
    document.addEventListener("keydown", onKey)
    cancel.focus()
  })
}

/* ---------- 输入对话框（替换 prompt） ---------- */

export interface PromptField {
  placeholder?: string
  value?: string
  /** 多行输入（textarea，Enter 换行；存在多行字段时 Ctrl+Enter 提交） */
  multiline?: boolean
}

/** 输入对话框：返回各字段输入值数组；取消 / Esc / 遮罩点击 = null。Enter 提交（含多行字段时 Ctrl+Enter 提交）。 */
export function promptDialog(opts: { title: string; fields: PromptField[] }): Promise<string[] | null> {
  return new Promise((resolve) => {
    const overlay = el("div", "preview-overlay")
    const card = el("div", "confirm-card")
    card.append(el("div", "confirm-title", opts.title))
    const body = el("div", "confirm-text")
    const multiline = opts.fields.some((f) => f.multiline)
    const inputs = opts.fields.map((f) => {
      const inp = f.multiline ? document.createElement("textarea") : document.createElement("input")
      inp.placeholder = f.placeholder ?? ""
      inp.value = f.value ?? ""
      inp.style.width = "100%"
      inp.style.marginBottom = "8px"
      if (f.multiline) (inp as HTMLTextAreaElement).rows = 4
      body.appendChild(inp)
      return inp
    })
    card.appendChild(body)
    const actions = el("div", "confirm-actions")
    const cancel = el("button", "confirm-cancel", "取消")
    const ok = el("button", "confirm-primary", "确定")
    actions.append(cancel, ok)
    card.appendChild(actions)
    overlay.appendChild(card)
    document.body.appendChild(overlay)

    const close = (result: string[] | null) => {
      overlay.remove()
      document.removeEventListener("keydown", onKey)
      resolve(result)
    }
    const submit = () => close(inputs.map((i) => i.value))
    const onKey = (ev: KeyboardEvent) => {
      if (ev.key === "Escape") close(null)
      else if (ev.key === "Enter" && (!multiline || ev.ctrlKey || ev.metaKey)) submit()
    }
    cancel.onclick = () => close(null)
    overlay.onclick = (ev) => {
      if (ev.target === overlay) close(null)
    }
    ok.onclick = submit
    document.addEventListener("keydown", onKey)
    inputs[0]?.focus()
    inputs[0]?.select()
  })
}

/* ---------- Tooltip（替换 title 属性） ---------- */

/** 给元素挂 tooltip 提示（替代 title 属性；配合 bindTooltips 的全局委托渲染）。 */
export function tip(el: HTMLElement, text: string): void {
  if (text) el.dataset.tip = text
  else delete el.dataset.tip
}

let tooltipEl: HTMLElement | null = null
let tooltipHost: HTMLElement | null = null

function hideTooltip(): void {
  tooltipEl?.remove()
  tooltipEl = null
  tooltipHost = null
}

function showTooltip(host: HTMLElement): void {
  if (tooltipHost === host) return
  const text = host.dataset.tip
  if (!text) return
  hideTooltip()
  tooltipHost = host
  tooltipEl = el("div", "tip")
  tooltipEl.textContent = text
  document.body.appendChild(tooltipEl)
  positionTooltip(host, tooltipEl)
}

function positionTooltip(host: HTMLElement, tt: HTMLElement): void {
  const r = host.getBoundingClientRect()
  const tw = tt.offsetWidth
  const th = tt.offsetHeight
  const vw = document.documentElement.clientWidth
  // 上缘空间不足时翻转到下方
  const above = r.top > th + 14
  let x = r.left + r.width / 2
  if (x < tw / 2 + 8) x = tw / 2 + 8
  else if (x > vw - tw / 2 - 8) x = vw - tw / 2 - 8
  tt.style.left = `${x}px`
  tt.style.top = above ? `${r.top - 10}px` : `${r.bottom + 10}px`
  tt.style.transform = `translate(-50%, ${above ? "-100%" : "0"})`
}

/** 全局委托：为所有 [data-tip] 元素渲染自定义 tooltip（hover / 键盘焦点，边缘翻转，不受 overflow 裁剪）。 */
export function bindTooltips(): void {
  document.addEventListener("pointerover", (e) => {
    const host = (e.target as HTMLElement).closest<HTMLElement>("[data-tip]")
    if (host) showTooltip(host)
  })
  document.addEventListener("pointerout", (e) => {
    const host = (e.target as HTMLElement).closest<HTMLElement>("[data-tip]")
    if (host && tooltipHost === host) hideTooltip()
  })
  document.addEventListener("focusin", (e) => {
    const host = (e.target as HTMLElement).closest<HTMLElement>("[data-tip]")
    if (host) showTooltip(host)
  })
  document.addEventListener("focusout", (e) => {
    const host = (e.target as HTMLElement).closest<HTMLElement>("[data-tip]")
    if (host && tooltipHost === host) hideTooltip()
  })
  document.addEventListener("scroll", hideTooltip, true)
}

/* ---------- 自绘下拉选择（替换 <select>） ---------- */

export interface SelectOption {
  value: string
  label: string
}

export interface SelectHandle {
  root: HTMLElement
  /** 当前选中值 */
  value: string
  /** 外部同步值（如跨标签 storage 变更） */
  setValue: (value: string) => void
}

/** 自绘下拉选择框：按钮 + 固定定位浮层，Esc/外部点击关闭。 */
export function customSelect(opts: {
  options: SelectOption[]
  value: string
  onChange: (value: string) => void
}): SelectHandle {
  const root = el("div", "sel")
  const btn = el("button", "sel-btn")
  btn.type = "button"
  btn.setAttribute("aria-haspopup", "listbox")
  btn.setAttribute("aria-expanded", "false")
  const label = el("span", "sel-val")
  const arrow = el("span", "sel-arrow")
  btn.append(label, arrow)
  root.appendChild(btn)

  // 浮层懒创建且挂到 body（不嵌套在 root 内）：
  // 1. 经典主题等会给行加 hover transform（.settings-row / #session-list li 的 translateX），
  //    transform 祖先会成为 fixed 子元素的 containing block，left/top 被解释为行内局部坐标
  //    （视觉上偏移约 820px 飞出屏外）；挂 body 后 fixed 始终相对视口，
  //    btn.getBoundingClientRect() 的视口坐标直接可用
  // 2. 每次打开重建、关闭即移除：设置面板重渲染（innerHTML 清空）不会在 body 堆积隐藏的孤儿浮层
  let pop: HTMLElement | null = null
  const buildPop = () => {
    pop = el("div", "sel-pop")
    pop.setAttribute("role", "listbox")
    for (const o of opts.options) {
      const opt = el("button", "sel-opt")
      opt.type = "button"
      opt.setAttribute("role", "option")
      opt.dataset.value = o.value
      opt.textContent = o.label
      opt.onclick = () => {
        opts.onChange(o.value)
        setValue(o.value)
        close()
      }
      pop.appendChild(opt)
    }
    document.body.appendChild(pop)
    return pop
  }

  const current = opts.options.find((o) => o.value === opts.value) ?? opts.options[0]
  label.textContent = current?.label ?? opts.value

  const setValue = (value: string) => {
    handle.value = value
    const next = opts.options.find((o) => o.value === value)
    if (next) label.textContent = next.label
    if (pop) {
      for (const opt of pop.querySelectorAll<HTMLButtonElement>(".sel-opt")) {
        opt.classList.toggle("active", opt.dataset.value === value)
        opt.setAttribute("aria-selected", String(opt.dataset.value === value))
      }
    }
  }

  const close = () => {
    pop?.remove()
    pop = null
    btn.setAttribute("aria-expanded", "false")
    document.removeEventListener("pointerdown", onOutside)
    document.removeEventListener("keydown", onKey)
    document.removeEventListener("scroll", onScroll, true)
  }

  const open = () => {
    // 浮层 fixed 定位（避开滚动容器裁剪），视口内收边
    const p = pop ?? buildPop()
    const r = btn.getBoundingClientRect()
    p.style.left = `${Math.max(8, Math.min(r.right - 220, window.innerWidth - 228))}px`
    p.style.top = `${Math.min(r.bottom + 6, window.innerHeight - 12 - 168)}px`
    setValue(handle.value) // 重建后的浮层同步当前选中态（active 高亮 / aria-selected）
    btn.setAttribute("aria-expanded", "true")
    document.addEventListener("pointerdown", onOutside)
    document.addEventListener("keydown", onKey)
    document.addEventListener("scroll", onScroll, true)
  }

  const onScroll = () => close()

  const onOutside = (e: Event) => {
    // 浮层已挂 body（不在 root 内）：点中浮层本身不算外部
    if (!root.contains(e.target as Node) && !(pop && pop.contains(e.target as Node))) close()
  }
  const onKey = (e: KeyboardEvent) => {
    if (e.key === "Escape") close()
  }

  const handle: SelectHandle = { root, value: current?.value ?? opts.value, setValue }
  // 必须在 handle 定义之后再初始化选中态（setValue 内部引用 handle，提前调用会触发 TDZ ReferenceError）
  setValue(opts.value)
  btn.onclick = () => (pop ? close() : open())
  return handle
}

/* ---------- 输入建议下拉（替换 datalist） ---------- */

/** 为输入框绑定自定义联想建议浮层（固定定位，过滤匹配）。 */
export function bindSuggestions(input: HTMLInputElement, items: string[]): void {
  let pop: HTMLElement | null = null
  const close = () => {
    pop?.remove()
    pop = null
    document.removeEventListener("pointerdown", onOutside)
    document.removeEventListener("focusin", onFocusIn)
    document.removeEventListener("keydown", onKey)
    document.removeEventListener("scroll", onScroll, true)
  }
  const onScroll = () => close()
  const onOutside = (e: Event) => {
    if (!input.contains(e.target as Node) && !(pop && pop.contains(e.target as Node))) close()
  }
  const onFocusIn = (e: Event) => {
    if (!input.contains(e.target as Node) && !(pop && pop.contains(e.target as Node))) close()
  }
  const onKey = (e: KeyboardEvent) => {
    if (e.key === "Escape") close()
  }
  const render = () => {
    const q = input.value.trim().toUpperCase()
    const hits = items.filter((i) => i.toUpperCase().includes(q))
    close()
    if (!hits.length) return
    pop = el("div", "sel-pop suggest-pop")
    pop.setAttribute("role", "listbox")
    for (const item of hits.slice(0, 8)) {
      const opt = el("button", "sel-opt")
      opt.type = "button"
      opt.setAttribute("role", "option")
      opt.textContent = item
      opt.onclick = () => {
        input.value = item
        close()
        input.focus()
      }
      pop.appendChild(opt)
    }
    document.body.appendChild(pop)
    const r = input.getBoundingClientRect()
    pop.style.left = `${Math.max(8, Math.min(r.left, window.innerWidth - 232))}px`
    pop.style.top = `${r.bottom + 4}px`
    document.addEventListener("pointerdown", onOutside)
    document.addEventListener("focusin", onFocusIn)
    document.addEventListener("keydown", onKey)
    document.addEventListener("scroll", onScroll, true)
  }
  input.addEventListener("focus", render)
  input.addEventListener("input", render)
}

export interface AutoHideScrollbarOptions {
  /** 滚动停止后移除 .scrolling 的延迟（毫秒），默认 400。 */
  hideDelayMs?: number
}

/**
 * 滚动条自动隐藏（会话列表 / 主消息列共用）：滚动中加 .scrolling（CSS 显示滑块），
 * 停止 hideDelayMs 后移除——覆盖触摸板惯性滚动等指针不在容器上的情况；静止可见性由 :hover 承担。
 */
export function autoHideScrollbar(el: HTMLElement, opts: AutoHideScrollbarOptions = {}): void {
  const hideDelayMs = opts.hideDelayMs ?? 400
  let hideTimer: ReturnType<typeof setTimeout> | null = null
  el.addEventListener(
    "scroll",
    () => {
      el.classList.add("scrolling")
      if (hideTimer) clearTimeout(hideTimer)
      hideTimer = setTimeout(() => el.classList.remove("scrolling"), hideDelayMs)
    },
    { passive: true },
  )
}
