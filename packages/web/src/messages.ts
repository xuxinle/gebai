import { uuid } from "./uuid"
import type { ContentBlock, Message, SessionRunArchive } from "@gebai/sdk"
import {
  ROLE_NAME,
  client,
  el,
  filesPreview,
  formatTime,
  getCurrentSession,
  input,
  focusInput,
  msgEl,
  approvalsEl,
  pendingTools,
  pendingToolsKey,
  runs,
  todoState,
  type SessionRunState,
} from "./state"
import { blockText, markdownBlock } from "./markdown"
import { renderCodeCard, renderFileCard } from "./file-card"
import { askUserBubble, choiceAnswerBlock, choiceBubble, displayToolName, envRequestBubble, fileBlocksAsLinks, isBlockOnly, planBubble, planResultHead, renderToolArgsDone, shortToolName, todoBubble, toolBubbleFor, toolHead, toolOutput } from "./tool-cards"
import { renderBlocksLinked } from "./file-link"
import { openImageViewer, renderDiagram } from "./diagram"
import { renderDiffBlock } from "./diff"
import { renderHtmlBlock } from "./html-view"
import { lockToBottom, scrollIfSticky } from "./jump-bottom"
import { createStickyFollow, type StickyFollowHandle } from "./sticky-follow"
import { addMsgNavSeg } from "./msg-nav"
import { autosize } from "./composer"
import { confirmDialog, copyText, tip, toast } from "./ui"

/** 渲染一组内容块：连续的 diagram 块收进 `.diagram-row` 横排展示（节省纵向空间），其余块逐个渲染。 */
export function renderBlocks(container: HTMLElement, blocks: ContentBlock[], sessionId: string) {
  let row: HTMLElement | null = null
  for (const b of blocks) {
    if (b.type === "diagram") {
      if (!row) {
        row = el("div", "diagram-row")
        container.appendChild(row)
      }
      void renderDiagram(row, b)
    } else {
      row = null
      renderBlock(container, b, sessionId)
    }
  }
}

export function renderBlock(container: HTMLElement, b: ContentBlock, sessionId: string) {
  switch (b.type) {
    case "text":
      container.appendChild(blockText(b.text))
      break
    case "code":
      // 文件内容卡：markdown 语言渲染 md、其余语法高亮；path 附带时提供复制/原文件/下载工具栏
      renderCodeCard(container, b, sessionId)
      break
    case "image": {
      const img = document.createElement("img")
      // files/preview 统一取数：产物块路径可能是会话相对路径或项目绝对路径（read 项目文件）
      const src = filesPreview(sessionId, b.path)
      img.src = src
      img.alt = b.name || "image"
      img.className = "block-img"
      // 点击进入全屏查看器（缩放/平移/复制/下载）：show 图表分支 render=backend 产出的 PNG 与普通图片块同样可全屏查看
      img.onclick = () => openImageViewer(src, b.name || "image")
      tip(img, "点击查看大图")
      // 图片异步加载改变高度：粘底则跟随滚动到底（jump-bottom.ts 委托捕获阶段 load 统一处理，含 markdown 内嵌图片）
      container.appendChild(img)
      break
    }
    case "file":
      // 文件内容卡：按 mime/扩展分派（图片内联/PDF iframe/沙箱 html/md 渲染/文本高亮/二进制占位），
      // 内容进入视口后按需加载；工具栏统一复制/原文件/下载
      renderFileCard(container, b, sessionId)
      break
    case "diagram":
      void renderDiagram(container, b)
      break
    case "diff":
      renderDiffBlock(container, b)
      break
    case "html":
      renderHtmlBlock(container, b)
      break
  }
}

/** 解析工具调用卡片文本 `→ name {args}`，返回 null 表示普通内容。 */
const ICON_COPY = `<svg viewBox="0 0 24 24" width="14" height="14" aria-hidden="true"><path d="M16 1H4a2 2 0 0 0-2 2v14h2V3h12V1zm3 4H8a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2zm0 16H8V7h11v14z" fill="currentColor"/></svg>`
const ICON_REVOKE = `<svg viewBox="0 0 24 24" width="14" height="14" aria-hidden="true"><path d="M12 5V1L7 6l5 5V7a6 6 0 1 1-6 6H4a8 8 0 1 0 8-8z" fill="currentColor"/></svg>`
const ICON_THUMBS_UP = `<svg viewBox="0 0 24 24" width="14" height="14" aria-hidden="true"><path d="M1 21h4V9H1v12zM23 10c0-1.1-.9-2-2-2h-6.31l.95-4.57.03-.32c0-.41-.17-.79-.44-1.06L14.17 1 7.59 7.59C7.22 7.95 7 8.45 7 9v10c0 1.1.9 2 2 2h9c.83 0 1.54-.5 1.84-1.22l3.02-7.05c.09-.23.14-.47.14-.73v-2z" fill="currentColor"/></svg>`
const ICON_THUMBS_DOWN = `<svg viewBox="0 0 24 24" width="14" height="14" aria-hidden="true"><path d="M15 3H6c-.83 0-1.54.5-1.84 1.22l-3.02 7.05c-.09.23-.14.47-.14.73v2c0 1.1.9 2 2 2h6.31l-.95 4.57-.03.32c0 .41.17.79.44 1.06L9.83 23l6.59-6.59c.36-.36.58-.86.58-1.41V5c0-1.1-.9-2-2-2zm4 0v12h4V3h-4z" fill="currentColor"/></svg>`
/** 推理块图标：大脑（线性描边，随 summary 文字颜色，样式见 chat.css .reasoning summary svg）。 */
const ICON_REASONING = `<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M9.5 2A2.5 2.5 0 0 1 12 4.5v15a2.5 2.5 0 0 1-4.96.44 2.5 2.5 0 0 1-2.96-3.08 3 3 0 0 1-.34-5.58 2.5 2.5 0 0 1 1.32-4.24 2.5 2.5 0 0 1 1.98-3A2.5 2.5 0 0 1 9.5 2Z"/><path d="M14.5 2A2.5 2.5 0 0 0 12 4.5v15a2.5 2.5 0 0 0 4.96.44 2.5 2.5 0 0 0 2.96-3.08 3 3 0 0 0 .34-5.58 2.5 2.5 0 0 0-1.32-4.24 2.5 2.5 0 0 0-1.98-3A2.5 2.5 0 0 0 14.5 2Z"/></svg>`

