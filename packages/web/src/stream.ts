/** 流式消费与渲染（自 main.ts 拆分）：ChatChunk 应用、模型错误提示、任务流消费、流式文本节流渲染。 */
import type { ChatChunk } from "@gebai/sdk"
import { cnyCatTurnEnd } from "./cny-cat"
import { syncSendButton } from "./composer"
import { refreshJumpBottom, scrollIfSticky } from "./jump-bottom"
import { addMetaActions, appendMsg, assistantContent, clearInteractionCards, reasoningBlock, scrollSessionSticky, sealSegment, sealSessionSegment, subSessionEntry } from "./messages"
import { blockText, markdownBlock } from "./markdown"
import { createModelErrorNotice, modelErrorText } from "./model-error"
import { createStreamRenderer } from "./stream-render"
import { clearApprovals, hasPendingInteraction } from "./approvals"
import { clearPendingTools, focusInput } from "./state"
import { loadMessages, markSessionRunning, maybeAutoTitle } from "./sessions"
import { resyncPendingInteractions } from "./pending-interactions"
import { drainQueue } from "./queue"
import { scrollReasoningSticky } from "./reasoning-scroll"
import { client, el, getCurrentSession, runs, syncConnThinking, type RunState, type SubSessionState } from "./state"
import { appendTail } from "./msg-window"
import { finishRun, noteRound, startRun } from "./subsessions"
import { activatePane, activePane, ensurePane, paneOf, setPaneRunning } from "./msg-panes"
import { effectiveIdleMs, initPageIdle, isPageHidden } from "./page-idle"
import { uuid } from "./uuid"
import { IDLE_TIMEOUT_MS, startTurnTimer, stopTurnTimer } from "./turn-view"

// 页面不可见时段登记（后台标签页/手机切 App/锁屏）：空闲看门狗据此把后台时长从「无数据」里扣除
initPageIdle()

/** 历史重载后重建在途渲染（缺口重同步专用）：重载清空了消息容器（在途气泡一并被移除），而重载期间的
 *  增量只做了累积——按累积内容重新起气泡，避免「重载完成后正文不再显示」。 */
function rehydrateRunView(run: RunState, sessionId: string): void {
  if (run.abort.signal.aborted) return
  run.el = null
  run.reasoningEl = null
  // 只为本会话自己的容器重建（后台会话的容器仍在，无需放弃渲染）；无容器（已结束/未接管）不碰 DOM
  if (!paneOf(sessionId)) return
  if (!run.acc.trim() && !run.reasoningAcc.trim()) return
  run.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true, undefined, paneOf(sessionId))
  const bubble = run.el.querySelector<HTMLElement>(".msg-body .bubble")
  if (run.reasoningAcc.trim() && bubble) {
    run.reasoningEl = reasoningBlock()
    bubble.prepend(run.reasoningEl)
  }
  scheduleStreamRender(run)
  scrollIfSticky()
  refreshJumpBottom()
}

