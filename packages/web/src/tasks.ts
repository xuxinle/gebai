/**
 * 任务管理视图（DESIGN「统一任务管理」）：设置抽屉同款的右侧 overlay 面板（复用 settings-* 布局类），
 * 三个 tab——
 * - 任务：定时/普通/闲时三类任务的清单与行内操作（执行/置顶/出队/终止/启用停用/详情/删除）+ 新建与编辑表单；
 * - 队列：并发额度占用、排队顺序（含等待原因）、运行中条目；
 * - 详情：选中任务的运行历史（含执行会话跳转）与资源文件（脚本/文档）的查看、保存、删除。
 *
 * 数据来源为 REST `/api/v1/tasks`（SDK client.task*），实时性靠 `event.task.*` 事件去抖刷新 +
 * 面板打开期间的 15s 兜底轮询（关闭即停）。纯逻辑（文案/筛选/表单映射）在 tasks-core.ts，便于单测。
 */
import type { Task, TaskFileEntry, TaskQueueView, TaskRunRecord } from "@gebai/sdk"
import { client, el } from "./state"
import { confirmDialog, promptDialog, toast } from "./ui"
import { refreshSessions } from "./sessions"
import {
  canDequeue,
  canFront,
  canRun,
  canStop,
  emptyForm,
  fileLine,
  filterTasks,
  formFromTask,
  formToCreateInput,
  formToUpdateInput,
  idleBlockedText,
  KIND_LABELS,
  lastResultLine,
  limitText,
  metaLine,
  notifySummary,
  RUNNER_LABELS,
  runLine,
  scheduleSummary,
  shortId,
  SOURCE_LABELS,
  stateLabel,
  STATUS_LABELS,
  TARGET_LABELS,
  taskTitle,
  validateForm,
  type KindFilter,
  type TaskFormValues,
} from "./tasks-core"

/** 面板打开期间兜底轮询间隔（事件为主，轮询只防漏）。 */
const POLL_MS = 15_000
/** 事件驱动的刷新去抖（一轮任务执行会连发多个事件）。 */
const REFRESH_DEBOUNCE_MS = 300

type TabKey = "list" | "queue" | "detail"

let tasks: Task[] = []
let queueView: TaskQueueView | null = null
let filter: KindFilter = "all"
let selectedId: string | null = null
let tab: TabKey = "list"
/** 后端任务能力未启用（GEBAI_TASKS_ENABLED=false → REST 503）：视图内提示且不渲染操作。 */
let unavailable = false
let formOpen = false
let editingId: string | null = null
/** 正在编辑的表单元素与它的编辑目标标识（新建为 "new"，编辑为任务 id）：
 *  后台刷新会重绘整个 tab，若不用缓存节点就不会重建表单——**重绘会抹掉用户正在输入的内容**。 */
let editorEl: HTMLElement | null = null
let editorKey: string | null = null
/** 详情 tab 的资源文件状态。 */
let fileEntries: TaskFileEntry[] = []
let openPath: string | null = null
let openContent = ""
let pollTimer: ReturnType<typeof setInterval> | null = null
let debounceTimer: ReturnType<typeof setTimeout> | null = null

function overlayEl(): HTMLElement | null {
  return document.getElementById("tasks-overlay")
}

function tabsRoot(): HTMLElement | null {
  return document.getElementById("tasks-tabs")
}

function bodyRoot(): HTMLElement | null {
  return document.getElementById("tasks-body")
}

export function isTasksOpen(): boolean {
  const overlay = overlayEl()
  return !!overlay && !overlay.hidden
}

/** 绑定入口按钮与面板交互（main.ts 初始化时调用一次）。 */
export function bindTasks(): void {
  const btn = document.getElementById("tasks-btn")
  const overlay = overlayEl()
  if (!btn || !overlay) return
  btn.addEventListener("click", () => {
    if (isTasksOpen()) closeTasks()
    else openTasks()
  })
  document.getElementById("tasks-close")?.addEventListener("click", () => closeTasks())
  overlay.addEventListener("click", (e) => {
    if (e.target === overlay) closeTasks()
  })
  tabsRoot()?.addEventListener("click", (e) => {
    const b = (e.target as HTMLElement).closest("button[data-tab]") as HTMLButtonElement | null
    if (!b?.dataset.tab) return
    tab = b.dataset.tab as TabKey
    render()
  })
  // 任务事件（入队/开始/结束/队列变化）：面板打开时去抖刷新，关闭时零开销
  client.onEvent((ev) => {
    if (!ev.type.startsWith("event.task.") || !isTasksOpen()) return
    scheduleRefresh()
  })
}

export function openTasks(initial: TabKey = "list"): void {
  const overlay = overlayEl()
  if (!overlay) return
  overlay.hidden = false
  tab = initial
  startPoll()
  void refresh()
}

export function closeTasks(): void {
  const overlay = overlayEl()
  if (overlay) overlay.hidden = true
  stopPoll()
}