/** 点赞/点踩原因标签（DESIGN「用户反馈」：附可选原因标签；单选、可反选）。 */
const FB_LABELS: Record<"thumbs_up" | "thumbs_down", string[]> = {
  thumbs_up: ["优秀", "有用", "完整"],
  thumbs_down: ["错误", "不完整", "不符合预期"],
}

/** 当前打开的反馈弹层（同一时刻至多一个：再次打开先关旧的）。 */
let fbPopover: HTMLElement | null = null
function closeFeedbackPopover() {
  fbPopover?.remove()
  fbPopover = null
}

/** 打开质量反馈弹层（👍/👎 → 原因标签 + 补充说明 → 提交）：写入用户反馈存储，设置面板「反馈」页可见。
 *  提交成功后禁用该消息的 👍/👎 防重复提交。 */
function openFeedbackPopover(type: "thumbs_up" | "thumbs_down", host: HTMLElement, msg: Pick<Message, "id">, buttons: HTMLElement[]) {
  closeFeedbackPopover()
  const pop = el("div", "fb-popover")
  pop.addEventListener("click", (e) => e.stopPropagation()) // 弹层内点击不冒泡到外部关闭
  const labels = el("div", "fb-labels")
  let selected = ""
  for (const label of FB_LABELS[type]) {
    const chip = el("button", "fb-chip", label)
    chip.onclick = () => {
      selected = selected === label ? "" : label
      for (const c of labels.querySelectorAll(".fb-chip")) {
        c.classList[(c as HTMLElement).textContent === selected ? "add" : "remove"]("selected")
      }
    }
    labels.appendChild(chip)
  }
  const text = el("textarea", "fb-text") as HTMLTextAreaElement
  text.rows = 2
  text.placeholder = "补充说明（可选）"
  const row = el("div", "fb-row")
  const cancel = el("button", "fb-cancel", "取消")
  cancel.onclick = closeFeedbackPopover
  const submit = el("button", "fb-submit", type === "thumbs_up" ? "👍 提交" : "👎 提交")
  submit.onclick = () => {
    const cur = getCurrentSession()
    if (!cur || !msg.id) return closeFeedbackPopover()
    for (const b of buttons) {
      ;(b as HTMLButtonElement).disabled = true
      b.classList.add("done")
    }
    tip(buttons[0], "已反馈，谢谢")
    closeFeedbackPopover()
    void client
      .submitFeedback({
        messageId: msg.id,
        sessionId: cur.id,
        type,
        ...(selected ? { label: selected } : {}),
        ...(text.value.trim() ? { text: text.value.trim() } : {}),
      })
      .catch(() => {
        // 失败恢复可重试
        for (const b of buttons) {
          ;(b as HTMLButtonElement).disabled = false
          b.classList.remove("done")
        }
        tip(buttons[0], type === "thumbs_up" ? "反馈：回答有用" : "反馈：回答不佳")
      })
  }
  row.append(cancel, submit)
  pop.append(labels, text, row)
  host.appendChild(pop)
  fbPopover = pop
  // 点击弹层外部关闭（延后绑定，避开本次打开按钮的点击冒泡）
  setTimeout(() => document.addEventListener("click", closeFeedbackPopover, { once: true }), 0)
}

/** 消息称谓行操作按钮组：复制（全部消息）/ 撤回（用户与助手消息）/ 重新生成（助手消息）。
 *  悬浮于消息上显示（JS 控制 .show），不占空间、不遮盖内容。noRevoke 抑制撤回按钮（新会话容器内
 *  回放消息/本地收尾说明气泡等非持久化消息，撤回按 id 找不到落点）。 */
