import { attachBtn, client, composer, fileInput, focusInput, getCurrentSession, input, msgEl, pendingFiles, runs, sendBtn } from "./state"
import { addPendingFiles } from "./attachments"
import { tip, toast } from "./ui"

/* ---------- 发送/停止按钮 ---------- */

/**
 * 同步发送/停止按钮状态（由「当前显示的会话是否在运行」决定；多会话后台运行时，
 * 每个会话结束只应影响自己——按钮跟随当前会话，而非全局运行状态）。
 * 运行中且有草稿：发送箭头（提交 = 排队，Ctrl+Enter = 中断插入）；运行中无草稿：停止方块（点击 = 停止）。
 * 图标显隐由 CSS class 控制（#send.stopping 切换 .ic-send/.ic-stop 的 display）。
 */
export function syncSendButton() {
  const cur = getCurrentSession()
  const running = !!cur && runs.has(cur.id)
  const hasDraft = !!input.value.trim() || pendingFiles.length > 0
  sendBtn.classList.toggle("stopping", running && !hasDraft)
  tip(sendBtn, running ? (hasDraft ? "排队发送（Ctrl+Enter 中断插入）" : "停止回答") : "发送")
}

/** 下次提交为「中断插入」（Ctrl+Enter）：运行中取消当前任务循环后立即执行本条；空闲等同普通发送。 */
let interruptNext = false
export function requestInterruptSubmit(): void {
  interruptNext = true
  composer.requestSubmit()
}
/** 读取并清除中断插入标记（submit 处理器消费）。 */
export function takeInterruptNext(): boolean {
  const v = interruptNext
  interruptNext = false
  return v
}

export function bindComposer() {
  sendBtn.addEventListener("click", (e) => {
    // 运行中点击 = 中断当前会话任务（阻止默认 submit）
    if (sendBtn.classList.contains("stopping")) {
      e.preventDefault()
      const cur = getCurrentSession()
      if (cur) {
        // 停止未送达时如实提示：静默吞掉会让用户以为停了，任务却继续跑到自然结束（且可能重复下单/重复写入）
        void client.cancelTask(cur.id).catch((err: unknown) => {
          toast(`停止未送达：${(err as Error).message || "连接不可用"}，重连后可再点一次停止。`)
        })
      }
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

/**
 * 把一段外部文本**追加**到输入框（文件工作台右键「发送会话」）。
 *
 * 三条约定：
 * - **追加而不是覆盖**：输入框里往往已经有用户写了一半的话，替掉它 = 静默丢草稿；
 *   与已有内容之间空一行，拼起来就是「你说的 + 我发的这段代码」。
 * - **把光标与焦点都放到末尾**（`focusInput`）：发完这段，用户下一步就是接着写“把这里改成…”，
 *   还要再点一下输入框才算能用。
 * - **按 `input` 事件那一套收尾**（自动高度 + 发送按钮形态 + 清掉历史导航游标）：
 *   直接改 `.value` 不会触发 `input`，而这三件事都靠它驱动（不清历史导航的话，
 *   接着按 ↑ 会被当成“在浏览历史”而把刚发进来的内容换掉）。
 */
export function insertIntoComposer(text: string): string {
  const add = String(text ?? "")
  if (!add) return input.value
  const cur = input.value
  // 已有内容且末尾不是空行时先断一行（光标处的直接拼接会把两句粘成一行）
  const sep = !cur ? "" : cur.endsWith("\n\n") ? "" : cur.endsWith("\n") ? "\n" : "\n\n"
  input.value = `${cur}${sep}${add}`
  autosize()
  syncSendButton()
  resetHistoryNav()
  focusInput()
  return input.value
}

export function autosize() {
  input.style.height = "auto"
  // 上限随可视高度收窄：小屏（尤其键盘弹起后）输入框最多占可视高度约 1/4，不把会话区挤没
  const cap = Math.max(32, Math.min(180, Math.round(window.innerHeight * 0.26)))
  input.style.height = `${Math.min(input.scrollHeight, cap)}px`
}

/**
 * 软键盘避让：Android 经视口 `interactive-widget=resizes-content` 直接收缩布局视口，
 * iOS 则只缩 visualViewport——布局视口不变，键盘直接盖住输入区。这里把「键盘占掉的高度」
 * 写进根元素变量 `--kb-inset`（#app 高度按它扣除），键盘弹起/收起、页面被顶起都随 visualViewport 跟随。
 */
export function bindKeyboardInset() {
  const vv = window.visualViewport
  if (!vv) return
  const apply = () => {
    const inset = Math.max(0, Math.round(window.innerHeight - vv.height - vv.offsetTop))
    document.documentElement.style.setProperty("--kb-inset", `${inset}px`)
    autosize() // 可视高度变了，输入框上限跟着重算
  }
  vv.addEventListener("resize", apply)
  vv.addEventListener("scroll", apply)
  apply()
}

export function bindInputBehavior() {
  bindKeyboardInset()
  input.addEventListener("input", () => {
    autosize()
    syncSendButton() // 草稿有无影响运行中按钮形态（排队发送 ↔ 停止）
  })

  input.addEventListener("keydown", (e) => {
    // 长按回车的键盘自动重复不是新的发送意图：按住不放会在建会话/上传附件期间反复触发提交
    // （首条消息未落地前反复提交 = 同一句提示词落进多个新会话）
    if (e.key === "Enter" && e.repeat) {
      e.preventDefault()
      return
    }
    // Ctrl+Enter = 中断插入提交（运行中取消当前循环后立即执行；空闲等同普通发送）
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey) && !e.isComposing) {
      e.preventDefault()
      requestInterruptSubmit()
      return
    }
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
