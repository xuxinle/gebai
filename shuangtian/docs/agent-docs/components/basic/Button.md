---
title: Button · 基础展示
summary: 按钮（含变体与尺寸；支持前导图标与点击回调）。
---

# Button

按钮（含变体与尺寸；支持前导图标与点击回调）。

## 头与实现

- 头：`include/st/ui/components/button.hpp`
- 实现：`src/ui/components/basic.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Button`

```cpp
#include "st/ui/components/button.hpp"
// 构造：Button(std::string label, Variant variant = Variant::Primary, Size size = Size::Medium)
auto w = std::make_unique<st::ui::Button>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`label` · `text` · `icon` · `variant` · `scale` · `hint` · `tone`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`click` · `activate`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 头注释里的逐条取舍（读源码前先看这里）

- 「为何用 `ui::Variant` 而不是自己一套（2026-11-10，词表统一）」— `include/st/ui/components/button.hpp:21`

> 这些是**为什么这样做**的记录（多半是踩过的坑）。改行为之前逐条读，否则会重新踩一遍。

## 设计章节

`DESIGN.md` §4.2.6

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/basic.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
