/**
 * 推理体（.reasoning-body）内部滚动条粘底跟随：
 * 推理流式更新时，只要用户没有主动向上翻阅，滚动条自动贴底（最新推理始终可见）；
 * 用户滚动脱离底部立即解除跟随，滚回底部自动恢复。与 jump-bottom 的消息流粘底同构。
 */

/** 吸底锁定阈值（严格贴着底部才视为锁定：小幅上滚即解除，防止被拽回底部）。 */
const PIN_THRESHOLD = 4

/** 每个推理体的吸底状态：false = 用户在看历史（不跟随）；true = 跟随。首次调用前视为跟随。 */
const stick = new WeakMap<HTMLElement, boolean>()

/** 最近一次程序滚动的实际目标位置（设置 scrollTop 后同步读取的 clamp 值）。
 *  用于识别迟到的程序滚动事件：scroll 事件异步送达，期间内容可能已增长，事件报告的位置
 *  虽是我们刚设置的目标、却已不在「当前底部」——若按用户滚动处理会把跟随误翻为 false，
 *  导致流式跟随悄悄失效。 */
const programmaticTarget = new WeakMap<HTMLElement, number>()

function within(body: HTMLElement, threshold: number): boolean {
  return body.scrollHeight - body.scrollTop - body.clientHeight <= threshold
}

function bind(body: HTMLElement): void {
  body.addEventListener(
    "scroll",
    () => {
      const target = programmaticTarget.get(body)
      if (target !== undefined && body.scrollTop === target) {
        // 程序滚动产生的滚动事件：恢复跟随；若内容在事件送达前已增长（目标过期、
        // 位置已不在当前底部），继续滚动到最新底部。
        stick.set(body, true)
        programmaticTarget.delete(body)
        if (!within(body, PIN_THRESHOLD)) body.scrollTop = body.scrollHeight
      } else {
        // 用户滚动（滚轮 / 触控板 / 键盘 / 滚动条拖动）：贴底自动恢复跟随，脱离底部立即解除
        stick.set(body, within(body, PIN_THRESHOLD))
        if (target !== undefined) programmaticTarget.delete(body)
      }
    },
    { passive: true },
  )
}

/** 推理内容流式更新后调用：跟随中则滚动到底，用户翻阅历史时不动。 */
export function scrollReasoningSticky(body: HTMLElement): void {
  if (!body.isConnected) return
  if (!stick.has(body)) bind(body)
  if (stick.get(body) === false) return
  body.scrollTop = body.scrollHeight
  programmaticTarget.set(body, body.scrollTop)
}