function applyStreamChunk(run: RunState, sessionId: string, chunk: ChatChunk): void {
  // 子会话在途状态：容器缺失（重连重置了 subSessions，或 start 事件已在断线前投递）时惰性重建——
  // 观察面板的记录与过程区常驻，重建只补在途渲染引用，不丢已渲染内容
  const subStateOf = (runId: string): SubSessionState => {
    run.subSessions ??= new Map()
    const existing = run.subSessions.get(runId)
    if (existing) return existing
    const rec = startRun({ sessionId, runId, agents: [], input: "" })
    const created: SubSessionState = { runId, agents: [], input: "", body: rec.body, acc: "", el: null, messageId: "", reasoningAcc: "", reasoningEl: null }
    run.subSessions.set(runId, created)
    return created
  }
  if (chunk.kind === "resume") {
    // 断线重连后的全量重同步：重置本轮回累积并重建消息元素，防止内容重复渲染
    run.acc = ""
    run.reasoningAcc = ""
    run.messageId = ""
    run.lastTextKind = undefined
    run.lastTextMsgId = undefined
    run.subSessions = undefined // 新会话容器随消息元素一并重建（服务端每轮重推 start，容器会重新创建）
    if (run.el) {
      run.el.classList.remove("streaming")
      run.el.remove()
    }
    run.el = null
    run.reasoningEl = null
    // 日志缺口（overrun）后的重同步：缺口期间的结构化事件（工具卡/子会话容器/待决交互）已丢失且不可重放——
    // 重读消息列表重建历史，并以服务端待决清单重建交互卡；重载会清空消息容器（在途气泡一并移除），
    // 故暂停正文渲染（只累积）至重载完成，再按累积内容重建在途气泡
    if (chunk.reloadHistory) {
      run.reloading = true
      void (async () => {
        try {
          await loadMessages(sessionId)
        } catch {
          /* 仍不稳定：下次进入会话/重连由 attach 兜底 */
        }
        await resyncPendingInteractions(sessionId)
        run.reloading = false
        rehydrateRunView(run, sessionId)
      })()
    }
    return
  }
  if (chunk.kind === "text") {
    // 模型恢复输出：异常记录标注为已恢复（记录本身常驻，报错信息不随恢复消失）
    run.modelError?.recovered()
    const prevMsgId = run.messageId // 轮界检测用前值（messageId 随后刷新，见下方 text 分支）
    if (chunk.messageId) run.messageId = chunk.messageId
    const runId = chunk.subSessionId
    if (runId) {
      // 子会话运行过程文本：渲染进观察面板的过程区（进程区常驻 DOM，切走/未展开都不丢）
      const sub = subStateOf(runId)
      // 新一轮回复（messageId 变化）：封存上一段，并计入轮次
      if (chunk.messageId && sub.messageId && chunk.messageId !== sub.messageId) sealSessionSegment(sub)
      if (chunk.messageId) {
        sub.messageId = chunk.messageId
        noteRound(sessionId, runId, chunk.messageId)
      }
      sub.acc += chunk.text ?? ""
      // 空白内容不渲染：工具调用之间的空文本段不产生空气泡
      if (!sub.acc.trim()) return
      if (!sub.el?.isConnected) {
        sub.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true, sub.body)
      }
      scheduleSessionRender(sub)
      return
    }
    // 新会话输出与主回复分段（换行分隔）：来源切换（主↔新会话）或
    // 新会话新一轮回复（messageId 变化）时封存当前文本段，避免内容一直追加成同一条
    const isSub = chunk.subSession === true
    if (run.lastTextKind !== undefined) {
      const kindChanged = run.lastTextKind !== (isSub ? "sub" : "main")
      const subRoundChanged = isSub && !!run.lastTextMsgId && !!chunk.messageId && run.lastTextMsgId !== chunk.messageId
      if (kindChanged || subRoundChanged) sealSegment(sessionId)
    }
    // 主循环轮界：messageId 变化 = 新一轮回复，清空累积后新文本另起气泡。前台本在工具
    // 调用处封段（onToolCall），但后台会话工具事件不渲染卡片直接丢弃，累积从前只增不减——
    // 切回时整任务多轮文本渲染进同一张流式卡片（新回复追加在前面的卡片内）。
    // 前值含子会话最后消息 id（runId 分支已早退，此处仅主循环文本到达）：子会话结束后主循环
    // 新轮 id 必异于子会话尾轮 id，同样正确触发轮界重置
    if (chunk.messageId && prevMsgId && chunk.messageId !== prevMsgId) {
      if (run.lastTextKind === "sub") sealSegment(sessionId)
      run.acc = ""
    }
    run.lastTextKind = isSub ? "sub" : "main"
    if (chunk.messageId) run.lastTextMsgId = chunk.messageId
    run.acc += chunk.text ?? ""
    // 推理完成、正文开始：自动折叠推理块（用户可点 summary 重新展开）
    if (run.reasoningEl?.isConnected && (run.reasoningEl as HTMLDetailsElement).open) (run.reasoningEl as HTMLDetailsElement).open = false
    // 空白内容不渲染：工具调用之间的空文本段不产生空气泡
    if (!run.acc.trim()) return
    if (run.reloading) return // 历史重载中：只累积（重载完成后由 rehydrateRunView 重建气泡）
    // 容器守卫：该会话没有自己的容器（未运行/已释放）时才只累积；运行中会话（含后台）照常建 DOM
    if (!paneOf(sessionId)) return
    // 工具调用已封段后（run.el 为 null）或元素脱离 DOM：惰性重建消息元素
    if (!run.el?.isConnected) {
      run.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true, undefined, paneOf(sessionId))
    }
    scheduleStreamRender(run)
  } else if (chunk.kind === "reasoning") {
    const runId = chunk.subSessionId
    if (runId) {
      // 子会话运行过程推理：渲染进观察面板过程区（与主循环同构的折叠推理块）
      const sub = subStateOf(runId)
      sub.reasoningAcc += chunk.text ?? ""
      if (!sub.reasoningAcc.trim()) return
      if (!sub.el?.isConnected) {
        sub.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true, sub.body)
      }
      const bubble = sub.el.querySelector(".msg-body .bubble")
      if (bubble && sub.el.isConnected) {
        if (!sub.reasoningEl?.isConnected) {
          sub.reasoningEl = reasoningBlock()
          bubble.prepend(sub.reasoningEl)
        }
        scheduleSessionRender(sub)
        scrollSessionSticky(sub.body)
      }
      return
    }
    run.reasoningAcc += chunk.text ?? ""
    // 空白推理内容不展示（不创建折叠块）
    if (!run.reasoningAcc.trim()) return
    if (run.reloading) return // 历史重载中：只累积（重载完成后由 rehydrateRunView 重建）
    // 容器守卫（同正文路径）：无自己容器的会话才只累积
    if (!paneOf(sessionId)) return
    if (!run.el?.isConnected) {
      run.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true, undefined, paneOf(sessionId))
    }
    const bubble = run.el.querySelector(".msg-body .bubble")
    if (bubble && run.el.isConnected) {
      if (!run.reasoningEl?.isConnected) {
        run.reasoningEl = reasoningBlock()
        bubble.prepend(run.reasoningEl)
      }
      // 推理内容 markdown 渲染：与正文同走 120ms 尾沿节流渲染路径
      scheduleStreamRender(run)
      if (getCurrentSession()?.id === sessionId) {
        scrollIfSticky()
        refreshJumpBottom()
      }
    }
  } else if (chunk.kind === "subsession_start") {
    // 子会话 run 开始：在消息流里留一行入口（点击开观察面板），过程进面板；服务端每轮重推同 runId start
    const runId = chunk.subSessionId ?? ""
    if (!runId || !paneOf(sessionId)) return
    sealSegment(sessionId) // 子会话开始：父会话文本段在此分段
    run.subSessions ??= new Map()
    if (run.subSessions.get(runId)) return
    const meta = chunk.subSessionMeta
    const name = meta?.subsession
    const rec = startRun({ sessionId, runId, name, model: meta?.model, agents: meta?.agents ?? [], input: meta?.input ?? "" })
    subSessionEntry({ sessionId, runId, name, model: meta?.model, agents: meta?.agents ?? [], state: rec.stateText, status: "running" })
    run.subSessions.set(runId, {
      runId,
      agents: meta?.agents ?? [],
      input: meta?.input ?? "",
      subsession: name ? { name, model: meta?.model } : undefined,
      body: rec.body,
      acc: "",
      el: null,
      messageId: "",
      reasoningAcc: "",
      reasoningEl: null,
    })
    scrollIfSticky()
    refreshJumpBottom()
  } else if (chunk.kind === "subsession_done") {
    // 子会话 run 结束：过程区文本段封存，输出与耗时定稿（消息流入口行与面板行同步刷新）
    const runId = chunk.subSessionId ?? ""
    const sub = run.subSessions?.get(runId)
    if (sub) {
      if (sub.el?.isConnected) {
        sub.el.classList.remove("streaming")
        const bubble = sub.el.querySelector<HTMLElement>(".msg-body .bubble")
        if (bubble) addMetaActions(sub.el.querySelector<HTMLElement>(".msg-meta") ?? sub.el, sub.el, bubble, { role: "assistant", content: sub.acc, id: sub.messageId }, { noRevoke: true })
      }
      sealSessionSegment(sub)
      run.subSessions?.delete(runId)
    }
    finishRun(sessionId, runId, chunk.subSessionMeta?.output ?? "")
  } else if (chunk.kind === "model_error") {
    // 模型服务异常（引擎自动重试中）：消息流内常驻记录，非终态
    showModelErrorNotice(run, sessionId, chunk)
  }
}

