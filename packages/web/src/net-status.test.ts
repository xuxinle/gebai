/**
 * 网络连通性判据（`net-status.ts`）单测。
 *
 * 守的是「网没了 / 网回来了」这两瞬间的行为，因为它们决定用户看到什么：
 * - **断网**：必须立即如实标断开 + 主动断连。浏览器判离线时 WS 对象往往仍 OPEN、心跳照跑、
 *   不自己 close（"半死连接"）——不断开的话界面会持续谎报"已连接"，用户发的消息石沉大海。
 * - **恢复**：必须立即自检。只靠 10 秒定时自检的话，网回来后最长白等 10 秒，
 *   这段时间界面停旧状态，用户看到的就是"网回来了但还卡着"。
 */
import { describe, expect, test } from "bun:test"
import { actionForNetEvent, bindNetworkEvents, shouldRewriteConnLabel, type NetEvent } from "./net-status"

describe("actionForNetEvent：网络事件 → 动作", () => {
  test("offline：立即标断开 + 主动断连（半死连接只能靠主动 close 触发重连）", () => {
    const a = actionForNetEvent("offline")
    expect(a.dropConnection).toBe(true)
    expect(a.warn).toBe(true)
    expect(a.label).toContain("断开")
    // 断网时不该去自检（网络都没了，请求必然失败，徒增噪声）
    expect(a.verify).toBe(false)
  })

  test("online：立即自检", () => {
    const a = actionForNetEvent("online")
    expect(a.verify).toBe(true)
    // 恢复时不该主动断连（正想接回去）
    expect(a.dropConnection).toBe(false)
  })

  test("online 的文案不写死「已连接」——是否真连上由自检结果决定", () => {
    const a = actionForNetEvent("online")
    expect(a.label ?? "").not.toBe("已连接")
    expect(a.label ?? "").not.toContain("已连接")
  })

  test("两种事件都给出文案（用户要能立刻知道发生了什么）", () => {
    for (const ev of ["online", "offline"] as NetEvent[]) {
      expect(actionForNetEvent(ev).label).toBeTruthy()
    }
  })

  test("两个事件的文案不同（否则用户分不清断网与恢复）", () => {
    expect(actionForNetEvent("online").label).not.toBe(actionForNetEvent("offline").label)
  })
})

describe("shouldRewriteConnLabel：避免每 10 秒自检反复冲掉状态栏", () => {
  test("相同文案不重写（自检周期跑，重复写会冲掉具体错误信息）", () => {
    expect(shouldRewriteConnLabel("已连接", "已连接")).toBe(false)
  })

  test("文案变化才重写", () => {
    expect(shouldRewriteConnLabel("已连接", "网络已断开，等待恢复…")).toBe(true)
  })
})

describe("bindNetworkEvents：订阅与解绑", () => {
  function fakeSource() {
    const listeners = new Map<string, Array<() => void>>()
    return {
      addEventListener: (t: string, fn: () => void) => {
        listeners.set(t, [...(listeners.get(t) ?? []), fn])
      },
      removeEventListener: (t: string, fn: () => void) => {
        listeners.set(t, (listeners.get(t) ?? []).filter((f) => f !== fn))
      },
      fire: (t: string) => {
        for (const fn of listeners.get(t) ?? []) fn()
      },
      count: () => [...listeners.values()].reduce((n, arr) => n + arr.length, 0),
    }
  }

  test("online/offline 都能收到", () => {
    const src = fakeSource()
    const seen: NetEvent[] = []
    bindNetworkEvents(src, (ev) => seen.push(ev))
    src.fire("online")
    src.fire("offline")
    expect(seen).toEqual(["online", "offline"])
  })

  test("返回的解绑函数真的解绑（否则页面反复初始化会堆叠监听，一次断网跑 N 次自检）", () => {
    const src = fakeSource()
    const seen: NetEvent[] = []
    const off = bindNetworkEvents(src, (ev) => seen.push(ev))
    off()
    src.fire("online")
    expect(seen).toEqual([])
    expect(src.count()).toBe(0)
  })

  test("无 navigator（非浏览器环境）时安全降级为 no-op", () => {
    const off = bindNetworkEvents(null, () => {})
    expect(typeof off).toBe("function")
    off() // 不应抛错
    const off2 = bindNetworkEvents(undefined, () => {})
    off2()
  })

  test("**收到没有 addEventListener 的对象时降级为 no-op**（不抛错，但也什么都不做）", () => {
    // 真实踩过的坑：`navigator` 只有 onLine 属性，没有 addEventListener
    // （online/offline 是 window 的事件）。挂错对象时这里会静默 no-op——
    // 不报错、测试也全绿，但断网时界面照旧显示已连接，看起来就像“改了没生效”。
    const navLike = { onLine: true }
    let called = false
    const off = bindNetworkEvents(navLike as never, () => (called = true))
    expect(typeof off).toBe("function")
    off()
    expect(called).toBe(false)
  })

  test("重复订阅两次 → 解绑一次仍有一次生效（解绑只摘自己那个）", () => {
    const src = fakeSource()
    let n = 0
    const off1 = bindNetworkEvents(src, () => n++)
    bindNetworkEvents(src, () => n++)
    off1()
    src.fire("online")
    expect(n).toBe(1)
  })
})
