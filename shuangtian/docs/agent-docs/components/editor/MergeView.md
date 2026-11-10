---
title: MergeView · 编辑器与文档视图
summary: MergeView 的构造、属性面、动作面与源码位置。
---

# MergeView

## 头与实现

- 头：`include/st/ui/components/merge_view.hpp`
- 实现：`src/ui/components/merge_view.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`MergeView`

```cpp
#include "st/ui/components/merge_view.hpp"
// 构造：MergeView()
auto w = std::make_unique<st::ui::MergeView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`conflicts` · `resolved` · `pending` · `segments` · `current` · `show_base` · `scroll`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`take_ours` · `take_theirs` · `take_ours_all` · `take_theirs_all` · `next_conflict` · `previous_conflict`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/editor.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
