/**
 * 闲时执行时间段控件（三处共用：任务面板表单、待办弹窗的时段编辑、用户偏好设置）。
 *
 * 为何抽成共享控件而不是各面板各写一份：这套控件包含开关 + 快捷预设 + 时刻校验 +
 * 星期归属 + 时区 + 「人话描述」，是本功能里最容易出错也最容易抄歪的一段。
 * 三份手写的直接后果是同一个时段在三个地方显示成三种说法（用户会以为设错了）。
 *
 * 纯 DOM 构造（不依赖任何面板）：输入 = 初始 `IdleWindowForm`，输出 = `IdleWindowFields`
 * （带 `root` 与 `get()`）。判定「现在是否在窗内」不在这里做——那是服务端的事。
 */
import type { IdleWindow } from "@gebai/sdk"
import { el } from "./state"
import {
  describeIdleWindow,
  idleWindowFromForm,
  IDLE_PRESETS,
  matchPreset,
  WEEKDAY_LABELS,
  type IdleWindowForm,
} from "./idle-window-core"

export interface IdleWindowFields {
  root: HTMLElement
  /** 读当前控件值（含 enabled）。 */
  get: () => IdleWindowForm
  /** 以新值重置控件（打开对话框时回显用）。 */
  set: (next: IdleWindowForm) => void
}

function textInput(value: string, placeholder = ""): HTMLInputElement {
  const i = el("input", "tasks-input")
  i.type = "text"
  i.value = value
  i.placeholder = placeholder
  i.autocomplete = "off"
  return i
}

function selectInput(options: Array<[string, string]>, value: string): HTMLSelectElement {
  const s = el("select", "tasks-input")
  for (const [v, label] of options) {
    const o = document.createElement("option")
    o.value = v
    o.textContent = label
    s.appendChild(o)
  }
  s.value = value
  return s
}

/** 「从 22:00 到 06:00」+ 星期 + 时区 + 实时人话描述。`compact` 用于窄容器（待办弹窗）。 */
export function createIdleWindowFields(initial: IdleWindowForm, opts: { compact?: boolean } = {}): IdleWindowFields {
  const root = el("div", `tasks-idlewin${opts.compact ? " compact" : ""}`)
  const state: IdleWindowForm = { ...initial, days: [...initial.days] }

  const enable = document.createElement("input")
  enable.type = "checkbox"
  enable.checked = state.enabled
  const enableRow = el("label", "tasks-idlewin-toggle")
  enableRow.append(enable, el("span", undefined, "只在指定时间段内执行（夜间/周末跑，省算力与 token）"))

  const presetSel = selectInput(
    [["", "（自定义 / 不选预设）"], ...IDLE_PRESETS.map((p) => [p.key, p.label] as [string, string])],
    matchPreset(
      initial.enabled
        ? { start: initial.start, end: initial.end, ...(initial.days.length ? { days: initial.days } : {}), ...(initial.timezone ? { timezone: initial.timezone } : {}) }
        : null,
    )?.key ?? "",
  )
  const start = textInput(state.start, "22:00")
  start.classList.add("tasks-idlewin-time")
  const end = textInput(state.end, "06:00")
  end.classList.add("tasks-idlewin-time")
  const timeRow = el("div", "tasks-idlewin-row")
  timeRow.append(el("span", "tasks-idlewin-sep", "从"), start, el("span", "tasks-idlewin-sep", "到"), end)

  const dayBoxes: HTMLInputElement[] = []
  const daysRow = el("div", "tasks-idlewin-days")
  for (let d = 0; d < 7; d++) {
    const cb = document.createElement("input")
    cb.type = "checkbox"
    cb.checked = state.days.includes(d)
    const lab = el("label", "tasks-idlewin-day")
    lab.append(cb, el("span", undefined, WEEKDAY_LABELS[d].replace("周", "")))
    lab.title = WEEKDAY_LABELS[d]
    dayBoxes.push(cb)
    daysRow.appendChild(lab)
  }
  // 提示用「」而不是 Markdown 星号：tasks-hint 是纯文本节点（不渲染 Markdown，星号会原样露出来）
  const daysHint = el("div", "tasks-hint", "不勾 = 每天；星期指时段的「开始那天」（跨零点的凌晨算前一天那一场）")

  const tz = textInput(state.timezone, "时区（缺省服务器本地，如 Asia/Shanghai）")
  tz.classList.add("tasks-idlewin-tz")
  const desc = el("div", "tasks-idlewin-desc")

  const detail = el("div", "tasks-idlewin-detail")
  detail.append(presetSel, timeRow, daysRow, daysHint, tz)
  root.append(enableRow, desc, detail)

  const read = (): IdleWindowForm => ({
    enabled: enable.checked,
    start: start.value.trim(),
    end: end.value.trim(),
    days: dayBoxes.flatMap((cb, i) => (cb.checked ? [i] : [])),
    timezone: tz.value.trim(),
  })

  const sync = (): void => {
    detail.hidden = !enable.checked
    if (!enable.checked) {
      desc.textContent = "不限时段：队列空闲即执行"
      desc.classList.remove("bad")
      return
    }
    const parsed = idleWindowFromForm({ ...read(), enabled: true })
    desc.textContent = parsed.error ? `⚠ ${parsed.error}` : `生效时段：${describeIdleWindow(parsed.value)}`
    desc.classList.toggle("bad", !!parsed.error)
  }

  enable.addEventListener("change", sync)
  for (const cb of dayBoxes) cb.addEventListener("change", sync)
  for (const box of [start, end, tz]) box.addEventListener("input", sync)
  presetSel.addEventListener("change", () => {
    const preset = IDLE_PRESETS.find((p) => p.key === presetSel.value)
    if (!preset) return
    enable.checked = true
    start.value = preset.window.start
    end.value = preset.window.end
    const days = preset.window.days ?? []
    // 预设"每天"时把七天都勾上，用户改回自定义时看到的就是"全选"而非"空选"（两者语义相同，但全选更直观）
    dayBoxes.forEach((cb, i) => (cb.checked = days.length === 0 || days.includes(i)))
    tz.value = preset.window.timezone ?? ""
    sync()
  })
  sync()

  const set = (next: IdleWindowForm): void => {
    enable.checked = next.enabled
    start.value = next.start
    end.value = next.end
    dayBoxes.forEach((cb, i) => (cb.checked = next.days.includes(i)))
    tz.value = next.timezone
    presetSel.value =
      matchPreset(next.enabled ? { start: next.start, end: next.end, ...(next.days.length ? { days: next.days } : {}), ...(next.timezone ? { timezone: next.timezone } : {}) } : null)?.key ?? ""
    sync()
  }

  return { root, get: read, set }
}

