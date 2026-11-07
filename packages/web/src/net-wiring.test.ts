/**
 * 网络事件接线的静态护栏。
 *
 * 这条测试存在的唯一原因是一个**已经踩过**的坑：
 * `online`/`offline` 是 **window** 的事件，`navigator` 只有只读的 `onLine` 属性、
 * **没有 addEventListener**。如果有人（包括我）顺手写成 `navigator.addEventListener("online", …)`：
 * - TypeScript 不报错（类型断言或 navigate 类型宽松处都能混过去）；
 * - `bindNetworkEvents` 会安全降级成 no-op（不抛错）；
 * - 单看单元测试全绿——但**断网时界面照旧显示"已连接"**，因为监听根本没挂上。
 *
 * 这就是"改了没生效"三变体里最难查的一种：不报错、测试绿、行为没变。
 * 静态盯住绑定对象，比事后到浏览器里观察便宜得多。
 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const ATTACH = readFileSync(join(import.meta.dir, "attach.ts"), "utf8")

describe("bindConnectionVerify 的网络事件接线", () => {
  test("网络事件绑定在 **window** 上（navigator 没有 addEventListener）", () => {
    const i = ATTACH.indexOf("bindNetworkEvents(")
    expect(i).toBeGreaterThan(0)
    // 取该调用的第一个实参（绑定对象）
    const call = ATTACH.slice(i, i + 260)
    expect(call).toContain("window")
    expect(call).not.toContain("navigator")
  })

  test("确实订阅了网络事件（不是删掉绑定留个空函数）", () => {
    expect(ATTACH).toContain("bindNetworkEvents(")
    expect(ATTACH).toContain("handleNetEvent(")
  })

  test("offline 时主动判死连接（半死连接不会自己 close）", () => {
    const i = ATTACH.indexOf("function handleNetEvent(")
    const body = ATTACH.slice(i, i + 900)
    expect(body).toContain("dropConnection")
  })

  test("online 时立即自检（不等 10 秒定时器）", () => {
    const i = ATTACH.indexOf("function handleNetEvent(")
    const body = ATTACH.slice(i, i + 900)
    expect(body).toContain("verifyConnection")
  })
})
