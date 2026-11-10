---
title: 下拉选择族 · Select/SelectPanel
summary: 选项放不下时用 Select；它是"按钮 + 弹出面板"的组合，面板也可单独用于弹出列表。
---

# 下拉选择（select）

## 选型

| 情况 | 用 |
|---|---|
| 选项 > 5 个，或选项文字较长 | `Select`（点击开面板、选中回调） |
| 选项 ≤ 4 个且要一眼看全 | `SegmentedControl`（见输入族） |
| 二选一且互斥、需要"立刻生效"感 | `Switch`（见 toggle 族） |
| 要弹出**任意列表**（不只是选项） | `SelectPanel`（`surface` + `shadow_md` + 选项行） |

## 机制

- `Select` 自己持有展开状态，点开时把 `SelectPanel` 挂成浮层（overlay），并负责点外部关闭。
- 选中后回调把值交给应用；**组件不替你持久化**。
- `SelectPanel` 的选项行高 32，hover 走 `surface_alt`、选中走 `primary_soft`——想换色改令牌，不要改组件。

## 坑

| 坑 | 说明 |
|---|---|
| 在滚动容器里直接绝对定位弹层 | 会被父级裁剪；浮层要挂到 root 级 overlay |
| 选项 id 用序号当身份 | 列表重排后选中会串台；用业务 key |
| 无头下断言"面板开了没" | 看 `tree` 里有没有 `SelectPanel` 节点，或断言 `Select` 的 `open` 类属性；不要靠截图猜 |

## 本族清单

`Select` · `SelectPanel`

- 逐个：`components/select/Select.md` · `components/select/SelectPanel.md`

## 更细

- 浮层与遮罩的生命周期：`DESIGN.md` §4.5.2（声明式 overlay）
- 选择器与浮层的自动化定位：`overview/control-protocol.md`
