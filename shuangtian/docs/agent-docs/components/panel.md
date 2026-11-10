---
title: 框架容器族 · Panel/Spacer
summary: Panel（行/列布局）与 Spacer（弹性/固定占位）。定义在 element.hpp，聚合头只做登记。
---

# 框架容器（panel）

## 两个容器

| 容器 | 干什么 | 注意 |
|---|---|---|
| `Panel` | 行/列布局面板（`direction` + `gap` + `align`/`justify`） | 布局由 flex 规则算；不要给子元素手算 x/y |
| `Spacer` | 弹性/固定占位 | `grow` 撑开空白、固定尺寸挤出间距；**默认值曾使右对齐静默失效**（已修） |

## 用法要点

- **右对齐**：`Panel { ...children, Spacer(grow=1), Button }`；
  不要用固定 `margin-left`（窗口一宽就错）。
- **两行行项**：`Panel(column) { Button(上行) + Text(下行) }`——`Button` 只画一行 label。
- **分组间距**：用 `gap`，不要给每个子元素加 `margin`（间距会双份）。

## 坑

| 坑 | 说明 |
|---|---|
| 给 `Panel` 的子元素手写 `x`/`y` | flex 会覆盖，且下次重排就丢 |
| `Spacer` 当"空白 Text"用 | 空 `Text` 仍有行高与基线，会多出间距 |
| 深度嵌套 `Panel` 做表格 | 列宽会各算各的；表格用 `Table` |

## 本族清单

`Panel` · `Spacer`

- 逐个：`components/panel/Panel.md` · `components/panel/Spacer.md`

## 更细

- 布局与样式字段（`Style`）：`include/st/ui/style.hpp`
- 行排版分层（"一行文字坐哪儿"的唯一实现）：`DESIGN.md` §4.5.0
- 元素身份与交互元素：`DESIGN.md` §4.5.1
