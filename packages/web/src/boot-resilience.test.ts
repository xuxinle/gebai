/**
 * 启动期与自检链路的**弱网容错**护栏（源码扫描，与 attach.test.ts / net-wiring.test.ts 同一约定）。
 *
 * 这几条接线的共同点是：坏掉不会让 typecheck/lint 变红，浏览器里也不报错——
 * 表现只是「断网刷新一次，页面就永久停在死状态」。
 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const src = (f: string) => readFileSync(join(import.meta.dirname, f), "utf8")

describe("启动链不得因一次网络失败而终止", () => {
  test("首连失败不再抛出终止 init（转入离线态，由自检/退避接回）", () => {
    const s = src("main.ts")
    expect(s).toContain("await connPromise.catch(")
    // 裸 await（无 catch）会让失败直接结束 init：自检已绑但后续渲染全没了
    expect(s).not.toMatch(/\n\s*await connPromise\n/)
  })

  test("启动期的 REST 都带超时（默认 fetch 无超时，弱网下会挂死整个 init）", () => {
    const s = src("main.ts")
    expect(s).toContain("withTimeout(loadToolCardMeta()")
    expect(s).toContain("withTimeout(tryExternalAuth()")
    expect(s).toContain("withTimeout(client.listSessions()")
  })

  test("启动失败给出可操作的重试（而不是只把错误写进状态栏）", () => {
    const s = src("main.ts")
    expect(s).toContain("bootStatus(")
    expect(s).toContain("bindBootRetry(")
    // 离线态不揭启动画面：露出空白聊天页会让人以为“已经好了”
    expect(s).toContain("client.canReconnect()")
  })

  test("启动画面具备状态文案与重试按钮的挂点", () => {
    const html = readFileSync(join(import.meta.dirname, "..", "index.html"), "utf8")
    expect(html).toContain("gb-splash-status")
    expect(html).toContain("gb-splash-retry")
  })

  test("连接状态回调在重连成功后揭启动画面并补拉列表", () => {
    const s = src("main.ts")
    const at = s.indexOf("client.onStatusChange(")
    const body = s.slice(at, at + 1200)
    expect(body).toContain("hideSplash()")
    expect(body).toContain("refreshSessions()")
    // 弱网下必须能看出“在努力”：断开文案带重连次数
    expect(body).toContain("第 ${reconnects} 次")
  })
})

describe("自检在弱网下必须真的跑起来", () => {
  test("节流用时间戳而非布尔量（布尔量卡住会让后续自检全被静默吃掉）", () => {
    const s = src("attach.ts")
    expect(s).toContain("lastVerifyAt")
    expect(s).not.toMatch(/let verifying = false/)
    expect(s).toContain("VERIFY_MIN_GAP_MS")
  })

  test("单次自检有预算上限，超预算即推进重连", () => {
    const s = src("attach.ts")
    expect(s).toContain("VERIFY_BUDGET_MS")
    const at = s.indexOf("export async function verifyConnection")
    const body = s.slice(at, at + 1800)
    expect(body).toContain("Promise.race")
    expect(body).toContain("hurryReconnect()")
  })

  test("online（网络恢复）先打断退避立即重连，再核对真值", () => {
    const s = src("attach.ts")
    const at = s.indexOf("function handleNetEvent(")
    const body = s.slice(at, at + 1000)
    const hurry = body.indexOf("client.hurryReconnect()")
    const verify = body.indexOf("verifyConnection()")
    expect(hurry).toBeGreaterThan(0)
    expect(hurry).toBeLessThan(verify)
  })

  test("SDK 自检内部用短超时（默认 60s 的 RPC 超时会让一次挂住吃掉 6 拍自检）", () => {
    const s = readFileSync(join(import.meta.dirname, "..", "..", "sdk", "src", "client.ts"), "utf8")
    expect(s).toContain("requestSnapshot(this.heartbeatTimeoutMs)")
    expect(s).toContain("syncEvents(this.lastSeq, this.heartbeatTimeoutMs)")
  })
})

describe("SDK 弱网能力的存在性", () => {
  test("hurryReconnect 与 REST 超时都在（缺任一，弱网恢复即退化）", () => {
    const s = readFileSync(join(import.meta.dirname, "..", "..", "sdk", "src", "client.ts"), "utf8")
    expect(s).toContain("hurryReconnect(): void")
    expect(s).toContain("requestTimeoutMs")
    expect(s).toContain("private async rest(")
    // 所有 REST 辅助方法都必须经 rest（直接 fetch 就没有超时保护）
    const direct = s.match(/fetch\(this\.apiUrl/g) ?? []
    expect(direct.length).toBe(1) // 仅 rest() 内部那一处
  })
})
