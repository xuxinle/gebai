---
title: Radio · 开关与勾选
summary: 单选框：圆形外圈 + 选中内圆点。同组互斥由调用方用 `set_checked` 管理（本组件不做分组）。
---

# Radio

单选框：圆形外圈 + 选中内圆点。同组互斥由调用方用 `set_checked` 管理（本组件不做分组）。

## 头与实现

- 头：`include/st/ui/components/radio.hpp`
- 实现：`src/ui/components/toggle.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Radio`

```cpp
#include "st/ui/components/radio.hpp"
// 构造：Radio(std::string label = {})
auto w = std::make_unique<st::ui::Radio>();
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