function addMetaActions(meta: HTMLElement, wrapper: HTMLElement, bubble: HTMLElement, msg: Pick<Message, "role" | "content" | "id">, opts: { noRevoke?: boolean } = {}) {
  if (meta.querySelector(".msg-actions")) return
  const actions = el("div", "msg-actions")
  const copyBtn = el("button", "msg-act", "")
  tip(copyBtn, "复制")
  copyBtn.innerHTML = ICON_COPY
  copyBtn.onclick = async () => {
    const content = msg.role === "assistant" && msg.content ? msg.content : bubble.textContent?.trim() ?? ""
    if (!content) return
    try {
      await copyText(content)
      copyBtn.classList.add("done")
      setTimeout(() => copyBtn.classList.remove("done"), 1600)
    } catch {
      /* 忽略 */
    }
  }
  actions.appendChild(copyBtn)

  if (msg.role === "assistant" && msg.id) {
    // 质量反馈：👍/👎 打开弹层提交（原因标签+补充说明，写入用户反馈，设置面板「反馈」页可见）；无稳定消息 id 时（异常流）不渲染
    const fbUp = el("button", "msg-act", "")
    tip(fbUp, "反馈：回答有用")
    fbUp.innerHTML = ICON_THUMBS_UP
    fbUp.onclick = () => openFeedbackPopover("thumbs_up", actions, msg, [fbUp, fbDown])
    const fbDown = el("button", "msg-act", "")
    tip(fbDown, "反馈：回答不佳")
    fbDown.innerHTML = ICON_THUMBS_DOWN
    fbDown.onclick = () => openFeedbackPopover("thumbs_down", actions, msg, [fbUp, fbDown])
    actions.append(fbUp, fbDown)
  }

  if ((msg.role === "user" || msg.role === "assistant") && !opts.noRevoke) {
    const isUser = msg.role === "user"
    const revokeBtn = el("button", "msg-act", "")
    tip(revokeBtn, isUser ? "撤回该消息及其后续" : "撤回该回复及其后续")
    revokeBtn.innerHTML = ICON_REVOKE
    revokeBtn.onclick = async () => {
      const cur = getCurrentSession()
      if (!cur || !msg.id) return
      // 运行中任务持有自己的上下文并继续追加消息，中途截断会产生交错历史（服务端同样 409 拒绝）
      if (runs.has(cur.id)) {
        toast("任务运行中，暂不能撤回；请先停止或等待任务完成", "error")
        return
      }
      const text = isUser ? "撤回这条消息及其后续内容？" : "撤回这条助手消息及其后续内容？撤回后可输入新的指导修正。"
      if (!(await confirmDialog({ title: "撤回消息", text }))) return
      try {
        await client.truncateSession(cur.id, msg.id)
        // 确认期间用户可能已切到其他会话：视图/输入框只操作原会话，不打断当前浏览
        if (getCurrentSession()?.id !== cur.id) return
        await loadMessages(cur.id)
        if (isUser && msg.content) {
          // 回滚：将该用户消息内容填充到输入框，便于重新编辑/发送
          input.value = msg.content
          autosize()
        }
        // 助手消息撤回不回填内容：聚焦输入框直接输入修正指导
        focusInput()
      } catch (err) {
        // 撤回失败（消息 id 不匹配/服务端拒绝等）：明确提示，避免静默失败
        toast(`撤回失败: ${(err as Error).message}`, "error")
      }
    }
    actions.appendChild(revokeBtn)
  }

  // 输入（用户消息）按钮组在称谓行左侧；输出（助手/工具消息）在右侧
  if (msg.role === "user") meta.prepend(actions)
  else meta.appendChild(actions)
  const show = () => actions.classList.add("show")
  const hide = () => actions.classList.remove("show")
  wrapper.addEventListener("mouseenter", show)
  wrapper.addEventListener("mouseleave", hide)
  actions.addEventListener("mouseenter", show)
  actions.addEventListener("mouseleave", hide)
}
/** 供 main 组装（流式完成后绑定操作按钮）。 */
export { addMetaActions }

/** 批量渲染容器：loadMessages 批量挂载历史消息用（DocumentFragment 一次 append，避免逐条触发滚动/重排）。 */
let batchFrag: DocumentFragment | null = null
export function beginMsgBatch(): void {
  if (batchFrag) flushMsgBatch() // 并发切换会话时防御：先落盘上一个未完成的批次
  batchFrag = document.createDocumentFragment()
}
export function flushMsgBatch(): void {
  const frag = batchFrag
  batchFrag = null
  if (frag) {
    msgEl.appendChild(frag)
    scrollIfSticky()
  }
}

