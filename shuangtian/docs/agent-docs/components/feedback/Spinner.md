---
title: Spinner · 反馈与状态
summary: 旋转指示器：3/4 圆弧，相位完全由 `RenderContext::time_seconds` 决定（可截屏回归）。
---

# Spinner

旋转指示器：3/4 圆弧，相位完全由 `RenderContext::time_seconds` 决定（可截屏回归）。

## 头与实现

- 头：`include/st/ui/components/spinner.hpp`
- 实现：`src/ui/components/feedback.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Spinner`

```cpp
#include "st/ui/components/spinner.hpp"
// 构造：Spinner(float diameter = kDiameter)
auto w = std::make_unique<st::ui::Spinner>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`size` · `diameter` · `tone`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/feedback.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
