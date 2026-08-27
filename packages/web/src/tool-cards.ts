import type { Message, TodoItem, ToolInfo } from "@gebai/sdk"
import { client, composer, el, getCurrentSession, getSubAgentNames, input, todoState } from "./state"
import { codeBlock, highlightedCode, markdownBlock } from "./markdown"
import { loadLocalEnv, saveLocalEnv } from "./env-local"
import { isFilePopup } from "./file-display"

/* ---------- 工具名解析：`{agent}_{tool}` → 子Agent 名 + 短工具名 ---------- */

/** 拆分带命名空间前缀的工具全名，返回 `{ agent?, tool }`（子Agent 最长前缀匹配）。 */
export function splitToolName(name: string): { agent?: string; tool: string } {
  for (const a of getSubAgentNames()) {
    if (name.startsWith(`${a}_`)) return { agent: a, tool: name.slice(a.length + 1) }
  }
  return { tool: name }
}

/** 显示用工具名：`code · write`；全局工具原样 `write`。 */
export function displayToolName(name: string): string {
  const { agent, tool } = splitToolName(name)
  return agent ? `${agent} · ${tool}` : tool
}

/** 短工具名（不带 agent 前缀），用于行为判定（todo 系列 / ask 分支 / 脚本高亮）。 */
export function shortToolName(name: string): string {
  return splitToolName(name).tool
}

/** agent_run 工具判定：执行新会话卡片（头部列出全部预加载子Agent 名，参数区只显示输入提示词）。 */
export function isAgentRun(name: string): boolean {
  return shortToolName(name) === "agent_run"
}

/* ---------- 工具卡片渲染（toolBubble / toolCard / 待办 / 选择） ---------- */

/** 解析工具调用卡片文本 `→ name {args}`，返回 null 表示普通内容。 */
function parseToolCall(content: string): { name: string; args: string } | null {
  const m = content.match(/^→\s*([^\s]+)\s*(\{[\s\S]*\})?$/)
  if (!m) return null
  return { name: m[1], args: (m[2] ?? "").trim() }
}

/** 长文本折叠块：超长输出默认收起，点击展开查看。 */
function toolOutputBlock(text: string): HTMLElement {
  const details = document.createElement("details")
  details.className = "tool-out"
  const summary = el("summary", undefined, `查看输出（${text.length} 字符）`)
  const pre = el("pre")
  const code = el("code")
  code.textContent = text
  pre.appendChild(code)
  details.append(summary, pre)
  return details
}

/** 是否需要折叠：超长或超多行的输出。 */
function needsCollapse(text: string): boolean {
  return text.length > 220 || text.split("\n").length > 6
}

/** 工具输出：短内容直接展示，长内容自动折叠（点击展开完整内容）。 */
export function toolOutput(text: string): HTMLElement {
  if (needsCollapse(text)) return toolOutputBlock(text)
  const pre = el("pre")
  pre.className = "tool-code"
  const code = el("code")
  code.textContent = text
  pre.appendChild(code)
  return pre
}

/** 工具卡片展示元数据缓存（服务端 Tool.card 注册声明，前端按声明渲染，不硬编码工具名）。 */
const toolCardMeta = new Map<string, NonNullable<ToolInfo["card"]>>()

/** 测试注入：直接填充元数据缓存（bun test 环境无 WS，绕过 listTools）。 */
export function __setToolCardMetaForTest(entries: Array<[string, NonNullable<ToolInfo["card"]>]>): void {
  toolCardMeta.clear()
  for (const [k, v] of entries) toolCardMeta.set(k, v)
}

/** 拉取当前会话可用工具元数据（启动时调用一次；失败静默按默认渲染）。 */
export async function loadToolCardMeta(): Promise<void> {
  try {
    const tools = await client.listTools()
    toolCardMeta.clear()
    for (const t of tools) if (t.card) toolCardMeta.set(t.name, t.card)
  } catch {
    /* 工具清单不可用：按默认渲染 */
  }
}

/** 按工具全名/短名查展示元数据（子 Agent 工具带 {agent}_ 前缀，全名查不到时退短名）。 */
function metaOf(name: string): NonNullable<ToolInfo["card"]> | undefined {
  return toolCardMeta.get(name) ?? toolCardMeta.get(shortToolName(name))
}

/** 结果直出内容块型工具（card.args="block" 声明）：调用不显示通用卡片，结果直接渲染内容块（show/diff）。 */
export function isBlockOnly(name: string): boolean {
  return metaOf(name)?.args === "block"
}