function startPoll(): void {
  if (pollTimer) return
  pollTimer = setInterval(() => {
    if (isTasksOpen()) void refresh(true)
  }, POLL_MS)
}

function stopPoll(): void {
  if (pollTimer) clearInterval(pollTimer)
  pollTimer = null
}

function scheduleRefresh(): void {
  if (debounceTimer) clearTimeout(debounceTimer)
  debounceTimer = setTimeout(() => {
    debounceTimer = null
    if (isTasksOpen()) void refresh(true)
  }, REFRESH_DEBOUNCE_MS)
}

/** 能力未启用的错误识别（REST 503 的提示里带环境变量名）。 */
function isUnavailableError(err: unknown): boolean {
  const msg = String((err as Error)?.message ?? err)
  return msg.includes("GEBAI_TASKS_ENABLED") || msg.includes("HTTP 503")
}

/** 拉取任务清单与队列视图并重绘（silent=true 为后台刷新：失败不打扰用户）。 */
async function refresh(silent = false): Promise<void> {
  try {
    const [list, queue] = await Promise.all([client.listTasks(), client.taskQueue()])
    tasks = list
    queueView = queue
    unavailable = false
  } catch (err) {
    if (isUnavailableError(err)) {
      unavailable = true
      tasks = []
      queueView = null
    } else {
      if (!silent) toast(`加载任务失败: ${(err as Error).message}`)
      return
    }
  }
  if (selectedId && !tasks.some((t) => t.id === selectedId)) selectedId = null
  render()
}

function noticeBox(text: string): HTMLElement {
  return el("div", "tasks-notice", text)
}

function render(): void {
  const tabs = tabsRoot()
  const body = bodyRoot()
  if (!tabs || !body) return
  for (const b of tabs.querySelectorAll<HTMLElement>("button[data-tab]")) {
    b.classList.toggle("active", b.dataset.tab === tab)
  }
  body.innerHTML = ""
  if (unavailable) {
    body.appendChild(noticeBox("任务能力未启用（服务端 GEBAI_TASKS_ENABLED=false），无法查看或执行任务。"))
    return
  }
  if (tab === "queue") renderQueueTab(body)
  else if (tab === "detail") renderDetailTab(body)
  else renderListTab(body)
}

/* ---------- 任务 tab ---------- */

/** 当前编辑目标标识（新建 / 某任务）。 */
function editorKeyNow(): string {
  return editingId ?? "new"
}

/** 取本次渲染的表单节点：编辑目标未变则**复用已有元素**（后台刷新重绘不抹已输入内容）。 */
function editorForRender(): HTMLElement {
  const key = editorKeyNow()
  if (!editorEl || editorKey !== key) {
    editorEl = buildEditor()
    editorKey = key
  }
  return editorEl
}

/** 关闭编辑器（丢弃缓存节点，下次打开按当前编辑目标重建）。 */
function closeEditorState(): void {
  formOpen = false
  editingId = null
  editorEl = null
  editorKey = null
}

function renderListTab(body: HTMLElement): void {
  const bar = el("div", "tasks-toolbar")
  const seg = el("div", "tasks-seg")
  for (const [value, label] of [["all", "全部"], ["scheduled", "定时"], ["manual", "普通"], ["idle", "闲时"]] as Array<[KindFilter, string]>) {
    const b = el("button", "tasks-seg-btn", label)
    b.type = "button"
    if (filter === value) b.classList.add("active")
    b.onclick = () => {
      filter = value
      render()
    }
    seg.appendChild(b)
  }
  const refreshBtn = el("button", "mini-btn", "刷新")
  refreshBtn.type = "button"
  refreshBtn.onclick = () => void refresh()
  const newBtn = el("button", "mini-btn", formOpen && !editingId ? "收起表单" : "新建任务")
  newBtn.type = "button"
  newBtn.onclick = () => {
    if (formOpen && !editingId) closeEditorState()
    else {
      formOpen = true
      editingId = null
      editorEl = null
      editorKey = null
    }
    render()
  }
  bar.append(seg, el("div", "tasks-toolbar-gap"), refreshBtn, newBtn)
  body.appendChild(bar)

  if (formOpen) body.appendChild(editorForRender())

  const shown = filterTasks(tasks, filter)
  if (!shown.length) {
    body.appendChild(el("div", "tasks-empty", tasks.length ? "该类别下暂无任务。" : "还没有任务：点「新建任务」创建定时 / 普通 / 闲时任务。"))
    return
  }
  const list = el("div", "tasks-list")
  for (const t of shown) list.appendChild(renderTaskRow(t))
  body.appendChild(list)
}

