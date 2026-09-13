/**
 * Web 测试基线环境（`bunfig.toml` 的 `[test] preload`）：在任何测试文件之前装好一套**完整**的最小 DOM。
 *
 * 为什么需要：不少页面模块在 **import 期就绑定真实 DOM**（顶层的 `document.addEventListener`、
 * `createStickyScroll` 需要 `MutationObserver`、`document.getElementById` 取固定节点等），而
 * bun test 本身没有 DOM。此前各测试文件各自在模块顶层临时补桩，且**从不清除**——于是同一进程里
 * 后加载的测试文件看到的是「上一个文件留下的桩」，而留下哪个取决于**文件执行顺序**（Windows 与
 * Linux 的顺序不同），表现为同一提交在一个平台全绿、另一个平台整片报错。基线在这里装一次，
 * 顺序就再也不是变量；各文件自有的富桩（messages/state/ui 的 Proxy 元素树）仍然覆盖本基线。
 *
 * 元素与 document 都用 Proxy 兜底未定义成员为 no-op 函数：DOM 成员极多，逐个补不现实，
 * 兜底后「多访问一个方法」不会炸；返回值类接口（getElementById/createElement/querySelector）
 * 必须显式实现，否则调用方拿到的是 no-op 函数而不是元素。
 *
 * 只装「模块加载期必需」的成员：被用例刻意当作**缺省**验证回退路径的全局（如
 * IntersectionObserver）不能装，否则会把回退分支盖掉。
 */

type AnyEl = Record<string, unknown>

/** 最小元素桩：Proxy 兜底未定义成员为 no-op，已实现的多为「会被读值/被断言」的那些。 */
function makeEl(tag = "div"): AnyEl {
  const listeners = new Map<string, Array<(ev?: unknown) => void>>()
  const children: AnyEl[] = []
  const base: AnyEl = {
    tagName: tag.toUpperCase(),
    nodeType: 1,
    nodeName: tag.toUpperCase(),
    children,
    childNodes: children,
    classList: {
      add() {},
      remove() {},
      toggle() {},
      contains: () => false,
    },
    style: {},
    dataset: {},
    hidden: false,
    isConnected: true,
    parentElement: null,
    parentNode: null,
    firstChild: null,
    lastChild: null,
    firstElementChild: null,
    nextElementSibling: null,
    previousElementSibling: null,
    textContent: "",
    innerText: "",
    innerHTML: "",
    outerHTML: "",
    value: "",
    checked: false,
    disabled: false,
    scrollTop: 0,
    scrollHeight: 0,
    clientHeight: 0,
    clientWidth: 0,
    offsetHeight: 0,
    offsetWidth: 0,
    appendChild: (c: AnyEl) => {
      children.push(c)
      return c
    },
    append: (...cs: AnyEl[]) => {
      children.push(...cs)
    },
    prepend: (...cs: AnyEl[]) => {
      children.unshift(...cs)
    },
    insertBefore: (c: AnyEl) => {
      children.unshift(c)
      return c
    },
    replaceChildren: (...cs: AnyEl[]) => {
      children.splice(0, children.length, ...cs)
    },
    removeChild: (c: AnyEl) => {
      const i = children.indexOf(c)
      if (i >= 0) children.splice(i, 1)
      return c
    },
    remove: () => {
      const p = (base.parentNode as AnyEl | null)?.children as AnyEl[] | undefined
      const i = p ? p.indexOf(base) : -1
      if (p && i >= 0) p.splice(i, 1)
    },
    contains: () => false,
    closest: () => null,
    matches: () => false,
    querySelector: () => null,
    querySelectorAll: () => [],
    getElementsByTagName: () => [],
    getElementsByClassName: () => [],
    setAttribute() {},
    getAttribute: () => null,
    removeAttribute() {},
    hasAttribute: () => false,
    focus() {},
    blur() {},
    click() {},
    scrollTo() {},
    scrollIntoView() {},
    cloneNode: () => makeEl(tag),
    getBoundingClientRect: () => ({ top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0, x: 0, y: 0 }),
    addEventListener: (type: string, cb: (ev?: unknown) => void) => {
      listeners.set(type, [...(listeners.get(type) ?? []), cb])
    },
    removeEventListener: (type: string, cb: (ev?: unknown) => void) => {
      listeners.set(type, (listeners.get(type) ?? []).filter((f) => f !== cb))
    },
    dispatchEvent: (ev: unknown) => {
      const type = (ev as { type?: string })?.type ?? ""
      for (const cb of listeners.get(type) ?? []) cb(ev)
      return true
    },
  }
  return new Proxy(base, {
    get(t, k) {
      if (typeof k === "string" && k in t) return (t as Record<string, unknown>)[k]
      return () => {}
    },
  })
}