/** 文件卡声明判定（card.file）：read/write/edit/patch 等文件工具（code 子Agent 同款包装自动继承）。 */
function isFileCardTool(name: string, meta?: NonNullable<ToolInfo["card"]>): boolean {
  return !!(meta ?? metaOf(name))?.file
}

/** 弹窗查看模式下文件工具产物块改为文件链接：参数区与输出不受影响，仅下方产物文件卡（file 块）
 *  在「嵌入内容卡 ↔ 链接弹窗」间切换（DESIGN「文件展示方式」）。 */
export function fileBlocksAsLinks(name: string | undefined): boolean {
  return !!name && isFilePopup() && isFileCardTool(name)
}

/* ---------- 卡片头部（图标 + 工具名 + 标题参数后缀，结构化灵活展示） ---------- */

/** 标题后缀信息：text 展示文本（含前导 `·`）；wrap 允许多行完整展示（agent_run 专用）；
 *  full 为未截断全文（悬浮 title 用，仅与 text 不同时携带）。 */
interface TitleSuffixInfo {
  text: string
  wrap?: boolean
  full?: string
}

/** 标题参数上限：后缀单值超过该长度智能截断（路径型保留尾部、URL 保留头部、其余保留首尾），悬浮见全文。 */
const TITLE_SUFFIX_MAX = 48

/** 标题参数单值智能截断：路径型（含分隔符且非 URL）保留尾部（尾部目录段辨识度最高）、URL 保留头部（域名），
 *  其余保留首尾（中间省略）。原文由卡片头悬浮 title 提供。 */
