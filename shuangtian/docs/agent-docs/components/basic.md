---
title: 基础展示族 · 文本/标题/按钮/卡片
summary: 界面的地基：文字层级、按钮变体、容器与分隔。选型先看"这一块要不要可点"。
---

# 基础展示（basic）

## 选型

| 我要显示… | 用 | 不用 |
|---|---|---|
| 一段正文 | `Text`（可 `set_multiline` 折行、可 `set_selectable` 选中复制） | —— |
| 标题层级 | `Heading`（`level` 决定字号与字重） | 手调 `Text` 的字号（层级会漂） |
| 图标 | `IconView`（内置图标按名取） | 贴图片（高 DPI 下糊） |
| 一个可点的动作 | `Button`（5 变体 × 3 尺寸） | 给 `Text` 挂点击（没有命中/焦点/键盘语义） |
| 一块带底/边/阴影的内容区 | `Card` | 自己画 `background` + `radius`（圆角/描边像素对齐会错） |
| 一条分割 | `Divider`（`vertical` 可选） | 空 `Text` |
| 带标题的分割 | `DividerLabel` | `Divider` + 两个 `Text` 手工对齐 |
| 详情面板的一行 | `KeyValueRow` | 两个 `Text` 靠 `flex_grow` 抢宽度 |
| 展示快捷键 | `Kbd`（组合键序列） | 手写 `"Ctrl+S"` 字符串（按键帽样式不统一） |
| 一个链接 | `Link`（可带外部跳转标记） | 蓝色 `Text`（不可点、无悬停） |

## 组合套路

**列表行**：`Button(variant=Ghost, align=Start) + grow` 铺满一行，比"Panel 里塞 Text + on_event"稳定
——但**只画一行 label**；要"标题 + 副标题"两行时用
`column { Button(上行) + Text(下行) }`，不要试图给 `Button` 塞子元素（它没有透明命中层）。

**统计卡**：`Card { column { Text(muted, 小) + Heading(value) + Badge(变化量) } }`。

**详情面板**：一列 `KeyValueRow`，键固定宽、值 `flex_grow`——不要每行手算宽度。

## 坑

| 坑 | 说明 |
|---|---|
| `Button` 只画一行 label | 塞两段 Text 期望堆叠不成立（实测：内容跑到左侧贴中轴线） |
| `Button::Variant` 曾与 `ui::Variant` 同名不同表 | 已统一到 `ui::Variant`；XML 写 `variant="ghost"` 与 `get_property("variant")` 现在同源 |
| `Button` 的 `size` 是**尺寸档位**，`Text` 的 `size` 是**字号** | 同名不同义，各自解释；新代码用 `font_size` |
| 焦点环只在键盘导航时出现 | 想断言"焦点在不在"用 `:focused` 选择器，不要看有没有边框 |

## 本族清单

`Text` · `Heading` · `Icon` · `Button` · `Card` · `Divider` · `KeyValueRow` · `DividerLabel` · `Kbd` · `Link`

- `components/basic/Text.md` · `components/basic/Heading.md` · `components/basic/Icon.md` · `components/basic/Button.md` · `components/basic/Card.md`
- `components/basic/Divider.md` · `components/basic/KeyValueRow.md` · `components/basic/DividerLabel.md` · `components/basic/Kbd.md` · `components/basic/Link.md`

## 更细

- 设计令牌与主题（对比度/层次/阴影分层）：`DESIGN.md` §4.2.4 · §5
- 图标口径（圆头描边 + 光学归一化）：`DESIGN.md` §5.0.4
- 声明式的四种典型写法：`overview/ui-writing.md`
- 组件按需与注册机制：`include/st/ui/components/README.md`
