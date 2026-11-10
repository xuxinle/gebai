---
title: FormRow · 输入与取值
summary: 表单行。
---

# FormRow

表单行。

## 头与实现

- 头：`include/st/ui/components/form_row.hpp`
- 实现：`src/ui/components/form_row.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`FormRow`

```cpp
#include "st/ui/components/form_row.hpp"
// 构造：FormRow()
auto w = std::make_unique<st::ui::FormRow>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`label` · `hint` · `error` · `required` · `label_width` · `stacked`

**只读**（能 `get`，`set` 不生效）：`effective_label_width`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/input.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
