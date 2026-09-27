/**
 * 标题栏最右的「按钮轮盘」入口：hover `#wheel-btn` 展开扇形快捷菜单。
 *
 * 内圈 = 会话操作组（导出 / 待办 / 任务），外圈 = 应用操作组（自动审批 / 主题 / 快捷链 / 设置 / 登出）；
 * 分组边界必换圈，所以两组不会被混在同一圈里。半径与分圈全在 `wheel-core.ts` 里算
 *（首圈 85px，之后每圈 + 按钮边长 + 间隙 步进；每圈容量按该圈半径与可用张角算，放不下的自动落到下一圈）——
 * 几何与交互（hover 展开、离开保持区收起、外点 / Esc / resize 关闭）也都在那里，
 * 文件工作台标签栏的动作轮盘用同一套，这里只负责点名。
 */
import { createWheel } from "./wheel-core"

export function bindWheel() {
  const wheelBtn = document.getElementById("wheel-btn") as HTMLButtonElement | null
  if (!wheelBtn) return
  const pick = (ids: string[]): HTMLButtonElement[] =>
    ids.map((id) => document.getElementById(id)).filter((b): b is HTMLButtonElement => !!b)
  const inner = pick(["export-btn", "todo-btn", "tasks-btn"])
  const outer = pick(["approval-skip", "theme-btn", "shortcuts-btn", "settings-btn", "logout-btn"])
  // 半径与分圈交给 wheel-core（首圈 85px，之后每圈自动步进）：项数或窗口变化都不需要改这里
  createWheel({
    trigger: wheelBtn,
    items: [...inner.map((el) => ({ el, group: "inner" as const })), ...outer.map((el) => ({ el }))],
  })
}
