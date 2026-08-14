import { attachBtn, client, composer, fileInput, focusInput, getCurrentSession, input, msgEl, runs, sendBtn } from "./state"
import { addPendingFiles } from "./attachments"
import { tip } from "./ui"

/* ---------- 发送/停止按钮 ---------- */

/**
 * 同步发送/停止按钮状态：由「当前显示的会话是否在运行」决定（多会话后台运行时，
 * 每个会话结束只应影响自己——按钮跟随当前会话，而非全局运行状态）。
 * 图标显隐由 CSS class 控制（#send.stopping 切换 .ic-send/.ic-stop 的 display）。
 */
export function syncSendButton() {
  const cur = getCurrentSession()
  const running = !!cur && runs.has(cur.id)
  sendBtn.classList.toggle("stopping", running)
  tip(sendBtn, running ? "停止回答" : "发送")
}

export function bindComposer() {
  sendBtn.addEventListener("click", (e) => {
    // 运行中点击 = 中断当前会话任务（阻止默认 submit）
    if (sendBtn.classList.contains("stopping")) {
      e.preventDefault()
      const cur = getCurrentSession()
      if (cur) void client.cancelTask(cur.id).catch(() => {})
    }
  })

  attachBtn.onclick = () => fileInput.click()
  fileInput.addEventListener("change", () => {
    if (fileInput.files) addPendingFiles(fileInput.files)
    fileInput.value = ""
    focusInput()
  })

  input.addEventListener("paste", (e) => {
    const files = e.clipboardData?.files
    if (files?.length) {
      e.preventDefault()
      addPendingFiles(files)
    }
  })

  let dragDepth = 0
  const main = document.querySelector("main")!
  main.addEventListener("dragenter", (e) => {
    e.preventDefault()
    dragDepth++
    msgEl.classList.add("drag-over")
  })
  main.addEventListener("dragover", (e) => e.preventDefault())
  main.addEventListener("dragleave", () => {
    dragDepth = Math.max(0, dragDepth - 1)
    if (!dragDepth) msgEl.classList.remove("drag-over")
  })
  main.addEventListener("drop", (e) => {
    e.preventDefault()
    dragDepth = 0
    msgEl.classList.remove("drag-over")
    if (e.dataTransfer?.files.length) addPendingFiles(e.dataTransfer.files)
  })
}

/* ---------- 输入区行为 ---------- */

const HISTORY_LIMIT = 50
const HISTORY_KEY = "gebai.ui.inputHistory"
/** 用户级全局输入历史：最新（最后使用）在前，同一文本只保留一条，localStorage 持久化。 */
let inputHistory: string[] = loadInputHistory()
let historyIndex = -1
let historyDraft = ""
/** 各会话第一条输入（自动标题用），独立于全局历史。 */
const firstInputs = new Map<string, string>()

function loadInputHistory(): string[] {
  try {
    const raw = localStorage.getItem(HISTORY_KEY)
    if (!raw) return []
    const parsed: unknown = JSON.parse(raw)
    if (!Array.isArray(parsed)) return []
    return parsed
      .filter((s): s is string => typeof s === "string" && !!s.trim())
      .slice(0, HISTORY_LIMIT)
  } catch {
    return []
  }
}

function saveInputHistory() {
  try {
    localStorage.setItem(HISTORY_KEY, JSON.stringify(inputHistory))
  } catch {
    /* 存储不可用（隐私模式/配额）时静默忽略 */
  }
}

/** 该会话第一条输入（自动标题用）。 */
export function firstInputOf(sessionId: string): string | undefined {
  return firstInputs.get(sessionId)
}

/** 记录一次已发送的输入：全局历史按最后使用去重排序；会话首条输入供自动标题。 */
export function recordInput(sessionId: string, text: string) {
  if (!firstInputs.has(sessionId)) firstInputs.set(sessionId, text)
  const t = text.trim()
  if (t) {
    inputHistory = [t, ...inputHistory.filter((s) => s !== t)].slice(0, HISTORY_LIMIT)
    saveInputHistory()
  }
  resetHistoryNav()
}

export function resetHistoryNav() {
  historyIndex = -1
  historyDraft = ""
}

export function autosize() {
  input.style.height = "auto"
  input.style.height = `${Math.min(input.scrollHeight, 180)}px`
}

export function bindInputBehavior() {
  input.addEventListener("input", autosize)

  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey && !e.isComposing) {
      e.preventDefault()
      composer.requestSubmit()
      return
    }
    // 输入历史导航：↑/↓ 浏览用户级全局历史（空输入进入；有草稿则暂存，↓ 可恢复）
    if (e.isComposing || e.shiftKey || e.altKey || e.metaKey || e.ctrlKey) return
    if (e.key === "ArrowUp") {
      if (historyIndex === -1) {
        if (!inputHistory.length || input.value.trim()) return
        historyIndex = 0
        historyDraft = ""
      } else if (historyIndex < inputHistory.length - 1) {
        historyIndex++
      } else return
      e.preventDefault()
      input.value = inputHistory[historyIndex]
    } else if (e.key === "ArrowDown") {
      if (historyIndex === -1) return
      historyIndex--
      if (historyIndex < 0) {
        historyIndex = -1
        input.value = historyDraft
      } else {
        input.value = inputHistory[historyIndex]
      }
      e.preventDefault()
    } else {
      return
    }
    autosize()
    input.setSelectionRange(input.value.length, input.value.length)
  })
}
