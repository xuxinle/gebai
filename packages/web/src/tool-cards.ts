import type { Message, TodoItem, ToolInfo } from "@gebai/sdk"
import { client, composer, el, getCurrentSession, getSubAgentNames, input, todoState } from "./state"
import { codeBlock, highlightedCode, markdownBlock } from "./markdown"
import { loadLocalEnv, saveLocalEnv } from "./env-local"

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

/** 短工具名（不带 agent 前缀），用于行为判定（todo 系列 / ask_user / 脚本高亮）。 */
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

/** 结果直出内容块型工具（card.args="block" 声明）：调用不显示通用卡片，结果直接渲染内容块（draw/diff/render_html）。 */
export function isBlockOnly(name: string): boolean {
  return metaOf(name)?.args === "block"
}

/** 标题参数拼接：titleParams 声明的参数值直接拼入卡片标题（`· key=value`，长值截断）。 */
function titleSuffix(meta: NonNullable<ToolInfo["card"]> | undefined, args: Record<string, unknown> | null): string {
  if (!meta?.titleParams?.length || !args) return ""
  const parts: string[] = []
  for (const k of meta.titleParams) {
    const v = args[k]
    if (v === undefined || v === null || v === "") continue
    const s = String(v)
    parts.push(`${k}=${s.length > 24 ? `${s.slice(0, 24)}…` : s}`)
  }
  return parts.length ? ` · ${parts.join(" · ")}` : ""
}

/** 按工具注册声明的标题后缀（`· key=value`）：实时卡片创建与完成态复用（与历史 toolCard 一致）。
 *  agent_run 专用：头部直接列出全部预加载子Agent 名（`· code + playwright`，不截断、省略 `key=` 前缀）。 */
