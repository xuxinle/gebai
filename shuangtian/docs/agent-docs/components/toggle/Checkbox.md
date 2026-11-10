---
title: Checkbox · 开关与勾选
summary: 复选框：18×18 圆角方框（圆角取 `min(metrics.radius_sm, 5)`），选中态填充 `primary` 并描白色对勾。
---

# Checkbox

复选框：18×18 圆角方框（圆角取 `min(metrics.radius_sm, 5)`），选中态填充 `primary` 并描白色对勾。

## 头与实现

- 头：`include/st/ui/components/checkbox.hpp`
- 实现：`src/ui/components/toggle.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Checkbox`

```cpp
#include "st/ui/components/checkbox.hpp"
// 构造：Checkbox(std::string label = {})
auto w = std::make_unique<st::ui::Checkbox>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`checked` · `value` · `label`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/toggle.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