function clipTitleValue(v: string): string {
  if (v.length <= TITLE_SUFFIX_MAX) return v
  const keep = TITLE_SUFFIX_MAX - 1
  if (/^https?:\/\//i.test(v)) return `${v.slice(0, keep)}…`
  if (/[/\\]/.test(v)) return `…${v.slice(-keep)}`
  const head = Math.ceil(keep * 0.6)
  return `${v.slice(0, head)}…${v.slice(-(keep - head))}`
}

/** 标题参数拼接：titleParams 声明的参数值拼入标题——单参数仅显示值（`· src/main.ts`，省略 `key=` 前缀），
 *  多参数 `key=value`（`·` 连接）。超长单值智能截断（悬浮见全文），标题参数始终入头部（不再降级参数气泡）。 */
function titleSuffix(meta: NonNullable<ToolInfo["card"]> | undefined, args: Record<string, unknown> | null): TitleSuffixInfo | null {
  if (!meta?.titleParams?.length || !args) return null
  const present: Array<{ k: string; raw: string }> = []
  for (const k of meta.titleParams) {
    const v = args[k]
    if (v === undefined || v === null || v === "") continue
    present.push({ k, raw: String(v) })
  }
  if (!present.length) return null
  // 单值裸显按「实际存在」的参数数决定：声明多个但本次只传一个（如 project 参数未传的文件工具）时仅显示值
  const single = present.length === 1
  const parts = present.map((p) => (single ? clipTitleValue(p.raw) : `${p.k}=${clipTitleValue(p.raw)}`))
  const fullParts = present.map((p) => (single ? p.raw : `${p.k}=${p.raw}`))
  const text = `· ${parts.join(" · ")}`
  const full = `· ${fullParts.join(" · ")}`
  return { text, full: full === text ? undefined : full }
}

/** 标题后缀统一入口：agent_run 专用（头部直接列出全部预加载子Agent 名，`+` 连接、不截断、允许多行）；其余按 titleParams 声明。 */
function titleSuffixInfo(name: string, args: Record<string, unknown> | null): TitleSuffixInfo | null {
  if (isAgentRun(name)) {
    const agents = Array.isArray(args?.agents) ? args.agents.map(String).filter(Boolean) : []
    return agents.length ? { text: `· ${agents.join(" + ")}`, wrap: true } : null
  }
  return titleSuffix(metaOf(name), args)
}

/** 工具卡片头部：图标（🛠 调用中 / ✓ 完成）+ 工具名 + 标题参数后缀（超长智能截断，悬浮见全文）。
 *  实时调用、完成态更新与历史重载共用，保证三态一致。 */
export function toolHead(state: "call" | "done", name: string, args: Record<string, unknown> | null): HTMLElement {
  const head = el("div", "tool-head")
  head.append(el("span", "tool-ico", state === "done" ? "✓" : "🛠"), el("span", "tool-name", displayToolName(name)))
  const sfx = titleSuffixInfo(name, args)
  if (sfx) {
    const span = el("span", sfx.wrap ? "tool-suffix wrap" : "tool-suffix", sfx.text)
    if (sfx.full) span.title = sfx.full
    head.appendChild(span)
  }
  return head
}

/** agent_run 参数区：输入提示词以块展示（与普通工具参数块同款样式，预加载子Agent 名已在卡片头部列出）。 */
function agentRunArgsBlock(args: string): HTMLElement | null {
  let obj: Record<string, unknown> | null = null
  try {
    obj = JSON.parse(args) as Record<string, unknown>
  } catch {
    return null
  }
  if (!obj || typeof obj.input !== "string" || !obj.input.trim()) return null
  return el("div", "agent-run-input", obj.input)
}

/* ---------- 参数区（键值行 / JSON 高亮 / 代码块，超长自动折叠） ---------- */

/** 参数区超长折叠阈值（字符数）：超出后默认收起为「查看参数」折叠块（与输出折叠同款交互）。 */
const ARGS_FOLD_CHARS = 800

/** 参数值标量判定：标量走键值行，嵌套结构回退 JSON 高亮。 */
function isScalar(v: unknown): boolean {
  return v === null || typeof v === "string" || typeof v === "number" || typeof v === "boolean"
}

/** 键值行参数块：扁平参数的紧凑可读展示（嵌套值以紧凑 JSON 单行展示，空串显示为 ""）。 */
function kvArgsBlock(obj: Record<string, unknown>): HTMLElement {
  const wrap = el("div", "tool-kv")
  for (const [k, v] of Object.entries(obj)) {
    const row = el("div", "tool-kv-row")
    row.appendChild(el("span", "tool-kv-key", k))
    row.appendChild(el("div", "tool-kv-val", isScalar(v) ? (v === "" ? '""' : String(v)) : JSON.stringify(v)))
    wrap.appendChild(row)
  }
  return wrap
}

/** 超长参数内容包装为折叠块（默认收起，点击展开完整参数）。 */
function foldArgsBlock(inner: HTMLElement, chars: number): HTMLElement {
  if (chars <= ARGS_FOLD_CHARS) return inner
  const details = document.createElement("details")
  details.className = "tool-fold"
  details.append(el("summary", undefined, `查看参数（${chars} 字符）`), inner)
  return details
}

/** edits 参数项判定：{ oldString, newString } 字符串对。 */
function isEditPair(v: unknown): v is { oldString: string; newString: string } {
  return !!v && typeof v === "object" && typeof (v as { oldString?: unknown }).oldString === "string" && typeof (v as { newString?: unknown }).newString === "string"
}

/** edits 参数块：每处修改渲染为旧（红）/ 新（绿）对比块（多处编号），比 JSON 数组直观；空串侧省略（纯新增/纯删除）。 */
function editsArgsBlock(list: Array<{ oldString: string; newString: string }>): HTMLElement {
  const wrap = el("div", "tool-edits")
  list.forEach((e, i) => {
    if (list.length > 1) wrap.appendChild(el("div", "tool-edit-idx", `修改 ${i + 1}/${list.length}`))
    // replaceAll 标记：该项替换全部匹配（审批时可见替换范围）
    if ((e as { replaceAll?: unknown }).replaceAll === true) wrap.appendChild(el("div", "tool-edit-idx", "replaceAll（替换全部匹配）"))
    if (e.oldString) wrap.appendChild(el("pre", "tool-edit-old", e.oldString))
    if (e.newString) wrap.appendChild(el("pre", "tool-edit-new", e.newString))
  })
  return wrap
}

/** code/edits 模式共用：其余参数附注（codeField 与已入标题的参数不重复；超长未入标题的标题参数降级为键值行气泡）。
 *  扁平标量以键值行展示。返回 null 表示无其余参数。 */
function restArgsNote(obj: Record<string, unknown>, meta: NonNullable<ToolInfo["card"]>, titleInHead: boolean): HTMLElement | null {
  const rest: Record<string, unknown> = {}
  for (const [k, v] of Object.entries(obj)) if (k !== meta.codeField && !(titleInHead && meta.titleParams?.includes(k))) rest[k] = v
  if (!Object.keys(rest).length) return null
  return Object.values(rest).every(isScalar) ? kvArgsBlock(rest) : el("div", "tool-rest", JSON.stringify(rest, null, 2))
}

/** 参数区渲染：按服务端 card 声明——"none" 不展示；"code" 渲染 codeField 为代码块；"edits" 渲染 codeField 数组为旧/新对比块
 *  （其余参数键值行/JSON 附注；edit 工具无声明时按参数形态内建兜底同样渲染对比块）；"kv" 强制键值行；"json" 强制完整 JSON 高亮（不省略标题参数）；
 *  缺省自适应（扁平标量→键值行，嵌套→JSON 高亮）。
 *  标题参数（titleParams）已入卡片标题时参数区不再重复（显式 "json" 声明除外）；超长参数自动折叠。返回 null 表示无参数区。
 *  文件展示方式（嵌入/弹窗）不影响参数区与输出——只作用于下方产物文件卡（见 fileBlocksAsLinks）。 */
function toolArgsBlock(name: string, args: string, meta?: NonNullable<ToolInfo["card"]>): HTMLElement | null {
  let obj: Record<string, unknown> | null = null
  try {
    obj = JSON.parse(args) as Record<string, unknown>
  } catch {
    /* 非 JSON，按纯文本展示 */
  }
  if (meta?.args === "none") return null
  if (obj && !Object.keys(obj).length) return null
  // 标题参数是否已入头部：是则参数区省略该键；否则（超长降级）以参数气泡形式在参数区展示全文
  const titleInHead = obj ? titleSuffixInfo(name, obj) !== null : false
  if (meta?.args === "code" && obj && meta.codeField) {
    const codeText = obj[meta.codeField]
    if (typeof codeText === "string" && codeText.trim()) {
      const wrap = el("div")
      wrap.appendChild(codeBlock(meta.codeLang ?? "", codeText))
      const note = restArgsNote(obj, meta, titleInHead)
      if (note) wrap.appendChild(note)
      return foldArgsBlock(wrap, codeText.length)
    }
  }
  // edits 形态内建兜底：edit 工具 card 声明不可用（工具清单拉取失败/旧服务端未声明）时
  // 按参数形态识别渲染对比块，长修改同样先渲染后折叠，不回退直显 JSON
  const editsField = meta?.args === "edits" ? meta.codeField : !meta && shortToolName(name) === "edit" ? "edits" : undefined
  if (obj && editsField) {
    const list = obj[editsField]
    if (Array.isArray(list) && list.length && list.every(isEditPair)) {
      const wrap = el("div")
      wrap.appendChild(editsArgsBlock(list))
      if (meta) {
        const note = restArgsNote(obj, meta, titleInHead)
        if (note) wrap.appendChild(note)
      } else if (Object.keys(obj).some((k) => k !== editsField)) {
        // 兜底无声明：其余参数（path 等）键值行展示
        wrap.appendChild(kvArgsBlock(Object.fromEntries(Object.entries(obj).filter(([k]) => k !== editsField))))
      }
      const chars = list.reduce((n, e) => n + e.oldString.length + e.newString.length, 0)
      return foldArgsBlock(wrap, chars)
    }
    /* 形态不符（非 edits 数组）：回退自适应渲染 */
  }
  // 已入标题的参数不在参数区重复（显式 "json" 声明除外——强制完整 JSON 保真展示）
  let shown = obj
  if (shown && meta?.args !== "json" && meta?.titleParams?.length && titleInHead) {
    shown = Object.fromEntries(Object.entries(shown).filter(([k]) => !meta.titleParams!.includes(k)))
  }
  if (shown && !Object.keys(shown).length) return null
  // 键值行：显式 "kv" 声明，或缺省自适应（扁平标量参数）
  if (shown && (meta?.args === "kv" || (meta?.args !== "json" && Object.values(shown).every(isScalar)))) {
    return foldArgsBlock(kvArgsBlock(shown), JSON.stringify(shown).length)
  }
  // JSON 语法高亮：嵌套结构 / 显式 "json" / 非 JSON 纯文本
  const text = shown ? JSON.stringify(shown, null, 2) : args
  const pre = el("pre")
  pre.className = "tool-code"
  pre.appendChild(highlightedCode("json", text))
  return foldArgsBlock(pre, text.length)
}

function toolBubble(content: string): HTMLElement {
  const bubble = el("div", "bubble")
  // 工具结果卡片：`✓ name\n<输出>`
  const result = content.match(/^✓\s*([^\n]+)\n?([\s\S]*)$/)
  if (result) {
    const head = el("div", "tool-head")
    head.append(el("span", "tool-ico", "✓"), el("span", "tool-name", result[1].trim()))
    bubble.appendChild(head)
    const out = result[2].trim()
    if (out) bubble.appendChild(toolOutput(out))
    return bubble
  }
  // 工具调用卡片：`→ name {args}`（单卡片三区块：工具名 / 参数 / 输出）
  const parsed = parseToolCall(content)
  if (parsed) {
    let argsObj: Record<string, unknown> | null = null
    if (parsed.args) {
      try {
        argsObj = JSON.parse(parsed.args) as Record<string, unknown>
      } catch {
        /* 非 JSON：无标题后缀 */
      }
    }
    bubble.appendChild(toolHead("call", parsed.name, argsObj))
    if (parsed.args) {
      const ab = isAgentRun(parsed.name) ? agentRunArgsBlock(parsed.args) : toolArgsBlock(parsed.name, parsed.args, metaOf(parsed.name))
      if (ab) bubble.appendChild(ab)
    }
    return bubble
  }
  // 历史纯文本工具消息（服务端持久化的输出）
  if (content) bubble.appendChild(toolOutput(content))
  return bubble
}

/** 待办折叠：清单超过阈值时隐藏较早的已完成项（保留最近 KEEP 项完成作上下文），未完成项始终可见。 */
const TODO_COLLAPSE_THRESHOLD = 8
const TODO_KEEP_COMPLETED = 3

/** 待办行：状态图标 + 标题 + 元信息。 */
function todoRow(t: TodoItem): HTMLElement {
  const row = el("div", "todo-item")
  const ico = t.status === "completed" ? "✅" : t.status === "in_progress" ? "🔄" : t.status === "failed" ? "❌" : t.status === "cancelled" ? "🚫" : "⬜"
  row.appendChild(el("span", "todo-ico", ico))
  const body = el("div", "todo-body")
  body.appendChild(el("div", "todo-title", t.title))
  const meta = [t.status, t.priority, t.progress != null ? `${t.progress}%` : undefined, t.note].filter(Boolean).join(" · ")
  if (meta) body.appendChild(el("div", "todo-meta", meta))
  row.appendChild(body)
  return row
}

/** 折叠切换行（隐藏段收敛为一行按钮，点击展开/收起）。 */
function todoToggleRow(text: string, onClick: () => void): HTMLButtonElement {
  const btn = el("button", "todo-collapse", text)
  btn.onclick = onClick
  return btn
}

/** 待办卡片：清单形式（状态图标 + 标题 + 元信息）。清单过长时折叠较早的已完成项，点击折叠行展开、展开后可收起。 */
export function todoBubble(todos: TodoItem[]): HTMLElement {
  const bubble = el("div", "bubble")
  bubble.appendChild(el("div", "tool-head", "📋 待办清单"))
  const list = el("div", "todo-list")
  bubble.appendChild(list)
  let expanded = false
  const render = () => {
    list.textContent = ""
    if (!todos.length) {
      list.appendChild(el("div", "block-text", "（暂无待办）"))
      return
    }
    const completedIdx: number[] = []
    todos.forEach((t, i) => {
      if (t.status === "completed") completedIdx.push(i)
    })
    const collapsible = todos.length > TODO_COLLAPSE_THRESHOLD && completedIdx.length > TODO_KEEP_COMPLETED
    const hidden = new Set<number>()
    if (collapsible && !expanded) {
      for (const i of completedIdx.slice(0, completedIdx.length - TODO_KEEP_COMPLETED)) hidden.add(i)
    }
    let run = 0
    todos.forEach((t, i) => {
      if (hidden.has(i)) {
        run++
        if (!hidden.has(i + 1)) {
          list.appendChild(todoToggleRow(`⋯ 已折叠 ${run} 项已完成 · 点击展开`, () => {
            expanded = true
            render()
          }))
          run = 0
        }
        return
      }
      list.appendChild(todoRow(t))
    })
    if (expanded && collapsible) {
      list.appendChild(todoToggleRow("收起已完成待办", () => {
        expanded = false
        render()
      }))
    }
  }
  render()
  return bubble
}

/** 规范化选项：纯文本 → { title }；复杂选项 { title, description } 原样（提交值取 title）。 */
function normalizeChoiceOpts(options: Array<string | Record<string, unknown>>): Array<{ title: string; description?: string }> {
  return options.map((o) => {
    if (o && typeof o === "object") {
      const title = String((o as { title?: unknown }).title ?? "")
      if (title) {
        const d = (o as { description?: unknown }).description
        return { title, description: d != null ? String(d) : undefined }
      }
    }
    return { title: String(o ?? "") }
  })
}

/**
 * 选择卡片（实时：提交选择决策并等待引擎继续；历史：提交作为消息发送）。
 * 支持点选选项（multi=true 多选）、复杂选项（标题+说明）、自定义文本输入、拒绝回答。
 */
export function choiceBubble(
  prompt: string,
  options: Array<string | Record<string, unknown>>,
  choiceId?: string,
  sessionId?: string,
  multi = false,
): HTMLElement {
  const bubble = el("div", "bubble")
  const head = el("div", "tool-head", multi ? "🧭 请选择（可多选）" : "🧭 请选择")
  bubble.appendChild(head)
  if (prompt) bubble.appendChild(el("div", "block-text", prompt))
  const opts = el("div", "choice-opts")
  const live = !!choiceId && !!sessionId
  // 实时模式首次成功提交后锁定整卡（防重复决策排队），历史模式保持可交互
  let settled = false
  const submit = async (selection: string | string[] | null, btn: HTMLButtonElement) => {
    btn.disabled = true
    if (live) {
      // 实时：提交选择决策，引擎的 ask 选项询问分支随即返回并继续；提交成功卡片立即关闭
      try {
        await client.decideChoice(sessionId!, choiceId!, selection)
        settled = true
        bubble.closest<HTMLElement>(".interaction-card")?.remove()
        return
      } catch {
        btn.disabled = false
        btn.textContent = "提交失败，请重试"
        return
      }
    }
    // 历史消息（无 choiceId）：作为用户消息发送
    input.value = selection == null ? "我拒绝回答" : Array.isArray(selection) ? `我选择：${selection.join("、")}` : `我选择：${selection}`
    composer.requestSubmit()
  }
  let confirmBtn: HTMLButtonElement | undefined
  if (multi) {
    // 多选确认按钮：勾选后才可提交
    confirmBtn = el("button", "choice-confirm", "确认选择")
    confirmBtn.disabled = true
    confirmBtn.onclick = () => submit([...selected], confirmBtn!)
  }
  const selected = new Set<string>()
  const toggleOpt = (btn: HTMLButtonElement, title: string) => {
    if (settled) return
    if (selected.has(title)) {
      selected.delete(title)
      btn.classList.remove("selected")
      btn.setAttribute("aria-pressed", "false")
    } else {
      selected.add(title)
      btn.classList.add("selected")
      btn.setAttribute("aria-pressed", "true")
    }
    if (confirmBtn) {
      confirmBtn.disabled = selected.size === 0
      confirmBtn.textContent = selected.size ? `确认选择（${selected.size}）` : "确认选择"
    }
  }
  for (const o of normalizeChoiceOpts(options)) {
    const title = o.title
    if (o.description) {
      // 复杂选项：标题 + 说明
      const btn = el("button", "choice-opt-row")
      btn.setAttribute("aria-pressed", "false")
      btn.append(el("span", "choice-opt-title", title), el("span", "choice-opt-desc", o.description))
      btn.onclick = () => (multi ? toggleOpt(btn, title) : submit(title, btn))
      opts.appendChild(btn)
    } else {
      const btn = el("button", "choice-opt", title)
      btn.setAttribute("aria-pressed", "false")
      btn.onclick = () => (multi ? toggleOpt(btn, title) : submit(title, btn))
      opts.appendChild(btn)
    }
  }
  bubble.appendChild(opts)
  if (confirmBtn) bubble.appendChild(confirmBtn)
  // 自定义文本输入（直接输入自己的答案，不限于给定选项；多选时追加到已勾选项一并提交）
  const customRow = el("div", "choice-custom")
  const field = el("input", "choice-input")
  field.placeholder = "输入自定义答案…"
  field.enterKeyHint = "send"
  const sendBtn = el("button", "choice-opt", "提交")
  sendBtn.onclick = () => {
    const v = field.value.trim()
    if (!v) return
    submit(multi ? [...selected, v] : v, sendBtn)
  }
  field.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault()
      sendBtn.click()
    }
  })
  customRow.appendChild(field)
  customRow.appendChild(sendBtn)
  bubble.appendChild(customRow)
  // 拒绝回答（不再询问，模型自行决策）
  const refuseBtn = el("button", "choice-refuse", "拒绝回答")
  refuseBtn.onclick = () => submit(null, refuseBtn)
  bubble.appendChild(refuseBtn)
  return bubble
}