function renderTaskRow(t: Task): HTMLElement {
  const row = el("div", "tasks-row")
  if (t.id === selectedId) row.classList.add("selected")

  const main = el("div", "tasks-row-main")
  const head = el("div", "tasks-row-head")
  head.appendChild(el("span", "tasks-row-name", taskTitle(t)))
  head.appendChild(el("span", "tasks-badge", KIND_LABELS[t.kind]))
  head.appendChild(el("span", "tasks-badge", RUNNER_LABELS[t.runner]))
  const stateEl = el("span", `tasks-state ${t.state}`, stateLabel(t, queueView))
  head.appendChild(stateEl)
  if (!t.enabled) head.appendChild(el("span", "tasks-badge off", "已停用"))
  if (t.todoId) head.appendChild(el("span", "tasks-badge", "待办联动"))
  main.appendChild(head)
  main.appendChild(el("div", "tasks-row-meta", `id ${shortId(t.id)} · ${metaLine(t)}`))
  main.appendChild(el("div", "tasks-row-result", lastResultLine(t)))
  row.appendChild(main)

  const actions = el("div", "tasks-row-actions")
  const add = (label: string, tip: string, enabled: boolean, fn: () => void | Promise<void>, danger = false) => {
    const b = el("button", danger ? "tasks-act danger" : "tasks-act", label)
    b.type = "button"
    b.title = tip
    b.dataset.tip = tip
    b.disabled = !enabled
    b.onclick = () => void fn()
    actions.appendChild(b)
  }
  add("执行", "立即入队执行一次（不改动既定调度）", canRun(t), () => runTask(t))
  add("置顶", "排到普通任务之前执行", canFront(t), () => frontTask(t))
  add("出队", "取消本次排队（不终止运行中的）", canDequeue(t), () => dequeueTask(t))
  add("终止", "终止正在运行的这次执行", canStop(t), () => stopTask(t))
  add("详情", "查看运行历史与资源文件", true, () => {
    selectedId = t.id
    tab = "detail"
    openPath = null
    fileEntries = []
    runsFor = null
    runs = []
    render()
  })
  add(t.enabled ? "停用" : "启用", t.enabled ? "停用后退出调度与队列" : "重新启用并重算调度时间", true, () => toggleEnabled(t), t.enabled)
  add("删除", "删除任务（不可恢复；资源目录文件保留）", true, () => removeTask(t), true)
  row.appendChild(actions)
  return row
}
/* ---------- 新建 / 编辑表单 ---------- */

interface EditorRefs {
  kind: HTMLSelectElement
  runner: HTMLSelectElement
  name: HTMLInputElement
  schedule: HTMLInputElement
  timezone: HTMLInputElement
  misfire: HTMLSelectElement
  script: HTMLTextAreaElement
  prompt: HTMLTextAreaElement
  target: HTMLSelectElement
  sessionId: HTMLInputElement
  agents: HTMLInputElement
  timeoutMs: HTMLInputElement
  maxConsecutiveErrors: HTMLInputElement
  envText: HTMLTextAreaElement
  notifyOn: HTMLSelectElement
  notifyText: HTMLTextAreaElement
  enabled: HTMLInputElement
  runNow: HTMLInputElement
  front: HTMLInputElement
  /** 字段容器（按 kind/runner 动态显隐）。 */
  fields: Record<string, HTMLElement>
}

function field(label: string, control: HTMLElement, hint?: string): HTMLElement {
  const wrap = el("div", "tasks-field")
  wrap.append(el("div", "settings-form-label", label), control)
  if (hint) wrap.appendChild(el("div", "tasks-hint", hint))
  return wrap
}

function textInput(value: string, placeholder = ""): HTMLInputElement {
  const i = document.createElement("input")
  i.type = "text"
  i.value = value
  i.placeholder = placeholder
  return i
}

function textArea(value: string, rows = 4, placeholder = ""): HTMLTextAreaElement {
  const t = document.createElement("textarea")
  t.rows = rows
  t.value = value
  t.placeholder = placeholder
  return t
}

function selectInput(options: Array<[string, string]>, value: string): HTMLSelectElement {
  const s = document.createElement("select")
  for (const [v, label] of options) {
    const o = document.createElement("option")
    o.value = v
    o.textContent = label
    if (v === value) o.selected = true
    s.appendChild(o)
  }
  return s
}

function checkboxField(label: string, checked: boolean): { root: HTMLElement; input: HTMLInputElement } {
  const root = el("label", "tasks-check")
  const input = document.createElement("input")
  input.type = "checkbox"
  input.checked = checked
  root.append(input, el("span", undefined, label))
  return { root, input }
}

/** 收集表单值（kind 由控件持有；编辑态不可改类别）。 */
function collectForm(r: EditorRefs): TaskFormValues {
  return {
    kind: r.kind.value as TaskFormValues["kind"],
    runner: r.runner.value as TaskFormValues["runner"],
    name: r.name.value,
    schedule: r.schedule.value,
    timezone: r.timezone.value,
    misfire: r.misfire.value as TaskFormValues["misfire"],
    script: r.script.value,
    prompt: r.prompt.value,
    target: r.target.value as TaskFormValues["target"],
    sessionId: r.sessionId.value,
    agents: r.agents.value,
    timeoutMs: r.timeoutMs.value,
    maxConsecutiveErrors: r.maxConsecutiveErrors.value,
    envText: r.envText.value,
    notifyOn: r.notifyOn.value as TaskFormValues["notifyOn"],
    notifyText: r.notifyText.value,
    enabled: r.enabled.checked,
    runNow: r.runNow.checked,
    front: r.front.checked,
  }
}

