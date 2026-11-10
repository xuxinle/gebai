---
title: 输入与取值族 · 文本/数值/连续量
summary: 按"取值空间"选控件：无界文本、有界数值、连续量、离散枚举、集合。表单统一用 FormRow 包。
---

# 输入与取值（input）

## 按取值空间选

| 取值空间 | 用 | 要点 |
|---|---|---|
| 任意文本，单行 | `Input` | 有 `set_text` 类接口会重置光标/撤销栈——只在"值真变了"时写 |
| 任意文本，多行 | `TextArea`（自动换行 + 内部滚动，光标始终可见） | 同上 |
| 要搜索的文本 | `SearchBox`（放大镜 + 清除 + 结果计数徽标） | 计数徽标要由应用喂，组件不算结果 |
| 有界数值 | `NumberInput`（文本编辑 + 步进 + 区间夹取 + 精度/后缀） | 夹取在组件内做，不要在回调里再手算 |
| 连续量（0~1 或自定义区间） | `Slider`（键盘 `←/→` 按 `step`、`Home/End` 到两端） | 拖拽过程中**逐次**通知 `on_change` |
| 少量离散枚举 | `SegmentedControl`（自带滑块指示） | 选项宽度会均分；文字要短 |
| 打分 | `Rating`（星标、半星、只读、键盘可调） | `allow_half` 打开才是半星 |
| 颜色 | `ColorPicker`（色相条 + SV 块 + 十六进制 + 吸管） | 吸管需要平台能力，无头下不可用 |
| 一组标签 | `TagInput`（Chip + 末尾空位） | `max` 到限要给出提示，否则用户以为卡了 |

**表单里一律用 `FormRow` 包**：左侧标签、可选说明/错误、三档标签宽度策略——自己排会出现
"标签宽度不一致、错误提示挤在一起"。

## 组合套路

**筛选栏**：`Toolbar { SearchBox + SegmentedControl + Button }`，`flex_grow` 给 `SearchBox`。

**设置表单**：一列 `FormRow`；保存按钮放底部右对齐（`Spacer` 撑开，不要用固定 margin）。

**参数调整**：`FormRow(label) { Slider + NumberInput }`——滑杆给手感、数值框给精确输入，两者共享同一状态。

## 坑

| 坑 | 说明 |
|---|---|
| 每帧无脑 `set_text` | 光标跳回行首、撤销历史消失、视口回顶。只能"值真变了"时写；要保位置用 `replace_text_keep_state` |
| 在回调里重复夹取 | `NumberInput`/`Slider` 已经夹取；再夹一次会让"越界输入"变成静默修正 |
| `Slider` 每步都刷整页 | 进度类界面变化频繁时，只让邻近元素重绘（`mark_dirty` 精确到元素） |
| 用 `Input` 承载多行 | 它没有内部滚动，多行会溢出；用 `TextArea` |

## 本族清单

`Input` · `TextArea` · `SearchBox` · `NumberInput` · `Slider` · `Rating` · `ColorPicker` · `TagInput` ·
`SegmentedControl` · `FormRow`

- 逐个：`components/input/Input.md` · `components/input/TextArea.md` · `components/input/SearchBox.md` · `components/input/NumberInput.md`
- 逐个：`components/input/Slider.md` · `components/input/Rating.md` · `components/input/ColorPicker.md` · `components/input/TagInput.md`
- 逐个：`components/input/SegmentedControl.md` · `components/input/FormRow.md`

## 更细

- 文本编辑模型（选中/撤销/替换语义）：`DESIGN.md` §4.2.17g · §4.2.17i
- 单行文本的垂直居中口径：`DESIGN.md` §5.0.3
- 声明式里"值真变了才写"的做法：`overview/ui-writing.md`