/** 结构化工具卡片：工具名 + 参数（可选）+ 输出。实时与历史会话渲染一致。 */
export function toolCard(msg: Message): HTMLElement {
  const short = msg.name ? shortToolName(msg.name) : ""
  // todo 工具：渲染待办清单卡片（替代通用工具卡片）
  if (short === "todo") {
    const cur = getCurrentSession()
    return todoBubble(todoState.get(cur?.id ?? "") ?? [])
  }
  // ask 选项询问分支：带结果（content）渲染问答记录卡
  // （问题 + 选项展示态 + 回答），无结果的裸调用（异常中断）回退可交互选择卡兜底
  if (short === "ask" && msg.arguments?.options != null) {
    const args = msg.arguments ?? {}
    const prompt = String(args.prompt ?? "")
    const options = Array.isArray(args.options) ? (args.options as Array<string | Record<string, unknown>>) : []
    if (msg.content) return askUserBubble(prompt, options, args.multi === true, msg.content)
    return choiceBubble(prompt, options, undefined, undefined, args.multi === true)
  }
  // ask 计划审批分支：渲染计划卡片（标题 + 计划 Markdown 全文，展示态；
  // 历史消息带审批结果时头部更新为结果态并附结果文本）
  if (short === "ask" && msg.arguments?.title != null) {
    const args = msg.arguments ?? {}
    const title = String(args.title ?? "")
    const steps = Array.isArray(args.steps) ? args.steps.map(String) : []
    const content = args.content != null ? String(args.content) : undefined
    const bubble = planBubble(title, steps, content)
    if (msg.content) {
      const head = bubble.querySelector(".tool-head")
      if (head) head.textContent = planResultHead(msg.content)
      bubble.appendChild(el("div", "choice-answer", msg.content))
    }
    return bubble
  }
  const bubble = el("div", "bubble")
  const meta = metaOf(msg.name ?? "")
  bubble.appendChild(toolHead("call", msg.name ?? "tool", msg.arguments ?? null))
  if (msg.arguments && Object.keys(msg.arguments).length) {
    const ab = isAgentRun(msg.name ?? "") ? agentRunArgsBlock(JSON.stringify(msg.arguments, null, 2)) : toolArgsBlock(msg.name ?? "", JSON.stringify(msg.arguments, null, 2), meta)
    if (ab) bubble.appendChild(ab)
  }
  if (msg.content) {
    // agent_run 工具（携带 sessionRun 存档；旧版 agent_call 的 subAgentRun 兼容）：最终返回为 markdown 输出，直接渲染（与助手消息同构）
    if (msg.sessionRun || msg.subAgentRun) {
      bubble.appendChild(markdownBlock(msg.content))
    } else {
      bubble.appendChild(toolOutput(msg.content))
    }
  }
  return bubble
}

