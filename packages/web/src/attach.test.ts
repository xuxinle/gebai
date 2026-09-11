import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

// attach.ts 是纯副作用模块（向 sessions.ts 注册 setRunningAttach 钩子，无具名导出）：
// 入口 main.ts 丢失 `import "./attach"` 不会触发 typecheck/lint 报错、运行期钩子恒为 null 静默跳过
// ——刷新后运行中会话不恢复（在途流不续接/待决卡片不重建、任务干等到超时）。本测试以源码扫描守住该导入。
describe("attach 副作用导入", () => {
  test("入口 main.ts 保留 import \"./attach\"（丢失即运行中会话恢复链路断裂）", () => {
    const src = readFileSync(join(import.meta.dirname, "main.ts"), "utf8")
    expect(/import\s+["']\.\/attach["']/.test(src)).toBe(true)
  })
})

// 运行态同步（两条腿）：任务并非总由本页发起——重启续跑/飞书桥接/定时任务/其他标签页会在页面空闲时
// 开始运行。仅靠「进入会话时探测一次」会漏掉此后的开始（典型：重启后页面先刷新、续跑才启动），
// 页面就一直没有信号灯/停止按钮/单轮计时。两条腿：event.task.start（实时）与快照 running 清单
// （覆盖刷新晚于任务开始的时序）。任一缺失都会静默退化为「任务在跑、页面看空闲」，故以源码扫描守住。
describe("运行态同步（任务开始事件 + 快照 running 清单）", () => {
  test("main.ts 处理 event.task.start 并触发附加恢复", () => {
    const src = readFileSync(join(import.meta.dirname, "main.ts"), "utf8")
    expect(src).toContain('ev.type === "event.task.start"')
    expect(/event\.task\.start[\s\S]{0,400}attachRunningIfNeeded\(ev\.sessionId\)/.test(src)).toBe(true)
  })

  test("main.ts 快照到达时按 running 清单附加当前会话", () => {
    const src = readFileSync(join(import.meta.dirname, "main.ts"), "utf8")
    expect(/snap\.running\.includes\([\s\S]{0,40}\.id\)/.test(src)).toBe(true)
  })

  test("sessions.ts 导出 attachRunningIfNeeded（委托 runningAttachHook）", () => {
    const src = readFileSync(join(import.meta.dirname, "sessions.ts"), "utf8")
    expect(/export function attachRunningIfNeeded\(sessionId: string\): void \{[\s\S]{0,200}runningAttachHook\?\.\(sessionId\)/.test(src)).toBe(true)
  })
})
