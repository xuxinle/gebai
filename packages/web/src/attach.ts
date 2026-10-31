/** 运行中会话附加恢复（自 main.ts 拆分）：attach 快照应用与待决交互卡重渲染。 */
import { renderPendingInteraction, renderPendingToolCalls } from "./pending-interactions"
import { attachRunningIfNeeded, loadMessages, markSessionRunning, setRunningAttach } from "./sessions"
import { attaching, client, getCurrentSession, runs } from "./state"
import { consumeTaskStream, discardRunBubble } from "./stream"

/** 附加运行中会话（DESIGN「运行中会话恢复」，loadMessages 尾部钩子调用）：
 *  页面刷新/切换进入运行中会话时——快照（在途流 + 待决交互）→ 待决卡片重建（审批/选择/填值/
 *  画图/捕获的事件已推送过、本页收不到，不重建则任务干等到超时）→ consumeTaskStream 接管
 *  attachStream（在途文本种子 + 实时续流，与发起页同构渲染：流式消息/工具卡/信号灯/停止按钮/
 *  单轮计时）。未运行或本页已接管（发起/附加过）时 no-op。 */
async function attachRunningSession(sessionId: string): Promise<void> {
  if (runs.has(sessionId) || attaching.has(sessionId)) return
  attaching.add(sessionId)
  let attached = false
  try {
    const snap = await client.attachSession(sessionId)
    if (!snap?.running) return // 未运行：无需接管，不算失败
    for (const it of snap.pending ?? []) renderPendingInteraction(sessionId, it)
    // 在途工具调用（已发出未出结果）：重建「等待中的工具卡」，结果到达时按 toolCallId 配对填充
    renderPendingToolCalls(sessionId, snap.tools)
    attached = true
    await consumeTaskStream(sessionId, (run) => client.attachStream(sessionId, { signal: run.abort.signal }), { startedAt: snap.startedAt })
  } catch {
    /* 附加失败：见 finally 的退避重试 */
  } finally {
    attaching.delete(sessionId)
    // 静默失败会让界面谎报空闲（无信号灯/停止按钮/单轮计时），用户此后的发送还会被 already_running
    // 拒绝——退避重试，仍失败则交给「下次进入会话 / 重连快照 running 清单」的常规路径
    if (!attached) scheduleAttachRetry(sessionId)
  }
}

/** 附加失败的退避重试（1s/3s/9s 共三次）：已接管（本页 runs 建立）即停。 */
function scheduleAttachRetry(sessionId: string, attempt = 1): void {
  if (attempt > 3) return
  setTimeout(() => {
    if (runs.has(sessionId) || attaching.has(sessionId)) return
    void attachRunningSession(sessionId).then(() => {
      if (!runs.has(sessionId)) scheduleAttachRetry(sessionId, attempt + 1)
    })
  }, attempt * attempt * 1000)
}
setRunningAttach(attachRunningSession)

/** 自检节流：一次核对在途时不再叠一次（定时器与回前台事件可能同时到达）。 */
let verifying = false
/** 可见时自检的周期（毫秒）：手机后台回来是「半死连接」，靠它把攒住的事件与真相补上。 */
export const VERIFY_INTERVAL_MS = 10_000

/**
 * 核对连接与运行态真相（回前台 / pageshow / 定时自检三处共用这一个入口）。
 *
 * 为何必须有：手机后台冻结页面时，WS 对象仍可能 `readyState === OPEN`、也不发 `close`——
 * 事件攒在浏览器里、心跳早过期，界面显示「已连接」而内容不更新；服务端任务也可能已在
 * 离线期间跑完。实测冻结场景下 `visibilitychange` **一次都不触发**（只有 `pageshow`
 * 与「定时器恢复首拍」可靠），所以不能只靠可见性事件，必须配合定时自检。
 *
 * 收敛规则（只依据服务端应答，不凭本地标志位猜测）：
 * - 心跳不通 → SDK 内部判死断开，退避重连接管（界面如实变红）
 * - 服务端 `running` 不含某会话而本页还在等 → 从存储补齐正文后收尾
 * - 服务端 `running` 含某会话而本页没接管 → 附加恢复
 */
export async function verifyConnection(): Promise<void> {
  if (verifying) return
  verifying = true
  try {
    const r = await client.verify()
    // 运行态以服务端为准收敛：本页在等但服务端已没在跑的会话，逐个收尾并补齐正文
    const stale = [...runs.keys()].filter((id) => !r.running.includes(id))
    for (const sid of stale) {
      // 收尾顺序有讲究：先把本轮中止、清掉在途气泡，再重读历史——
      // 反了（先重读后中止）会留下两个气泡：历史渲染的完整回答 + 旧的半截流式气泡。
      runs.get(sid)?.abort.abort()
      // 中止后本轮气泡不再由流逻辑接管，直接移除（服务端已结束，它永远不会再被填充）
      discardRunBubble(sid)
      await loadMessages(sid).catch(() => {})
      markSessionRunning(sid, false)
    }
    // 服务端在跑而本页没接管：附加恢复（内部有 runs/attaching 去重，已接管则 no-op）
    for (const sid of r.running) {
      if (runs.has(sid)) continue
      if (getCurrentSession()?.id === sid) attachRunningIfNeeded(sid)
    }
  } catch {
    /* 自检失败不改动界面：连接状态由 SDK 状态回调如实呈现 */
  } finally {
    verifying = false
  }
}

/**
 * 绑定「回前台」与周期自检（模块初始化时调用一次）。
 *
 * 信号选择以实测为准（CDP 冻结模拟手机后台）：
 * - `visibilitychange` 在冻结场景下**不触发**（保留作桌面切标签的快速路径）
 * - `pageshow` 可靠触发（BFCache 恢复与首次展示都走它）
 * - 定时器会停摆后再恢复：跳拍远超周期即说明系统刚被冻结过 → 立即自检
 */
export function bindConnectionVerify(): void {
  if (typeof document === "undefined") return
  const trigger = () => {
    // 页面不可见时不自检（省电/省流量）；不可见期的真相留到回前台一次补齐
    if (document.hidden) return
    void verifyConnection()
  }
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) trigger()
  })
  window.addEventListener("pageshow", trigger)
  // 定时自检：手机后台冻结时 visibilitychange 不触发、定时器也会停摆，
  // 恢复后的首拍是最可靠的信号——以此兜底（不可见时不跑，省电省流量）。
  setInterval(trigger, VERIFY_INTERVAL_MS)
}