export function appendMsg(msg: Message, stream = false, parent?: HTMLElement): HTMLElement {
  const wrapper = el("div", `msg ${msg.role}${stream ? " streaming" : ""}`)
  const body = el("div", "msg-body")
  const meta = el("div", "msg-meta")
  meta.append(el("span", "msg-name", ROLE_NAME[msg.role] ?? msg.role), el("span", "msg-time", formatTime(msg.createdAt)))
  body.appendChild(meta)

  // 子Agent 装载提示词消息（role=system + loadedAgent）：渲染为简短装载提示，不占正文（全文在会话记录存档）
  if (msg.role === "system" && msg.loadedAgent) {
    const bubble = el("div", "bubble load-notice")
    bubble.appendChild(el("div", "load-title", `📦 已装载子Agent：${msg.loadedAgent}`))
    body.appendChild(bubble)
    wrapper.append(body)
    if (parent) parent.appendChild(wrapper)
    else if (batchFrag) batchFrag.appendChild(wrapper)
    else msgEl.appendChild(wrapper)
    if (!parent) addMsgNavSeg(wrapper)
    if (!batchFrag && !parent) scrollIfSticky()
    return wrapper
  }

  // 上下文压缩摘要消息（role=system + compacted）：渲染为灰底压缩通知，不占正文
  if (msg.role === "system" && msg.compacted) {
    const bubble = el("div", "bubble compact-notice")
    bubble.appendChild(el("div", "compact-title", "🗜️ 上下文已压缩"))
    if (msg.summary) bubble.appendChild(el("div", "compact-meta", msg.summary))
    bubble.appendChild(el("div", "compact-summary", msg.content))
    body.appendChild(bubble)
    wrapper.append(body)
    if (parent) parent.appendChild(wrapper)
    else if (batchFrag) batchFrag.appendChild(wrapper)
    else msgEl.appendChild(wrapper)
    if (!parent) addMsgNavSeg(wrapper)
    if (!batchFrag && !parent) scrollIfSticky()
    return wrapper
  }

  // 工具消息：实时（无 name，content 为 `→/✓` 标记文本）走 toolBubble；历史（带 name/arguments）走结构化卡片；
  // card.args="block" 工具消息不显示工具卡片，只渲染内容块；无内容块（渲染失败/能力受限）时以输出文本兜底
  const isBlockOnlyMsg = msg.role === "tool" && !!msg.name && isBlockOnly(msg.name)
  let bubble: HTMLElement | null
  if (msg.role === "tool") {
    bubble = isBlockOnlyMsg ? null : toolBubbleFor(msg, msg.content)
    if (isBlockOnlyMsg && msg.content && !msg.blocks?.length) {
      bubble = el("div", "bubble")
      bubble.appendChild(toolOutput(msg.content))
    }
  } else {
    bubble = el("div", "bubble")
  }
  if ((msg.content || msg.reasoning) && msg.role !== "tool") {
    bubble!.appendChild(
      msg.role === "assistant"
        ? msg.reasoning
          ? assistantWithReasoning(msg.reasoning, msg.content)
          : assistantContent(msg.content)
        : blockText(msg.content),
    )
  }
  // assistant 流式消息需要空 bubble 占位
  const hasBody = (msg.role === "assistant" && stream) || (bubble?.childNodes.length ?? 0) > 0 || msg.blocks?.length || msg.attachments?.length
  if (hasBody && bubble) body.appendChild(bubble)

  // 头部行复制按钮（hover 显示，不占气泡空间）；助手消息复制 markdown 源文；
  // 容器内消息（子Agent 执行过程回放，id 为本地生成）不提供撤回
  if (!stream && bubble) addMetaActions(meta, wrapper, bubble, msg, { noRevoke: !!parent })

  const cur = getCurrentSession()
  // 弹窗查看模式下文件工具（card.file）的产物 file 块收敛为文件链接 chip（其余块照常；参数区与输出不受影响）
  if (fileBlocksAsLinks(msg.name)) renderBlocksLinked(body, msg.blocks ?? [], renderBlock, cur?.id || "")
  else renderBlocks(body, msg.blocks ?? [], cur?.id || "")
  for (const a of msg.attachments ?? []) {
    renderBlock(body, { type: a.mime?.startsWith("image/") ? "image" : "file", path: a.path, name: a.name, mime: a.mime }, cur?.id || "")
  }

  wrapper.append(body)
  // 容器内消息（子Agent 执行过程）：容器整体作为一个导航段/滚动单位，内部消息不单独处理；
  // parent 优先于 batchFrag（回放批量挂载时容器内消息直接进容器，容器本身已在 batchFrag 中）
  if (parent) parent.appendChild(wrapper)
  else if (batchFrag) batchFrag.appendChild(wrapper)
  else msgEl.appendChild(wrapper)
  if (!parent) addMsgNavSeg(wrapper)
  if (!batchFrag && !parent) scrollIfSticky()
  return wrapper
}

/** 实时压缩通知气泡（event.message.compact 推送时渲染，不持久化）。 */
export function appendCompactNotice(title: string, summary: string) {
  const wrapper = el("div", "msg system")
  const body = el("div", "msg-body")
  const bubble = el("div", "bubble compact-notice")
  bubble.appendChild(el("div", "compact-title", `🗜️ ${title}`))
  if (summary) bubble.appendChild(el("div", "compact-summary", summary))
  body.appendChild(bubble)
  wrapper.appendChild(body)
  msgEl.appendChild(wrapper)
  addMsgNavSeg(wrapper)
  scrollIfSticky()
}

/** 冲刷节流窗口内未上屏的流式内容（封段前调用）：作废排期并把累积文本/推理同步渲染进气泡，
 *  与 main.ts renderStreamText 同构；不含滚动副作用（封段后紧跟工具卡片追加/容器折叠，滚动无意义）。 */
function flushStreamRender(s: { renderTimer?: ReturnType<typeof setTimeout>; el: HTMLElement | null; reasoningEl: HTMLElement | null; acc: string; reasoningAcc: string }): void {
  clearTimeout(s.renderTimer)
  s.renderTimer = undefined
  if (s.reasoningEl?.isConnected) {
    const rb = s.reasoningEl.querySelector<HTMLElement>(".reasoning-body")
    if (rb) {
      rb.textContent = ""
      if (s.reasoningAcc.trim()) rb.appendChild(markdownBlock(s.reasoningAcc.trim()))
    }
  }
  if (!s.acc || !s.el?.isConnected) return
  const bubble = s.el.querySelector<HTMLElement>(".msg-body .bubble")
  if (!bubble) return
  let textWrap = bubble.querySelector<HTMLElement>(".msg-text")
  if (!textWrap) {
    textWrap = el("div", "msg-text")
    bubble.appendChild(textWrap)
  }
  textWrap.innerHTML = ""
  textWrap.appendChild(assistantContent(s.acc))
}

/** 封存文本段通用逻辑：冲刷未上屏的节流窗口内容、移除流式光标、折叠推理块、清空累积（主循环与新会话容器共用）。 */
function sealTextState(s: { renderTimer?: ReturnType<typeof setTimeout>; el: HTMLElement | null; reasoningEl: HTMLElement | null; acc: string; reasoningAcc: string }): void {
  // 节流排期待触发 = acc/reasoningAcc 存在未上屏增量：先同步冲刷最后一帧再清零，
  // 否则封段前最后一个节流窗口（120ms）内的内容永久丢失——快速模型整轮回复可全部
  // 落在一个窗口内，气泡残留空白（agent_run 执行过程「只见结果不见过程」的主因）
  if (s.renderTimer) flushStreamRender(s)
  // 无条件移除流式光标：工具执行期间即使正文为空（只有推理/无内容）也不应持续闪烁；
  // 工具调用后若继续输出，文本/推理分支会惰性重建 streaming 气泡
  if (s.el?.isConnected) s.el.classList.remove("streaming")
  // 推理结束于工具调用处：先折叠再丢弃引用——正文为空时流结束兜底折叠依赖 el，
  // 而封段后 el 为 null，推理块若不在此折叠将残留展开态
  if (s.reasoningEl?.isConnected && (s.reasoningEl as HTMLDetailsElement).open) (s.reasoningEl as HTMLDetailsElement).open = false
  s.acc = ""
  s.el = null
  s.reasoningAcc = ""
  s.reasoningEl = null
}

