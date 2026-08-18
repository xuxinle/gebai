import { uuid } from "./uuid"
import type { ContentBlock, Message, SessionRunArchive } from "@gebai/sdk"
import {
  ROLE_NAME,
  client,
  el,
  filesContent,
  filesDownload,
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
import { blockText, highlightedCode, markdownBlock } from "./markdown"
import { askUserBubble, choiceBubble, displayToolName, envRequestBubble, isBlockOnly, planBubble, planResultHead, shortToolName, todoBubble, toolBubbleFor, toolHead, toolOutput } from "./tool-cards"
import { openImageViewer, renderDiagram } from "./diagram"
import { renderDiffBlock } from "./diff"
import { renderHtmlBlock } from "./html-view"
import { lockToBottom, scrollIfSticky } from "./jump-bottom"
import { addMsgNavSeg } from "./msg-nav"
import { autosize } from "./composer"
import { confirmDialog, copyText, tip, toast } from "./ui"

/* ---------- 按文件名/扩展名推断语法高亮语言 ---------- */

const EXT_LANG: Record<string, string> = {
  ts: "typescript", tsx: "typescript", js: "javascript", jsx: "javascript", mjs: "javascript",
  json: "json", py: "python", sh: "bash", bash: "bash", zsh: "bash", css: "css", html: "xml", htm: "xml",
  xml: "xml", svg: "xml", md: "markdown", markdown: "markdown", yml: "markdown", yaml: "markdown",
}

function langForFile(name: string, mime: string): string {
  const ext = name.split(".").pop()?.toLowerCase() ?? ""
  if (!ext && mime.startsWith("text/")) return ""
  return EXT_LANG[ext] ?? ""
}

/** 文件在线预览：图片直接展示，文本/代码语法高亮，其他提示下载。 */
function openFilePreview(sessionId: string, b: Extract<ContentBlock, { type: "file" }>) {
  const overlay = el("div", "preview-overlay")
  const card = el("div", "preview-card")
  const head = el("div", "preview-head")
  const closeBtn = el("button", "preview-close", "✕")
  closeBtn.onclick = closePreview
  head.append(el("span", "preview-title", b.name || b.path), closeBtn)
  const body = el("div", "preview-body")
  card.append(head, body)
  overlay.appendChild(card)
  document.body.appendChild(overlay)

  function closePreview() {
    overlay.remove()
    document.removeEventListener("keydown", onKey)
  }
  const onKey = (e: KeyboardEvent) => {
    if (e.key === "Escape") closePreview()
  }
  document.addEventListener("keydown", onKey)
  overlay.onclick = (e) => {
    if (e.target === overlay) closePreview()
  }

  const mime = b.mime ?? ""
  if (mime.startsWith("image/")) {
    const img = document.createElement("img")
    img.src = filesContent(sessionId, b.path)
    img.alt = b.name || "preview"
    body.appendChild(img)
    return
  }
  void fetch(filesContent(sessionId, b.path))
    .then(async (r) => {
      if (!r.ok) throw new Error(`HTTP ${r.status}`)
      const text = await r.text()
      const pre = el("pre")
      pre.className = "preview-code"
      pre.appendChild(highlightedCode(langForFile(b.name || b.path, mime), text))
      body.appendChild(pre)
    })
    .catch((err) => {
      body.appendChild(blockText(`无法预览该文件: ${(err as Error).message}。可点击「下载」查看。`))
    })
}

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
    case "code": {
      const pre = el("pre", "code")
      if (b.language) {
        const lab = el("div", "code-label", b.language)
        pre.appendChild(lab)
      }
      const code = el("code")
      code.textContent = b.text
      pre.appendChild(code)
      container.appendChild(pre)
      break
    }
    case "image": {
      const img = document.createElement("img")
      const src = filesContent(sessionId, b.path)
      img.src = src
      img.alt = b.name || "image"
      img.className = "block-img"
      // 点击进入全屏查看器（缩放/平移/复制/下载）：draw 工具 render=backend 产出的 PNG 与普通图片块同样可全屏查看
      img.onclick = () => openImageViewer(src, b.name || "image")
      tip(img, "点击查看大图")
      // 图片异步加载改变高度：粘底则跟随滚动到底（jump-bottom.ts 委托捕获阶段 load 统一处理，含 markdown 内嵌图片）
      container.appendChild(img)
      break
    }
    case "file": {
      const box = el("div", "file-box")
      const a = document.createElement("a")
      a.className = "file-link"
      a.href = filesDownload(sessionId, b.path)
      a.textContent = `📎 ${b.name || b.path}`
      a.onclick = (e) => {
        e.preventDefault()
        openFilePreview(sessionId, b)
      }
      const dl = document.createElement("a")
      dl.className = "file-dl"
      dl.href = filesDownload(sessionId, b.path)
      dl.download = b.name || "file"
      dl.textContent = "下载"
      box.append(a, dl)
      container.appendChild(box)
      break
    }
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

/** 提交质量反馈（👍/👎）：写入用户反馈存储，设置面板「反馈」页可见。成功后禁用防重复提交。 */
function submitFeedback(type: "thumbs_up" | "thumbs_down", btn: HTMLElement, msg: Pick<Message, "id">) {
  const cur = getCurrentSession()
  if (!cur || !msg.id) return
  btn.classList.add("done")
  tip(btn, "已反馈，谢谢")
  ;(btn as HTMLButtonElement).disabled = true
  void client
    .submitFeedback({ messageId: msg.id, sessionId: cur.id, type })
    .catch(() => {
      // 失败恢复可重试
      btn.classList.remove("done")
      tip(btn, type === "thumbs_up" ? "反馈：回答有用" : "反馈：回答不佳")
      ;(btn as HTMLButtonElement).disabled = false
    })
}

/** 消息称谓行操作按钮组：复制（全部消息）/ 撤回（用户消息）/ 重新生成（助手消息）。
 *  悬浮于消息上显示（JS 控制 .show），不占空间、不遮盖内容。 */
function addMetaActions(meta: HTMLElement, wrapper: HTMLElement, bubble: HTMLElement, msg: Pick<Message, "role" | "content" | "id">) {
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
    // 质量反馈：👍/👎 提交到用户反馈（设置面板「反馈」页可见）；无稳定消息 id 时（异常流）不渲染
    const fbUp = el("button", "msg-act", "")
    tip(fbUp, "反馈：回答有用")
    fbUp.innerHTML = ICON_THUMBS_UP
    fbUp.onclick = () => submitFeedback("thumbs_up", fbUp, msg)
    const fbDown = el("button", "msg-act", "")
    tip(fbDown, "反馈：回答不佳")
    fbDown.innerHTML = ICON_THUMBS_DOWN
    fbDown.onclick = () => submitFeedback("thumbs_down", fbDown, msg)
    actions.append(fbUp, fbDown)
  }

  if (msg.role === "user") {
    const revokeBtn = el("button", "msg-act", "")
    tip(revokeBtn, "撤回该消息及其后续")
    revokeBtn.innerHTML = ICON_REVOKE
    revokeBtn.onclick = async () => {
      const cur = getCurrentSession()
      if (!cur || !msg.id) return
      if (!(await confirmDialog({ title: "撤回消息", text: "撤回这条消息及其后续内容？" }))) return
      try {
        await client.truncateSession(cur.id, msg.id)
        // 确认期间用户可能已切到其他会话：视图/输入框只操作原会话，不打断当前浏览
        if (getCurrentSession()?.id !== cur.id) return
        await loadMessages(cur.id)
        // 回滚：将该用户消息内容填充到输入框，便于重新编辑/发送
        if (msg.content) {
          input.value = msg.content
          autosize()
          focusInput()
        }
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

/** 低性能模式 content-visibility 视口外高度估算（px）：meta 行 + 内容行数（约 40 字符/行、
 *  23px 行高）+ 块卡片/附件固定高度（图表/HTML 卡片可达数百 px）。纯估算只为滚动条高度
 *  接近真实——统一按 320px 估算时长回答（真实高度可达数千 px）滚动经过首渲染会顶开视口
 *  「跳一大段」；渲染后由 contain-intrinsic-size 的 auto 关键字记忆真实高度（回看稳定）。 */
function estimateIntrinsicHeight(msg: Message): number {
  const chars = msg.content.length + (msg.reasoning?.length ?? 0) + (msg.arguments ? JSON.stringify(msg.arguments).length : 0)
  const extra = (msg.blocks?.length ?? 0) + (msg.attachments?.length ?? 0)
  return Math.min(24_000, Math.max(96, 56 + Math.ceil(chars / 40) * 23 + extra * 360))
}

export function appendMsg(msg: Message, stream = false, parent?: HTMLElement): HTMLElement {
  const wrapper = el("div", `msg ${msg.role}${stream ? " streaming" : ""}`)
  // 低性能模式高度估算内联（样式表 320px 为兜底；非低性能模式无 content-visibility，属性无效不生效）
  wrapper.style.containIntrinsicSize = `auto ${estimateIntrinsicHeight(msg)}px`
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

  // 头部行复制按钮（hover 显示，不占气泡空间）；助手消息复制 markdown 源文
  if (!stream && bubble) addMetaActions(meta, wrapper, bubble, msg)

  const cur = getCurrentSession()
  renderBlocks(body, msg.blocks ?? [], cur?.id || "")
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

/** 块级工具结果（draw/diff/render_html 等 card.args="block" 工具）封段：工具结果卡片追加前封存当前文本段
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
 * body 渲染完整执行过程（输入块/推理/工具卡片/回复）。
 * - 执行中（output 未提供）：默认展开（过程实时可见，main.ts 配合滚动）
 * - 已结束（output 提供，可为空串表示无返回）：默认折叠，只显示最终返回（点 summary 展开看过程，输入以块展示）
 */
export function sessionRunBox(opts: { runId: string; agents: string[]; input: string; output?: string }, parent?: HTMLElement): { container: HTMLDetailsElement; body: HTMLElement; outputEl: HTMLElement } {
  const container = document.createElement("details")
  container.className = "session-run"
  container.dataset.runId = opts.runId
  const summary = el("summary", "session-summary")
  summary.append(el("span", "session-title", `🚀 新会话${opts.agents.length ? ` · ${opts.agents.join(" + ")}` : ""}`))
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
  const box = sessionRunBox({ runId: archive.runId, agents: archive.agents, input: archive.input, output: archive.output }, parent)
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
 * 用户上翻历史过程时停止跟随，回到底部后恢复）。
 * 状态挂在 body.dataset.sticky（"1"=跟随，"0"=用户已上翻）。
 * 与 jump-bottom 同构的程序滚动迟到事件防护：scroll 事件异步送达，期间内容可能已增长，
 * 事件报告的位置虽是我们刚设置的落位、却已不在当前底部——若按用户滚动处理会把跟随误翻为 "0"，
 * 导致容器内流式跟随悄悄失效。按 clamp 后的实际落位精确比对识别（scrollTop = scrollHeight
 * 会被浏览器钳制，必须读回 scrollTop 记录），命中则恢复跟随并续滚到最新底部。 */
const sessionProgTarget = new WeakMap<HTMLElement, number>()

export function scrollSessionSticky(body: HTMLElement): void {
  if (body.dataset.sticky === "0") return
  body.scrollTop = body.scrollHeight
  sessionProgTarget.set(body, body.scrollTop)
}

/** 新会话容器滚动监听：用户上翻（未贴底）时停止跟随，滚回底部时恢复跟随。 */
export function bindSessionScroll(body: HTMLElement): void {
  body.dataset.sticky = "1"
  body.addEventListener("scroll", () => {
    const target = sessionProgTarget.get(body)
    if (target !== undefined && body.scrollTop === target) {
      // 程序滚动产生的滚动事件：恢复跟随；若内容在事件送达前已增长（目标过期、
      // 位置已不在当前底部），继续滚动到最新底部。
      sessionProgTarget.delete(body)
      body.dataset.sticky = "1"
      if (body.scrollHeight - body.scrollTop - body.clientHeight >= 8) body.scrollTop = body.scrollHeight
      return
    }
    if (target !== undefined) sessionProgTarget.delete(body)
    // 用户滚动（滚轮 / 触控板 / 键盘 / 滚动条拖动）：贴底自动恢复跟随，脱离底部立即解除
    const nearBottom = body.scrollHeight - body.scrollTop - body.clientHeight < 8
    body.dataset.sticky = nearBottom ? "1" : "0"
  })
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
      const bubble = entry.wrapper.querySelector(".bubble")
      if (bubble) bubble.replaceWith(todoBubble(todoState.get(entry.session) ?? []))
      pendingTools.delete(pendingToolsKey(sessionId, toolCallId, runId))
      return
    }
    if (entry.kind === "ask_user") {
      // ask_user：问答卡片更新头部为完成态并追加回答（交互作答由审批容器选择卡片承载）
      const head = entry.wrapper.querySelector(".tool-head")
      if (head) head.textContent = "✓ 用户回答"
      const bubble = entry.wrapper.querySelector(".bubble")
      if (bubble && output) bubble.appendChild(choiceAnswerBlock(output))
      pendingTools.delete(pendingToolsKey(sessionId, toolCallId, runId))
      return
    }
    if (entry.kind === "plan") {
      // plan：计划卡片更新头部为审批结果并追加结果文本（审批由审批容器选择卡片承载）
      const head = entry.wrapper.querySelector(".tool-head")
      if (head) head.textContent = planResultHead(output)
      const bubble = entry.wrapper.querySelector(".bubble")
      if (bubble && output) bubble.appendChild(choiceAnswerBlock(output))
      pendingTools.delete(pendingToolsKey(sessionId, toolCallId, runId))
      return
    }
    const head = entry.wrapper.querySelector(".tool-head")
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
    const bubble = entry.wrapper.querySelector(".bubble")
    if (bubble && output) {
      // agent_run：最终返回为 markdown 输出，直接渲染（与历史 toolCard 一致）
      if (shortToolName(name) === "agent_run") bubble.appendChild(markdownBlock(output))
      else bubble.appendChild(toolOutput(output))
    }
    if (blocks?.length) renderBlocks(entry.body, blocks, entry.session)
    pendingTools.delete(pendingToolsKey(sessionId, toolCallId, runId))
    return
  }
  // 无配对调用（如历史重载）：新建独立结果消息（容器内调用渲染到容器）
  appendMsg({ id: uuid(), role: "tool", content: `✓ ${displayToolName(name)}${output ? `\n${output}` : ""}`, blocks, createdAt: Date.now() }, false, parent)
}

/** ask_user 回答文本块（问答卡片完成态追加，与问题展示区分）。 */
function choiceAnswerBlock(output: string): HTMLElement {
  return el("div", "choice-answer", output)
}

/** ask_user 问答输出卡片（消息流内展示问答交换，像 draw 内容块一样开启新输出卡片；
 *  展示态不可交互，作答由审批容器选择卡片承载；结果到达后由 appendToolResult 更新）。 */
export function appendAskUserCard(prompt: string, options: Array<string | Record<string, unknown>>, multi: boolean, parent?: HTMLElement): HTMLElement {
  const wrapper = el("div", "msg tool")
  const body = el("div", "msg-body")
  const meta = el("div", "msg-meta")
  meta.append(el("span", "msg-name", "工具"), el("span", "msg-time", formatTime(Date.now())))
  body.appendChild(meta)
  body.appendChild(askUserBubble(prompt, options, multi))
  wrapper.appendChild(body)
  if (parent) parent.appendChild(wrapper)
  else {
    msgEl.appendChild(wrapper)
    addMsgNavSeg(wrapper)
    scrollIfSticky()
  }
  return wrapper
}

/** plan 计划卡片（消息流内展示计划全文，像 ask_user 一样开启新输出卡片；展示态，
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
 * 渲染到审批容器（独立于消息流）：切走隐藏、切回恢复，不随消息重载丢失；任务结束随审批一并清理。 */
export function renderChoiceCard(
  prompt: string,
  options: Array<string | Record<string, unknown>>,
  choiceId: string,
  sessionId: string,
  multi = false,
) {
  const wrapper = el("div", "msg tool interaction-card")
  wrapper.dataset.session = sessionId
  wrapper.dataset.kind = "choice"
  const body = el("div", "msg-body")
  const meta = el("div", "msg-meta")
  meta.append(el("span", "msg-name", "工具"), el("span", "msg-time", formatTime(Date.now())))
  body.appendChild(meta)
  const bubble = choiceBubble(prompt, options, choiceId, sessionId, multi)
  body.appendChild(bubble)
  wrapper.appendChild(body)
  approvalsEl.appendChild(wrapper)
  applyInteractionVisibility()
  // 计划审批（服务端 plan 工具提示词前缀「请审核计划」）：计划全文卡片在消息流底部，
  // 用户可能正上翻阅读历史——滚动到底把计划展示出来再作审批决策（选择卡片本身在
  // 消息流下方常驻可见，缺的是消息流里的计划内容）
  if (prompt.startsWith("请审核计划") && getCurrentSession()?.id === sessionId) lockToBottom()
}

/** 环境变量请求卡片（event.env.request 实时渲染，绑定 envId 提交用户填值）。渲染到审批容器（同选择卡片）。 */
export function renderEnvRequestCard(name: string, description: string, secret: boolean, envId: string, sessionId: string) {
  const wrapper = el("div", "msg tool interaction-card")
  wrapper.dataset.session = sessionId
  wrapper.dataset.kind = "env"
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
