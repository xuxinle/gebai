---
title: 列表族 · List/ListItem
summary: 行高固定、需要选中态与键盘导航的短列表；上百条要用虚拟滚动方案。
---

# 列表（list）

## 选型

| 数据规模 | 用 |
|---|---|
| ≤ 几十条、行高固定、要选中 + 键盘导航 | `List` + `ListItem` |
| 上千条 | `Table`（虚拟滚动）或 `Tree`（扁平数组驱动） |
| 需要层级 | `Tree` |
| 只是"一列可点的东西"且要自定义行内容 | `ScrollView` + 自己排（`List` 的行是定高 `ListItem`） |

## 用法要点

- `add_item` 追加，选中态与键盘导航**由容器统一管理**（不要自己在行上处理方向键）。
- `ListItem` 是"单行标签 + 可选次行说明"，**仍固定 40px 高**（说明用 `font_xs` 小字）。
  要真正的两行块，用 `ScrollView` 自己排。
- 选中回调给业务 key，不给下标。

## 坑

| 坑 | 说明 |
|---|---|
| 用 `List` 承载可编辑行 | 行内编辑要焦点/输入法/滚动跟随，`List` 不管这些；用 `Table` 或自定义行 |
| 行内放 `Button` 期望整行可点 | `Button` 会吃掉点击；要么整行用 `Button(Ghost, align=Start)`，要么行内留出明确的按钮区 |
| 选中项在数据更新后错位 | 按 key 维护选中，不要按 index |

## 本族清单

`List` · `ListItem`

- 逐个：`components/list/List.md` · `components/list/ListItem.md`

## 更细

- 列表与滚动在声明式里的 key 对齐：`docs/declarative.md` §6.1
- 虚拟滚动与视口：`components/data.md`（`Table`）· `components/scroll.md`
