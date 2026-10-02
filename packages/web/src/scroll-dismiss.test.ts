import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"
import { dismissOnScroll } from "./scroll-dismiss"

const src = (file: string) => readFileSync(join(import.meta.dirname, file), "utf8")

/** 滚动容器替身：contains 只对给定的后代集合为真。 */
function host(descendants: unknown[] = []): { contains: (n: unknown) => boolean } {
  return { contains: (n: unknown) => descendants.includes(n) }
}

const documentNode = { nodeType: 9 }

describe("浮层随滚动关闭的判定", () => {
  test("宿主滚动容器自身滚动 → 关闭", () => {
    const h = host()
    expect(dismissOnScroll(h, h)).toBe(true)
  })

  test("宿主容器内的元素滚动 → 关闭", () => {
    const inner = {}
    expect(dismissOnScroll(inner, host([inner]))).toBe(true)
  })

  test("页面级滚动（target 为 document）→ 关闭", () => {
    expect(dismissOnScroll(documentNode, host())).toBe(true)
  })

  test("无关容器的滚动 → 不关闭（生成中消息流的粘底自动滚动不冲掉菜单）", () => {
    const messages = {}
    expect(dismissOnScroll(messages, host())).toBe(false)
  })

  test("宿主不在滚动容器内（host 为 null）时只有页面级滚动关闭", () => {
    expect(dismissOnScroll({}, null)).toBe(false)
    expect(dismissOnScroll(documentNode, null)).toBe(true)
  })

  test("空 target 不关闭（防御：事件可能无 target）", () => {
    expect(dismissOnScroll(null, host())).toBe(false)
    expect(dismissOnScroll(undefined, host())).toBe(false)
  })
})

// 接线守卫（约定同 continuity.test.ts）：判定必须真的被会话菜单用上，且不得退回「任意滚动一律关」
describe("会话右键菜单的滚动关闭接线", () => {
  const s = src("sessions.ts")

  test("滚动关闭按锚点容器判定", () => {
    expect(s).toContain("dismissOnScroll(e.target, ctxMenuHost)")
    expect(s).toContain("ctxMenuHost = sessionList")
    expect(s).not.toContain('document.addEventListener("scroll", () => closeSessionMenu(), true)')
  })

  test("点击 / 新右键关闭保留", () => {
    expect(s).toContain('document.addEventListener("click", () => closeSessionMenu())')
    expect(s).toContain('document.addEventListener("contextmenu", () => closeSessionMenu(), true)')
  })
})
