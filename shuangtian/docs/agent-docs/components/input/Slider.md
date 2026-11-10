---
title: Slider · 输入与取值
summary: 单值滑杆：6px 圆角轨道 + 已选段 `primary` + 16px 圆滑块（`shadow_sm`，拖拽时主色光圈）。
---

# Slider

单值滑杆：6px 圆角轨道 + 已选段 `primary` + 16px 圆滑块（`shadow_sm`，拖拽时主色光圈）。

## 头与实现

- 头：`include/st/ui/components/slider.hpp`
- 实现：`src/ui/components/slider.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Slider`

```cpp
#include "st/ui/components/slider.hpp"
// 构造：Slider(float value = 0.0f)
auto w = std::make_unique<st::ui::Slider>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`value` · `step` · `label`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`increment` · `increase` · `next` · `decrement` · `decrease` · `previous` · `set`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 设计章节

`DESIGN.md` §4.5 · `DESIGN.md` §8.2

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/input.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
