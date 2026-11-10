---
title: 组件总表 · 按用途分组
summary: 76 个已注册组件 + 4 个不进注册表的三维族，按"我要做什么"分组；每组先给选型建议再指向具体组件页。
---

# 组件总表

## 先读三条共用规则

1. **用哪个**：先看下面分组表的"何时用"，再点进族页读"怎么选"；
2. **怎么用**：`new` 直接建（自动注册）；只有"运行期按名字建"（XML / `ui.create` / JS）才需要
   `import_components<T>()` 把那个 `.o` 拉进产物；
3. **怎么验**：每个组件页都列了**属性面**（`get`/`set`）与**动作面**（`invoke`）——
   自动化就用它们，不要靠坐标猜。

## 分组

### 基础展示（`components/basic.md`）

`Text` `Heading` `Icon` `Button` `Card` `Divider` `KeyValueRow` `DividerLabel` `Kbd` `Link`

> 任何界面的地基。文字用 `Text`/`Heading`，容器用 `Card`，按钮用 `Button`（5 变体 × 3 尺寸）。

### 反馈与状态（`components/feedback.md`）

`ProgressBar` `Spinner` `Badge` `Chip` `Avatar` `AvatarGroup` `Banner` `EmptyState` `Skeleton`
`ProgressRing` `Sparkline` `StatusDot` `Toast` `Tooltip`

> 让用户知道"发生了什么/进展到哪/没有数据"。加载用 `Skeleton`/`Spinner`，完成度用 `ProgressBar`/`ProgressRing`，
> 空态用 `EmptyState`（不是"空白页"）。

### 开关与勾选（`components/toggle.md`）

`Checkbox` `Radio` `RadioGroup` `Switch`

> 布尔值用 `Switch`（立即生效）或 `Checkbox`（提交生效）；多选一用 `RadioGroup`。

### 输入与取值（`components/input.md`）

`Input` `TextArea` `SearchBox` `NumberInput` `Slider` `Rating` `ColorPicker` `TagInput`
`SegmentedControl` `FormRow`

> 连续量用 `Slider`，离散量用 `SegmentedControl`，文本用 `Input`/`TextArea`，
> 表单统一用 `FormRow` 包（标签/说明/错误的排版别自己写）。

### 下拉选择（`components/select.md`）

`Select` `SelectPanel`

> 选项多到放不下时用 `Select`；`SelectPanel` 是它的下拉面板（也可单独用来做弹出列表）。

### 列表（`components/list.md`）

`List` `ListItem`

> 几十条以内、行高固定、需要选中态与键盘导航时用它；上百条或要虚拟滚动用 `Table`/`Tree`/`ScrollView` 方案。

### 滚动（`components/scroll.md`）

`ScrollView` `ScrollBar`

> 内容超视口就套 `ScrollView`；`ScrollBar` 单独用只在自绘面板里。

### 浮层（`components/overlay.md`）

`Dialog`

> 需要用户确认/填写后继续时用 `Dialog`（模态、遮罩、按钮行）。轻提示用 `Toast`，气泡提示用 `Tooltip`。

### 导航与组织（`components/navigation.md`）

`Accordion` `Breadcrumb` `Pagination` `Steps` `Tabs` `Toolbar` `StatusBar`
`MenuPanel` `MenuBar` `ContextMenu` `CommandPalette` `Tree`

> 层级导航用 `Breadcrumb` + `Tabs`，分步流程用 `Steps`，长列表分页用 `Pagination`，
> 命令入口用 `CommandPalette`（键盘优先）或 `MenuBar`。

### 数据展示（`components/data.md`）

`Table` `TimelineView` `GraphView`

> 表格/时间轴/关系图。三者都是"外部数据模型 + 组件负责画与交互"。
> 时间轴是**委托编辑**形态（组件不持有真值，编辑请求回调给应用）。

### 编辑器与文档视图（`components/editor.md`）

`CodeEditor` `DiffView` `MergeView` `MarkdownView` `HtmlView` `Terminal` `FileDialog` `CompletionPopup`

> 这一族最重。`CodeEditor` 自带语法高亮/行号/选择/撤销；`DiffView`/`MergeView` 共用行装饰与三层坐标系；
> `MarkdownView` 支持流式 `append_chunk`（面向大模型输出）。

### 三维与连线（`components/3d.md`）

`ThreeView` `SceneView` `LabelOverlay` `Connector`

> 前三个**不在声明式注册表里**（无头环境按名建三维没有意义），由 C++ 直接 `new` 挂树；
> `Connector` 是纯二维连线（流程图/节点图），可声明式。

### 布局与窗口（`components/layout.md`）

`SplitView` `Viewport` `SlowContent` `ThemePicker` `WindowFrame` `TitleBar`

> 分栏用 `SplitView`（注意 measure 约束与 arrange 几何必须一致），
> 缩放平移画布用 `Viewport`，慢内容用 `SlowContent`（异步排版不卡帧）。

### 框架容器（`components/panel.md`）

`Panel` `Spacer`

> `Panel`（行/列布局）与 `Spacer`（弹性/固定占位）。定义在 `element.hpp`，聚合头只做登记。

## 完整清单（英文名 → 页）

76 个注册组件全部有页（漏一个就会被 `tests/docs_agent_tree_test.cpp` 抓住），另有三维族的 4 页
（`ThreeView` / `SceneView` / `LabelOverlay` / `GraphView`——它们**不进注册表**，由 C++ 直接 `new` 挂树）。
按名字直读 `components/<族>/<组件名>.md`；不确定族就看各页页头与上面分组表。

## 更细

- 组件的按需机制（include / import / 自动注册的边界）：`include/st/ui/components/README.md`
- 加一个新组件要做的五步：同上「四、加一个组件要做什么」
- 属性/动作口径：`overview/properties.md`
- 声明式里怎么摆组件：`overview/ui-writing.md`
