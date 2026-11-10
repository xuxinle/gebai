/**
 * 待办状态查表的兜底契约（回归护栏）。
 *
 * 事故形态（2026-11-10 实测，会话 ace35552d）：待办工具 `update` 收到了 `status: "deleted"`
 * （模型把「删除」当成状态值用），服务端原样落盘；前端 `TODO_STATUS_ICONS[status].svg` 因此
 * 在 `undefined` 上取 `.svg` 抛 TypeError，**整个消息块的渲染中断**——窗口化（virtualize.ts）
 * 随后把那一大段永远留成空白（页面上下滚动全是白的，DOM 里连块都不存在）。
 *
 * 两层防线，本文件守第二层：
 * ① 服务端 todo 工具的状态白名单校验（packages/server core/tools/interact.ts 的 isTodoStatus）；
 * ② 渲染层对陌生状态值**绝不抛错**：回退兜底图标 + 把原值带进 label（这里）。
 */
import { describe, expect, test } from "bun:test"
import { isKnownTodoStatus, statusClassSuffix, statusIcon, TODO_STATUS_ICONS, UNKNOWN_STATUS_ICON } from "./todo-status"

describe("statusIcon（状态图标查表）", () => {
  test("白名单内的状态取到对应图标与中文标签", () => {
    for (const s of ["pending", "in_progress", "completed", "failed", "cancelled"]) {
      const ico = statusIcon(s)
      expect(ico).toBe(TODO_STATUS_ICONS[s]!)
      expect(ico.svg).toContain("<svg")
      expect(ico.label.length).toBeGreaterThan(0)
    }
  })

  test("白名单外的状态不抛错：回退兜底图标，并把原值带进 label（可定位脏数据来源）", () => {
    // 事故原值
    const bad = statusIcon("deleted")
    expect(bad.svg).toBe(UNKNOWN_STATUS_ICON.svg)
    expect(bad.label).toContain("deleted")
    // 其它可能的脏值形态：null/undefined/数字/对象/空串
    for (const v of [null, undefined, 0, {}, [], ""]) {
      const ico = statusIcon(v)
      expect(ico.svg).toBe(UNKNOWN_STATUS_ICON.svg)
      expect(typeof ico.label).toBe("string")
    }
  })

  test("原型链上的键不算状态（'toString' 之类不得命中白名单）", () => {
    for (const v of ["toString", "constructor", "__proto__", "hasOwnProperty"]) {
      expect(isKnownTodoStatus(v)).toBe(false)
      expect(statusIcon(v).svg).toBe(UNKNOWN_STATUS_ICON.svg)
    }
  })
})

describe("isKnownTodoStatus / statusClassSuffix", () => {
  test("白名单判定与类名后缀同源", () => {
    expect(isKnownTodoStatus("completed")).toBe(true)
    expect(isKnownTodoStatus("deleted")).toBe(false)
    // 类名后缀只用于语义色：未知一律归 unknown（CSS `.todo-ico.st-unknown` 兜底着色）
    expect(statusClassSuffix("in_progress")).toBe("in_progress")
    expect(statusClassSuffix("deleted")).toBe("unknown")
    expect(statusClassSuffix(undefined)).toBe("unknown")
    // 脏值不得拼进类名（否则会生成 st-[object Object] 这类无效类名）
    expect(statusClassSuffix({ a: 1 })).toBe("unknown")
  })
})
