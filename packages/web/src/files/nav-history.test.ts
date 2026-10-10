import { describe, expect, test } from "bun:test"
import { createNavHistory, NAV_LIMIT, type NavEntry } from "./nav-history"

const at = (path: string, line: number, column = 1, root = "proj:x"): NavEntry => ({ root, path, line, column })

describe("编辑位置历史（后退/前进）", () => {
  test("空栈不可退不可进", () => {
    const h = createNavHistory()
    expect(h.canBack()).toBe(false)
    expect(h.canForward()).toBe(false)
    expect(h.back()).toBeNull()
    expect(h.forward()).toBeNull()
  })

  test("单个位置：跳过一次后仍不可后退（没有「上一个」）", () => {
    const h = createNavHistory()
    h.jump(at("a.ts", 10))
    expect(h.canBack()).toBe(false)
    expect(h.back()).toBeNull()
  })

  test("跨文件后退/前进：目标与顺序", () => {
    const h = createNavHistory()
    h.jump(at("a.ts", 10))
    h.jump(at("b.ts", 5))
    h.jump(at("c.ts", 1))
    expect(h.canBack()).toBe(true)
    expect(h.back()).toEqual(at("b.ts", 5))
    expect(h.back()).toEqual(at("a.ts", 10))
    expect(h.canBack()).toBe(false)
    // 前进沿原路回去
    expect(h.forward()).toEqual(at("b.ts", 5))
    expect(h.forward()).toEqual(at("c.ts", 1))
    expect(h.canForward()).toBe(false)
    expect(h.forward()).toBeNull()
  })

  test("同位置重复 jump 不入栈（点击已开标签不产生假历史）", () => {
    const h = createNavHistory()
    h.jump(at("a.ts", 10))
    h.jump(at("b.ts", 1))
    h.jump(at("b.ts", 1)) // 激活已有标签 / 重复打开
    expect(h.peek().stack).toHaveLength(2)
    expect(h.back()).toEqual(at("a.ts", 10))
  })

  test("同文件近距离跳转合并进栈顶，不单开记录", () => {
    const h = createNavHistory()
    h.jump(at("a.ts", 10))
    h.jump(at("a.ts", 12)) // 差 2 行 < NEAR_LINE
    expect(h.peek().stack).toHaveLength(1)
    expect(h.peek().stack[0]).toEqual(at("a.ts", 12))
    h.jump(at("a.ts", 40)) // 远距离 = 一次真正的跳转
    expect(h.peek().stack).toHaveLength(2)
  })

  test("后退后再 jump 截断前进段（与浏览器地址栏同构）", () => {
    const h = createNavHistory()
    h.jump(at("a.ts", 10))
    h.jump(at("b.ts", 5))
    h.jump(at("c.ts", 1))
    h.back() // → b.ts
    expect(h.canForward()).toBe(true)
    h.jump(at("d.ts", 3)) // 新导航：c.ts 的前进分支作废
    expect(h.canForward()).toBe(false)
    expect(h.peek().stack).toHaveLength(3)
    expect(h.back()).toEqual(at("b.ts", 5))
    expect(h.back()).toEqual(at("a.ts", 10))
  })

  test("updateTop 用当前光标修正栈顶（前进回到离开时最后看的位置）", () => {
    const h = createNavHistory()
    h.jump(at("a.ts", 10))
    h.jump(at("b.ts", 5))
    // 在 a.ts 里又读了 300 行才切走——a.ts 不是栈顶，它的记录不该被动
    h.updateTop({ root: "proj:x", path: "a.ts", line: 310, column: 4 })
    expect(h.peek().stack[0]).toEqual(at("a.ts", 10))
    // 当前文件（b.ts，栈顶）的光标移动修正栈顶
    h.updateTop({ root: "proj:x", path: "b.ts", line: 88, column: 2 })
    expect(h.peek().stack[1]).toEqual(at("b.ts", 88, 2))
    expect(h.back()).toEqual(at("a.ts", 10))
    // 前进回到的是**离开时最后看的位置**（88 行），不是 jump 时的 5 行
    expect(h.forward()).toEqual(at("b.ts", 88, 2))
  })

  test("后退后的位置作为「现在」留在栈里：从那里 jump 会正确截断", () => {
    const h = createNavHistory()
    h.jump(at("a.ts", 10))
    h.jump(at("b.ts", 5))
    h.back() // → a.ts:10
    h.jump(at("z.ts", 1))
    const { stack, index } = h.peek()
    expect(stack).toHaveLength(2)
    expect(index).toBe(1)
    expect(h.back()).toEqual(at("a.ts", 10))
  })

  test("上限截断：最早的条目被挤出，仍可连续后退", () => {
    const h = createNavHistory()
    for (let i = 1; i <= NAV_LIMIT + 10; i++) h.jump(at(`f${i}.ts`, i * 10))
    const { stack } = h.peek()
    expect(stack).toHaveLength(NAV_LIMIT)
    expect(stack[0]).toEqual(at(`f11.ts`, 110))
    let n = 0
    while (h.back()) n++
    expect(n).toBe(NAV_LIMIT - 1)
  })

  test("跨根（root 不同）永远算不同位置", () => {
    const h = createNavHistory()
    h.jump(at("a.ts", 10, 1, "proj:x"))
    h.jump(at("a.ts", 10, 1, "proj:y")) // 同路径不同根 = 两个文件
    expect(h.peek().stack).toHaveLength(2)
    expect(h.back()).toEqual(at("a.ts", 10, 1, "proj:x"))
  })

  test("非法行号归一（0/负 → 1）", () => {
    const h = createNavHistory()
    h.jump(at("a.ts", 0, 0))
    expect(h.peek().stack[0]).toEqual(at("a.ts", 1, 1))
  })
})