/**
 * 模型服务异常记录（重试期间）：首次出现时挂到消息流尾部，后续按重试序号原地更新
 * ——单一记录块、断线重放不堆叠，也不随恢复输出/任务结束消失。
 */
function showModelErrorNotice(run: RunState, sessionId: string, chunk: ChatChunk): void {
  if (!paneOf(sessionId)) return
  if (!run.modelError) {
    run.modelError = createModelErrorNotice()
    appendTail(run.modelError.el)
    scrollIfSticky()
    refreshJumpBottom()
  }
  const key = `${chunk.retry ?? 0}/${chunk.maxRetry ?? 0}:${chunk.error ?? ""}`
  run.modelError.record(key, modelErrorText(chunk.retry, chunk.maxRetry, chunk.error))
}

/** 单轮耗时展示格式：<1m 整秒；<1h 分+秒；以上时+分。 */

function appendFinalNotice(sessionId: string, text: string): void {
  if (!paneOf(sessionId)) return
  const wrapper = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() })
  const bubble = wrapper.querySelector<HTMLElement>(".msg-body .bubble")
  if (bubble) {
    bubble.appendChild(blockText(text))
    addMetaActions(wrapper.querySelector<HTMLElement>(".msg-meta") ?? wrapper, wrapper, bubble, { role: "assistant", content: text, id: uuid() }, { noRevoke: true })
  }
}

