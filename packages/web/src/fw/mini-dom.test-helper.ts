/**
 * 微框架单测用的**最小真实 DOM**。
 *
 * 为什么不复用 `scripts/test-preload.ts`：那套桩是为「模块 import 期绑定 DOM」准备的
 * （未定义成员一律兜底成 no-op），属性不落、子节点不真挂、`getAttribute` 恒 null。
 * 而微框架的核心正是**属性/子节点/事件的真实语义**（布尔属性、data-*、文本节点、解绑后
 * 不再触发）。用 no-op 桩去测它，等于什么都没测——所以这里实现一份够用的真实语义 DOM。
 *
 * 覆盖范围刚好够 fw 的用例：createElement / appendChild / 属性 / dataset / classList /
 * textContent / 事件（含解绑）/ 查询选择器（只支持 tag 与 .class 两种选择器，够用即止）。
 */

type Listener = { fn: (ev: unknown) => void; once?: boolean; capture?: boolean }

class MiniClassList {
  private set = new Set<string>()
  constructor(_own: MiniElement) {}
  add(...names: string[]): void {
    for (const n of names) this.set.add(n)
  }
  remove(...names: string[]): void {
    for (const n of names) this.set.delete(n)
  }
  contains(n: string): boolean {
    return this.set.has(n)
  }
  toggle(n: string, force?: boolean): boolean {
    const next = force === undefined ? !this.set.has(n) : force
    if (next) this.set.add(n)
    else this.set.delete(n)
    return next
  }
  get value(): string {
    return [...this.set].join(" ")
  }
  [Symbol.iterator](): Iterator<string> {
    return this.set[Symbol.iterator]()
  }
}

export class MiniElement {
  tagName: string
  nodeType = 1
  childNodes: MiniElement[] = []
  parentNode: MiniElement | null = null
  attrs = new Map<string, string>()
  /** 样式：贴近真实 CSSStyleDeclaration 的最小实现（cssText 与 set/getPropertyValue 都要有）。 */
  style = makeStyle()
  dataset: Record<string, string> = {}
  classList: MiniClassList
  private listeners = new Map<string, Listener[]>()
  value = ""
  checked = false
  placeholder = ""
  rows = 0
  type = ""
  title = ""
  htmlFor = ""
  private text = ""

  constructor(tag: string) {
    this.tagName = tag.toUpperCase()
    this.classList = new MiniClassList(this)
  }

  get className(): string {
    return this.classList.value
  }
  set className(v: string) {
    this.classList = new MiniClassList(this)
    for (const n of String(v).split(/\s+/).filter(Boolean)) this.classList.add(n)
  }

  get textContent(): string {
    return this.text + this.childNodes.map((c) => c.textContent).join("")
  }
  set textContent(v: string) {
    this.text = String(v)
    this.childNodes = []
  }

  set innerHTML(v: string) {
    // 只做最小解析（用例里仅用于「确认 HTML 被当作标签解析」这一条）
    this.text = ""
    this.childNodes = []
    const re = /<(\w+)>([^<]*)<\/\1>/g
    let m: RegExpExecArray | null
    while ((m = re.exec(String(v)))) {
      const child = new MiniElement(m[1])
      child.textContent = m[2]
      this.appendChild(child)
    }
    if (!this.childNodes.length) this.text = String(v)
  }
  get innerHTML(): string {
    return this.childNodes.map((c) => `<${c.tagName.toLowerCase()}>${c.textContent}</${c.tagName.toLowerCase()}>`).join("") || this.text
  }

  get children(): MiniElement[] {
    return this.childNodes
  }
  /** <select> 的选项集合（真实 DOM 里是 HTMLOptionsCollection；用例只读 length 与下标）。 */
  get options(): MiniElement[] {
    return this.childNodes
  }
  get firstChild(): MiniElement | null {
    return this.childNodes[0] ?? null
  }

