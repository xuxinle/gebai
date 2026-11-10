---
title: Tree · 导航与组织
summary: Tree 的构造、属性面、动作面与源码位置。
---

# Tree

## 头与实现

- 头：`include/st/ui/components/tree.hpp`
- 实现：`src/ui/components/tree.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Tree`

```cpp
#include "st/ui/components/tree.hpp"
// 构造：Tree()
auto w = std::make_unique<st::ui::Tree>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`rows` · `count` · `row_height` · `first_visible` · `visible_rows` · `scroll` · `selected_key` · `selected` · `labels` · `keys` · `rows_detail` · `activate_key`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 设计章节

`DESIGN.md` §11

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/navigation.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
