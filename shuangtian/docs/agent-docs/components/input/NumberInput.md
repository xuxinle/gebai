---
title: NumberInput · 输入与取值
summary: 数值输入框。
---

# NumberInput

数值输入框。

## 头与实现

- 头：`include/st/ui/components/number_input.hpp`
- 实现：`src/ui/components/number_input.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`NumberInput`

```cpp
#include "st/ui/components/number_input.hpp"
// 构造：NumberInput()
auto w = std::make_unique<st::ui::NumberInput>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`value` · `text` · `min` · `max` · `step` · `precision` · `suffix`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`step_up` · `increment` · `up` · `step_down` · `decrement` · `down` · `commit` · `submit`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/input.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