/** 封存当前文本段：工具调用开始处截断，后续文本另起一段（content 分开展示）。 */
export function sealSegment(sessionId: string) {
  const run = runs.get(sessionId)
  if (!run) return
  sealTextState(run)
}

/** 封存新会话 run 内当前文本段：新会话内工具调用处截断（与主循环 sealSegment 同构）。 */
export function sealSessionSegment(sub: SessionRunState): void {
  sealTextState(sub)
}

/** 块级工具结果（show/diff 等 card.args="block" 工具）封段：工具结果卡片追加前封存当前文本段
 *  （含新会话执行容器内），使图表等块卡片独立展示、画图后的输出另起新卡片——防输出继续追加到图上方同一张卡片。 */
export function sealBlockResultSegment(sessionId: string, runId?: string): void {
  if (runId) {
    const sub = runs.get(sessionId)?.sessionRuns?.get(runId)
    if (sub) sealSessionSegment(sub) // 容器缺失（切走场景）：无段可封
    return
  }
  sealSegment(sessionId)
}

/** 单行截断摘要（折叠容器标题用）：保留头尾、省略号收尾，单行展示不换行。 */
function inlineSnippet(text: string, max = 60): string {
  const flat = text.replace(/\s+/g, " ").trim()
  if (flat.length <= max) return flat
  return `${flat.slice(0, Math.floor(max * 0.6))}…${flat.slice(-Math.floor(max * 0.3))}`
}

/**
 * 新会话 run 折叠容器（details.session-run）：summary 显示「🚀 新会话 · 预加载 agents · 执行中/→ 返回: …」，
 * 分支运行（branch_run，DESIGN「会话分支运行与合并」）显示「🌿 分支 · name(model)」，
 * body 渲染完整执行过程（输入块/推理/工具卡片/回复）。
 * - 执行中（output 未提供）：默认展开（过程实时可见，main.ts 配合滚动）
 * - 已结束（output 提供，可为空串表示无返回）：默认折叠，只显示最终返回（点 summary 展开看过程，输入以块展示）
 */
export function sessionRunBox(opts: { runId: string; agents: string[]; input: string; output?: string; branch?: { name: string; model?: string } }, parent?: HTMLElement): { container: HTMLDetailsElement; body: HTMLElement; outputEl: HTMLElement } {
  const container = document.createElement("details")
  container.className = "session-run"
  container.dataset.runId = opts.runId
  const summary = el("summary", "session-summary")
  summary.append(
    el(
      "span",
      "session-title",
      opts.branch
        ? `🌿 分支 · ${opts.branch.name}${opts.branch.model ? `（${opts.branch.model}）` : ""}`
        : `🚀 新会话${opts.agents.length ? ` · ${opts.agents.join(" + ")}` : ""}`,
    ),
  )
  const outputEl = el("span", "session-output")
  summary.appendChild(outputEl)
  const body = el("div", "session-body")
  container.append(summary, body)
  // 执行中默认展开（过程实时可见；完成后 finishSessionRun 自动折叠）
  if (opts.output === undefined || opts.output === null) container.open = true
  // 输入以参数块渲染在容器顶部（与工具参数块同款样式，不带「输入」提示）
  if (opts.input) body.appendChild(el("div", "agent-run-input", opts.input))
  finishSessionRun(container, outputEl, opts.output)
  if (parent) parent.appendChild(container)
  else if (batchFrag) batchFrag.appendChild(container)
  else msgEl.appendChild(container)
  addMsgNavSeg(container)
  bindSessionScroll(body)
  if (!batchFrag) scrollIfSticky()
  return { container, body, outputEl }
}

/**
 * 历史回放：从 agent_run 工具调用记录的扩展字段（sessionRun 存档）渲染新会话折叠容器。
 * 递归渲染嵌套执行（新会话内再 agent_run 的存档挂在工具消息上）；parent 指定时嵌套进外层容器 body。
 */
export function renderSessionArchive(archive: SessionRunArchive, parent?: HTMLElement): { container: HTMLDetailsElement; body: HTMLElement; outputEl: HTMLElement } {
  const box = sessionRunBox({ runId: archive.runId, agents: archive.agents, input: archive.input, output: archive.output, branch: archive.branch }, parent)
  for (const am of archive.messages) {
    if (am.sessionRun) {
      renderSessionArchive(am.sessionRun, box.body)
      continue
    }
    appendMsg(
      { id: uuid(), role: am.role, name: am.name, content: am.content, toolCalls: am.toolCalls, arguments: am.arguments, createdAt: Date.now() },
      false,
      box.body,
    )
  }
  return box
}

/**
 * 历史回放（旧版兼容）：agent_call 时代子Agent run 存档（LegacySubAgentRunArchive，agent 单值）渲染折叠容器。
 */
export function renderLegacySubAgentArchive(archive: import("@gebai/sdk").LegacySubAgentRunArchive, parent?: HTMLElement): { container: HTMLDetailsElement; body: HTMLElement; outputEl: HTMLElement } {
  const box = sessionRunBox({ runId: archive.runId, agents: [archive.agent], input: archive.input, output: archive.output }, parent)
  for (const am of archive.messages) {
    if (am.subAgentRun) {
      renderLegacySubAgentArchive(am.subAgentRun, box.body)
      continue
    }
    appendMsg(
      { id: uuid(), role: am.role, name: am.name, content: am.content, toolCalls: am.toolCalls, arguments: am.arguments, createdAt: Date.now() },
      false,
      box.body,
    )
  }
  return box
}