export function toolTitleSuffix(name: string, args: Record<string, unknown> | null): string {
  if (isAgentRun(name)) {
    const agents = Array.isArray(args?.agents) ? args.agents.map(String).filter(Boolean) : []
    return agents.length ? ` · ${agents.join(" + ")}` : ""
  }
  return titleSuffix(metaOf(name), args)
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

/** 参数区渲染：按服务端 card 声明——"none" 不展示；"code" 渲染 codeField 为代码块；默认 JSON 高亮。返回 null 表示无参数区。 */
function toolArgsBlock(args: string, meta?: NonNullable<ToolInfo["card"]>): HTMLElement | null {
  let obj: Record<string, unknown> | null = null
  try {
    obj = JSON.parse(args) as Record<string, unknown>
  } catch {
    /* 非 JSON，按纯文本展示 */
  }
  if (meta?.args === "none") return null
  if (meta?.args === "code" && obj && meta.codeField) {
    const codeText = obj[meta.codeField]
    if (typeof codeText === "string" && codeText.trim()) {
      const wrap = el("div")
      wrap.appendChild(codeBlock(meta.codeLang ?? "", codeText))
      // 其余参数（如 write 的 path）以 JSON 附注展示
      const rest: Record<string, unknown> = {}
      for (const [k, v] of Object.entries(obj)) if (k !== meta.codeField) rest[k] = v
      if (Object.keys(rest).length) {
        const note = el("div", "tool-rest", JSON.stringify(rest, null, 2))
        wrap.appendChild(note)
      }
      return wrap
    }
  }
  // 默认：参数 JSON 语法高亮
  const pre = el("pre")
  pre.className = "tool-code"
  pre.appendChild(highlightedCode("json", args))
  return pre
}

function toolBubble(content: string): HTMLElement {
  const bubble = el("div", "bubble")
  // 工具结果卡片：`✓ name\n<输出>`
  const result = content.match(/^✓\s*([^\n]+)\n?([\s\S]*)$/)
  if (result) {
    const head = el("div", "tool-head", `✓ ${result[1]}`)
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
    const head = el("div", "tool-head", `🛠 ${displayToolName(parsed.name)}${toolTitleSuffix(parsed.name, argsObj)}`)
    bubble.appendChild(head)
    if (parsed.args) {
      const ab = isAgentRun(parsed.name) ? agentRunArgsBlock(parsed.args) : toolArgsBlock(parsed.args, metaOf(parsed.name))
      if (ab) bubble.appendChild(ab)
    }
    return bubble
  }
  // 历史纯文本工具消息（服务端持久化的输出）
  if (content) bubble.appendChild(toolOutput(content))
  return bubble
}

/** 待办卡片：清单形式（状态图标 + 标题 + 元信息）。 */
export function todoBubble(todos: TodoItem[]): HTMLElement {
  const bubble = el("div", "bubble")
  const head = el("div", "tool-head", "📋 待办清单")
  bubble.appendChild(head)
  if (!todos.length) bubble.appendChild(el("div", "block-text", "（暂无待办）"))
  for (const t of todos) {
    const row = el("div", "todo-item")
    const ico = t.status === "completed" ? "✅" : t.status === "in_progress" ? "🔄" : t.status === "failed" ? "❌" : t.status === "cancelled" ? "🚫" : "⬜"
    row.appendChild(el("span", "todo-ico", ico))
    const body = el("div", "todo-body")
    body.appendChild(el("div", "todo-title", t.title))
    const meta = [t.status, t.priority, t.progress != null ? `${t.progress}%` : undefined, t.note].filter(Boolean).join(" · ")
    if (meta) body.appendChild(el("div", "todo-meta", meta))
    row.appendChild(body)
    bubble.appendChild(row)
  }
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
      // 实时：提交选择决策，引擎的 ask_user 工具随即返回并继续；提交成功卡片立即关闭
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
  // ask_user 工具：渲染选择卡片（prompt + 选项按钮，支持复杂选项与多选）
  if (short === "ask_user") {
    const args = msg.arguments ?? {}
    const prompt = String(args.prompt ?? "")
    const options = Array.isArray(args.options) ? (args.options as Array<string | Record<string, unknown>>) : []
    return choiceBubble(prompt, options, undefined, undefined, args.multi === true)
  }
  const bubble = el("div", "bubble")
  const meta = metaOf(msg.name ?? "")
  const head = el("div", "tool-head", `🛠 ${msg.name ? displayToolName(msg.name) : "tool"}${toolTitleSuffix(msg.name ?? "", msg.arguments ?? null)}`)
  bubble.appendChild(head)
  if (msg.arguments && Object.keys(msg.arguments).length) {
    const ab = isAgentRun(msg.name ?? "") ? agentRunArgsBlock(JSON.stringify(msg.arguments, null, 2)) : toolArgsBlock(JSON.stringify(msg.arguments, null, 2), meta)
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

/**
 * ask_user 消息流问答卡片（展示态，不可交互）：问题 + 选项静态展示（禁用态按钮），
 * 结果到达后由 appendToolResult 更新头部并追加回答——交互作答由审批容器的选择卡片承载。
 */
export function askUserBubble(prompt: string, options: Array<string | Record<string, unknown>>, multi: boolean): HTMLElement {
  const bubble = el("div", "bubble")
  const head = el("div", "tool-head", multi ? "🧭 请选择（可多选）" : "🧭 请选择")
  bubble.appendChild(head)
  if (prompt) bubble.appendChild(el("div", "block-text", prompt))
  const opts = el("div", "choice-opts")
  for (const o of normalizeChoiceOpts(options)) {
    const btn = el("button", "choice-opt", o.title)
    btn.disabled = true
    btn.title = "请在上方选择卡片作答"
    opts.appendChild(btn)
  }
  bubble.appendChild(opts)
  return bubble
}

/** 环境变量请求卡片（event.env.request 实时渲染）：展示变量名与说明，用户填值提交后保存到浏览器本地并回传引擎（ask_env 工具阻塞等待）。 */
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
