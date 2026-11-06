/**
 * 微框架 · DOM 原语：把「建节点 / 设属性 / 挂事件 / 设样式」收敛成少量函数。
 *
 * ## 为什么有它（而不是继续各文件各写 createElement）
 * 现状是每个面板都自己拼 DOM：`createElement` + `className` + `appendChild` + `setAttribute` +
 * 事件监听散在 10+ 个文件里，同一件事有 3~5 种写法。后果不是"不好看"，而是：
 * - **改一处漏一处**：给某个控件加 aria/键盘可达性，得记得在每份手写里都加；
 * - **资源泄漏**：手动 addEventListener 的面板在关闭时经常忘记解绑（或只解绑一部分），
 *   面板开合十几次后监听器堆叠、回调重复触发；
 * - **无法静态检查**：手写 DOM 的写法千变万化，写不出「这个面板是否遵守约定」的护栏。
 *
 * 这里**不引入虚拟 DOM**（这些面板是短列表、低频更新，虚拟 DOM 只会增加一层 diff 开销与
 * 心智负担），只提供：
 * - `h()`：声明式建节点（属性/事件/子节点一次给全）；
 * - `on()`：**返回解绑函数**的监听注册（配合 `mount()` 自动回收）；
 * - `frag()`：文档片段；
 * - `css()`：按对象设样式（字段名 getter 自动带 `--`，方便设 CSS 变量）。
 */

/** 事件属性写法：`on: { click: fn }`。 */
export type DomEventMap = Record<string, (ev: Event) => void>

/** `h()` 的属性：普通属性走 setAttribute，特殊键：`class`/`style`/`on`/`data`/`text`/`html`。 */
export interface DomProps {
  class?: string
  /** 内联样式：字符串或用对象（键为 kebab 或 camel，值自动 toString）。 */
  style?: string | Partial<Record<string, string | number>>
  /** 事件监听（键为事件名，不带 on 前缀）。 */
  on?: DomEventMap
  /** data-* 属性。 */
  data?: Record<string, string | number | boolean | undefined>
  /** 文本内容（与 children 二选一；同时给时先设文本再追加子节点）。 */
  text?: string
  /** 内部 HTML（**仅用于已知可信内容**，如内置 SVG 图标；用户内容一律用 text）。 */
  html?: string
  /** 其它属性（布尔 true 设为空值属性，false/undefined 跳过）。 */
  [key: string]: unknown
}

/** 子节点：节点 / 字符串 / 假值（跳过，便于条件渲染）。 */
export type DomChild = Node | string | number | null | undefined | false

/** 建元素：`h("div", { class: "x", on: { click } }, "文字")`。 */
export function h<K extends keyof HTMLElementTagNameMap>(tag: K, props?: DomProps | null, ...children: DomChild[]): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag)
  applyProps(el, props)
  append(el, ...children)
  return el
}

/** 建 SVG 元素（图标位用；SVG 需命名空间）。 */
export function svg(tag: string, props?: DomProps | null, ...children: DomChild[]): SVGElement {
  const el = document.createElementNS("http://www.w3.org/2000/svg", tag)
  applyProps(el as unknown as HTMLElement, props)
  append(el as unknown as HTMLElement, ...children)
  return el
}

/** 文档片段（一次性appendChild 多个节点，减少多次重排）。 */
export function frag(...children: DomChild[]): DocumentFragment {
  const f = document.createDocumentFragment()
  append(f as unknown as HTMLElement, ...children)
  return f
}

/** 把 props 应用到元素（`h`/`svg` 共用）。 */
export function applyProps(el: HTMLElement, props?: DomProps | null): void {
  if (!props) return
  for (const [k, v] of Object.entries(props)) {
    if (v === undefined || v === null || v === false) continue
    switch (k) {
      case "class":
        el.className = String(v)
        break
      case "text":
        el.textContent = String(v)
        break
      case "html":
        el.innerHTML = String(v)
        break
      case "style":
        css(el, v as DomProps["style"])
        break
      case "on":
        for (const [ev, fn] of Object.entries(v as DomEventMap)) el.addEventListener(ev, fn as EventListener)
        break
      case "value":
        // 用 property 而不是 attribute：`<input value>` 只改默认值，用户输入后读 property 才是真值
        ;(el as unknown as { value: string }).value = String(v)
        break
      case "checked":
        ;(el as unknown as { checked: boolean }).checked = v === true || v === "true"
        break
      case "data":
        for (const [dk, dv] of Object.entries(v as Record<string, unknown>)) {
          if (dv === undefined || dv === null) continue
          el.dataset[dk] = String(dv)
        }
        break
      default:
        // 兼容 `onclick: fn` 风格（与 `on: { click }` 等价）：files/ 的既有调用习惯，合流后不必改 38 个引用方
        if (k.startsWith("on") && k.length > 2 && typeof v === "function") el.addEventListener(k.slice(2), v as EventListener)
        else if (v === true) el.setAttribute(k, "")
        else el.setAttribute(k, String(v))
    }
  }
}

/** 追加子节点（跳过空值；字符串走 textNode）。 */
export function append(el: HTMLElement, ...children: DomChild[]): void {
  for (const c of children) {
    if (c === null || c === undefined || c === false) continue
    el.appendChild(typeof c === "string" || typeof c === "number" ? document.createTextNode(String(c)) : c)
  }
}

/** 按对象设样式；键以 `--` 开头或 kebab 名用 setProperty，camel 名走 style 属性。 */
export function css(el: HTMLElement, style?: string | Partial<Record<string, string | number>>): void {
  if (!style) return
  if (typeof style === "string") {
    el.style.cssText = style
    return
  }
  for (const [k, v] of Object.entries(style)) {
    if (v === undefined || v === null) continue
    if (k.startsWith("--") || k.includes("-")) el.style.setProperty(k, String(v))
    else (el.style as unknown as Record<string, string>)[k] = String(v)
  }
}

/**
 * 注册事件监听并**返回解绑函数**。
 *
 * 为什么强调返回解绑函数：面板反复开合时忘记解绑是最常见的泄漏源，而"记得解绑"这件事
 * 靠自觉不可靠。配合 `mount()`，组件销毁时自动回收本组件注册的全部监听。
 */
export function on(
  el: HTMLElement | Document | Window,
  type: string,
  handler: (ev: Event) => void,
  opts?: AddEventListenerOptions,
): () => void {
  const fn = handler as EventListener
  el.addEventListener(type, fn, opts)
  return () => el.removeEventListener(type, fn, opts)
}

/** 清空容器（移除全部子节点）。 */
export function clear(el: HTMLElement): void {
  while (el.firstChild) el.removeChild(el.firstChild)
}

/** 语法糖：条件为真才建节点（便于在 append 参数里内联条件渲染）。 */
export function when(cond: boolean | undefined | null, make: () => DomChild): DomChild {
  return cond ? make() : null
}
