/**
 * 待办状态 → 图标/文案（纯逻辑，无 DOM）：会话内待办卡片（tool-cards.ts）查表用。
 *
 * **为什么单独成模块且必须兜底**：状态是渲染的枚举键（图标、语义色类名、折叠判定都按它查表）。
 * 数据里出现白名单外的值是真会发生的事——服务端曾把一个 `status: "deleted"`（模型把「删除」
 * 当状态用）原样落盘，前端 `TODO_STATUS_ICONS[status].svg` 直接抛 TypeError，**整个消息块的渲染
 * 就此中断**；窗口化（virtualize.ts）随后把这一大段永远留成空白。
 *
 * 结论：渲染层对陌生状态值的正确反应是「不炸 + 让人看见异常」，不是抛错。
 * 服务端已同步加白名单校验（core/tools/interact.ts 的 isTodoStatus），这里是第二道防线——
 * 旧数据、手工导入、未来新增状态都不能把界面炸掉。
 */

/** 状态图标：内联 SVG 描边风格（16×16、stroke currentColor，与全站图标语言一致、随主题着色），
 *  label 为中文状态标签（悬浮提示，替代元信息行中的状态词）。 */
export const TODO_STATUS_ICONS: Record<string, { svg: string; label: string }> = {
  pending: { svg: '<svg viewBox="0 0 16 16"><circle cx="8" cy="8" r="6"/></svg>', label: "待处理" },
  in_progress: { svg: '<svg viewBox="0 0 16 16"><path d="M13.5 8a5.5 5.5 0 1 1-1.6-3.9M13.5 1.5v3h-3"/></svg>', label: "进行中" },
  completed: { svg: '<svg viewBox="0 0 16 16"><circle cx="8" cy="8" r="6"/><path d="M5.3 8.4l1.9 1.9 3.5-4.3"/></svg>', label: "已完成" },
  failed: { svg: '<svg viewBox="0 0 16 16"><circle cx="8" cy="8" r="6"/><path d="M6 6l4 4M10 6l-4 4"/></svg>', label: "已失败" },
  cancelled: { svg: '<svg viewBox="0 0 16 16"><circle cx="8" cy="8" r="6"/><path d="M3.8 3.8l8.4 8.4"/></svg>', label: "已取消" },
}

/** 未知状态的兜底图标（带问号的圆）：不抛错，且把原值带进 label 便于定位脏数据来源。 */
export const UNKNOWN_STATUS_ICON = {
  svg: '<svg viewBox="0 0 16 16"><circle cx="8" cy="8" r="6"/><path d="M6.2 6.2a1.8 1.8 0 1 1 2.4 1.7c-.4.2-.6.5-.6.9v.4M8 11.4v.2"/></svg>',
  label: "未知状态",
}

/** 状态值是否在白名单内（渲染侧的判据；服务端另有同一份口径的校验）。 */
export function isKnownTodoStatus(status: unknown): status is keyof typeof TODO_STATUS_ICONS {
  return typeof status === "string" && Object.prototype.hasOwnProperty.call(TODO_STATUS_ICONS, status)
}

/** 取状态图标：未知状态回退兜底图标（**绝不抛错**），原值放进 label。 */
export function statusIcon(status: unknown): { svg: string; label: string } {
  if (isKnownTodoStatus(status)) return TODO_STATUS_ICONS[status]
  return { ...UNKNOWN_STATUS_ICON, label: `${UNKNOWN_STATUS_ICON.label}（${String(status ?? "空")}）` }
}

/** 图标元素的语义色类名后缀：未知状态统一归到 `unknown`（CSS 按 `--text-faint` 处理，见 chat.css）。 */
export function statusClassSuffix(status: unknown): string {
  return isKnownTodoStatus(status) ? status : "unknown"
}