/** 实时工具消息渲染入口：历史（带 name/arguments）走结构化卡片，实时（→/✓ 标记文本）走 toolBubble。 */
export function toolBubbleFor(msg: Message, content: string): HTMLElement {
  return msg.name ? toolCard(msg) : toolBubble(content)
}

/** ask 消息流问答卡片（展示态，不可交互）：问题 + 选项静态展示（禁用态按钮），
 *  带 answer（结果输出）时为问答记录卡：头部按结果文案更新并追加回答块；
 *  等待作答的交互由审批容器的选择卡片承载，消息流不再重复渲染问题预览。 */
/** 从问答结果输出解析用户选中的选项值（「用户选择：A、B」→ [A,B]；拒绝/超时/自定义文本不命中选项则空集）。 */
function parseChoicePicked(answer: string, multi: boolean): string[] {
  if (!answer.startsWith("用户选择：")) return []
  const rest = answer.slice("用户选择：".length)
  return multi ? rest.split("、") : [rest]
}

export function askUserBubble(prompt: string, options: Array<string | Record<string, unknown>>, multi: boolean, answer?: string): HTMLElement {
  const bubble = el("div", "bubble")
  const head = el("div", "tool-head", multi ? "🧭 请选择（可多选）" : "🧭 请选择")
  bubble.appendChild(head)
  if (prompt) bubble.appendChild(el("div", "block-text", prompt))
  const opts = el("div", "choice-opts")
  // 用户选中的选项高亮（selected 与交互卡同款选中态；自定义文本不命中选项则无高亮，回答块仍示原文）
  const picked = parseChoicePicked(answer ?? "", multi)
  for (const o of normalizeChoiceOpts(options)) {
    const btn = el("button", "choice-opt", o.title)
    btn.disabled = true
    btn.title = "历史问答记录"
    if (picked.includes(o.title)) btn.classList.add("selected")
    opts.appendChild(btn)
  }
  bubble.appendChild(opts)
  if (answer) {
    head.textContent = askUserResultHead(answer)
    bubble.appendChild(choiceAnswerBlock(answer))
  }
  return bubble
}

