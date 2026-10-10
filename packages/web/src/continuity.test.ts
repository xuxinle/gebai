/** 后台会话连续性（前端断开不停任务 / 随时重连恢复状态）的链路守卫。
 *  这些接线分散在多个模块、彼此隐式依赖，缺一根都不会触发 typecheck/lint（表现为：任务被看门狗误杀、
 *  待决卡片不重建、界面谎报空闲），故以源码扫描钉住关键接线（与 attach.test.ts 同一约定）。 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const src = (file: string) => readFileSync(join(import.meta.dirname, file), "utf8")

describe("看门狗不得误杀后台任务", () => {
  test("断线期间与「等待用户作答」期间都不取消任务（取消请求会随重连送达）", () => {
    const s = src("stream.ts")
    expect(s).toContain("if (!client.isConnected()) return")
    expect(s).toContain("if (hasPendingInteraction(sessionId)) return")
    // 取消只在「已连接 + 无待决交互 + 长时间无数据」时才发出
    const idle = s.slice(s.indexOf("const idleTimer = setInterval"), s.indexOf("const idleTimer = setInterval") + 900)
    expect(idle.indexOf("!client.isConnected()")).toBeLessThan(idle.indexOf("client.cancelTask"))
    expect(idle.indexOf("hasPendingInteraction(sessionId)")).toBeLessThan(idle.indexOf("client.cancelTask"))
  })

  test("交互等待心跳与工具心跳同等刷新活跃时间（服务端等待期无其他数据）", () => {
    const s = src("main.ts")
    expect(s).toContain('ev.type === "event.tool.alive" || ev.type === "event.interaction.alive"')
  })

  test("附加失败退避重试（静默失败会让界面谎报空闲）", () => {
    const s = src("attach.ts")
    expect(s).toContain("scheduleAttachRetry")
    expect(/if \(!attached\) scheduleAttachRetry\(sessionId\)/.test(s)).toBe(true)
  })
})

describe("缺口（overrun）后的全量重同步闭环", () => {
  test("resume 携带 reloadHistory：重读消息列表 + 重建待决交互卡 + 重载期间暂停正文渲染", () => {
    const s = src("stream.ts")
    expect(s).toContain("chunk.reloadHistory")
    expect(s).toContain("await loadMessages(sessionId)")
    expect(s).toContain("await resyncPendingInteractions(sessionId)")
    expect(s).toContain("run.reloading = true")
    expect(s).toContain("rehydrateRunView(run, sessionId)")
    expect(s).toContain("if (run.reloading) return") // 重载期间只累积，由 rehydrateRunView 重建气泡
  })

  test("待决卡片重建与 attach 路径共用同一分派（不漏任何一类交互）", () => {
    const shared = src("pending-interactions.ts")
    for (const kind of ["approval", "choice", "env", "draw", "capture"]) {
      expect(shared).toContain(`\"${kind}\"`)
    }
    expect(src("attach.ts")).toContain('from "./pending-interactions"')
    expect(src("stream.ts")).toContain('from "./pending-interactions"')
  })

  test("在途工具卡重建：attach 快照的 tools 走实时事件同一渲染入口（结果到达即配对填充）", () => {
    const shared = src("pending-interactions.ts")
    expect(shared).toContain("export function renderPendingToolCalls")
    expect(shared).toContain("onToolCall({ sessionId, toolCallId: t.toolCallId")
    expect(shared).toContain("renderPendingToolCalls(sessionId, snap?.tools)")
    expect(src("attach.ts")).toContain("renderPendingToolCalls(sessionId, snap.tools)")
  })
})

describe("运行态可见性与列表角标", () => {
  test("快照 runtime 进入前端状态并驱动列表角标", () => {
    expect(src("main.ts")).toContain("setRuntimeInfo(snap.runtime ?? {})")
    const s = src("sessions.ts")
    expect(s).toContain("export function setRuntimeInfo")
    expect(s).toContain("export function markSessionRunning")
    expect(s).toContain("session-run") // 行内角标元素
    expect(s).toContain("signatureOfRun(runMarkOf(s.id))") // 运行态变化触发列表重建
  })

  test("任务开始/结束事件即时收敛角标（不必等下一次快照）", () => {
    const s = src("main.ts")
    expect(s).toContain("markSessionRunning(ev.sessionId, true)")
    expect(s).toContain("markSessionRunning(ev.sessionId, false)")
  })
})

describe("断线/失效期间的用户动作不静默丢", () => {
  test("审批提交失败保留卡片并提示；服务端判失效则撕卡说明", () => {
    const s = src("approvals.ts")
    expect(s).toContain("await client.decideApproval")
    expect(s).toContain('=== "expired"')
    expect(s).toContain("export function hasPendingInteraction")
  })

  test("选择/填值卡的失效与提交失败同样如实反馈", () => {
    const s = src("tool-cards.ts")
    expect(s).toContain("该询问已失效")
    expect(s).toContain("该填值请求已失效")
    expect(s).toContain("await client.decideEnv(sessionId, envId, null)") // 拒绝也要送到
  })

  test("停止未送达如实提示；已有任务在跑时不静默吞消息（撤回幻影消息 + 保住输入）", () => {
    expect(src("composer.ts")).toContain("停止未送达")
    const s = src("main.ts")
    expect(s).toContain('code === "already_running"')
    expect(s).toContain("userMsgEl?.remove()")
  })
})

describe("会话列表刷新不得回退数据", () => {
  test("任务开始/结束的列表刷新拉新数据，不用 lastSessions 陈旧数组重流（否则自动命名的新标题被 seq 守卫丢弃）", () => {
    const s = src("sessions.ts")
    const fn = s.slice(s.indexOf("export function markSessionRunning"), s.indexOf("export function markSessionRunning") + 800)
    // 刷新必须是不带 preloaded 的拉新调用；带 lastSessions 的旧写法会把在途自动命名刷新挤掉
    expect(fn).toContain("void refreshSessions()")
    expect(fn).not.toContain("refreshSessions(lastSessions)")
  })

  test("自动命名 rename 后必发一次全量刷新（与任务结束刷新并发时 seq 守卫保最后完成者赢）", () => {
    const s = src("sessions.ts")
    const fn = s.slice(s.indexOf("export async function maybeAutoTitle"), s.indexOf("export async function maybeAutoTitle") + 1600)
    expect(fn).toContain("await client.renameSession(sessionId, title)")
    expect(fn.indexOf("await client.renameSession(sessionId, title)")).toBeLessThan(fn.indexOf("await refreshSessions()"))
  })
})
