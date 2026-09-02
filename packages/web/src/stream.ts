/** 流式消费与渲染（自 main.ts 拆分）：ChatChunk 应用、模型错误提示、任务流消费、流式文本节流渲染。 */
import type { ChatChunk } from "@gebai/sdk"
import { cnyCatTurnEnd } from "./cny-cat"
import { syncSendButton } from "./composer"
import { refreshJumpBottom, scrollIfSticky } from "./jump-bottom"
import { addMetaActions, appendMsg, assistantContent, clearInteractionCards, finishSessionRun, reasoningBlock, scrollSessionSticky, sealSegment, sealSessionSegment, sessionRunBox } from "./messages"
import { blockText, markdownBlock } from "./markdown"
import { clearApprovals } from "./approvals"
import { clearPendingTools, focusInput } from "./state"
import { maybeAutoTitle } from "./sessions"
import { drainQueue } from "./queue"
import { scrollReasoningSticky } from "./reasoning-scroll"
import { client, el, getCurrentSession, msgEl, runs, syncConnThinking, type RunState, type SessionRunState } from "./state"
import { uuid } from "./uuid"
import { IDLE_TIMEOUT_MS, startTurnTimer, stopTurnTimer } from "./turn-view"

function applyStreamChunk(run: RunState, sessionId: string, chunk: ChatChunk): void {
  if (chunk.kind === "resume") {
    // 断线重连后的全量重同步：重置本轮回累积并重建消息元素，防止内容重复渲染
    run.acc = ""
    run.reasoningAcc = ""
    run.messageId = ""
    run.lastTextKind = undefined
    run.lastTextMsgId = undefined
    run.sessionRuns = undefined // 新会话容器随消息元素一并重建（服务端每轮重推 start，容器会重新创建）
    if (run.el) {
      run.el.classList.remove("streaming")
      run.el.remove()
    }
    run.el = null
    run.reasoningEl = null
    return
  }
  if (chunk.kind === "text") {
    // 模型恢复输出：移除模型服务异常瞬时提示
    if (run.modelErrorEl?.isConnected) clearModelErrorNotice(run)
    if (chunk.messageId) run.messageId = chunk.messageId
    const runId = chunk.sessionRunId
    if (runId) {
      // 新会话执行过程文本：渲染进该 run 的折叠容器（执行中展开，与主回复同流显示）
      let sub = run.sessionRuns?.get(runId)
      if (!sub) {
        // 容器缺失（重连全量重同步清空 sessionRuns 后服务端不重推 start——事件已在断线前投递）：
        // 惰性重建容器兜底，否则该 run 后续输出静默丢弃、容器永久停留旧状态（分支标题等元信息
        // 随 sessionRuns 一并丢失，下一轮 start 重推时容器已存在会被忽略——可接受的降级）
        run.sessionRuns ??= new Map()
        const box = sessionRunBox({ runId, agents: [], input: "" })
        sub = { runId, agents: [], input: "", container: box.container, body: box.body, outputEl: box.outputEl, acc: "", el: null, messageId: "", reasoningAcc: "", reasoningEl: null }
        run.sessionRuns.set(runId, sub)
        scrollIfSticky()
        refreshJumpBottom()
      }
      // 切走/重载中（容器脱离 DOM）：先累积文本（切回由 loadMessages 恢复渲染），与主循环累积语义一致
      if (!sub.container.isConnected) {
        sub.acc += chunk.text ?? ""
        return
      }
      // 新会话新一轮回复（messageId 变化）：封存上一段
      if (chunk.messageId && sub.messageId && chunk.messageId !== sub.messageId) sealSessionSegment(sub)
      if (chunk.messageId) sub.messageId = chunk.messageId
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
    const isSub = chunk.session === true
    if (run.lastTextKind !== undefined) {
      const kindChanged = run.lastTextKind !== (isSub ? "sub" : "main")
      const subRoundChanged = isSub && !!run.lastTextMsgId && !!chunk.messageId && run.lastTextMsgId !== chunk.messageId
      if (kindChanged || subRoundChanged) sealSegment(sessionId)
    }
    run.lastTextKind = isSub ? "sub" : "main"
    if (chunk.messageId) run.lastTextMsgId = chunk.messageId
    run.acc += chunk.text ?? ""
    // 推理完成、正文开始：自动折叠推理块（用户可点 summary 重新展开）
    if (run.reasoningEl?.isConnected && (run.reasoningEl as HTMLDetailsElement).open) (run.reasoningEl as HTMLDetailsElement).open = false
    // 空白内容不渲染：工具调用之间的空文本段不产生空气泡
    if (!run.acc.trim()) return
    // 会话守卫：切到其他会话时只累积不触碰 DOM（切回时由 loadMessages 从 run.acc 恢复渲染）
    if (getCurrentSession()?.id !== sessionId) return
    // 工具调用已封段后（run.el 为 null）或元素脱离 DOM：惰性重建消息元素
    if (!run.el?.isConnected) {
      run.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true)
    }
    scheduleStreamRender(run)
  } else if (chunk.kind === "reasoning") {
    const runId = chunk.sessionRunId
    if (runId) {
      // 新会话执行过程推理：渲染进容器内气泡（与主循环同构的折叠推理块）
      const sub = run.sessionRuns?.get(runId)
      if (!sub) return
      if (!sub.container.isConnected) {
        // 切走/重载中：只累积推理（切回由 loadMessages 恢复），与主循环累积语义一致
        sub.reasoningAcc += chunk.text ?? ""
        return
      }
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
        scrollIfSticky()
        refreshJumpBottom()
      }
      return
    }
    run.reasoningAcc += chunk.text ?? ""
    // 空白推理内容不展示（不创建折叠块）
    if (!run.reasoningAcc.trim()) return
    // 会话守卫：切走时只累积（推理内容不持久化，切回由正文恢复；再流式时重建折叠块）
    if (getCurrentSession()?.id !== sessionId) return
    if (!run.el?.isConnected) {
      run.el = appendMsg({ id: uuid(), role: "assistant", content: "", createdAt: Date.now() }, true)
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
  } else if (chunk.kind === "session_start") {
    // 新会话 run 开始：创建折叠容器（执行中展开并滚动到可见；服务端每轮重推同 runId start，已存在则忽略）
    // 分支运行（branch_run）容器标题带分支名/模型路由（sessionMeta.branch/model）
    const runId = chunk.sessionRunId ?? ""
    if (!runId || getCurrentSession()?.id !== sessionId) return
    sealSegment(sessionId) // 新会话开始：主文本段在此分段
    run.sessionRuns ??= new Map()
    if (run.sessionRuns.get(runId)?.container.isConnected) return
    const branch = chunk.sessionMeta?.branch ? { name: chunk.sessionMeta.branch, model: chunk.sessionMeta.model } : undefined
    const box = sessionRunBox({ runId, agents: chunk.sessionMeta?.agents ?? [], input: chunk.sessionMeta?.input ?? "", branch })
    run.sessionRuns.set(runId, {
      runId,
      agents: chunk.sessionMeta?.agents ?? [],
      input: chunk.sessionMeta?.input ?? "",
      branch,
      container: box.container,
      body: box.body,
      outputEl: box.outputEl,
      acc: "",
      el: null,
      messageId: "",
      reasoningAcc: "",
      reasoningEl: null,
    })
    scrollIfSticky()
    refreshJumpBottom()
  } else if (chunk.kind === "session_done") {
    // 新会话 run 结束：封存流式文本段，写入最终返回摘要并自动折叠容器（只显示输入与最终返回）
    const runId = chunk.sessionRunId ?? ""
    const sub = run.sessionRuns?.get(runId)
    if (sub) {
      if (sub.el?.isConnected) {
        sub.el.classList.remove("streaming")
        const bubble = sub.el.querySelector<HTMLElement>(".msg-body .bubble")
        if (bubble) addMetaActions(sub.el.querySelector<HTMLElement>(".msg-meta") ?? sub.el, sub.el, bubble, { role: "assistant", content: sub.acc, id: sub.messageId }, { noRevoke: true })
      }
      sealSessionSegment(sub)
      finishSessionRun(sub.container, sub.outputEl, chunk.sessionMeta?.output ?? "")
      run.sessionRuns?.delete(runId)
    }
  } else if (chunk.kind === "model_error") {
    // 模型服务异常（引擎自动重试中）：消息流尾部瞬时提示，非终态——文本恢复时移除
    showModelErrorNotice(run, sessionId, chunk)
  }
}

