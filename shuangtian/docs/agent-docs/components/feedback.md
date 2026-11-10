---
title: 反馈与状态族 · 进度/空态/提示
summary: 让用户知道"发生了什么、进展到哪、为什么没有内容"。这张表按"用户此刻的疑问"选控件。
---

# 反馈与状态（feedback）

## 按"用户的疑问"选

| 用户此刻想知道的 | 用 | 何时结束 |
|---|---|---|
| 还要等多久（已知比例） | `ProgressBar`（线性）· `ProgressRing`（环形/仪表） | 到 1.0 后自己消失或变完成态 |
| 还要等多久（未知） | `Spinner`（相位由 `time_seconds` 决定，**可截屏回归**） | 数据到达后替换 |
| 正在加载但要有内容骨架 | `Skeleton`（矩形/圆形/文本行，脉冲） | 真实内容到达后替换 |
| 这块为什么是空的 | `EmptyState`（图标 + 标题 + 说明 + 可选动作） | 有内容后替换 |
| 刚才那一瞬发生了什么 | `Toast`（轻提示，会自己消失） | 定时或用户关闭 |
| 这个字段/按钮是什么 | `Tooltip`（`set_target_rect` 后自动定位） | 鼠标离开 |
| 数量/状态摘要 | `Badge`（计数/标签）· `StatusDot`（语义色小圆 + 脉冲）· `Chip`（可关闭标签） | —— |
| 谁 | `Avatar`（首码点）· `AvatarGroup`（重叠 + "+N"） | —— |
| 需要持续提示（不是一闪而过） | `Banner`（左色条 + 图标 + 说明 + 关闭/动作） | 用户关闭或条件解除 |

## 组合套路

- **列表加载三态**：首次 `Skeleton` → 有数据渲染列表 → 空 `EmptyState`（**不要留白**）。
- **长任务**：`ProgressBar` + 可取消按钮 + `StatusBar` 里一行文字（进度与说明分开，进度条不承载文字）。
- **操作结果**：成功用 `Toast`（2~3 秒），失败用 `Banner`（持续到用户处理）——失败不该一闪而过。

## 坑

| 坑 | 说明 |
|---|---|
| `Spinner` 的相位来自 `RenderContext::time_seconds` | 它**故意**不按墙钟走：这样截屏与回归可复现。要"看起来在转"必须让帧循环推进时间（无头跑时时间源由 app 提供） |
| `Skeleton` 的脉冲是时间驱动 | 静止帧截图会看到"卡住"，那是正常的；断言用 `capture_hash` 前后两次比较 |
| `Toast` 自动消失 | 不要用它承载"必须被读到"的信息（如错误详情）——用 `Banner` |
| `Tooltip` 要先 `set_target_rect` | 没设目标时它不知道自己该去哪，不会自己找父元素 |

## 本族清单

`ProgressBar` · `Spinner` · `Badge` · `Chip` · `Avatar` · `AvatarGroup` · `Banner` · `EmptyState` ·
`Skeleton` · `ProgressRing` · `Sparkline` · `StatusDot` · `Toast` · `Tooltip`

- 逐个：`components/feedback/ProgressBar.md` · `components/feedback/Spinner.md` · `components/feedback/Badge.md` · `components/feedback/Chip.md`
- 逐个：`components/feedback/Avatar.md` · `components/feedback/AvatarGroup.md` · `components/feedback/Banner.md` · `components/feedback/EmptyState.md`
- 逐个：`components/feedback/Skeleton.md` · `components/feedback/ProgressRing.md` · `components/feedback/Sparkline.md`
- 逐个：`components/feedback/StatusDot.md` · `components/feedback/Toast.md` · `components/feedback/Tooltip.md`

## 更细

- 动画与过渡（CSS 式组件动画的三层职责）：`DESIGN.md` §4.2.9.2
- 悬停提示的平台级实现：`DESIGN.md` §4.2.18
- 观感默认值怎么定（不是拍脑袋）：`docs/perceptual_changes.md`
