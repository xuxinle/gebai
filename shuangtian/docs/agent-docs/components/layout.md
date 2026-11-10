---
title: 布局与窗口族 · SplitView/Viewport/WindowFrame
summary: 分栏、缩放平移、慢内容异步排版、主题选择、窗框与标题栏。四条易错契约。
---

# 布局与窗口（layout）

## 选型

| 要什么 | 用 | 关键契约 |
|---|---|---|
| 两栏可拖动分隔 | `SplitView` | 给子面板的 **measure 约束必须与 arrange 几何一致** |
| 一块画布的缩放/平移（含棋盘格底） | `Viewport` | `zoom`/`pan_x`/`pan_y`；动作 `fit`/`reset` |
| 内容很慢（大文档/大表格）又要界面不卡 | `SlowContent` | 先占位后异步排版；`layout_pending` 观测 |
| 切主题（内置色卡墙） | `ThemePicker` | 十套内置主题；`change` 回调 |
| 组件化的窗口（自绘标题栏 + 边框 + 阴影） | `WindowFrame` | 阴影与圆角是**自绘**（不再用 DWM 方案） |
| 只要标题栏 | `TitleBar` | 三颗窗口钮的动作是 `minimize`/`maximize`/`close` |

## 四条契约

**① `SplitView` 的 measure 与 arrange 必须一致**：给子面板的约束宽度和应用时的几何不一致，
表现是"子面板内容按错误宽度排版"（文字换行位置错、选中背景错位）。改比例后要重排版。

**② `Viewport` 的缩放是"内容坐标 → 视口坐标"的映射**：内容自己按内容坐标画，
不要预先乘缩放——`Viewport` 会做变换。手算会得到"缩放两次"。

**③ `SlowContent` 的顺序**：先挂占位（可测量的空壳），异步结果到达后在**帧首**
（`pump_async` 那一步）换成真内容并标脏——顺序反了就是永远停在占位。

**④ 窗框纯自绘**：外围阴影与圆角由 `WindowFrame` 自己画，系统装饰被去掉；
所以"窗口外阴影没了"不是 bug，是你没用 `WindowFrame`。

## 坑

| 坑 | 说明 |
|---|---|
| `SplitView` 拖动时每帧重建子面板 | 会丢焦点/滚动位置；拖动只改比例，松手才重排 |
| `Viewport` 里放需要命中测试的控件 | 命中区要同步变换；别自己算反变换 |
| `SlowContent` 每次重建都重新排 | 会反复闪；按内容指纹判"要不要重排" |
| 用 `TitleBar` 但没设 `window_icon_png` | 任务栏图标空白（图标三条链路互不相通） |

## 本族清单

`SplitView` · `Viewport` · `SlowContent` · `ThemePicker` · `WindowFrame` · `TitleBar`

- 逐个：`components/layout/SplitView.md` · `components/layout/Viewport.md` · `components/layout/SlowContent.md`
- 逐个：`components/layout/ThemePicker.md` · `components/layout/WindowFrame.md` · `components/layout/TitleBar.md`

## 更细

- 分栏约束与几何一致：`DESIGN.md` §4.2.17f
- 慢内容异步排版：`DESIGN.md` §4.5.2.1
- 窗框纯自绘（取代 DWM 方案）与拖动缩放帧路径：`DESIGN.md` §4.6.5 · §4.6.1
- 主题自定义与 `theme_io`：`DESIGN.md` §5.1
- 卡片描边的像素对齐：`DESIGN.md` §5.0.5
