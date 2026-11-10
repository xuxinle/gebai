---
title: Breadcrumb · 导航与组织
summary: 面包屑导航：分段路径 + 分隔符 + 「末段即当前位置」语义。
---

# Breadcrumb

面包屑导航：分段路径 + 分隔符 + 「末段即当前位置」语义。

## 头与实现

- 头：`include/st/ui/components/breadcrumb.hpp`
- 实现：`src/ui/components/breadcrumb.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Breadcrumb`

```cpp
#include "st/ui/components/breadcrumb.hpp"
// 构造：Breadcrumb()
auto w = std::make_unique<st::ui::Breadcrumb>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`segments` · `current` · `separator` · `last_clickable`

**只读**（能 `get`，`set` 不生效）：`count`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`goto_crumb` · `goto` · `crumb` · `back` · `up`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/navigation.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
