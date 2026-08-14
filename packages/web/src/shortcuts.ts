import { el, input } from "./state"
import { uuid } from "./uuid"
import { autosize } from "./composer"
import { confirmDialog, promptDialog, tip, toast } from "./ui"

/**
 * 快捷按钮（浏览器本地 localStorage）：标题胶囊展示在空白页，点击把内容填入输入框。
 * 空白页直接支持「＋ 添加」弹窗新增与 hover ✕ 删除；仅存本浏览器，不上传服务端。
 */
export interface Shortcut {
  id: string
  title: string
  content: string
}

const SHORTCUTS_KEY = "gebai.ui.shortcuts"

/** 内置快捷（与用户快捷同等待遇渲染，不可删除）：点击填入提示词，由模型编排工具作答。 */
const BUILT_IN_VIEW: Shortcut = { id: "__builtin_view_tools__", title: "查看子代理和工具", content: "查看可用的子代理和工具" }

export function loadShortcuts(): Shortcut[] {
  try {
    const raw = localStorage.getItem(SHORTCUTS_KEY)
    if (!raw) return []
    const parsed = JSON.parse(raw) as Shortcut[]
    if (!Array.isArray(parsed)) return []
    return parsed.filter((s) => s && typeof s.title === "string" && typeof s.content === "string")
  } catch {
    return []
  }
}

export function saveShortcuts(list: Shortcut[]): void {
  try {
    localStorage.setItem(SHORTCUTS_KEY, JSON.stringify(list))
  } catch {
    /* 存储不可用（隐私模式/配额满）时静默忽略，与主题/环境变量等模块一致 */
  }
  document.dispatchEvent(new CustomEvent("gebai:shortcuts-change"))
}

/**
 * 渲染空白页快捷按钮区（只替换 .es-shortcut / .es-shortcut-hint 元素）：
 * 内置「查看子代理和工具」居首（与用户快捷同级样式，不可删），
 * 用户胶囊点击填入输入框、hover 显现 ✕ 删除（确认弹窗），
 * 末尾「＋ 添加」弹窗新增。
 */
export function renderShortcutButtons(container: HTMLElement): void {
  container.querySelectorAll(".es-shortcut, .es-shortcut-add").forEach((b) => b.remove())
  const list = loadShortcuts()
  renderPill(container, BUILT_IN_VIEW, false)
  for (const s of list) renderPill(container, s, true)
  const add = el("button", "es-shortcut-add", "＋ 添加")
  add.onclick = () => void addShortcutFlow(container)
  container.appendChild(add)
}

function renderPill(container: HTMLElement, s: Shortcut, deletable: boolean): void {
  const btn = el("button", "es-shortcut", s.title)
  tip(btn, s.content)
  btn.onclick = () => {
    input.value = s.content
    input.focus()
    autosize()
  }
  if (deletable) {
    const del = el("span", "es-shortcut-del", "✕")
    del.onclick = async (ev) => {
      ev.stopPropagation()
      if (!(await confirmDialog({ title: "删除快捷按钮", text: `删除「${s.title}」？` }))) return
      saveShortcuts(loadShortcuts().filter((x) => x.id !== s.id))
      toast("已删除", "ok")
      renderShortcutButtons(container)
    }
    btn.appendChild(del)
  }
  container.appendChild(btn)
}

async function addShortcutFlow(container: HTMLElement): Promise<void> {
  const values = await promptDialog({
    title: "添加快捷按钮",
    fields: [
      { placeholder: "标题（显示在空白页）" },
      { placeholder: "内容（点击填入输入框）", multiline: true },
    ],
  })
  if (!values) return
  const title = (values[0] ?? "").trim()
  const content = (values[1] ?? "").trim()
  if (!title || !content) {
    toast("标题与内容均必填")
    return
  }
  saveShortcuts([...loadShortcuts(), { id: uuid(), title, content }])
  toast("已添加", "ok")
  renderShortcutButtons(container)
}
