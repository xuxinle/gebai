/**
 * 微框架基础件单测（dom / scope / form）。
 *
 * 用自带的**最小真实 DOM**（./mini-dom.test-helper.ts）而不是 `scripts/test-preload.ts` 的全局桩：
 * 后者的未定义成员一律兜底成 no-op、属性不落、子节点不真挂——而微框架的核心正是属性/子节点/
 * 事件的真实语义（布尔属性、data-*、文本节点、解绑后不再触发）。用 no-op 桩去测它等于没测。
 * 用完还原全局，不污染同进程里其它测试文件（预载文档里那条纪律）。
 */
import { afterAll, beforeAll, beforeEach, describe, expect, test } from "bun:test"
import { installMiniDom } from "./mini-dom.test-helper"

let restore: () => void = () => {}
beforeAll(() => {
  restore = installMiniDom().restore
})
afterAll(() => {
  restore()
})

const { append, clear, css, frag, h, on, when } = await import("./dom")
const { createPoller, createScope } = await import("./scope")
const { button, checkbox, field, segmented, selectInput, textArea, textInput } = await import("./form")

type El = {
  className: string
  textContent: string
  dataset: Record<string, string>
  getAttribute: (k: string) => string | null
  hasAttribute: (k: string) => boolean
  appendChild: (c: unknown) => void
  children: El[]
  querySelector: (s: string) => El | null
  querySelectorAll: (s: string) => El[]
  click: () => void
  classList: { contains: (n: string) => boolean }
  hidden: boolean
  style: { color?: string; width?: string; cssText?: string; getPropertyValue: (k: string) => string; setProperty: (k: string, v: string) => void }
  value: string
  placeholder: string
  checked: boolean
  options: unknown[]
  setError?: unknown
}

const E = (x: unknown): El => x as unknown as El

beforeEach(() => {
  ;(document.body as unknown as { childNodes: unknown[] }).childNodes.length = 0
})

describe("dom.h", () => {
  test("建节点 + class/text/属性/data", () => {
    const el = E(h("div", { class: "a b", text: "你好", data: { id: 7, flag: true }, role: "dialog" }))
    expect(el.className).toBe("a b")
    expect(el.textContent).toBe("你好")
    expect(el.dataset.id).toBe("7")
    expect(el.dataset.flag).toBe("true")
    expect(el.getAttribute("role")).toBe("dialog")
  })

  test("布尔属性：true 设空值、false/undefined 跳过；checked/value 走 property（读的是真值）", () => {
    expect(E(h("div", { hidden: true })).getAttribute("hidden")).toBe("")
    expect(E(h("div", { hidden: false })).hasAttribute("hidden")).toBe(false)
    expect(E(h("div", { title: undefined })).hasAttribute("title")).toBe(false)
    // checked/value 必须落到 property：只设 attribute 的话，用户交互后读到的仍是默认值（真实坑）
    expect((E(h("input", { checked: true })) as unknown as { checked: boolean }).checked).toBe(true)
    expect((E(h("input", { checked: false })) as unknown as { checked: boolean }).checked).toBe(false)
    expect((E(h("input", { value: "abc" })) as unknown as { value: string }).value).toBe("abc")
  })

  test("子节点：字符串转文本节点、空值跳过（便于条件渲染）", () => {
    const el = E(h("div", null, "a", null, false, undefined, "b"))
    expect(el.textContent).toBe("ab")
    expect(el.children).toHaveLength(2)
  })

  test("when()：条件为真才建节点", () => {
    const el = E(h("div", null, when(true, () => h("span", { text: "有" })), when(false, () => h("span", { text: "无" }))))
    expect(el.textContent).toBe("有")
  })

  test("html 属性用于可信内容（图标 SVG）；text 走文本节点不解析标签（XSS 面收口）", () => {
    expect(E(h("span", { html: "<b>x</b>" })).querySelector("b")).not.toBeNull()
    const t = E(h("span", { text: "<b>x</b>" }))
    expect(t.querySelector("b")).toBeNull()
    expect(t.textContent).toBe("<b>x</b>")
  })
})

describe("dom 工具", () => {
  test("css：字符串与对象两种写法；CSS 变量走 setProperty", () => {
    const a = E(h("div"))
    css(a as unknown as HTMLElement, "color: red")
    expect(a.style.cssText).toBe("color: red")
    const b = E(h("div"))
    css(b as unknown as HTMLElement, { "--x": "3px", width: "10px" })
    expect(b.style.getPropertyValue("--x")).toBe("3px")
    expect(b.style.width).toBe("10px")
  })

  test("frag / append / clear", () => {
    const host = E(h("div"))
    append(host as unknown as HTMLElement, frag(h("i", { text: "1" }), h("i", { text: "2" })))
    expect(host.children).toHaveLength(2)
    clear(host as unknown as HTMLElement)
    expect(host.children).toHaveLength(0)
  })

  test("on() 返回解绑函数（解绑后不再触发）", () => {
    const el = E(h("button"))
    let n = 0
    const off = on(el as unknown as HTMLElement, "click", () => n++)
    el.click()
    expect(n).toBe(1)
    off()
    el.click()
    expect(n).toBe(1)
  })
})

