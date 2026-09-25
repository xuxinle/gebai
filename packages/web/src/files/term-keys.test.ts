/**
 * 终端键位表（`files/term-keys.ts`，纯数据）。
 *
 * 这里跑的是 `validateKeymap`——把「接管浏览器默认要显式声明」「保留键一律报错」这两条
 * 政策约束**落在终端键上**：面板里的键位是运行时登记的，不进这张测试就没人拦得住
 * 「顺手写上 Ctrl+PageDown（浏览器标签页切换，页面根本收不到事件）」这类死键。
 */
import { describe, expect, test } from "bun:test"
import { browserConflict, toSpecList, validateKeymap } from "../keymap"
import { TERM_KEYS, termKeyBindings } from "./term-keys"

describe("term-keys（终端键位表）", () => {
  const bindings = termKeyBindings(() => null)

  test("通过键位表校验：无重复、无保留键、接管声明齐备", () => {
    expect(validateKeymap(bindings)).toEqual([])
  })

  test("不带保留键（Ctrl+PageUp/PageDown、Ctrl+Tab 这类浏览器自己处理，页面收不到）", () => {
    for (const b of bindings) {
      for (const spec of toSpecList(b.keys)) {
        expect(browserConflict(spec).level).not.toBe("reserved")
      }
    }
  })

  test("全部限定在终端焦点、捕获阶段（要抢在 xterm 与工作台全局键之前）", () => {
    for (const b of bindings) {
      expect(b.focus).toEqual(["terminal"])
      expect(b.phase).toBe("capture")
    }
  })

  test("复制/粘贴/查找/字号/清屏/全选/切标签各就位（VSCode 语汇或终端惯例键）", () => {
    const keysOf = (id: string) => bindings.find((b) => b.id === id)?.keys
    expect(keysOf("wb.term.copy")).toEqual(["Ctrl+Shift+C", "Ctrl+Insert"])
    expect(keysOf("wb.term.paste")).toEqual(["Ctrl+Shift+V", "Shift+Insert"])
    expect(keysOf("wb.term.search")).toEqual(["Ctrl+F"])
    expect(keysOf("wb.term.clear")).toEqual(["Ctrl+Shift+K"])
    expect(keysOf("wb.term.selectAll")).toEqual(["Ctrl+Shift+A"])
    expect(keysOf("wb.term.closeTab")).toEqual(["Alt+Shift+W"])
    expect(keysOf("wb.term.nextTab")).toEqual(["Ctrl+Shift+↓"])
  })

  test("不占 shell 的读行键：Ctrl+K（删至行尾）/ Ctrl+A（行首）/ Ctrl+E / Ctrl+W / Ctrl+L 均不在表内", () => {
    const specs = bindings.flatMap((b) => toSpecList(b.keys))
    // xterm 会把 Ctrl+K 编成 ^K 发给 shell（readline kill-line），Ctrl+L 则是 readline 的清屏，
    // 都不能被面板抢走；Ctrl+字母只有加上 Shift 才不产生字节（见 xterm Keyboard.ts）。
    for (const forbidden of ["Ctrl+K", "Ctrl+A", "Ctrl+E", "Ctrl+W", "Ctrl+L", "Ctrl+U", "Ctrl+P", "Ctrl+N", "Ctrl+B"]) {
      expect(specs).not.toContain(forbidden)
    }
  })

  test("动作转发到最新面板（面板重建后旧句柄不残留）", () => {
    const calls: string[] = []
    const table = termKeyBindings(() => ({
      copy: () => calls.push("copy"),
      paste: () => calls.push("paste"),
      search: () => calls.push("search"),
      fontSize: (d) => calls.push(`font${d}`),
      fontReset: () => calls.push("fontReset"),
      interrupt: () => calls.push("interrupt"),
      clear: () => calls.push("clear"),
      selectAll: () => calls.push("selectAll"),
      newTab: () => calls.push("newTab"),
      closeTab: () => calls.push("closeTab"),
      switchTab: (d) => calls.push(`tab${d}`),
      scroll: (to) => calls.push(`scroll:${to}`),
    }))
    for (const id of ["wb.term.copy", "wb.term.clear", "wb.term.nextTab", "wb.term.scrollBottom", "wb.term.interrupt"]) {
      table.find((b) => b.id === id)?.run({} as never)
    }
    expect(calls).toEqual(["copy", "clear", "tab1", "scroll:bottom", "interrupt"])
    // 句柄为 null（面板未装配）时静默忽略，不抛错
    for (const b of termKeyBindings(() => null)) b.run({} as never)
  })

  test("键位常量与绑定表一一对应（改表漏改绑定会被这里拦住）", () => {
    expect(TERM_KEYS.clear).toEqual(["Ctrl+Shift+K"])
    const ids = bindings.map((b) => b.id)
    expect(new Set(ids).size).toBe(ids.length)
  })
})