/**
 * 运行一个任务流并渲染（直接发送与排队输入自动执行共用）：
 * 建立运行态与空闲看门狗，迭代 ChatChunk 流式渲染，收尾统一清理（运行态/审批卡/配对/焦点），
 * 收尾后自动发送下一条排队输入（会话输入队列）。
 * makeSource 以运行态的 abort 信号构造流（信号由空闲超时兜底触发中止）。
 */
export async function consumeTaskStream(sessionId: string, makeSource: (run: RunState) => AsyncIterable<ChatChunk>, opts?: { startedAt?: number }): Promise<void> {
  const run: RunState = { sessionId, acc: "", el: null, reasoningAcc: "", reasoningEl: null, messageId: "", lastActivity: Date.now(), startedAt: opts?.startedAt ?? Date.now(), abort: new AbortController() }
  runs.set(sessionId, run)
  // 为运行中的会话确保自己的容器并标记运行态：后台期间也照常建 DOM（工具卡/正文/子容器），
  // 切回只是显隐切换（不用等网络往返与全量重建）
  ensurePane(sessionId)
  // 草稿态首条消息（会话刚创建、从未经 loadMessages）：pane 建出来是隐藏的，必须就地激活——
  // 否则消息区空白（内容都渲染进隐藏容器），用户以为没发出去
  if (!activePane() && getCurrentSession()?.id === sessionId) activatePane(sessionId)
  setPaneRunning(sessionId, true)
  // 会话列表角标：本页自己发起的任务也要标上。
  // 原先只有服务端事件（event.task.start）会写 runningSeen，而自己在当前页发送时那条事件
  // 到达得比本地状态晚（或干脆被视为本页已有运行态而不再处理），列表就一直不显示“在跑”——
  // 刷新页面后才靠快照 runtime 补上（用户看到的就是「标记刷新就没了」）。
  markSessionRunning(sessionId, true)
  startTurnTimer(run) // 单轮计时器：本轮耗时实时显示（外观 tab 可关）
  syncConnThinking() // 运行开始：信号灯闪烁
  syncSendButton() // 不禁用按钮：运行中点击 = 停止（stopping 拦截）
  const source = makeSource(run)
  // 空闲超时兜底：流有效空闲（可见时长）超 IDLE_TIMEOUT_MS 无任何数据视为挂起（服务端/网络异常），
  // 中断并清理，防止运行态/信号灯残留。三种「无数据」不算挂起，不得据此取消：
  // ① 连接断开——断线期间无从判定服务端死活，且取消请求会随重连送达而杀掉仍在跑的后台任务
  //    （连接恢复由 SDK 自动重连 + 事件重放收敛，是否真跑完交由快照核对判定）；
  // ② 页面不可见（后台标签页/手机切 App/锁屏）——定时器被冻结或节流，lastActivity 停在切走前，
  //    而本地连接标志仍为 true（close 事件尚未处理），恢复后补触发就会把整段后台当挂起杀掉
  //    正在跑的脚本；后台时长同样不计入有效空闲（关闭浏览器页面销毁反而不受影响）
  // ③ 等待用户作答（审批/选择/填值/画图/捕获）——服务端等待期有心跳（event.interaction.alive），
  //    超时也会推来结果事件刷新活跃时间
  const idleTimer = setInterval(() => {
    if (run.abort.signal.aborted) return
    if (isPageHidden()) return
    if (effectiveIdleMs(run.lastActivity) <= IDLE_TIMEOUT_MS) return
    if (!client.isConnected()) return
    if (hasPendingInteraction(sessionId)) return
    run.idleTimedOut = true // 标记：收尾时给出显式提示（此前静默取消，用户无从得知原因）
    run.abort.abort()
    void client.cancelTask(sessionId).catch(() => {})
  }, 10_000)
  try {
    // 文本/推理增量与审批/工具调用/结果等结构化事件统一走 WS（sendPrompt 内部订阅 event.*
    // 并转换为 ChatChunk）；WS 断开时迭代抛错（流中断），由 catch 分支渲染错误
    for await (const chunk of source) {
      run.lastActivity = Date.now()
      if (chunk.kind === "error") {
        if (chunk.error === "cancelled") {
          // 任务被取消：手动停止/审批拒绝/中断插入为有意为之，静默收尾；
          // 空闲超时兜底的取消非用户意图且无任何输出时，给出显式说明（有输出则保留内容静默）
          if (run.idleTimedOut && !run.el?.isConnected && !run.reasoningEl?.isConnected && !run.acc.trim()) {
            appendFinalNotice(sessionId, "生成超时：长时间未收到数据（模型响应过慢或连接中断），任务已中止。请重试或检查网络/模型配置。")
          }
          return
        }
        // 服务端任务失败（LLM 接口错误等）：与 catch 分支一致渲染错误气泡
        throw new Error(chunk.error || "任务失败")
      }
      applyStreamChunk(run, sessionId, chunk)
      // 单轮完成庆祝（人民币主题招财猫爆金币，运行越久爆得越多）；错误/取消路径不庆祝
      if (chunk.kind === "done") cnyCatTurnEnd(Date.now() - run.startedAt)
    }
    // 推理后无正文直接结束（如纯工具链）：兜底折叠推理块。
    // 不依赖 run.el 连接状态：最后一次工具调用封段后 run.el 为 null，但推理块仍在 DOM
    if (run.reasoningEl?.isConnected && (run.reasoningEl as HTMLDetailsElement).open) (run.reasoningEl as HTMLDetailsElement).open = false
    if (run.el?.isConnected) {
      run.el.classList.remove("streaming")
      const bubble = run.el.querySelector<HTMLElement>(".msg-body .bubble")
      if (bubble) addMetaActions(run.el.querySelector<HTMLElement>(".msg-meta") ?? run.el, run.el, bubble, { role: "assistant", content: run.acc, id: run.messageId })
    } else if (!run.reasoningEl?.isConnected && !run.acc.trim() && !run.reasoningAcc.trim()) {
      // 任务正常结束却无任何可见输出（接口返回空回复等异常形态）：显式提示，不留「无声无息就结束」的悬念
      appendFinalNotice(sessionId, "任务已结束，但没有收到任何回复内容。请重试；若反复出现请检查模型配置。")
    }
  } catch (err) {
    clearStreamRender(run) // 错误路径：作废低性能节流排期（防补渲覆盖错误气泡）
    // 会话已被另一处任务占用（本页不知情：其他标签页/飞书桥接/定时任务）：本条输入未被受理——
    // 不渲染错误气泡，上抛给发起方（main.ts 撤回幻影消息并把内容放回输入框）
    if ((err as Error & { code?: string }).code === "already_running") throw err
    if (run.el?.isConnected) {
      run.el.classList.remove("streaming")
      const bubble = run.el.querySelector<HTMLElement>(".msg-body .bubble")
      if (bubble) {
        // 空闲超时主动中断且回答已有内容：内容完整，静默收尾不渲染错误气泡；否则提示超时/错误
        if (!(run.abort.signal.aborted && run.acc.trim())) {
          const msg = run.abort.signal.aborted ? "生成超时，请重试" : `错误: ${(err as Error).message}`
          if (run.acc.trim()) {
            // 已有部分输出后失败：保留已生成内容，错误说明追加其后（不覆盖）
            const notice = el("div", "msg-error-notice")
            notice.appendChild(blockText(msg))
            bubble.appendChild(notice)
          } else {
            bubble.innerHTML = ""
            bubble.appendChild(blockText(msg))
          }
          // 错误路径的部分输出未持久化（无落点消息），不提供撤回
          addMetaActions(run.el.querySelector<HTMLElement>(".msg-meta") ?? run.el, run.el, bubble, { role: "assistant", content: run.acc || msg, id: run.messageId }, { noRevoke: true })
        }
      }
    } else {
      // 无任何输出即失败（首条内容到达前的接口错误/断连）：此前静默结束，补渲染错误气泡说明原因
      const msg = run.abort.signal.aborted ? "生成超时，请重试" : `错误: ${(err as Error).message}`
      appendFinalNotice(sessionId, msg)
    }
  } finally {
    run.modelError?.failed() // 任务结束：仍在重试中的异常记录定格（记录常驻，不随收尾消失）
    stopTurnTimer(run) // 任务结束：单轮计时停表定格
    clearInterval(idleTimer) // 空闲超时兜底定时器随流结束清理
    // 流结束：低性能节流排期未到点则同步补渲最后一帧（防末尾文本丢失）；错误路径已清排期，不重复渲染
    if (run.renderTimer) {
      clearTimeout(run.renderTimer)
      run.renderTimer = undefined
      renderStreamText(run)
    }
    runs.delete(sessionId)
    // 运行结束：容器降为可回收（已切走则下一次回收时释放，仍在看则保留到切走）
    setPaneRunning(sessionId, false)
  // 会话列表角标：本页任务的结束同样要收敛（与服务端 task.done 一致）
  markSessionRunning(sessionId, false)
    clearApprovals(sessionId) // 任务结束：该会话残留审批卡片随任务终止失效并解除输入锁定
    clearInteractionCards(sessionId) // 任务结束：选择/环境变量填值卡片随任务终止失效
    clearPendingTools(sessionId) // 任务结束：工具调用配对清理（结果已落盘历史）
    syncConnThinking() // 运行结束：信号灯恢复常亮
    syncSendButton()
    void maybeAutoTitle(sessionId) // 兜底重试：发送时点自动命名失败（未标记 autoNamed）时补一次，已命名空转
    // 队列继续：任务收尾（完成/取消/出错）后自动发送下一条排队输入（运行中为空转）
    drainQueue(sessionId)
    // 焦点守卫：仅当用户仍在发起会话（未切走）时恢复输入焦点，避免后台流结束抢走当前会话光标
    if (getCurrentSession()?.id === sessionId) focusInput()
  }
}