  appendChild<T extends MiniElement>(c: T): T {
    if (c.tagName === "FRAGMENT") {
      for (const child of [...c.childNodes]) this.appendChild(child)
      c.childNodes = []
      return c
    }
    c.parentNode = this
    this.childNodes.push(c)
    return c
  }
  /** 批量替换子节点（比较视图等处大量使用）。 */
  replaceChildren(...nodes: MiniElement[]): void {
    for (const c of this.childNodes) c.parentNode = null
    this.text = ""
    this.childNodes = []
    for (const n of nodes) this.appendChild(n)
  }
  removeChild(c: MiniElement): void {
    const i = this.childNodes.indexOf(c)
    if (i >= 0) this.childNodes.splice(i, 1)
    c.parentNode = null
  }
  remove(): void {
    this.parentNode?.removeChild(this)
  }

  setAttribute(k: string, v: string): void {
    this.attrs.set(k, String(v))
    if (k === "class") this.className = String(v)
    if (k.startsWith("data-")) this.dataset[k.slice(5).replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = String(v)
    // 真实 DOM 里 setAttribute 会反映到同名 property（h() 用属性写法设 placeholder/checked 等，
    // 读的时候读的是 property——不同步会让「设了却不生效」）
    const prop = PROP_ATTRS.get(k)
    if (prop) (this as unknown as Record<string, unknown>)[prop] = v === "" ? true : v
  }
  getAttribute(k: string): string | null {
    return this.attrs.has(k) ? this.attrs.get(k)! : null
  }
  hasAttribute(k: string): boolean {
    return this.attrs.has(k)
  }
  removeAttribute(k: string): void {
    this.attrs.delete(k)
  }

  addEventListener(type: string, fn: (ev: unknown) => void, opts?: { once?: boolean } | boolean): void {
    const list = this.listeners.get(type) ?? []
    list.push({ fn, once: typeof opts === "object" ? opts.once : false })
    this.listeners.set(type, list)
  }
  removeEventListener(type: string, fn: (ev: unknown) => void): void {
    const list = this.listeners.get(type) ?? []
    this.listeners.set(
      type,
      list.filter((l) => l.fn !== fn),
    )
  }
  dispatchEvent(ev: Record<string, unknown>): boolean {
    const type = String(ev.type ?? "")
    // 补齐真实事件的通用字段（用例只给 type + 业务字段）
    const event = { target: this, currentTarget: this, defaultPrevented: false, ...ev }
    const list = [...(this.listeners.get(type) ?? [])]
    for (const l of list) {
      l.fn(event)
      if (l.once) this.removeEventListener(type, l.fn)
    }
    // 冒泡：父节点同类型监听也收到（真实 DOM 行为；closest/代理式判断依赖它）
    this.parentNode?.dispatchEvent({ ...event, currentTarget: this.parentNode })
    return true
  }
  /** 便捷：模拟点击。 */
  click(): void {
    this.dispatchEvent({ type: "click" })
  }

  /** 极简选择器：`tag` / `.class` / `tag.class`（足够覆盖 fw 用例；复杂选择器不在覆盖范围）。 */
  querySelectorAll(sel: string): MiniElement[] {
    const out: MiniElement[] = []
    const walk = (n: MiniElement): void => {
      for (const c of n.childNodes) {
        if (matches(c, sel)) out.push(c)
        walk(c)
      }
    }
    walk(this)
    return out
  }
  querySelector(sel: string): MiniElement | null {
    return this.querySelectorAll(sel)[0] ?? null
  }
  closest(sel: string): MiniElement | null {
    let n: MiniElement | null = this
    while (n) {
      if (matches(n, sel)) return n
      n = n.parentNode
    }
    return null
  }
  contains(n: MiniElement): boolean {
    if (n === this) return true
    return this.childNodes.some((c) => c.contains(n))
  }
  getBoundingClientRect(): { left: number; top: number; width: number; height: number; right: number; bottom: number; x: number; y: number } {
    // 位置从内联样式读回（与真实 DOM 一致：设 left/top 后 rect 跟着变）
    const readPx = (k: string): number =>
      Number(String((this.style as unknown as { getPropertyValue: (n: string) => string }).getPropertyValue(k) ?? "").replace("px", "")) || 0
    const left = readPx("left")
    const top = readPx("top")
    const width = 360
    const height = 320
    return { left, top, width, height, right: left + width, bottom: top + height, x: left, y: top }
  }
  setPointerCapture(): void {}
  releasePointerCapture(): void {}

  /** 深拷贝节点（图标缓存会 clone 一份再插入：同一图标多处使用不能共享同一节点）。 */
  cloneNode(deep = false): MiniElement {
    const copy = new MiniElement(this.tagName.toLowerCase())
    copy.attrs = new Map(this.attrs)
    copy.dataset = { ...this.dataset }
    copy.className = this.className
    copy.textContent = this.text
    copy.value = this.value
    copy.checked = this.checked
    copy.placeholder = this.placeholder
    copy.rows = this.rows
    copy.title = this.title
    if (deep) for (const c of this.childNodes) copy.appendChild(c.cloneNode(true))
    return copy
  }
}

/** 样式对象最小实现。 */
function makeStyle(): Record<string, unknown> {
  const map = new Map<string, string>()
  const api: Record<string, unknown> = {
    setProperty: (k: string, v: string) => void map.set(k, String(v)),
    getPropertyValue: (k: string) => map.get(k) ?? "",
    removeProperty: (k: string) => void map.delete(k),
  }
  return new Proxy(api, {
    set(_t, k, v) {
      if (k === "cssText") {
        map.clear()
        for (const part of String(v).split(";").map((x) => x.trim()).filter(Boolean)) {
          const i = part.indexOf(":")
          if (i > 0) map.set(part.slice(0, i).trim(), part.slice(i + 1).trim())
        }
        return true
      }
      map.set(String(k), String(v))
      return true
    },
    get(t, k) {
      if (k in t) return (t as Record<string, unknown>)[String(k)]
      if (k === "cssText") return [...map.entries()].map(([a, b]) => `${a}: ${b}`).join("; ")
      return map.get(String(k)) ?? ""
    },
  })
}

/** 属性 → property 的同步映射（只列 fw 用例会用到的）。 */
const PROP_ATTRS = new Map<string, string>([
  ["value", "value"],
  ["placeholder", "placeholder"],
  ["type", "type"],
  ["rows", "rows"],
  ["title", "title"],
  ["checked", "checked"],
  ["disabled", "disabled"],
  ["hidden", "hidden"],
])

function matches(el: MiniElement, sel: string): boolean {
  // 选择器组：逗号分隔，任一命中即匹配（真实 DOM 行为；exclude="button, a, input" 依赖它）
  if (sel.includes(",")) return sel.split(",").some((part) => matches(el, part))
  const s = sel.trim()
  const m = /^([a-zA-Z]*)?(?:\.([\w-]+))?$/.exec(s)
  if (!m) return false
  const [, tag, cls] = m
  if (tag && el.tagName !== tag.toUpperCase()) return false
  if (cls && !el.classList.contains(cls)) return false
  return true
}

/** 安装到 globalThis（返回 restore 用于还原，保证不污染其它测试文件）。 */
export function installMiniDom(): { restore: () => void } {
  const g = globalThis as unknown as Record<string, unknown>
  const prev = { document: g.document, window: g.window }
  const body = new MiniElement("body")
  const doc = {
    body,
    createElement: (tag: string) => new MiniElement(tag),
    createElementNS: (_ns: string, tag: string) => new MiniElement(tag),
    createTextNode: (text: string) => {
      const el = new MiniElement("#text")
      el.textContent = text
      return el
    },
    createDocumentFragment: () => new MiniElement("fragment"),
    getElementById: () => null,
    querySelector: () => null,
    querySelectorAll: () => [],
    addEventListener: () => {},
    removeEventListener: () => {},
    visibilityState: "visible",
  }
  g.document = doc
  // 视口尺寸：真实浏览器恒有。缺了会让「钳回视口」算出 NaN（位置变成 NaNpx）
  const win = (g.window as Record<string, unknown>) ?? {}
  win.innerWidth = win.innerWidth ?? 1280
  win.innerHeight = win.innerHeight ?? 800
  win.devicePixelRatio = win.devicePixelRatio ?? 1
  g.window = win
  return {
    restore: () => {
      g.document = prev.document
      g.window = prev.window
    },
  }
}