/**
 * 新会话容器内粘底滚动：内容追加后若用户未在容器内上翻则跟随滚动到底（与主聊天区粘底语义一致，
 * 用户上翻历史过程时停止跟随，回到底部后恢复）。核心机制见 sticky-follow.ts（意图驱动）——
 * 容器内流式高频更新同样存在「程序滚动事件迟到 + 内容增长被误判为用户滚动」的失效窗口。 */

/** 容器贴底判定阈值（距底部 < 8px 视为贴底）。 */
const SESSION_STICKY_THRESHOLD = 8

/** 每个容器 body 的跟随核心（sessionRunBox 创建时绑定）。 */
const sessionFollowers = new WeakMap<HTMLElement, StickyFollowHandle>()

export function scrollSessionSticky(body: HTMLElement): void {
  sessionFollowers.get(body)?.contentChanged()
}

export function bindSessionScroll(body: HTMLElement): void {
  if (!sessionFollowers.has(body)) {
    sessionFollowers.set(body, createStickyFollow(body, { threshold: SESSION_STICKY_THRESHOLD, keepFrames: 120 }))
  }
}

/**
 * 新会话 run 收尾：写入最终返回摘要并折叠容器（只显示输入与最终返回，点 summary 展开看过程）。
 * - output 为 undefined/null：run 仍在执行（或未收到收尾），显示「执行中…」不折叠
 * - output 为空串：已结束但无返回（失败/取消/风暴终止），折叠并显示「（无返回）」
 * - output 非空：已结束，折叠并显示返回摘要
 */
export function finishSessionRun(container: HTMLDetailsElement, outputEl: HTMLElement, output: string | undefined | null): void {
  if (output === undefined || output === null) {
    outputEl.textContent = "执行中…"
    container.classList.remove("done")
    return
  }
  const text = inlineSnippet(output.replace(/<think>[\s\S]*?<\/think>/g, "").trim()) || "（无返回）"
  outputEl.textContent = output ? `→ 返回: ${text}` : "（无返回）"
  if (container.open) container.open = false
  container.classList.add("done")
}

/** 推理块：assistant 消息内可折叠暗色「推理」卡片。流式推理过程中默认展开（推理内容实时可见），
 *  推理完成（正文开始/流结束）由 main.ts 自动折叠，用户仍可点 summary 手动展开。 */
export function reasoningBlock(): HTMLElement {
  const details = document.createElement("details")
  details.className = "reasoning"
  details.open = true
  const summary = el("summary", undefined, "推理")
  summary.insertAdjacentHTML("afterbegin", ICON_REASONING)
  details.append(summary, el("div", "reasoning-body"))
  return details
}

/** 渲染带独立推理字段的 assistant 消息：推理折叠卡（默认收起，内容 markdown 渲染）+ 纯正文 markdown。 */
export function assistantWithReasoning(reasoning: string, content: string): HTMLElement {
  const wrap = el("div")
  const think = document.createElement("details")
  think.className = "reasoning"
  const summary = el("summary", undefined, "推理")
  summary.insertAdjacentHTML("afterbegin", ICON_REASONING)
  const rb = el("div", "reasoning-body")
  rb.appendChild(markdownBlock(reasoning))
  think.append(summary, rb)
  wrap.appendChild(think)
  if (content) wrap.appendChild(markdownBlock(content))
  return wrap
}

/** 渲染 assistant 正文：`<think>…</think>` 片段抽为暗色「推理」卡片（默认收起，内容 markdown 渲染），其余按 markdown 渲染。
 *  新版数据推理在独立字段（Message.reasoning，走 assistantWithReasoning），本函数仅服务流式正文与旧版 content 内嵌 think 块。 */
export function assistantContent(content: string): HTMLElement {
  const re = /<think>([\s\S]*?)<\/think>/g
  if (!re.test(content)) return markdownBlock(content)
  re.lastIndex = 0
  const wrap = el("div")
  let last = 0
  let m: RegExpExecArray | null
  while ((m = re.exec(content))) {
    if (m.index > last) wrap.appendChild(markdownBlock(content.slice(last, m.index)))
    const think = document.createElement("details")
    think.className = "reasoning"
    const summary = el("summary", undefined, "推理")
    summary.insertAdjacentHTML("afterbegin", ICON_REASONING)
    const rb = el("div", "reasoning-body")
    rb.appendChild(markdownBlock(m[1].trim()))
    think.append(summary, rb)
    wrap.appendChild(think)
    last = m.index + m[0].length
  }
  if (last < content.length) wrap.appendChild(markdownBlock(content.slice(last)))
  return wrap
}

/** 工具结果追加：同一卡片内更新头部为完成态，并追加输出区块。sessionId 绑定配对 key（跨会话隔离）；
 *  runId 区分主循环与新会话容器内调用，parent 指定容器内消息的渲染目标。 */
