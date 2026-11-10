---
title: CompletionPopup · 编辑器与文档视图
summary: CompletionPopup 的构造、属性面、动作面与源码位置。
---

# CompletionPopup

## 头与实现

- 头：`include/st/ui/components/completion_popup.hpp`
- 实现：`src/ui/components/completion_popup.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`CompletionPopup`

```cpp
#include "st/ui/components/completion_popup.hpp"
// 构造：CompletionPopup()
auto w = std::make_unique<st::ui::CompletionPopup>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`items` · `visible_items` · `selected` · `selected_label` · `filter` · `detail` · `labels` · `visible_labels` · `scroll`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`accept` · `dismiss` · `cancel` · `next` · `prev` · `select`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/editor.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