/** 表单：新建（编辑态由 editingId 决定预填与提交语义）。 */
function buildEditor(): HTMLElement {
  const editing = editingId ? tasks.find((t) => t.id === editingId) : undefined
  const base = editing ? formFromTask(editing) : emptyForm()

  const form = el("form", "settings-form tasks-form")
  form.setAttribute("novalidate", "")
  const refs = { fields: {} } as EditorRefs

  refs.kind = selectInput(
    [
      ["scheduled", "定时任务（按表达式到期执行）"],
      ["manual", "普通任务（入队按序执行）"],
      ["idle", "闲时任务（队列空闲时串行执行；一次执行即终，失败停用）"],
    ],
    base.kind,
  )
  if (editing) refs.kind.disabled = true // 类别不可改（任务语义由创建时确定）
  refs.fields.kind = field("任务类别", refs.kind, editing ? "类别创建后不可更改（如需其他类别请新建任务）" : undefined)

  refs.runner = selectInput(
    [
      ["prompt", "提示词（触发一次 Agent 会话）"],
      ["script", "脚本（在任务资源目录执行 shell）"],
    ],
    base.runner,
  )
  refs.fields.runner = field("执行体", refs.runner)
  refs.name = textInput(base.name, "如：每日晨报")
  refs.fields.name = field("任务名", refs.name)

  refs.schedule = textInput(base.schedule, "0 9 * * * 或 @every 30m / @daily / @at 2026-09-01T09:00")
  refs.fields.schedule = field("执行表达式", refs.schedule)
  refs.timezone = textInput(base.timezone, "缺省服务器本地时区，如 Asia/Shanghai")
  refs.fields.timezone = field("时区", refs.timezone)
  refs.misfire = selectInput(
    [
      ["", "缺省（错过即跳过）"],
      ["skip", "错过即跳过"],
      ["run", "启动后立即补跑一次"],
    ],
    base.misfire,
  )
  refs.fields.misfire = field("停机补跑", refs.misfire)

  refs.script = textArea(base.script, 4, "bash run.sh（相对路径基于任务资源目录）")
  refs.fields.script = field("脚本命令", refs.script)
  refs.prompt = textArea(base.prompt, 5, "要交给 Agent 完成的提示词")
  refs.fields.prompt = field("提示词", refs.prompt)

  refs.target = selectInput(
    (Object.keys(TARGET_LABELS) as Array<keyof typeof TARGET_LABELS>).map((k) => [k, TARGET_LABELS[k]] as [string, string]),
    base.target,
  )
  refs.fields.target = field("执行目标", refs.target)
  refs.sessionId = textInput(base.sessionId, "留空=创建任务时的当前会话")
  refs.fields.sessionId = field("绑定会话 id", refs.sessionId)
  refs.agents = textInput(base.agents, "逗号分隔，如 code, task")
  refs.fields.agents = field("预载子Agent", refs.agents)

  refs.timeoutMs = textInput(base.timeoutMs, "毫秒；缺省脚本 5 分钟 / 提示词 30 分钟")
  refs.fields.timeoutMs = field("单次执行超时", refs.timeoutMs)
  refs.maxConsecutiveErrors = textInput(base.maxConsecutiveErrors, "0=不停用")
  refs.fields.maxConsecutiveErrors = field("连续失败自动停用", refs.maxConsecutiveErrors)
  refs.envText = textArea(base.envText, 3, "每行一条：NAME=值，如\nAPI_BASE=https://example.com\nREPORT_DIR=data/reports")
  refs.fields.envText = field("环境变量", refs.envText, "服务端持久化（无人值守执行不依赖浏览器本地配置）；脚本型注入子进程、提示词型注入执行会话，优先级高于全局/会话环境；敏感键值回显为掩码")

  refs.notifyOn = selectInput(
    [
      ["auto", "执行结束自动通知（最后回复/输出）"],
      ["model", "由模型决定（执行会话用 task_notify 按需推送）"],
    ],
    base.notifyOn,
  )
  refs.fields.notifyOn = field("通知时机", refs.notifyOn)
  refs.notifyText = textArea(base.notifyText, 3, "每行一条：type target [secret]，如\nwebhook https://example.com/hook\nfeishu_chat oc_xxxxxxxx\nwebhook 32位webhookId")
  refs.fields.notifyText = field("通知通道", refs.notifyText, "type=webhook / feishu / feishu_chat；webhook 第二段为 32 位 hex 时按已注册 Webhook 引用")

  const enabled = checkboxField("启用（停用后退出调度与队列）", base.enabled)
  refs.enabled = enabled.input
  refs.fields.enabled = field("状态", enabled.root)
  const runNow = checkboxField("创建后立即入队执行一次", base.runNow)
  refs.runNow = runNow.input
  refs.fields.runNow = field("普通任务", runNow.root)
  const front = checkboxField("入队时置顶（排到普通任务之前）", base.front)
  refs.front = front.input
  refs.fields.front = field("优先级", front.root)

  refs.misfire.value = base.misfire
  refs.target.value = base.target
  refs.notifyOn.value = base.notifyOn

  const syncVisible = () => {
    const kind = refs.kind.value as TaskFormValues["kind"]
    const runner = refs.runner.value as TaskFormValues["runner"]
    const manual = kind === "manual"
    refs.fields.schedule.hidden = kind !== "scheduled"
    refs.fields.timezone.hidden = kind !== "scheduled"
    refs.fields.misfire.hidden = kind !== "scheduled"
    refs.fields.script.hidden = runner !== "script"
    refs.fields.prompt.hidden = runner !== "prompt"
    refs.fields.target.hidden = runner !== "prompt"
    refs.fields.sessionId.hidden = runner !== "prompt" || refs.target.value !== "session"
    refs.fields.agents.hidden = runner !== "prompt" || refs.target.value === "session"
    refs.fields.runNow.hidden = !manual
    refs.fields.front.hidden = !manual
  }
  refs.kind.addEventListener("change", syncVisible)
  refs.runner.addEventListener("change", syncVisible)
  refs.target.addEventListener("change", syncVisible)

  const grid = el("div", "tasks-form-grid")
  for (const key of ["kind", "runner", "name", "schedule", "timezone", "misfire", "target", "sessionId", "agents", "timeoutMs", "maxConsecutiveErrors", "notifyOn"]) {
    grid.appendChild(refs.fields[key])
  }
  const wide = el("div", "tasks-form-wide")
  wide.append(refs.fields.script, refs.fields.prompt, refs.fields.envText, refs.fields.notifyText, refs.fields.enabled, refs.fields.runNow, refs.fields.front)
  form.append(grid, wide)

  const actions = el("div", "tasks-form-actions")
  const submit = el("button", "mini-btn", editing ? "保存修改" : "创建任务")
  submit.type = "submit"
  const cancel = el("button", "mini-btn", "取消")
  cancel.type = "button"
  cancel.onclick = () => {
    closeEditorState()
    render()
  }
  actions.append(submit, cancel)
  form.appendChild(actions)

  form.onsubmit = (e) => {
    e.preventDefault()
    const values = collectForm(refs)
    const err = validateForm(values)
    if (err) {
      toast(err)
      return
    }
    void (async () => {
      try {
        if (editing) await client.updateTask(editing.id, formToUpdateInput(values))
        else await client.createTask(formToCreateInput(values))
        closeEditorState()
        toast(editing ? "任务已更新" : "任务已创建", "ok")
        await refresh(true)
      } catch (e2) {
        toast(`保存失败: ${(e2 as Error).message}`)
      }
    })()
  }

  syncVisible()
  return form
}

