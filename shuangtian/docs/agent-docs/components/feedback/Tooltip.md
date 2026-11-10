---
title: Tooltip · 反馈与状态
summary: 提示气泡：`set_target_rect` 后自动定位到目标上方居中；`set_active` 控制显隐。
---

# Tooltip

提示气泡：`set_target_rect` 后自动定位到目标上方居中；`set_active` 控制显隐。

## 头与实现

- 头：`include/st/ui/components/tooltip.hpp`
- 实现：`src/ui/components/feedback.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Tooltip`

```cpp
#include "st/ui/components/tooltip.hpp"
// 构造：Tooltip(std::string text = {})
auto w = std::make_unique<st::ui::Tooltip>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`text` · `value` · `active` · `target`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/feedback.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