export function appendToolResult(sessionId: string, toolCallId: string, name: string, output: string, blocks?: ContentBlock[], runId?: string, parent?: HTMLElement) {
  const entry = pendingTools.get(pendingToolsKey(sessionId, toolCallId, runId))
  if (entry) {
    if (entry.kind === "todo") {
      // todo 工具：占位卡替换为最新待办清单
      const bubble = entry.wrapper?.querySelector(".bubble")
      if (bubble) bubble.replaceWith(todoBubble(todoState.get(entry.session) ?? []))
      pendingTools.delete(pendingToolsKey(sessionId, toolCallId, runId))
      return
    }
    if (entry.kind === "ask_choice") {
      // ask 选项询问分支：结果到达时消息流落问答记录卡（等待期交互由审批容器选择卡片承载，消息流不重复预览问题）
      appendAskUserRecord(entry.askArgs ?? { prompt: "", options: [] }, output, runId ? parent : undefined)
      pendingTools.delete(pendingToolsKey(sessionId, toolCallId, runId))
      return
    }
    if (entry.kind === "ask_plan") {
      // ask 计划审批分支：等待期计划全文只在审批容器选择卡内展示（消息流不重复预览计划卡）；
      // 结果到达时落计划卡并更新头部为审批结果态、追加结果文本（与历史回放 toolCard 同构）
      let wrapper = entry.wrapper
      if (!wrapper) wrapper = appendPlanCard(entry.planArgs ?? { title: "" }, runId ? parent : undefined)
      const head = wrapper.querySelector(".tool-head")
      if (head) head.textContent = planResultHead(output)
      const bubble = wrapper.querySelector(".bubble")
      if (bubble && output) bubble.appendChild(choiceAnswerBlock(output))
      pendingTools.delete(pendingToolsKey(sessionId, toolCallId, runId))
      return
    }
    const head = entry.wrapper?.querySelector(".tool-head")
    if (head) {
      // 完成态：重建结构化头部（✓ 图标，保留标题后缀——titleParams 声明的参数不因完成而丢失）
      let argsObj: Record<string, unknown> | null = null
      if (entry.argsText) {
        try {
          argsObj = JSON.parse(entry.argsText) as Record<string, unknown>
        } catch {
          /* 非 JSON 参数：无标题后缀 */
        }
      }
      head.replaceWith(toolHead("done", name, argsObj))
    }
    const bubble = entry.wrapper?.querySelector(".bubble")
    // 超长参数收敛：执行/审批等待期完整直显，结果到达（完成态）收敛为折叠块（与历史回放同构；
    // agent_run/branch_run 专用参数块无 tool-args 标记，天然跳过）
    const argsEl = bubble?.querySelector<HTMLElement>(".tool-args")
    if (argsEl && entry.argsText) argsEl.replaceWith(renderToolArgsDone(name, entry.argsText) ?? argsEl)
    if (bubble && output) {
      // agent_run：最终返回为 markdown 输出，直接渲染（与历史 toolCard 一致）
      if (shortToolName(name) === "agent_run") bubble.appendChild(markdownBlock(output))
      else bubble.appendChild(toolOutput(output))
    }
    // 弹窗查看模式下文件工具产物 file 块收敛为文件链接 chip（其余块照常；参数区与输出不受影响）
    if (blocks?.length && entry.body) {
      if (fileBlocksAsLinks(name)) renderBlocksLinked(entry.body, blocks, renderBlock, entry.session)
      else renderBlocks(entry.body, blocks, entry.session)
    }
    pendingTools.delete(pendingToolsKey(sessionId, toolCallId, runId))
    return
  }
  // 无配对调用（如历史重载）：新建独立结果消息（容器内调用渲染到容器）
  appendMsg({ id: uuid(), role: "tool", content: `✓ ${displayToolName(name)}${output ? `\n${output}` : ""}`, blocks, createdAt: Date.now() }, false, parent)
}

/** ask 问答记录卡（结果到达 appendToolResult / 历史回放 toolCard 渲染）：问题 + 选项展示态 + 回答结果，
 *  像内容块一样在消息流开启新输出卡片；等待作答期间消息流不渲染问题预览（交互作答由审批容器选择卡片承载，
 *  与展示卡上下堆叠会被视为重复卡片）；parent 指定容器内渲染目标（子Agent 过程）。 */
export function appendAskUserRecord(args: { prompt: string; options: Array<string | Record<string, unknown>>; multi?: boolean }, output: string, parent?: HTMLElement): HTMLElement {
  const wrapper = el("div", "msg tool")
  const body = el("div", "msg-body")
  const meta = el("div", "msg-meta")
  meta.append(el("span", "msg-name", "工具"), el("span", "msg-time", formatTime(Date.now())))
  body.appendChild(meta)
  body.appendChild(askUserBubble(args.prompt, args.options, args.multi === true, output))
  wrapper.appendChild(body)
  if (parent) parent.appendChild(wrapper)
  else {
    msgEl.appendChild(wrapper)
    addMsgNavSeg(wrapper)
    scrollIfSticky()
  }
  return wrapper
}

/** ask 计划卡片（消息流内展示计划全文，像选项询问分支一样开启新输出卡片；展示态，
 *  交互作答由审批容器选择卡片承载；结果到达后由 appendToolResult 更新头部并追加结果）。 */
export function appendPlanCard(args: { title?: unknown; steps?: unknown; content?: unknown }, parent?: HTMLElement): HTMLElement {
  const wrapper = el("div", "msg tool")
  const body = el("div", "msg-body")
  const meta = el("div", "msg-meta")
  meta.append(el("span", "msg-name", "工具"), el("span", "msg-time", formatTime(Date.now())))
  body.appendChild(meta)
  body.appendChild(
    planBubble(String(args.title ?? ""), Array.isArray(args.steps) ? (args.steps as unknown[]).map(String) : [], args.content != null ? String(args.content) : undefined),
  )
  wrapper.appendChild(body)
  if (parent) parent.appendChild(wrapper)
  else {
    msgEl.appendChild(wrapper)
    addMsgNavSeg(wrapper)
    scrollIfSticky()
  }
  return wrapper
}