/* ---------- 队列 tab ---------- */

function renderQueueTab(body: HTMLElement): void {
  const q = queueView
  if (!q) {
    body.appendChild(el("div", "tasks-empty", "队列信息加载中…"))
    return
  }
  const head = el("div", "tasks-quota")
  head.append(
    el("span", "tasks-quota-label", "并发额度"),
    el("span", "tasks-quota-value", limitText(q)),
    el("span", "tasks-hint", idleBlockedText(q)),
  )
  body.appendChild(head)

  body.appendChild(el("div", "settings-section-title", `运行中（${q.running.length}）`))
  if (!q.running.length) body.appendChild(el("div", "tasks-empty", "当前没有运行中的任务。"))
  for (const r of q.running) {
    const row = el("div", "tasks-row compact")
    const main = el("div", "tasks-row-main")
    const head2 = el("div", "tasks-row-head")
    head2.append(
      el("span", "tasks-row-name", r.name?.trim() || `任务 ${shortId(r.taskId)}`),
      el("span", "tasks-badge", KIND_LABELS[r.kind]),
      el("span", "tasks-badge", RUNNER_LABELS[r.runner]),
      el("span", "tasks-state running", "运行中"),
    )
    main.append(head2, el("div", "tasks-row-meta", `开始 ${new Date(r.startedAt).toLocaleString("zh-CN")}${r.sessionId ? ` · 会话 ${shortId(r.sessionId)}` : ""}`))
    row.appendChild(main)
    const actions = el("div", "tasks-row-actions")
    const detailBtn = el("button", "tasks-act", "详情")
    detailBtn.type = "button"
    detailBtn.onclick = () => {
      selectedId = r.taskId
      tab = "detail"
      openPath = null
      fileEntries = []
      render()
    }
    const stopBtn = el("button", "tasks-act danger", "终止")
    stopBtn.type = "button"
    stopBtn.onclick = () => void stopTaskById(r.taskId, r.name)
    const sessionBtn = el("button", "tasks-act", "执行会话")
    sessionBtn.type = "button"
    sessionBtn.disabled = !r.sessionId
    sessionBtn.onclick = () => {
      if (r.sessionId) void openSession(r.sessionId)
    }
    actions.append(detailBtn, sessionBtn, stopBtn)
    row.appendChild(actions)
    body.appendChild(row)
  }

  body.appendChild(el("div", "settings-section-title", `排队中（${q.entries.length}）`))
  if (!q.entries.length) body.appendChild(el("div", "tasks-empty", "队列为空：没有等待执行的任务。"))
  for (const e of q.entries) {
    const row = el("div", "tasks-row compact")
    const main = el("div", "tasks-row-main")
    const head2 = el("div", "tasks-row-head")
    head2.append(
      el("span", "tasks-pos", `#${e.position}`),
      el("span", "tasks-row-name", e.name?.trim() || `任务 ${shortId(e.taskId)}`),
      el("span", "tasks-badge", KIND_LABELS[e.kind]),
      el("span", "tasks-badge", RUNNER_LABELS[e.runner]),
      el("span", "tasks-badge", SOURCE_LABELS[e.source]),
    )
    if (e.front) head2.appendChild(el("span", "tasks-badge", "置顶"))
    main.append(head2, el("div", "tasks-row-meta", `${e.waiting ?? "等待执行"} · 入队 ${new Date(e.enqueuedAt).toLocaleString("zh-CN")}`))
    row.appendChild(main)
    const actions = el("div", "tasks-row-actions")
    const frontBtn = el("button", "tasks-act", "置顶")
    frontBtn.type = "button"
    frontBtn.disabled = e.kind === "scheduled"
    frontBtn.onclick = () => void frontTaskById(e.taskId)
    const outBtn = el("button", "tasks-act", "出队")
    outBtn.type = "button"
    outBtn.onclick = () => void dequeueTaskById(e.taskId)
    actions.append(frontBtn, outBtn)
    row.appendChild(actions)
    body.appendChild(row)
  }
}
/* ---------- 详情 tab ---------- */

