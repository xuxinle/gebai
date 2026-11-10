---
title: SegmentedControl · 输入与取值
summary: 分段控件。
---

# SegmentedControl

分段控件。

## 头与实现

- 头：`include/st/ui/components/segmented_control.hpp`
- 实现：`src/ui/components/segmented_control.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`SegmentedControl`

```cpp
#include "st/ui/components/segmented_control.hpp"
// 构造：SegmentedControl()
auto w = std::make_unique<st::ui::SegmentedControl>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`options` · `index` · `stretch` · `orientation`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`select` · `set` · `step_forward` · `next` · `step_backward` · `prev` · `previous`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/input.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