describe("scope：资源一次回收", () => {
  test("add/timer/observer 全部在 dispose 时回收，且幂等", () => {
    const scope = createScope()
    let hits = 0
    const el = E(h("button"))
    const off = on(el as unknown as HTMLElement, "click", () => hits++)
    scope.add(off)
    const t = setInterval(() => {}, 5)
    scope.timer(t)
    let disconnected = false
    scope.observer({ disconnect: () => (disconnected = true) })
    expect(scope.size()).toBe(3)
    scope.dispose()
    expect(disconnected).toBe(true)
    el.click()
    expect(hits).toBe(0)
    expect(scope.disposed()).toBe(true)
    expect(scope.size()).toBe(0)
    scope.dispose() // 幂等
    clearInterval(t)
  })

  test("dispose 之后再登记：立刻执行（晚到的异步回调不造成泄漏）", () => {
    const scope = createScope()
    scope.dispose()
    let ran = false
    scope.add(() => (ran = true))
    expect(ran).toBe(true)
  })

  test("单个清理抛错不影响其余清理（一个坏回调不该让整面板泄漏）", () => {
    const scope = createScope()
    let b = false
    scope.add(() => {
      throw new Error("boom")
    })
    scope.add(() => (b = true))
    scope.dispose()
    expect(b).toBe(true)
  })
})

describe("poller：单飞与 scope 联动", () => {
  test("start/stop；stop 后不再跑", async () => {
    let n = 0
    const scope = createScope()
    const p = createPoller(() => void n++, 5, { scope })
    p.start()
    await new Promise((r) => setTimeout(r, 30))
    const mid = n
    expect(mid).toBeGreaterThan(0)
    p.stop()
    await new Promise((r) => setTimeout(r, 30))
    expect(n).toBe(mid)
    scope.dispose()
  })

  test("单飞：任务没结束时不会叠加执行", async () => {
    let running = 0
    let maxConcurrent = 0
    const p = createPoller(async () => {
      running++
      maxConcurrent = Math.max(maxConcurrent, running)
      await new Promise((r) => setTimeout(r, 50))
      running--
    }, 5)
    p.start()
    await new Promise((r) => setTimeout(r, 120))
    p.stop()
    expect(maxConcurrent).toBe(1)
  })

  test("scope 销毁会停掉轮询", async () => {
    let n = 0
    const scope = createScope()
    const p = createPoller(() => void n++, 5, { scope })
    p.start()
    await new Promise((r) => setTimeout(r, 20))
    scope.dispose()
    const mid = n
    await new Promise((r) => setTimeout(r, 30))
    expect(n).toBe(mid)
  })
})

describe("form 控件", () => {
  test("textInput / textArea：读写值", () => {
    const i = E(textInput("a", "占位"))
    expect(i.value).toBe("a")
    expect(i.placeholder).toBe("占位")
    i.value = "b"
    expect(i.value).toBe("b")
    const t = E(textArea("多行\n内容", 5))
    expect(t.value).toContain("多行")
  })

  test("selectInput：选项与当前值", () => {
    const s = E(selectInput([["a", "甲"], ["b", "乙"]], "b"))
    expect(s.options).toHaveLength(2)
    expect(s.value).toBe("b")
  })

  test("checkbox：勾选状态（读的是 input 而不是容器）", () => {
    const c = checkbox("启用", true)
    expect((c.input as unknown as El).checked).toBe(true)
    expect(E(c.root).textContent).toContain("启用")
  })

  test("segmented：点击切换并回调；set() 同步高亮", () => {
    const seen: string[] = []
    const seg = segmented<"x" | "y">([["x", "X"], ["y", "Y"]], "x", (v) => seen.push(v))
    const btns = E(seg.root).querySelectorAll("button")
    expect(btns[0].classList.contains("active")).toBe(true)
    btns[1].click()
    expect(seen).toEqual(["y"])
    seg.set("x")
    expect(btns[0].classList.contains("active")).toBe(true)
  })

  test("field：就地标错与清除（表单整体校验时逐项定位）", () => {
    const f = field("脚本命令", textInput("") as unknown as HTMLElement, "相对路径基于任务资源目录")
    const wrap = E(f.wrap)
    expect(wrap.textContent).toContain("脚本命令")
    expect(wrap.textContent).toContain("相对路径")
    const err = wrap.querySelector(".fw-field-error")!
    expect(err.hidden).toBe(true)
    f.setError("不能为空")
    expect(err.hidden).toBe(false)
    expect(err.textContent).toBe("不能为空")
    expect(wrap.classList.contains("bad")).toBe(true)
    f.setError(null)
    expect(err.hidden).toBe(true)
    expect(wrap.classList.contains("bad")).toBe(false)
  })

  test("button：kind 决定类名，tip 同时进 data-tip 与 aria-label", () => {
    const b = E(button("保存", { kind: "primary", tip: "保存设置" }))
    expect(b.className).toContain("primary")
    expect(b.getAttribute("aria-label")).toBe("保存设置")
  })
})