/** 已加载资源文件所属任务（切换任务时失效重取，避免显示上一个任务的文件）。 */
let filesFor: string | null = null
/** 已加载运行历史所属任务 + 运行次数（执行记录经 REST 异步拉取；运行次数变化即失效重取）。 */
let runsFor: string | null = null
/** 当前展示的执行记录（新→旧）。 */
let runs: TaskRunRecord[] = []

/** 运行历史的失效键（任务 + 已运行次数：新一轮执行完成后自动重取）。 */
function runsKey(t: Task): string {
  return `${t.id}:${t.runCount}`
}

function renderDetailTab(body: HTMLElement): void {
  const t = selectedId ? tasks.find((x) => x.id === selectedId) : undefined
  if (!t) {
    body.appendChild(el("div", "tasks-empty", "在「任务」列表点某条任务的「详情」，查看运行历史与资源文件。"))
    return
  }

  const head = el("div", "tasks-detail-head")
  const title = el("div", "tasks-detail-title")
  title.append(el("span", "tasks-row-name", taskTitle(t)), el("span", "tasks-badge", KIND_LABELS[t.kind]), el("span", "tasks-badge", RUNNER_LABELS[t.runner]), el("span", `tasks-state ${t.state}`, stateLabel(t, queueView)))
  head.appendChild(title)
  const back = el("button", "mini-btn", "返回列表")
  back.type = "button"
  back.onclick = () => {
    tab = "list"
    render()
  }
  head.appendChild(back)
  body.appendChild(head)

  const info = el("div", "tasks-detail")
  const lines = [
    `任务 id：${t.id}`,
    `状态：${t.enabled ? "启用" : "停用"} · ${stateLabel(t, queueView)} · 已运行 ${t.runCount} 次`,
    scheduleSummary(t) ? `执行表达式：${scheduleSummary(t)}` : "",
    t.kind === "scheduled" ? `下次执行：${t.nextRunAt ? new Date(t.nextRunAt).toLocaleString("zh-CN") : "-"}` : "",
    t.runner === "prompt" ? `执行目标：${TARGET_LABELS[t.target ?? "ephemeral"]}${t.sessionId ? `（绑定 ${t.sessionId}）` : ""}${t.agents?.length ? ` · 预载 ${t.agents.join(", ")}` : ""}` : "执行体：脚本（在任务资源目录运行）",
    t.timeoutMs ? `单次超时：${t.timeoutMs}ms` : "",
    t.maxConsecutiveErrors ? `连续失败 ${t.maxConsecutiveErrors} 次自动停用` : "",
    notifySummary(t) ? `通知：${notifySummary(t)}（${t.notifyOn === "model" ? "模型决定" : "执行结束自动"}）` : "",
    t.lastNotifyError ? `最近通知失败：${t.lastNotifyError}` : "",
  ].filter(Boolean)
  for (const line of lines) info.appendChild(el("div", "tasks-detail-line", line))
  body.appendChild(info)

  const editBtn = el("button", "mini-btn", "编辑配置")
  editBtn.type = "button"
  editBtn.onclick = () => {
    editingId = t.id
    formOpen = true
    tab = "list"
    editorEl = null
    editorKey = null
    render()
  }
  const runBtn = el("button", "mini-btn", "立即执行")
  runBtn.type = "button"
  runBtn.disabled = !canRun(t)
  runBtn.onclick = () => void runTask(t)
  const bar = el("div", "tasks-toolbar")
  bar.append(editBtn, runBtn)
  body.appendChild(bar)

  body.appendChild(el("div", "settings-section-title", "运行历史"))
  renderRunsPanel(body, t)

  body.appendChild(el("div", "settings-section-title", "资源文件（脚本 / 文档）"))
  body.appendChild(renderFilePanel(t))
  void loadFiles(t.id)
  void loadRuns(t)
}

