/**
 * 心跳参数与自检周期的取值契约。
 *
 * 这些数字是**产品决策**，但支撑它们的两条性质是硬约束，改参数时不能违背：
 *
 * 1. **判死速度**：半死链路（收得到但不回）多久能被识别——用户可见的「谎报已连接」
 *    时长就是这个数。实测：interval=2s / timeout=5s → ~7s（原 5s/10s 为 14.9s）。
 * 2. **误杀边界是 RTT ≥ timeout，与 interval 无关**：`sendHeartbeat` 在 heartbeatPending
 *    时直接返回，每拍 ping 用自己的超时计时，而不是「两拍之间必须回来」。所以调小
 *    interval 不会降低 RTT 容忍度（实测 5900ms 往返在 2s/5s 组合下长时间稳定不断）。
 *
 * 第三条是前端侧的配套约束（改错会自伤）：**自检周期必须明显小于判死延迟**，
 * 否则心跳再快也被自检的间隔拖住——半死链路要等「判死 + 下一拍自检」才变红。
 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const clientSrc = readFileSync(join(import.meta.dirname, "..", "..", "sdk", "src", "client.ts"), "utf8")
const attachSrc = readFileSync(join(import.meta.dirname, "attach.ts"), "utf8")

/** 取 SDK 里的心跳常量（源码常量，不实例化客户端）。 */
function sdkConst(name: string): number {
  const m = new RegExp(`const ${name} = ([\\d_]+)`).exec(clientSrc)
  if (!m) throw new Error(`未找到常量 ${name}`)
  return Number(m[1].replace(/_/g, ""))
}

/** 取 attach.ts 里的自检常量。不 import 模块：它拖进 state.ts 等 DOM 依赖，无 document 环境会直接抛错。 */
function attachConst(name: string): number {
  const m = new RegExp(`export const ${name} = ([\\d_]+)`).exec(attachSrc)
  if (!m) throw new Error(`未找到常量 ${name}`)
  return Number(m[1].replace(/_/g, ""))
}

describe("心跳参数：判死速度与 RTT 容忍度", () => {
  const interval = sdkConst("HEARTBEAT_INTERVAL_MS")
  const timeout = sdkConst("HEARTBEAT_TIMEOUT_MS")

  test("判死延迟（interval + timeout）不超过 8 秒", () => {
    // 判死发生在「上一拍已发出、下一拍超时」：最坏 interval + timeout，实测略小
    const worst = interval + timeout
    expect(worst).toBeLessThanOrEqual(8_000)
  })

  test("RTT 容忍度（= timeout）不低于 4 秒", () => {
    // 手机弱信号下单次往返 2~4s 属常见；低于 4s 会开始误杀（误杀比慢更糟：任务被打断）
    expect(timeout).toBeGreaterThanOrEqual(4_000)
  })

  test("心跳间隔明显小于超时（避免每拍都要等上一拍超时才发下一拍）", () => {
    expect(interval).toBeLessThan(timeout)
  })

  test("心跳间隔不小于 1 秒（防误配成扫射式请求）", () => {
    expect(interval).toBeGreaterThanOrEqual(1_000)
  })

  test("sendHeartbeat 在上一拍未归时跳拍（不堆叠请求），且注释不得再声称「间隔会误杀」", () => {
    const at = clientSrc.indexOf("private sendHeartbeat()")
    const body = clientSrc.slice(at, at + 1_200)
    expect(body).toContain("if (this.heartbeatPending) return")
    // 已实测否定：每拍用自己的 timeout 计时，误杀边界与 interval 无关
    expect(body).not.toContain("interval < timeout 的高延迟链路上")
    expect(body).toContain("RTT ≥ timeout")
  })
})

describe("前端自检周期必须与心跳判死速度配套", () => {
  const interval = sdkConst("HEARTBEAT_INTERVAL_MS")
  const timeout = sdkConst("HEARTBEAT_TIMEOUT_MS")
  const detectMs = interval + timeout
  const verifyInterval = attachConst("VERIFY_INTERVAL_MS")
  const verifyBudget = attachConst("VERIFY_BUDGET_MS")
  const verifyGap = attachConst("VERIFY_MIN_GAP_MS")

  test("自检周期小于判死延迟（否则变红要等「判死 + 下一拍自检」）", () => {
    expect(verifyInterval).toBeLessThan(detectMs)
  })

  test("自检周期不短于 2 秒（每拍要发 ping + 快照，太密是自伤）", () => {
    expect(verifyInterval).toBeGreaterThanOrEqual(2_000)
  })

  test("单次自检预算大于内部超时之和（否则正常链路的慢一拍会被误判为不可用）", () => {
    // 内部：可能先探一拍 ping（timeout），再发快照 + 重放（各 timeout）——按最坏两段算
    expect(verifyBudget).toBeGreaterThan(timeout * 2)
  })

  test("节流窗口小于自检周期（否则周期被节流吞掉）", () => {
    expect(verifyGap).toBeLessThan(verifyInterval)
  })

  test("在途自检不堆叠，但带自愈截止时间（不会因异常路径永久停摆）", () => {
    expect(attachSrc).toContain("verifyBusyUntil")
    // 取整个函数体（按下一个顶层 export 截止）：写死字符窗口会随函数变长而截掉 finally
    const start = attachSrc.indexOf("export async function verifyConnection")
    const rest = attachSrc.slice(start + 1)
    const end = rest.search(/\nexport /)
    const body = end < 0 ? rest : rest.slice(0, end)
    expect(body).toContain("if (now < verifyBusyUntil) return")
    expect(body).toContain("verifyBusyUntil = now + VERIFY_BUDGET_MS")
    expect(body).toContain("verifyBusyUntil = 0")
  })
})
