---
title: 数据展示族 · Table/Timeline/Graph/Mermaid
summary: 四者都是"外部数据 + 组件负责画与交互"；组件不持有领域真值，编辑请求回调给应用。
---

# 数据展示（data）

## 选型

| 数据形态 | 用 | 真值在哪 |
|---|---|---|
| 行 × 列的表格（可选中、可滚动、斑马纹） | `Table` | 应用（组件自己只管行数据与选中行） |
| 时间轴（轨道 + 片段 + 播放头） | `TimelineView` | **外部真值源**（`st::ui::timeline::Model`）+ 委托编辑 |
| 关系图（节点 + 连线，可转视角） | `GraphView`（三维图：线在三维、文字在屏幕） | 应用 |
| Mermaid 文本（流程图 / 时序图 / 饼图） | `MermaidView`（自绘解析 + 布局） | 文本本身（组件只解析与画） |

## Table

- 列定义 `TableColumn`，行由应用喂；`row_height` 固定，**虚拟滚动**（只画可见行）。
- 属性面给了 `first_visible` / `visible_rows` / `scroll`——断言"滚到哪了"用它们，不要数像素。
- 排序/筛选**不在组件里**：组件不会替你排序（否则真值有两份）。

## TimelineView

这一族最讲契约，三条最容易搞错：

1. **真值在外部模型**：组件不拥有轨道数据，只按 `Model` 画；改数据要通过模型再让组件刷新。
2. **委托编辑**：拖拽/修剪/移动产生的是**编辑请求**，组件不自己改数据——应用决定接受或拒绝。
3. **投影刷新防线**：模型变了必须显式让组件重投影（曾出现"模型改了画面不动"，原因是刷新路径漏了一段）。

## GraphView

- 布局由 `graph_layout` 算（力导向/分层），组件只画。
- 文字始终在屏幕空间（不随三维旋转变形）；连线在三维空间。
- 节点拾取用组件的 `picked` 属性面，不要用屏幕坐标反推。

## MermaidView

- 给一段 Mermaid 文本，自己解析 + 布局 + 画：`flowchart`/`graph`（TD/LR/RL/BT）、
  `sequenceDiagram`、`pie`。**不依赖浏览器/JS**（无头环境照样出图、截图可回归）。
- 解析/布局在 `st::md::mermaid`（纯计算，可单测），组件只负责画与交互。
- 不支持的图类型与语句**进 `diagnostics`**（属性面 `error` 读首条）：能画的照画，缺的部分有文字线索，
  绝不静默空白。
- `fit`（默认开，只缩不放）与 `zoom` 是**两个量**：窗口变化自动适配，用户放大不被清掉。

## 坑

| 坑 | 说明 |
|---|---|
| 在 `Table` 里放可编辑控件 | 滚动复用时控件状态会串台；编辑走"选中行 + 弹层/侧栏" |
| 时间轴拖拽时边改模型边刷新 | 会与拖拽状态打架；拖拽中只更新"预览"，松手才提交 |
| 用 `Table` 展示 10 万行 | 虚拟滚动是按行高算的，行高不一致会错位；固定行高 |
| 把 Mermaid 当作"什么都支持" | 只实现了三类图的可执行子集；`subgraph`/`loop`/`class`/样式指令都进 `diagnostics`——先看 `error` 再排 |

## 本族清单

`Table` · `TimelineView` · `GraphView` · `MermaidView`

- 逐个：`components/data/Table.md` · `components/data/TimelineView.md` · `components/data/GraphView.md` ·
  `components/data/MermaidView.md`

## 更细

- 时间轴的四块落地记录（播放头/头部栏/编辑委托/投影刷新）：`DESIGN.md` §4.2.25–§4.2.27
- 图布局算法：`include/st/ui/graph_layout.hpp`
- Mermaid 的解析/布局层：`include/st/md/mermaid.hpp`（`DESIGN.md` §4.4.1）
- 裁云（时间轴的重度使用者）：`docs/caiyun_ONBOARDING.md`
