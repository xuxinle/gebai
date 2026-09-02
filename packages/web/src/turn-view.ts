/** 回合计时器视图（自 main.ts 拆分）：耗时格式化/等级/渲染与计时循环。 */
import { isTurnTimerEnabled } from "./turn-timer"
import { getCurrentSession, headerCtxEl, runs, turnTimerEl, type RunState } from "./state"

export function formatTurnDuration(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000))
  if (s < 60) return `${s}s`
  const m = Math.floor(s / 60)
  if (m < 60) return `${m}m ${String(s % 60).padStart(2, "0")}s`
  return `${Math.floor(m / 60)}h ${String(m % 60).padStart(2, "0")}m`
}

/** 单轮计时器：标题栏右侧、上下文占比左侧常驻显示，**随会话视图切换**——当前会话运行中实时走
 *  （图标呼吸），切走不打扰其他会话视图、切回续走；非运行会话显示该会话最后定格时长，无记录隐藏。
 *  运行结束不清除（定格保留），下次运行重启归零。
 *  时长分级变色（<1min 中性 / 1-5min 主题色 / ≥5min 警告色，与上下文圆环分级着色语言一致）；
 *  hover 显示总运行时——该会话每轮净耗时累加（不含轮间空闲），而非距第一条消息的墙钟时间。 */
/** 各会话计时状态：累计净耗时（总运行时 tip）与最后定格时长（切回该会话时恢复显示）。 */
const sessionTimers = new Map<string, { totalMs: number; lastElapsed: number }>()

/** 时长分级（颜色渐进，冷→暖）：<10s 极淡 / 10-30s 中性 / 30s-1m 工具青绿 / 1-3m 主题色 / 3-5m 警告 / ≥5m 危险。 */
function turnDurLevel(ms: number): 0 | 1 | 2 | 3 | 4 | 5 {
  if (ms >= 300_000) return 5
  if (ms >= 180_000) return 4
  if (ms >= 60_000) return 3
  if (ms >= 30_000) return 2
  if (ms >= 10_000) return 1
  return 0
}

/** 刷新计时显示：文本 + 分级色 + 运行态；信号灯思考闪烁周期同步分级（越久越慢——闪得慢=执行得慢）。 */
function renderTurnTimer(elapsedMs: number, running: boolean): void {
  const dur = String(turnDurLevel(elapsedMs))
  turnTimerEl.dataset.dur = dur
  if (headerCtxEl) {
    if (running) headerCtxEl.dataset.dur = dur
    else delete headerCtxEl.dataset.dur // 非运行态：信号灯恢复默认闪烁周期
  }
  turnTimerEl.classList.toggle("running", running)
  ;(turnTimerEl.querySelector<HTMLElement>(".tt-text")!).textContent = formatTurnDuration(elapsedMs)
}

function turnTimerTick(run: RunState): void {
  if (!isTurnTimerEnabled()) {
    if (run.timerInterval) {
      clearInterval(run.timerInterval)
      run.timerInterval = undefined
    }
    return
  }
  // 显示跟随当前会话：后台会话运行不打扰当前视图（interval 空转，切回时接管显示）
  if (getCurrentSession()?.id !== run.sessionId) return
  const elapsed = Date.now() - run.startedAt
  turnTimerEl.hidden = false
  renderTurnTimer(elapsed, true)
  turnTimerEl.dataset.tip = `总运行 ${formatTurnDuration((sessionTimers.get(run.sessionId)?.totalMs ?? 0) + elapsed)}` // 含进行中这轮
}

export function startTurnTimer(run: RunState): void {
  if (!isTurnTimerEnabled()) return
  turnTimerTick(run)
  run.timerInterval = setInterval(() => turnTimerTick(run), 250)
}

export function stopTurnTimer(run: RunState): void {
  if (run.timerInterval) {
    clearInterval(run.timerInterval)
    run.timerInterval = undefined
  }
  const elapsed = Date.now() - run.startedAt
  const st = sessionTimers.get(run.sessionId) ?? { totalMs: 0, lastElapsed: 0 }
  st.totalMs += elapsed // 每段累加：总运行时 = 该会话各轮净耗时之和（不含轮间空闲）
  st.lastElapsed = elapsed
  sessionTimers.set(run.sessionId, st)
  // 定格仅在当前会话显示（切走会话的定格不覆盖当前视图，切回时经 sessionTimers 恢复）
  if (getCurrentSession()?.id === run.sessionId) {
    renderTurnTimer(elapsed, false) // 结束不清除：定格保留
    turnTimerEl.dataset.tip = `总运行 ${formatTurnDuration(st.totalMs)}`
  }
}

/** 会话视图切换联动（gebai:session-view）：计时显示随会话切换——当前会话运行中续走（interval
 *  到点接管刷新，此处先行渲染一帧），非运行会话恢复该会话定格时长，无记录隐藏。 */
export function syncTurnTimerView(): void {
  if (!isTurnTimerEnabled()) return // CSS data-turn-timer="off" 隐藏全部计时元素，无需渲染
  const cur = getCurrentSession()
  if (!cur) {
    turnTimerEl.hidden = true // 草稿页（无会话）：无计时展示
    if (headerCtxEl) delete headerCtxEl.dataset.dur
    return
  }
  const run = runs.get(cur.id)
  if (run) {
    const elapsed = Date.now() - run.startedAt
    turnTimerEl.hidden = false
    renderTurnTimer(elapsed, true)
    turnTimerEl.dataset.tip = `总运行 ${formatTurnDuration((sessionTimers.get(cur.id)?.totalMs ?? 0) + elapsed)}`
    return
  }
  const st = sessionTimers.get(cur.id)
  if (st) {
    turnTimerEl.hidden = false
    renderTurnTimer(st.lastElapsed, false)
    turnTimerEl.dataset.tip = `总运行 ${formatTurnDuration(st.totalMs)}`
  } else {
    turnTimerEl.hidden = true
    if (headerCtxEl) delete headerCtxEl.dataset.dur // 清运行态闪烁分级残留
  }
}
document.addEventListener("gebai:session-view", syncTurnTimerView)

/** 空闲超时兜底（无数据视为挂起的判定窗口，毫秒）：高于服务端 LLM 读空闲超时（120s）——
 *  模型调用假死先由服务端超时上报明确错误（「模型接口读超时」），前端看门狗只兜底
 *  服务端也检测不到的挂起；此前 60s 先于服务端超时静默取消，慢模型（长思考无流式输出）
 *  被误杀且用户看不到任何原因。 */
export const IDLE_TIMEOUT_MS = 150_000

/** 任务无任何可见输出时的收尾说明气泡（静默结束兜底：用户始终能看到「为什么没有回复」）。 */