/** 模型服务异常瞬时提示（重试期间）：单一元素复用更新（重连重放不堆叠），文本恢复/任务结束时移除。 */
function showModelErrorNotice(run: RunState, sessionId: string, chunk: ChatChunk): void {
  if (getCurrentSession()?.id !== sessionId) return
  const retry = chunk.retry ? (chunk.maxRetry ? `（第 ${chunk.retry}/${chunk.maxRetry} 次重试）` : `（第 ${chunk.retry} 次重试）`) : ""
  const text = `模型服务异常${retry}：${chunk.error ?? ""}，正在自动重试…`
  if (!run.modelErrorEl?.isConnected) {
    run.modelErrorEl = el("div", "model-error-notice")
    msgEl.appendChild(run.modelErrorEl)
    scrollIfSticky()
    refreshJumpBottom()
  }
  run.modelErrorEl.textContent = text
}

/** 移除模型服务异常瞬时提示（模型恢复输出/任务结束时调用）。 */
function clearModelErrorNotice(run: RunState): void {
  run.modelErrorEl?.remove()
  run.modelErrorEl = null
}

/** 单轮耗时展示格式：<1m 整秒；<1h 分+秒；以上时+分。 */

function appendFinalNotice(sessionId: string, text: string): void {
  if (getCurrentSession()?.id !== sessionId) return
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
  startTurnTimer(run) // 单轮计时器：本轮耗时实时显示（外观 tab 可关）
  syncConnThinking() // 运行开始：信号灯闪烁
  syncSendButton() // 不禁用按钮：运行中点击 = 停止（stopping 拦截）
  const source = makeSource(run)
  // 空闲超时兜底：流 IDLE_TIMEOUT_MS 无任何数据视为挂起（服务端/网络异常），中断并清理，
  // 防止运行态/信号灯残留；交互等待（选择/填值/画图/捕获）由 touchRunActivity 刷新活跃时间，
  // 等待用户回应的挂起不算无数据
  const idleTimer = setInterval(() => {
    if (Date.now() - run.lastActivity > IDLE_TIMEOUT_MS && !run.abort.signal.aborted) {
      run.idleTimedOut = true // 标记：收尾时给出显式提示（此前静默取消，用户无从得知原因）
      run.abort.abort()
      void client.cancelTask(sessionId).catch(() => {})
    }
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
    clearModelErrorNotice(run) // 任务结束：模型服务异常瞬时提示随流收尾移除
    stopTurnTimer(run) // 任务结束：单轮计时停表定格
    clearInterval(idleTimer) // 空闲超时兜底定时器随流结束清理
    // 流结束：低性能节流排期未到点则同步补渲最后一帧（防末尾文本丢失）；错误路径已清排期，不重复渲染
    if (run.renderTimer) {
      clearTimeout(run.renderTimer)
      run.renderTimer = undefined
      renderStreamText(run)
    }
    runs.delete(sessionId)
    clearApprovals(sessionId) // 任务结束：该会话残留审批卡片随任务终止失效并解除输入锁定
    clearInteractionCards(sessionId) // 任务结束：选择/环境变量填值卡片随任务终止失效
    clearPendingTools(sessionId) // 任务结束：工具调用配对清理（结果已落盘历史）
    syncConnThinking() // 运行结束：信号灯恢复常亮
    syncSendButton()
    void maybeAutoTitle(sessionId)
    // 队列继续：任务收尾（完成/取消/出错）后自动发送下一条排队输入（运行中为空转）
    drainQueue(sessionId)
    // 焦点守卫：仅当用户仍在发起会话（未切走）时恢复输入焦点，避免后台流结束抢走当前会话光标
    if (getCurrentSession()?.id === sessionId) focusInput()
  }
}

/** 待决交互卡片重建（attach 快照 → 既有渲染入口；替换式幂等——同 id 重复推送只保留一张）。 */

/** 流式文本渲染：整段累积文本重新走 markdown 解析（低性能模式下节流合并，降频重解析）。
 * 必须惰性重建消息元素场景由调用方保证（run.el 已建）。 */

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
  textWrap.innerHTML = ""
  if (run.acc) textWrap.appendChild(assistantContent(run.acc))
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
function renderSessionStreamText(sub: SessionRunState): void {
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
  textWrap.innerHTML = ""
  if (sub.acc) textWrap.appendChild(assistantContent(sub.acc))
  scrollSessionSticky(sub.body) // 容器内粘底：用户未上翻时跟随最新内容
  scrollIfSticky()
  refreshJumpBottom()
}

/** 新会话容器内流式文本渲染按 120ms 尾沿节流（与主循环 scheduleStreamRender 同构，所有模式统一）。 */
function scheduleSessionRender(sub: SessionRunState): void {
  if (sub.renderTimer) return
  sub.renderTimer = setTimeout(() => {
    sub.renderTimer = undefined
    renderSessionStreamText(sub)
  }, 120)
}