/** 待决交互卡片重建（attach 快照 → 既有渲染入口；替换式幂等——同 id 重复推送只保留一张）。 */

/** 流式正文渲染：按块边界增量（前缀常驻、只重渲染尾部，见 stream-render.ts）——
 * 逐帧重解析全文在长回答下是 O(n²)（会话运行耗帧的主要来源）。
 * 必须惰性重建消息元素场景由调用方保证（run.el 已建）。 */

/** 渲染器按容器分配（容器即气泡内 .msg-text，重建则自然新建）：一个实例只服务一个容器——
 * 共享实例会在容器间反复重建宿主，退化为全量重渲染。 */
const streamRenderers = new WeakMap<HTMLElement, ReturnType<typeof createStreamRenderer>>()

function rendererFor(container: HTMLElement) {
  let renderer = streamRenderers.get(container)
  if (!renderer) {
    renderer = createStreamRenderer({ renderFragment: markdownBlock, renderFull: assistantContent })
    streamRenderers.set(container, renderer)
  }
  return renderer
}

function renderStreamText(run: RunState): void {
  // 推理块 markdown 渲染：流式推理内容实时更新（低性能模式下与正文合并节流）
  if (run.reasoningEl?.isConnected) {
    const rb = run.reasoningEl.querySelector<HTMLElement>(".reasoning-body")
    if (rb) {
      rb.textContent = ""
      if (run.reasoningAcc.trim()) rb.appendChild(markdownBlock(run.reasoningAcc.trim()))
      scrollReasoningSticky(rb)
    }
  }
  const bubble = run.el?.querySelector(".msg-body .bubble")
  if (!bubble) return
  let textWrap = bubble.querySelector<HTMLElement>(".msg-text")
  if (!textWrap) {
    textWrap = el("div", "msg-text")
    bubble.appendChild(textWrap)
  }
  rendererFor(textWrap).update(textWrap, run.acc)
  if (getCurrentSession()?.id === run.sessionId) {
    scrollIfSticky()
    refreshJumpBottom()
  }
}

