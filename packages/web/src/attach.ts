/** 运行中会话附加恢复（自 main.ts 拆分）：attach 快照应用与待决交互卡重渲染。 */
import { renderPendingInteraction, renderPendingToolCalls } from "./pending-interactions"
import { attachRunningIfNeeded, loadMessages, markSessionRunning, setRunningAttach } from "./sessions"
import { attaching, client, getCurrentSession, runs, setConn } from "./state"
import { consumeTaskStream, discardRunBubble } from "./stream"
import { actionForNetEvent, bindNetworkEvents, type NetEvent } from "./net-status"

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

/** 自检节流：一次核对在途时不再叠一次（定时器与回前台/online 可能同时到达）。
 *  用**时间戳**而非纯布尔量：布尔量一旦因异常路径没有复位（或某次核对被长超时挂住），
 *  后面的自检就全部被静默吃掉（弱网下就是“看着在自检、实际形同没有”）。
 *  但**只靠时间戳也不够**：自检周期（3s）小于单次预算（12s）时，同一个慢链路会让好几拍
 *  同时跑——每拍都发 ping+快照+重放，在弱网上就是自己把自己堆死。
 *  所以两者并用：在途就跳（不堆叠），但在途标记带**自愈截止时间**（不会卡死）。 */
let lastVerifyAt = 0
/** 在途自检的截止时刻（0 = 无在途）：超过它即认为上一拍已异常丢失并允许新一拍。 */
let verifyBusyUntil = 0
/** 自检节流窗口（毫秒）：小于它视为重复触发（定时器与回前台/online 同时到达）。 */
export const VERIFY_MIN_GAP_MS = 1_000
/**
 * 单次自检的时长上限（毫秒）：超预算即视为链路不可用并推进重连。
 *
 * 需大于内部各段请求的超时之和（心跳探针一次 + 快照 + 重放，均用心跳口径 timeout）。
 * SDK 默认心跳 timeout 为 5s，全走最坏路径约 15s；这里取到能覆盖常见两段（10s）以上，
 * 否则正常链路卡顿一拍时会被当成「不可用」，白白把好连接断掉重连。
 */
export const VERIFY_BUDGET_MS = 12_000
/**
 * 可见时自检的周期（毫秒）。
 *
 * 它是「半死连接」不被及时发现的上限：手机后台冻结时 visibilitychange 不触发、定时器也停摆，
 * 靠恢复后的首拍兵底。取 3s 而不是 10s：心跳判死已缩到 6.5s，自检再隔 10s 就变成新瓶颈
 * （变红要等「判死 + 下一拍自检」，最坏 7s + 10s）。
 * 自检开销是一次 ping + 一次快照，不可见时不跑（省电省流量），且置有一个节流窗口。
 */
export const VERIFY_INTERVAL_MS = 3_000

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
  const now = Date.now()
  if (now - lastVerifyAt < VERIFY_MIN_GAP_MS) return
  // 已有在途且未过截止：跳过（不堆叠）；过了截止说明上一拍异常失联，允许新一拍接手
  if (now < verifyBusyUntil) return
  lastVerifyAt = now
  verifyBusyUntil = now + VERIFY_BUDGET_MS
  try {
    const r = await Promise.race([
      client.verify(),
      new Promise<null>((resolve) => setTimeout(() => resolve(null), VERIFY_BUDGET_MS)),
    ])
    if (!r) {
      // 预算内没回来：链路基本不可用，推进重连（不等下一拍退避）
      client.hurryReconnect()
      return
    }
    if (!r.heartbeatOk) {
      // 自检报“链路不可用”：SDK 内部已判死/或本来就未连接——推一把重连，
      // 否则网络已恢复的情况下还得干等最长 30s 的退避到点
      client.hurryReconnect()
      return
    }
    // 运行态明细不用在这里回写：verify 内部的 requestSnapshot 会走 applySnapshot →
    // snapshotHandlers，main.ts 的 onSnapshot 已在那个回调里调 setRuntimeInfo——
    // 自检因此天然把最新的 runtime 带到会话列表角标，不额外多一次往返。
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
    /* 自检失败不改动界面：连接状态由 SDK 状态回调如实呈现；但推一把重连，别让恢复干等退避 */
    client.hurryReconnect()
  } finally {
    verifyBusyUntil = 0
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
  /*
   * 网络连通性：浏览器自己尽最快给出的信号（早于任何定时器）。
   *
   * 挂在 **window** 上，不是 navigator：`online`/`offline` 是 window 的事件，
   * `navigator` 只有 `onLine` 这个只读属性、**没有 addEventListener**（实测：
   * `typeof navigator.addEventListener === "undefined"`）。挂错对象会被降级成 no-op，
   * 而 no-op 不会报错——断网时界面照旧显示已连接，看起来像“改了没生效”。
   */
  bindNetworkEvents(typeof window !== "undefined" ? (window as unknown as Parameters<typeof bindNetworkEvents>[0]) : null, (ev: NetEvent) => {
    handleNetEvent(ev)
  })
  // 定时自检：手机后台冻结时 visibilitychange 不触发、定时器也会停摆，
  // 恢复后的首拍是最可靠的信号——以此兜底（不可见时不跑，省电省流量）。
  setInterval(trigger, VERIFY_INTERVAL_MS)
}

/**
 * 网络事件处理：把「网没了 / 网回来了」变成具体动作（判据见 net-status.ts 的 actionForNetEvent）。
 *
 * 为何不能只靠定时自检：它每 10 秒跑一拍，网络恢复后**最长白等 10 秒**，而这段时间界面
 * 持续显示旧的运行态（用户看到的就是“网回来了但还卡着”）；断网时更是持续谎报“已连接”。
 */
function handleNetEvent(ev: NetEvent): void {
  const action = actionForNetEvent(ev)
  // 先改文案（断网时用户应立即看到原因，而不是干等心跳超时）
  if (action.label) setConn(action.label, !action.warn)
  if (action.dropConnection) {
    // 半死连接：WS 对象往往仍 OPEN、心跳照跑但服务端早已收不到，不会自己 close。
    // 主动判死 → onclose → 退避重连（不置 manualClose，保留重连意图）
    client.dropConnection()
    return
  }
  // 恢复：**先打断退避立即重连**，再核对服务端真值。
  // 不打断的话，网络恢复与“下一拍退避到点”毫无关系（实测退避最长 30s），用户看到的就是“网回来了但还卡着”；
  // 另外 verify() 在未连接态原本只空转返回——它是重连的真正推进点。
  if (action.verify) {
    client.hurryReconnect()
    void verifyConnection()
  }
}