/**
 * 模态对话框版的时段编辑（待办弹窗用：就地弹一个小面板，不跳去任务面板）。
 * 返回 `undefined` = 取消；返回 `null` = 清除时段（不限）；返回对象 = 新时段。
 */
export function askIdleWindow(current?: IdleWindow | null, opts: { title?: string } = {}): Promise<IdleWindow | null | undefined> {
  return new Promise((resolve) => {
    const overlay = el("div", "preview-overlay")
    const card = el("div", "confirm-card idlewin-card")
    card.append(el("div", "confirm-title", opts.title ?? "闲时执行时间段"))
    card.append(
      el(
        "div",
        "confirm-text",
        "只有落在这个时间段内（且队列空闲、没有运行中的会话）才会自动执行。用来把吃算力/耗 token 的活排到夜间或周末。",
      ),
    )
    const fields = createIdleWindowFields(
      current
        ? { enabled: true, start: current.start, end: current.end, days: [...(current.days ?? [])], timezone: current.timezone ?? "" }
        : { enabled: false, start: "22:00", end: "06:00", days: [], timezone: "" },
      { compact: true },
    )
    card.append(fields.root)

    const actions = el("div", "confirm-actions")
    const cancel = el("button", "confirm-cancel", "取消")
    const clear = el("button", "confirm-cancel", "不限时段")
    const ok = el("button", "confirm-ok", "保存")
    actions.append(cancel, clear, ok)
    card.append(actions)
    overlay.appendChild(card)
    document.body.appendChild(overlay)

    let settled = false
    const finish = (value: IdleWindow | null | undefined): void => {
      if (settled) return
      settled = true
      overlay.remove()
      document.removeEventListener("keydown", onKey, true)
      resolve(value)
    }
    const onKey = (e: KeyboardEvent): void => {
      if (e.key === "Escape") {
        e.stopPropagation()
        finish(undefined)
      } else if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
        e.preventDefault()
        submit()
      }
    }
    const submit = (): void => {
      const form = fields.get()
      if (!form.enabled) return finish(null) // 关掉开关 = 不限时段
      const parsed = idleWindowFromForm(form)
      if (parsed.error) {
        // 就地提示：不关闭对话框（用户正在改，关掉就要重填）
        const err = card.querySelector(".idlewin-err") as HTMLElement | null
        if (err) err.textContent = parsed.error
        return
      }
      finish(parsed.value)
    }
    card.append(el("div", "idlewin-err"))
    cancel.addEventListener("click", () => finish(undefined))
    clear.addEventListener("click", () => finish(null))
    ok.addEventListener("click", submit)
    overlay.addEventListener("click", (e) => {
      if (e.target === overlay) finish(undefined)
    })
    document.addEventListener("keydown", onKey, true)
    ok.focus()
  })
}