/** 流式文本渲染按 120ms 尾沿节流（markdown 全量重解析是流式期间最重的 CPU 开销，
 * 逐 chunk 同步渲染在长回答下 O(n²) 全量重解析——所有模式统一节流，消除性能悬崖）。
 * 计时器挂 run 上：封段/结束时随 run 清理。 */
function scheduleStreamRender(run: RunState): void {
  if (run.renderTimer) return // 已排期：本轮仅累积 run.acc，到点一起渲染
  run.renderTimer = setTimeout(() => {
    run.renderTimer = undefined
    renderStreamText(run)
  }, 120)
}

/** 清空低性能流式渲染排期（封段/重置/结束时调用，防滞留定时器对已封段气泡补渲染）。 */
function clearStreamRender(run: RunState): void {
  if (run.renderTimer) {
    clearTimeout(run.renderTimer)
    run.renderTimer = undefined
  }
}

/** 新会话容器内流式文本渲染：与主循环 renderStreamText 同构（推理块 + 正文 markdown），追加到容器并滚动。 */
function renderSessionStreamText(sub: SubSessionState): void {
  if (sub.reasoningEl?.isConnected) {
    const rb = sub.reasoningEl.querySelector<HTMLElement>(".reasoning-body")
    if (rb) {
      rb.textContent = ""
      if (sub.reasoningAcc.trim()) rb.appendChild(markdownBlock(sub.reasoningAcc.trim()))
      scrollReasoningSticky(rb)
    }
  }
  const bubble = sub.el?.querySelector(".msg-body .bubble")
  if (!bubble) return
  let textWrap = bubble.querySelector<HTMLElement>(".msg-text")
  if (!textWrap) {
    textWrap = el("div", "msg-text")
    bubble.appendChild(textWrap)
  }
  textWrap = textWrap ?? (() => {
    const box = el("div", "msg-text")
    bubble.appendChild(box)
    return box
  })()
  rendererFor(textWrap).update(textWrap, sub.acc)
  scrollSessionSticky(sub.body) // 容器内粘底：用户未上翻时跟随最新内容
  scrollIfSticky()
  refreshJumpBottom()
}

/** 新会话容器内流式文本渲染按 120ms 尾沿节流（与主循环 scheduleStreamRender 同构，所有模式统一）。 */
function scheduleSessionRender(sub: SubSessionState): void {
  if (sub.renderTimer) return
  sub.renderTimer = setTimeout(() => {
    sub.renderTimer = undefined
    renderSessionStreamText(sub)
  }, 120)
}