/** 运行历史面板（执行记录按文件落盘，故异步拉取：`runsFor` 标记已加载的任务与运行次数，加载中提示）。 */
function renderRunsPanel(body: HTMLElement, t: Task): void {
  if (runsFor !== runsKey(t)) {
    body.appendChild(el("div", "tasks-empty", "运行历史加载中…"))
    return
  }
  if (!runs.length) {
    body.appendChild(el("div", "tasks-empty", "还没有运行记录。"))
    return
  }
  for (const r of runs) {
    const row = el("div", "tasks-run")
    const main = el("div", "tasks-run-main")
    const head2 = el("div", "tasks-run-head")
    head2.append(el("span", `tasks-status ${r.status}`, STATUS_LABELS[r.status]), el("span", "tasks-run-time", new Date(r.at).toLocaleString("zh-CN")))
    if (r.manual) head2.appendChild(el("span", "tasks-badge", "手动"))
    main.append(head2, el("div", "tasks-run-meta", runLine(r)))
    if (r.sessionId) {
      const open = el("button", "tasks-act", "打开会话")
      open.type = "button"
      open.onclick = () => {
        if (r.sessionId) void openSession(r.sessionId)
      }
      head2.appendChild(open)
    }
    row.appendChild(main)
    body.appendChild(row)
  }
}

function renderFilePanel(t: Task): HTMLElement {
  const panel = el("div", "tasks-files")
  const bar = el("div", "tasks-toolbar")
  const refreshBtn = el("button", "mini-btn", "刷新")
  refreshBtn.type = "button"
  refreshBtn.onclick = () => {
    filesFor = null
    render()
  }
  const newBtn = el("button", "mini-btn", "新建文件")
  newBtn.type = "button"
  newBtn.onclick = () => void createFile(t)
  bar.append(refreshBtn, newBtn)
  panel.appendChild(bar)

  if (filesFor !== t.id) panel.appendChild(el("div", "tasks-empty", "文件列表加载中…"))
  else if (!fileEntries.length) panel.appendChild(el("div", "tasks-empty", "资源目录为空：脚本型任务把脚本写在这里（相对路径即工作目录），也可放资料文档给提示词型任务读取。"))
  else {
    const list = el("div", "tasks-file-list")
    for (const f of fileEntries) {
      const row = el("div", "tasks-file")
      const name = el("button", "tasks-file-name", fileLine(f))
      name.type = "button"
      name.disabled = f.dir
      name.onclick = () => void openFile(t.id, f.path)
      if (f.path === openPath) name.classList.add("active")
      row.appendChild(name)
      const del = el("button", "tasks-act danger", "删除")
      del.type = "button"
      del.onclick = () => void deleteFile(t.id, f.path)
      row.appendChild(del)
      list.appendChild(row)
    }
    panel.appendChild(list)
  }

  if (openPath) {
    const editorWrap = el("div", "tasks-file-editor")
    editorWrap.appendChild(el("div", "settings-form-label", `编辑 ${openPath}`))
    const area = textArea(openContent, 10)
    area.classList.add("tasks-file-text")
    editorWrap.appendChild(area)
    const actions = el("div", "tasks-form-actions")
    const save = el("button", "mini-btn", "保存")
    save.type = "button"
    save.onclick = () => void saveFile(t.id, area.value)
    const close = el("button", "mini-btn", "关闭")
    close.type = "button"
    close.onclick = () => {
      openPath = null
      openContent = ""
      render()
    }
    actions.append(save, close)
    editorWrap.appendChild(actions)
    panel.appendChild(editorWrap)
  }
  return panel
}

/** 资源文件懒加载（同任务只取一次；失败不阻塞详情渲染）。 */
async function loadFiles(taskId: string): Promise<void> {
  if (filesFor === taskId) return
  filesFor = taskId
  try {
    fileEntries = await client.listTaskFiles(taskId)
  } catch {
    fileEntries = []
  }
  if (selectedId === taskId && tab === "detail") render()
}

/** 运行历史懒加载（执行记录按文件落盘：打开详情时向 REST 拉取；失败按空清单降级）。 */
async function loadRuns(t: Task): Promise<void> {
  const key = runsKey(t)
  if (runsFor === key) return
  runsFor = key
  try {
    runs = await client.taskRuns(t.id)
  } catch {
    runs = []
  }
  if (selectedId === t.id && tab === "detail") render()
}

