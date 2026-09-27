/**
 * 小地图偏好（`files/minimap.ts`）：与自动换行是镜像关系——**默认开启**，所以「关闭才写键」。
 * 轮盘的切换动作与按钮文案由 `files/main.ts` 的 minimapTitle 对应文案驱动，本文件只测偏好与文案。
 */
import { afterEach, describe, expect, test } from "bun:test"
import { minimapTitle, readMinimap, saveMinimap } from "./minimap"

const KEY = "gebai.ui.minimap"

afterEach(() => {
  localStorage.removeItem(KEY)
})

describe("小地图偏好（默认开启）", () => {
  test("默认开启（无记忆）——与自动换行的默认相反", () => {
    expect(readMinimap()).toBe(true)
  })

  test("关→读回 false；再开→键被清除而不是写 1（默认态无残留）", () => {
    saveMinimap(false)
    expect(localStorage.getItem(KEY)).toBe("0")
    expect(readMinimap()).toBe(false)
    saveMinimap(true)
    expect(localStorage.getItem(KEY)).toBeNull()
    expect(readMinimap()).toBe(true)
  })

  test("脏值（非 \"0\"）一律当默认的开启处理", () => {
    localStorage.setItem(KEY, "false")
    expect(readMinimap()).toBe(true)
    localStorage.setItem(KEY, "1")
    expect(readMinimap()).toBe(true)
  })
})

describe("轮盘按钮文案", () => {
  test("按当前态给动作（两态文案不同）", () => {
    expect(minimapTitle(true)).toBe("关闭小地图")
    expect(minimapTitle(false)).toBe("开启小地图")
  })
})