/** ask 问答卡结果头部状态（按输出前缀识别，与服务端输出文案一致）。 */
export function askUserResultHead(output: string): string {
  if (output.startsWith("用户选择")) return "✓ 用户回答"
  if (output.startsWith("用户拒绝")) return "✕ 用户拒绝"
  if (output.startsWith("用户未在时限内")) return "⏱ 选择超时"
  return "✓ 用户回答"
}

/** ask 问答卡回答结果块（完成态追加，与问题展示区分）。 */
export function choiceAnswerBlock(output: string): HTMLElement {
  return el("div", "choice-answer", output)
}

/** 组装计划展示 Markdown：与服务端 plan 工具同一规则（content 优先，否则 title + steps 勾选清单，双端同构）。 */
export function buildPlanMarkdown(title: string, steps: string[], content?: string): string {
  const body = content && content.trim() ? content.trim() : ["# " + title, "", "## 执行计划", "", ...steps.map((s) => `- [ ] ${s}`)].join("\n")
  return body
}

/** plan 工具消息流计划卡片（展示态，不可交互）：计划标题 + 计划 Markdown 全文；
 *  交互作答由审批容器选择卡片承载（批准/拒绝/修改意见）。 */
export function planBubble(title: string, steps: string[], content?: string): HTMLElement {
  const bubble = el("div", "bubble")
  bubble.appendChild(el("div", "tool-head", `📋 计划 · ${title}`))
  bubble.appendChild(markdownBlock(buildPlanMarkdown(title, steps, content)))
  return bubble
}