async function openFile(taskId: string, path: string): Promise<void> {
  try {
    const res = await client.readTaskFile(taskId, path)
    openPath = res.path
    openContent = res.content
    render()
  } catch (err) {
    toast(`打开失败: ${(err as Error).message}`)
  }
}

async function createFile(t: Task): Promise<void> {
  const res = await promptDialog({ title: "新建资源文件", fields: [{ placeholder: "相对路径，如 run.sh 或 notes/todo.md", value: "" }] })
  if (!res) return
  const path = res[0]?.trim()
  if (!path) return
  try {
    await client.writeTaskFile(t.id, path, "")
    filesFor = null
    openPath = path
    openContent = ""
    render()
    toast("文件已创建，写入内容后点保存", "ok")
  } catch (err) {
    toast(`新建失败: ${(err as Error).message}`)
  }
}

async function saveFile(taskId: string, content: string): Promise<void> {
  if (!openPath) return
  try {
    await client.writeTaskFile(taskId, openPath, content)
    openContent = content
    filesFor = null
    toast("已保存", "ok")
    render()
  } catch (err) {
    toast(`保存失败: ${(err as Error).message}`)
  }
}

async function deleteFile(taskId: string, path: string): Promise<void> {
  const ok = await confirmDialog({ title: "删除资源文件", text: path, okLabel: "删除" })
  if (!ok) return
  try {
    await client.deleteTaskFile(taskId, path)
    if (openPath === path) {
      openPath = null
      openContent = ""
    }
    filesFor = null
    render()
  } catch (err) {
    toast(`删除失败: ${(err as Error).message}`)
  }
}

/* ---------- 操作 ---------- */

async function runTask(t: Task): Promise<void> {
  try {
    const res = await client.runTask(t.id)
    if (res.queued) toast(`已加入队列${res.position ? `（第 ${res.position} 位）` : ""}`, "ok")
    else toast(res.reason ?? "未入队", "ok")
  } catch (err) {
    toast(`执行失败: ${(err as Error).message}`)
  }
  await refresh(true)
}

async function frontTask(t: Task): Promise<void> {
  await frontTaskById(t.id)
}

async function frontTaskById(taskId: string): Promise<void> {
  try {
    const res = await client.frontTask(taskId)
    toast(res.queued ? `已置顶${res.position ? `（第 ${res.position} 位）` : ""}` : (res.reason ?? "未置顶"), "ok")
  } catch (err) {
    toast(`置顶失败: ${(err as Error).message}`)
  }
  await refresh(true)
}

async function dequeueTask(t: Task): Promise<void> {
  await dequeueTaskById(t.id)
}

async function dequeueTaskById(taskId: string): Promise<void> {
  try {
    await client.dequeueTask(taskId)
    toast("已出队", "ok")
  } catch (err) {
    toast(`出队失败: ${(err as Error).message}`)
  }
  await refresh(true)
}

async function stopTask(t: Task): Promise<void> {
  await stopTaskById(t.id, t.name)
}

async function stopTaskById(taskId: string, name?: string): Promise<void> {
  const ok = await confirmDialog({ title: "终止任务执行", text: `${name?.trim() || taskId}：正在运行的这次执行会被取消（脚本型任务需等其自身结束或超时）。`, okLabel: "终止" })
  if (!ok) return
  try {
    await client.stopTask(taskId)
    toast("已发出终止请求", "ok")
  } catch (err) {
    toast(`终止失败: ${(err as Error).message}`)
  }
  await refresh(true)
}

async function toggleEnabled(t: Task): Promise<void> {
  try {
    await client.updateTask(t.id, { enabled: !t.enabled })
    toast(t.enabled ? "已停用" : "已启用", "ok")
  } catch (err) {
    toast(`操作失败: ${(err as Error).message}`)
  }
  await refresh(true)
}

async function removeTask(t: Task): Promise<void> {
  const ok = await confirmDialog({ title: "删除任务", text: `${taskTitle(t)}（${KIND_LABELS[t.kind]} · ${RUNNER_LABELS[t.runner]}）`, okLabel: "删除" })
  if (!ok) return
  try {
    await client.deleteTask(t.id)
    if (selectedId === t.id) selectedId = null
    toast("任务已删除", "ok")
  } catch (err) {
    toast(`删除失败: ${(err as Error).message}`)
  }
  await refresh(true)
}

/** 打开任务执行会话：关面板 + 定位会话列表条目（列表未加载时先刷新）。 */
async function openSession(sessionId: string): Promise<void> {
  closeTasks()
  try {
    await refreshSessions()
    const li = document.querySelector<HTMLElement>(`#session-list li[data-sid="${sessionId}"]`)
    if (!li) {
      toast("会话不存在或已归档")
      return
    }
    li.click()
  } catch (err) {
    toast(`打开会话失败: ${(err as Error).message}`)
  }
}
