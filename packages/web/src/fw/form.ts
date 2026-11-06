/**
 * 微框架 · 表单控件（通用件）：字段容器 + 输入/多行/下拉/开关/校验提示。
 *
 * 现状问题：`tasks.ts`、`settings.ts` 各自写了 `field()`、`textInput()`、`selectInput()`、
 * `checkboxField()`，样式类名还不完全一致（`tasks-field` vs `settings-form-*`）。
 * 结果是同一个"标签 + 控件 + 提示"的组合在两处长得不完全一样，且新增一种控件（如开关）
 * 要在两个文件里各写一遍。
 *
 * 这里只抽**无业务语义**的部分：怎么摆、怎么标错、怎么读值。字段的业务校验（如"脚本型必须填脚本"）
 * 仍留在各面板的纯逻辑模块（tasks-core.ts 等），因为那是业务规则不是控件职责。
 */

import { h, on } from "./dom"

/** 一个字段的容器：`wrap` 用于挂到网格里，`setError` 用于就地标错。 */
export interface FieldRef {
  wrap: HTMLElement
  setError: (msg: string | null) => void
}

/** 字段容器：标签 + 控件 + 可选提示 + 错误位。 */
export function field(label: string, control: HTMLElement, hint?: string, opts: { wide?: boolean } = {}): FieldRef {
  const error = h("div", { class: "fw-field-error", hidden: true })
  const wrap = h(
    "div",
    { class: `fw-field${opts.wide ? " wide" : ""}` },
    h("div", { class: "fw-field-label", text: label }),
    control,
    hint ? h("div", { class: "fw-field-hint", text: hint }) : null,
    error,
  )
  return {
    wrap,
    setError: (msg) => {
      if (msg) {
        error.textContent = msg
        error.hidden = false
        wrap.classList.add("bad")
      } else {
        error.textContent = ""
        error.hidden = true
        wrap.classList.remove("bad")
      }
    },
  }
}

/** 单行输入。 */
export function textInput(value = "", placeholder = "", opts: { type?: string; mono?: boolean } = {}): HTMLInputElement {
  return h("input", {
    class: `fw-input${opts.mono ? " mono" : ""}`,
    type: opts.type ?? "text",
    value,
    placeholder,
    autocomplete: "off",
  }) as HTMLInputElement
}

/** 多行输入。 */
export function textArea(value = "", rows = 4, placeholder = ""): HTMLTextAreaElement {
  const el = h("textarea", { class: "fw-input fw-textarea", rows, placeholder }) as HTMLTextAreaElement
  el.value = value
  return el
}

/** 下拉选择（options 为 [值, 文案] 数组）。 */
export function selectInput(options: Array<[string, string]>, value: string): HTMLSelectElement {
  const sel = h("select", { class: "fw-input" }) as HTMLSelectElement
  for (const [v, label] of options) sel.appendChild(h("option", { value: v, text: label }))
  sel.value = value
  return sel
}

/** 开关（复选框 + 文案）。 */
export function checkbox(label: string, checked: boolean): { root: HTMLElement; input: HTMLInputElement } {
  const input = h("input", { type: "checkbox", checked }) as HTMLInputElement
  const root = h("label", { class: "fw-check" }, input, h("span", { text: label }))
  return { root, input }
}

/** 分段按钮组（等价单选的按钮条，比 select 更省一次点击）。 */
export function segmented<T extends string>(
  options: Array<[T, string]>,
  value: T,
  onChange: (v: T) => void,
): { root: HTMLElement; set: (v: T) => void } {
  const buttons: HTMLButtonElement[] = []
  const root = h("div", { class: "fw-seg" })
  const set = (v: T): void => {
    for (const b of buttons) b.classList.toggle("active", b.dataset.value === v)
  }
  for (const [v, label] of options) {
    const b = h("button", { class: "fw-seg-btn", type: "button", text: label, data: { value: v } }) as HTMLButtonElement
    const off = on(b, "click", () => {
      set(v)
      onChange(v)
    })
    b.addEventListener("DOMNodeRemoved", off, { once: true })
    buttons.push(b)
    root.appendChild(b)
  }
  set(value)
  return { root, set }
}

/** 一行按钮（确认/取消/危险操作）。 */
export function buttonRow(...buttons: HTMLElement[]): HTMLElement {
  return h("div", { class: "fw-btn-row" }, ...buttons)
}

/** 文字按钮。 */
export function button(label: string, opts: { kind?: "primary" | "danger" | "ghost"; onClick?: () => void; tip?: string } = {}): HTMLButtonElement {
  const b = h("button", {
    class: `fw-btn${opts.kind && opts.kind !== "primary" ? ` ${opts.kind}` : opts.kind === "primary" ? " primary" : ""}`,
    type: "button",
    text: label,
  }) as HTMLButtonElement
  if (opts.tip) {
    b.dataset.tip = opts.tip
    b.setAttribute("aria-label", opts.tip)
  }
  if (opts.onClick) b.addEventListener("click", opts.onClick)
  return b
}
