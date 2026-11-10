---
title: 开关与勾选族 · Checkbox/Radio/Switch
summary: 选一个还是多个、立即生效还是提交生效——两个问题定控件。
---

# 开关与勾选（toggle）

## 选型（两个问题定）

| 立即生效？ | 几个可选 | 用 |
|---|---|---|
| 是（改了就跑） | 一个布尔 | `Switch` |
| 否（随表单提交） | 一个布尔 | `Checkbox` |
| 否 | 多个布尔（互不排斥） | 多个 `Checkbox` |
| 否 | 多选一 | `RadioGroup`（**不要自己管多个 `Radio` 的互斥**） |
| 是 | 多选一、选项少且要一眼看全 | `SegmentedControl`（见输入族） |

`Radio` 单独用时不组成组——它的互斥由调用方 `set_checked` 管理。要做"一组只能选一个"，用 `RadioGroup`。

## 组合套路

- **设置面板**：`FormRow(label) { Switch }`——标签排版统一交给 `FormRow`。
- **偏好项列表**：一列 `Checkbox(label)`，行高与 `ListItem` 对齐（40px）。
- **分段筛选**：`SegmentedControl` 比三个 `Radio` 更省空间，且自带滑块指示。

## 坑

| 坑 | 说明 |
|---|---|
| 自己维护 `Radio` 互斥 | 漏一处就会同时选中两个；用 `RadioGroup` |
| 用 `Switch` 承载"提交后生效"的语义 | 用户会以为已经生效了；这类用 `Checkbox` |
| 只改布尔值不通知 | 状态变了要 `on_change`（或声明式里回写 state），否则界面与实际不一致 |

## 本族清单

`Checkbox` · `Radio` · `RadioGroup` · `Switch`

- 逐个：`components/toggle/Checkbox.md` · `components/toggle/Radio.md` · `components/toggle/RadioGroup.md` · `components/toggle/Switch.md`

## 更细

- 交互态底色与指针态生命周期：`DESIGN.md` §5.0.1 · §5.0.2
- 焦点环只在键盘导航时出现（`§4.2.17`）
- 表单排版：`components/input.md`（`FormRow`）