/** 元素按 id 缓存：同一 id 反复取到同一节点（贴近真实 DOM，也让「先取后挂」的模块行为一致）。 */
const byId = new Map<string, AnyEl>()
function elById(id: string): AnyEl {
  let el = byId.get(id)
  if (!el) {
    el = makeEl("div")
    byId.set(id, el)
  }
  return el
}

const documentBase: AnyEl = {
  getElementById: (id: string) => elById(String(id)),
  createElement: (tag?: string) => makeEl(tag ?? "div"),
  createElementNS: (_ns: string, tag?: string) => makeEl(tag ?? "div"),
  createTextNode: (text: string) => ({ nodeType: 3, textContent: text }),
  createDocumentFragment: () => makeEl("fragment"),
  querySelector: () => null,
  querySelectorAll: () => [],
  getElementsByTagName: () => [],
  addEventListener() {},
  removeEventListener() {},
  dispatchEvent: () => true,
  body: makeEl("body"),
  head: makeEl("head"),
  documentElement: makeEl("html"),
  currentScript: null,
  baseURI: "http://localhost/",
  hidden: false,
  visibilityState: "visible",
  title: "",
  cookie: "",
}

const doc = new Proxy(documentBase, {
  get(t, k) {
    if (typeof k === "string" && k in t) return (t as Record<string, unknown>)[k]
    return () => {}
  },
})

/** 内存版 Storage（localStorage/sessionStorage 语义足够：get/set/remove/clear/length/key）。 */
function makeStorage(): Storage {
  const map = new Map<string, string>()
  return {
    getItem: (k: string) => map.get(k) ?? null,
    setItem: (k: string, v: string) => void map.set(k, String(v)),
    removeItem: (k: string) => void map.delete(k),
    clear: () => map.clear(),
    key: (i: number) => [...map.keys()][i] ?? null,
    get length() {
      return map.size
    },
  } as Storage
}

const g = globalThis as Record<string, unknown>
g.document = doc
// window = globalThis（与浏览器同构的取用方式）；监听方法显式给 no-op，否则 `window.addEventListener` 为 undefined
g.addEventListener ??= () => {}
g.removeEventListener ??= () => {}
g.window = globalThis
g.MutationObserver = class {
  observe() {}
  disconnect() {}
  takeRecords() {
    return []
  }
}
g.ResizeObserver = class {
  observe() {}
  unobserve() {}
  disconnect() {}
}
// IntersectionObserver 故意不装（见文件头说明）
g.requestAnimationFrame = (cb: (t: number) => void) => {
  cb(0)
  return 0
}
g.cancelAnimationFrame = () => {}
g.navigator ??= {}
;(g.navigator as Record<string, unknown>).userAgent ??= "bun-test"
;(g.navigator as Record<string, unknown>).onLine ??= true
g.location ??= { protocol: "http:", host: "localhost", href: "http://localhost/", search: "", pathname: "/" }
g.localStorage ??= makeStorage()
g.sessionStorage ??= makeStorage()
g.getComputedStyle ??= () => ({ getPropertyValue: () => "", backgroundColor: "" })
g.matchMedia ??= () => ({ matches: false, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} })
g.Image = class {
  src = ""
  onload: (() => void) | null = null
  onerror: (() => void) | null = null
  width = 0
  height = 0
  naturalWidth = 0
  naturalHeight = 0
}
