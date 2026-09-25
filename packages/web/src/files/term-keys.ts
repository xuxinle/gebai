/**
 * 文件工作台 · 终端键位表（纯数据，可被单测校验）。
 *
 * 为什么单独一层：终端里的键与 shell 的 readline 键争同一片键盘，又不能碰浏览器的保留组合，
 * 这两条约束是**可执行判定**（`keymap.validateKeymap` 会逐条检查「接管要声明、保留键报错」），
 * 但键位在面板里是运行时登记的——放进页面模块就再也进不了测试。故表与动作接口留在这一层。
 *
 * 取键取终端自己的惯例：`Ctrl+Shift+C/V`（并认 `Ctrl+Insert`/`Shift+Insert`）复制粘贴、`Ctrl+F` 搜索、
 * `Ctrl+=/-/0` 字号、`Ctrl+K` 清屏、`Ctrl+Shift+A` 全选、`Ctrl+Shift+` 新建。
 * 这些组合在浏览器里都有默认行为（页面查找、地址栏搜索、页面缩放、整页查找）但不是**保留命令**——
 * 按键先到页面，捕获阶段 `preventDefault` 即接管（DevTools 打开时 `Ctrl+Shift+C` 会被它抢走，属已知取舍）。
 * 只有 **Ctrl+C（中断当前命令）**与 shell 一致：浏览器不独占它（C 是编辑键，页面可接管）。
 *
 * **为什么不照抄 VSCode 的切标签键**：VSCode 用 `Ctrl+PageUp/PageDown`，它们在 Chromium 里是标签页
 * 切换的保留组合，页面收不到事件（见 `keymap.browserConflict` 的保留清单）——照抄等于给一套
 * 在浏览器里按不动的快捷键。故改取 `Ctrl+Shift+↑/↓`；关闭当前终端同理另取 `Alt+Shift+W`
 * （工作台全局的 `Alt+W` 在终端内按设计让位给 shell）。
 */
import type { KeyBinding } from "../keymap"

/** 面板动作句柄：键位表只注册一次（面板可能重建），动作按最新面板转发。 */
export interface TermActions {
  copy(): void
  paste(): void
  search(): void
  fontSize(delta: number): void
  fontReset(): void
  interrupt(): void
  clear(): void
  selectAll(): void
  newTab(): void
  closeTab(): void
  switchTab(delta: 1 | -1): void
  scroll(to: "top" | "bottom"): void
}

export const TERM_KEYS = {
  copy: ["Ctrl+Shift+C", "Ctrl+Insert"],
  paste: ["Ctrl+Shift+V", "Shift+Insert"],
  search: ["Ctrl+F"],
  fontUp: ["Ctrl+="],
  fontDown: ["Ctrl+-"],
  fontReset: ["Ctrl+0"],
  interrupt: ["Ctrl+C"],
  clear: ["Ctrl+K"],
  selectAll: ["Ctrl+Shift+A"],
  newTab: ["Ctrl+Shift+`", "Ctrl+Shift+~"],
  closeTab: ["Alt+Shift+W"],
  nextTab: ["Ctrl+Shift+↓"],
  prevTab: ["Ctrl+Shift+↑"],
  scrollTop: ["Ctrl+Shift+Home"],
  scrollBottom: ["Ctrl+Shift+End"],
} satisfies Record<string, string[]>

/** 终端键位绑定表：焦点限定在终端面板内、捕获阶段（要抢在 xterm / 工作台全局键之前）。 */
export function termKeyBindings(get: () => TermActions | null): KeyBinding[] {
  const focus: ["terminal"] = ["terminal"]
  const base = { group: "wb.term" as const, focus, phase: "capture" as const, browser: "override" as const }
  return [
    { ...base, id: "wb.term.copy", keys: TERM_KEYS.copy, label: "终端：复制选区", run: () => get()?.copy() },
    { ...base, id: "wb.term.paste", keys: TERM_KEYS.paste, label: "终端：粘贴", run: () => get()?.paste() },
    { ...base, id: "wb.term.search", keys: TERM_KEYS.search, label: "终端：搜索滚动缓冲", run: () => get()?.search() },
    { ...base, id: "wb.term.fontUp", keys: TERM_KEYS.fontUp, label: "终端：放大字号", run: () => get()?.fontSize(1) },
    { ...base, id: "wb.term.fontDown", keys: TERM_KEYS.fontDown, label: "终端：缩小字号", run: () => get()?.fontSize(-1) },
    { ...base, id: "wb.term.fontReset", keys: TERM_KEYS.fontReset, label: "终端：重置字号", run: () => get()?.fontReset() },
    { ...base, id: "wb.term.clear", keys: TERM_KEYS.clear, label: "终端：清屏", run: () => get()?.clear() },
    { ...base, id: "wb.term.selectAll", keys: TERM_KEYS.selectAll, label: "终端：全选", run: () => get()?.selectAll() },
    { ...base, id: "wb.term.newTab", keys: TERM_KEYS.newTab, label: "终端：新建终端", run: () => get()?.newTab() },
    { ...base, id: "wb.term.nextTab", keys: TERM_KEYS.nextTab, label: "终端：下一个标签", run: () => get()?.switchTab(1) },
    { ...base, id: "wb.term.prevTab", keys: TERM_KEYS.prevTab, label: "终端：上一个标签", run: () => get()?.switchTab(-1) },
    { ...base, id: "wb.term.scrollTop", keys: TERM_KEYS.scrollTop, label: "终端：滚动到顶部", run: () => get()?.scroll("top") },
    { ...base, id: "wb.term.scrollBottom", keys: TERM_KEYS.scrollBottom, label: "终端：滚动到底部", run: () => get()?.scroll("bottom") },
    // 关闭终端：Alt+Shift+W 在浏览器里没有任何绑定，不需要接管声明
    { ...base, id: "wb.term.closeTab", keys: TERM_KEYS.closeTab, label: "终端：关闭当前终端", browser: undefined, run: () => get()?.closeTab() },
    // Ctrl+C：编辑键（浏览器不独占），有选区即复制、否则中断——与 shell 的中断语义同键
    { ...base, id: "wb.term.interrupt", keys: TERM_KEYS.interrupt, label: "终端：中断当前命令（有选区时复制）", browser: undefined, run: () => get()?.interrupt() },
  ]
}
