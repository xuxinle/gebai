/**
 * @plantuml/core 浏览器 DOM 垫层（模块加载副作用）：
 * - 该模块必须**最先**被 `plantuml.ts` 静态导入——ESM 依赖按导入顺序求值，
 *   保证 window/document/XMLSerializer 就绪后再执行 viz-global.js 与 plantuml.js（TeaVM 引擎）。
 * - 幂等：全局只注入一次；经实测不影响 web-tree-sitter 离线解析（其 wasm 加载走 process 路径）。
 */
/** 从 canvas font 字符串解析字号（px），如 "bold 14px sans-serif" → 14。 */
function parseFontSize(font: string): number {
  const m = String(font).match(/(\d+(?:\.\d+)?)px/)
  return m ? Number(m[1]) : 14
}

/** 按字符类型估算文本宽度（em）：CJK 全角 ≈1em，ASCII ≈0.55em，空格 ≈0.28em。 */
function estimateTextWidth(s: string, size: number): number {
  let w = 0
  for (const ch of String(s)) {
    const code = ch.codePointAt(0)!
    if (code === 0x20) w += 0.28
    else if (
      (code >= 0x4e00 && code <= 0x9fff) || // CJK 统一表意
      (code >= 0x3000 && code <= 0x303f) || // CJK 标点（全角，含全角空格）
      (code >= 0xff00 && code <= 0xffef) || // 全角形式
      (code >= 0xac00 && code <= 0xd7af) // 谚文
    )
      w += 1
    else w += 0.55
  }
  return w * size
}

const ctx2d = (): Record<string, unknown> => {
  let font = ""
  return {
    set font(v: unknown) {
      font = String(v ?? "")
    },
    get font(): string {
      return font
    },
    measureText(s: string) {
      const size = parseFontSize(font)
      return { width: estimateTextWidth(s, size), actualBoundingBoxAscent: size * 0.8, actualBoundingBoxDescent: size * 0.2 }
    },
  }
}

interface DomElement {
  nodeType: number
  parentNode: unknown
  nodeName: string
  childNodes: DomElement[]
  attributes: Record<string, string>
  style: Record<string, string>
  textContent: string
  setAttribute(k: string, v: unknown): void
  getAttribute(k: string): string | null
  appendChild(c: unknown): unknown
  insertBefore(c: unknown, ref: unknown): unknown
  removeChild(c: unknown): unknown
  getBBox(): { x: number; y: number; width: number; height: number }
  addEventListener(): void
  removeEventListener(): void
}

function makeDomShim(): void {
  const esc = (s: string) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;")
  const xmlText = (s: string) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")

  class TextNode {
    nodeName = "#text"
    nodeType = 3
    parentNode: unknown = null
    data = ""
    set textContent(v: unknown) {
      this.data = String(v ?? "")
    }
    get textContent(): string {
      return this.data
    }
  }
  class Element implements DomElement {
    nodeType = 1
    parentNode: unknown = null
    nodeName = "div"
    childNodes: DomElement[] = []
    attributes: Record<string, string> = {}
    style: Record<string, string> = {}
    id = ""
    setAttribute(k: string, v: unknown) {
      this.attributes[k] = String(v)
    }
    setAttributeNS(_ns: string, k: string, v: unknown) {
      this.attributes[k] = String(v)
    }
    getAttribute(k: string): string | null {
      return this.attributes[k] ?? null
    }
    hasAttribute(k: string): boolean {
      return k in this.attributes
    }
    removeAttribute(k: string) {
      delete this.attributes[k]
    }
    appendChild(c: unknown): unknown {
      if (c == null) return c
      ;(c as DomElement).parentNode = this
      this.childNodes.push(c as DomElement)
      return c
    }
    insertBefore(c: unknown, ref: unknown): unknown {
      ;(c as DomElement).parentNode = this
      const i = this.childNodes.indexOf(ref as DomElement)
      if (i < 0) this.childNodes.push(c as DomElement)
      else this.childNodes.splice(i, 0, c as DomElement)
      return c
    }
    removeChild(c: unknown): unknown {
      const i = this.childNodes.indexOf(c as DomElement)
      if (i >= 0) this.childNodes.splice(i, 1)
      ;(c as DomElement).parentNode = null
      return c
    }
    set textContent(v: unknown) {
      this.childNodes = []
      if (v != null && v !== "") {
        const t = new TextNode()
        t.data = String(v)
        this.childNodes.push(t as unknown as DomElement)
      }
    }
    get textContent(): string {
      return this.childNodes.map((c) => (c.nodeType === 3 ? (c as unknown as TextNode).data : c.textContent ?? "")).join("")
    }
    getBBox() {
      const size = parseFontSize(this.attributes["font-size"] ?? "")
      const len = estimateTextWidth(this.textContent, size)
      return { x: 0, y: 0, width: len, height: size * 1.2 }
    }
    getComputedTextLength() {
      const size = parseFontSize(this.attributes["font-size"] ?? "")
      return Math.max(estimateTextWidth(this.textContent, size), 1)
    }
    getBoundingClientRect() {
      return { x: 0, y: 0, width: 10, height: 10, top: 0, left: 0, right: 10, bottom: 10 }
    }
    addEventListener() {}
    removeEventListener() {}
  }

  const serialize = (node: DomElement): string => {
    if (node.nodeType === 3) return xmlText((node as unknown as TextNode).data)
    const attrs = Object.entries(node.attributes)
      .map(([k, v]) => ` ${k}="${esc(v)}"`)
      .join("")
    if (!node.childNodes.length) return `<${node.nodeName}${attrs}/>`
    return `<${node.nodeName}${attrs}>${node.childNodes.map((c) => serialize(c)).join("")}</${node.nodeName}>`
  }

  class XmlSerializer {
    serializeToString(node: DomElement): string {
      return serialize(node)
    }
  }

  const makeEl = (tag: string): Element => {
    const e = new Element()
    e.nodeName = tag
    if (tag === "canvas") {
      ;(e as Element & { getContext: unknown }).getContext = () => ctx2d()
    }
    return e
  }

  const document: Record<string, unknown> = {
    baseURI: "file:///gebai/",
    currentScript: null,
    createElement: (t: string) => makeEl(t),
    createElementNS: (_ns: string, t: string) => makeEl(t),
    createTextNode: (d: string) => {
      const t = new TextNode()
      t.data = d
      return t
    },
    getElementById: () => null,
    getElementsByTagName: () => [],
    head: makeEl("head"),
    body: makeEl("body"),
    documentElement: makeEl("html"),
    cookie: "",
    write() {},
    location: { href: "", protocol: "https:" },
    addEventListener() {},
    removeEventListener() {},
  }

  ;(globalThis as Record<string, unknown>).window = globalThis
  ;(globalThis as Record<string, unknown>).document = document
  ;(globalThis as Record<string, unknown>).XMLSerializer = XmlSerializer
  ;(globalThis as Record<string, unknown>).self = globalThis
  ;(globalThis as Record<string, unknown>).navigator = { userAgent: "gebai-feishu-bot" }
  ;(globalThis as Record<string, unknown>).location = { href: "", protocol: "https:" }
}

/**
 * 重新应用 PlantUML DOM 垫层（组合渲染器在 happy-dom 垫层与本品之间切换全局环境时调用）：
 * TeaVM 引擎渲染期间依赖本垫层的 document/XMLSerializer 实现。注意：本垫层会设置
 * `globalThis.window = globalThis`，与 Bun worker_threads（D2 渲染器）冲突，调用方渲染完成后需移除。
 */
export function applyPlantUmlDomShim(): void {
  makeDomShim()
}

makeDomShim()
