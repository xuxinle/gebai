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

/**
 * 手机后台的「半死连接」（页面被冻结）：WS 对象仍 `readyState === OPEN`、也不发 `close`，
 * 后台期间的事件攒在浏览器里、心跳早过期——界面显示「已连接」而内容不更新。
 * 实测（CDP 冻结模拟）：冻结 20s 后只 1 个 WS、`close=0`，且 `visibilitychange` **一次都不触发**
 * （只有 `pageshow` 与「定时器恢复首拍」可靠）。因此不能只靠可见性事件，必须定时自检兜底。
 * 这些接线分散在 SDK 与 attach.ts，缺一根都不会报 typecheck/lint，故以源码扫描钉住。
 */
describe("手机后台半死连接：回前台/定时自检必需接线", () => {
  const attachSrc = () => readFileSync(join(import.meta.dirname, "attach.ts"), "utf8")
  const clientSrc = () => readFileSync(join(import.meta.dirname, "..", "..", "sdk", "src", "client.ts"), "utf8")

  test("SDK 提供 verify()：主动核对连接与运行态真相", () => {
    const s = clientSrc()
    expect(s).toContain("async verify()")
    // 心跳超期先探活（WS 看着 OPEN 但链路静默），不通则断开重连
    expect(s).toContain("heartbeatStale")
    expect(s).toContain("pingOnce")
    // 事件 seq 落后要补拉（后台期间攒住的）
    expect(s).toContain("syncEvents(this.lastSeq)")
  })

  test("自检按服务端 running 与本地 runs 双向收敛（已结束要收尾、在跑要附加）", () => {
    const s = attachSrc()
    // 本地在等但服务端已没在跑：收尾并补齐正文
    expect(s).toContain("runs.keys()].filter((id) => !r.running.includes(id))")
    // 顺序有讲究：先中止 + 丢弃旧气泡，再重读历史——反了会留下半截气泡（历史一份完整 + 旧流式一份）
    const fn = s.slice(s.indexOf("const stale ="), s.indexOf("const stale =") + 700)
    const abortAt = fn.indexOf("abort.abort()")
    const discardAt = fn.indexOf("discardRunBubble(sid)")
    const reloadAt = fn.indexOf("await loadMessages(sid)")
    expect(abortAt).toBeGreaterThan(-1)
    expect(discardAt).toBeGreaterThan(abortAt)
    expect(reloadAt).toBeGreaterThan(discardAt)
    // 服务端在跑而本页没接管：附加恢复
    expect(s).toContain("attachRunningIfNeeded(sid)")
  })

  test("不可见时不自检（省电省流量）", () => {
    const s = attachSrc()
    const fn = s.slice(s.indexOf("export function bindConnectionVerify"), s.indexOf("export function bindConnectionVerify") + 900)
    expect(fn).toContain("document.hidden")
    expect(fn.indexOf("document.hidden")).toBeLessThan(fn.indexOf("verifyConnection()"))
  })

  test("回前台信号覆盖 pageshow 与定时自检（visibilitychange 在冻结场景不可靠）", () => {
    const s = attachSrc()
    const fn = s.slice(s.indexOf("export function bindConnectionVerify"), s.indexOf("export function bindConnectionVerify") + 900)
    expect(fn).toContain('addEventListener("pageshow"')
    expect(fn).toContain('addEventListener("visibilitychange"')
    expect(fn).toContain("setInterval(trigger, VERIFY_INTERVAL_MS)")
  })

  test("main.ts 建连后绑定自检（不挂钩即永不核对，半死连接无人发现）", () => {
    const s = readFileSync(join(import.meta.dirname, "main.ts"), "utf8")
    expect(s).toContain("bindConnectionVerify()")
    expect(/await connPromise[\s\S]{0,400}bindConnectionVerify\(\)/.test(s)).toBe(true)
  })
})