/** 待办卡片消息（todo 工具调用占位，结果到达后刷新为清单）。parent 指定容器内渲染目标（子Agent 过程）。 */
export function appendTodoCard(sessionId: string, parent?: HTMLElement): HTMLElement {
  const wrapper = el("div", "msg tool")
  const body = el("div", "msg-body")
  const meta = el("div", "msg-meta")
  meta.append(el("span", "msg-name", "工具"), el("span", "msg-time", formatTime(Date.now())))
  body.appendChild(meta)
  const bubble = todoBubble(todoState.get(sessionId) ?? [])
  const head = bubble.querySelector(".tool-head")
  if (head) head.textContent = "📋 待办清单 · 更新中…"
  body.appendChild(bubble)
  addMetaActions(meta, wrapper, bubble, { role: "tool", content: "", id: "" })
  wrapper.appendChild(body)
  if (parent) parent.appendChild(wrapper)
  else {
    msgEl.appendChild(wrapper)
    addMsgNavSeg(wrapper)
    scrollIfSticky()
  }
  return wrapper
}

/** 选择卡片（event.choice.request 实时渲染，绑定 choiceId 提交决策；支持复杂选项与多选）。
 * 渲染到审批容器（独立于消息流）：切走隐藏、切回恢复，不随消息重载丢失；任务结束随审批一并清理。
 * 同一 choiceId 重复推送（断线重连事件重放）时替换旧卡，避免选择卡片重复堆叠。
 * plan（计划审批分支）：卡内顶部内嵌计划全文（限高滚动）——审批时直接可见，
 * 不依赖消息流的计划展示卡位置（用户可能正上翻阅读历史）与刷新后的历史重载。 */
export function renderChoiceCard(
  prompt: string,
  options: Array<string | Record<string, unknown>>,
  choiceId: string,
  sessionId: string,
  multi = false,
  plan?: { title: string; content: string; path: string },
) {
  for (const old of approvalsEl.querySelectorAll<HTMLElement>(".interaction-card")) {
    if (old.dataset.reqId === choiceId) old.remove()
  }
  const wrapper = el("div", "msg tool interaction-card")
  // 计划内嵌卡跟随内容区宽度（CSS .has-plan：.msg 的 width:fit-content 会按最短计划行收缩成窄条）
  const showPlan = !!(plan && (plan.content || plan.title))
  if (showPlan) wrapper.classList.add("has-plan")
  wrapper.dataset.session = sessionId
  wrapper.dataset.kind = "choice"
  wrapper.dataset.reqId = choiceId
  const body = el("div", "msg-body")
  const meta = el("div", "msg-meta")
  meta.append(el("span", "msg-name", "工具"), el("span", "msg-time", formatTime(Date.now())))
  body.appendChild(meta)
  if (showPlan) {
    // 计划全文内嵌：复用消息流计划卡的渲染（标题 + Markdown 正文），限高滚动防长计划撑爆审批容器
    const planWrap = el("div", "choice-plan")
    planWrap.appendChild(planBubble(plan!.title, [], plan!.content))
    body.appendChild(planWrap)
  }
  const bubble = choiceBubble(prompt, options, choiceId, sessionId, multi)
  body.appendChild(bubble)
  wrapper.appendChild(body)
  approvalsEl.appendChild(wrapper)
  applyInteractionVisibility()
  // 计划审批（服务端 plan 工具提示词前缀「请审核计划」）：计划全文已在卡内可见；消息流底部的
  // 展示卡可能仍在视口外（用户上翻阅读历史）——滚动到底把审批卡带进视野再作决策
  if (prompt.startsWith("请审核计划") && getCurrentSession()?.id === sessionId) lockToBottom()
}

/** 环境变量请求卡片（event.env.request 实时渲染，绑定 envId 提交用户填值）。渲染到审批容器（同选择卡片，
 *  同一 envId 重复推送替换旧卡防堆叠）。 */
export function renderEnvRequestCard(name: string, description: string, secret: boolean, envId: string, sessionId: string) {
  for (const old of approvalsEl.querySelectorAll<HTMLElement>(".interaction-card")) {
    if (old.dataset.reqId === envId) old.remove()
  }
  const wrapper = el("div", "msg tool interaction-card")
  wrapper.dataset.session = sessionId
  wrapper.dataset.kind = "env"
  wrapper.dataset.reqId = envId
  const body = el("div", "msg-body")
  const meta = el("div", "msg-meta")
  meta.append(el("span", "msg-name", "工具"), el("span", "msg-time", formatTime(Date.now())))
  body.appendChild(meta)
  const bubble = envRequestBubble(name, description, secret, envId, sessionId)
  body.appendChild(bubble)
  wrapper.appendChild(body)
  approvalsEl.appendChild(wrapper)
  applyInteractionVisibility()
}

/** 交互卡片（选择/环境变量填值）随会话显示：仅当前会话的卡片可见，其余隐藏（切回恢复）。 */
export function applyInteractionVisibility(): void {
  const cur = getCurrentSession()
  for (const card of approvalsEl.querySelectorAll<HTMLElement>(".interaction-card")) {
    card.hidden = !cur || card.dataset.session !== cur.id
  }
}

/** 任务结束清理：移除该会话的交互卡片（选择/环境变量请求随任务终止失效）。 */
export function clearInteractionCards(sessionId: string): void {
  for (const card of approvalsEl.querySelectorAll<HTMLElement>(".interaction-card")) {
    if (card.dataset.session === sessionId) card.remove()
  }
}

/** 工具输出：短内容放带框代码块，长内容折叠。与参数区块同框样式。 */
// 循环依赖兜底：addMetaActions 需要 loadMessages（sessions 模块），延迟注入
import type { LoadMessagesFn } from "./sessions"
let loadMessages: LoadMessagesFn = async () => {}
export function bindMessagesSessions(fn: LoadMessagesFn): void {
  loadMessages = fn
}