/** plan 工具结果头部状态（按输出前缀识别审批结果，与服务端 plan 工具输出文案一致）。 */
export function planResultHead(output: string): string {
  if (output.startsWith("计划已批准")) return "✓ 计划已批准"
  if (output.startsWith("计划已拒绝")) return "✕ 计划已拒绝"
  if (output.startsWith("用户拒绝审核")) return "✕ 计划已取消"
  if (output.startsWith("计划审批超时")) return "⏱ 计划审批超时"
  return "✓ 计划已处理"
}

/** 环境变量请求卡片（event.env.request 实时渲染）：展示变量名与说明，用户填值提交后保存到浏览器本地并回传引擎（ask 填值分支阻塞等待）。 */
export function envRequestBubble(name: string, description: string, secret: boolean, envId: string, sessionId: string): HTMLElement {
  const bubble = el("div", "bubble")
  bubble.appendChild(el("div", "tool-head", "🔑 环境变量请求"))
  bubble.appendChild(el("div", "env-req-name", name))
  if (description) bubble.appendChild(el("div", "block-text", description))
  const field = el("div", "env-req-field")
  const input = document.createElement("input")
  input.type = secret ? "password" : "text"
  input.placeholder = secret ? "输入值（密钥，掩码显示）" : "输入值"
  input.autocomplete = "off"
  input.spellcheck = false
  field.appendChild(input)
  const row = el("div", "env-req-actions")
  const confirm = el("button", "mini-btn", "确定")
  const cancel = el("button", "mini-btn", "取消")
  row.append(confirm, cancel)
  bubble.append(field, row)
  const settle = (label: string) => {
    input.disabled = true
    confirm.disabled = true
    cancel.disabled = true
    confirm.textContent = label
  }
  confirm.onclick = async () => {
    const value = input.value.trim()
    if (!value) return
    input.disabled = true
    confirm.disabled = true
    cancel.disabled = true
    confirm.textContent = "提交中…"
    // 保存到浏览器本地（后续任务自动生效）+ 回传引擎注入本次任务；提交成功卡片立即关闭
    saveLocalEnv({ ...loadLocalEnv(), [name]: value })
    try {
      await client.decideEnv(sessionId, envId, value)
      bubble.closest<HTMLElement>(".interaction-card")?.remove()
    } catch {
      input.disabled = false
      confirm.disabled = false
      cancel.disabled = false
      confirm.textContent = "提交失败，请重试"
    }
  }
  cancel.onclick = () => {
    settle("已拒绝")
    void client.decideEnv(sessionId, envId, null).catch(() => {})
    bubble.closest<HTMLElement>(".interaction-card")?.remove()
  }
  return bubble
}
