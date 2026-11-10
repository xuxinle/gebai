---
title: Toast · 反馈与状态
summary: 轻提示：`surface` 底 + `shadow_md` + 左侧 4px 色条（高 40、横向内边距 14）。 自动消失由帧时间轴驱动（`context.time_seconds`）：不额外起线程/定时器， 无头单帧也能用 `expired()` 判定（详见 `set_auto_dismiss_ms`）。
---

# Toast

轻提示：`surface` 底 + `shadow_md` + 左侧 4px 色条（高 40、横向内边距 14）。 自动消失由帧时间轴驱动（`context.time_seconds`）：不额外起线程/定时器， 无头单帧也能用 `expired()` 判定（详见 `set_auto_dismiss_ms`）。

## 头与实现

- 头：`include/st/ui/components/toast.hpp`
- 实现：`src/ui/components/overlay.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Toast`

```cpp
#include "st/ui/components/toast.hpp"
// 构造：Toast(std::string message = {}, Tone tone = Tone::Default)
auto w = std::make_unique<st::ui::Toast>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`message` · `text` · `value` · `tone` · `auto_dismiss_ms` · `expired`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/feedback.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
