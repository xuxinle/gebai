---
title: SplitView · 布局与窗口
summary: 可拖拽分栏：两个面板 + 中间手柄，拖拽改变两者比例。
---

# SplitView

可拖拽分栏：两个面板 + 中间手柄，拖拽改变两者比例。

## 头与实现

- 头：`include/st/ui/components/split_view.hpp`
- 实现：`src/ui/components/split_view.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`SplitView`

```cpp
#include "st/ui/components/split_view.hpp"
// 构造：SplitView()
auto w = std::make_unique<st::ui::SplitView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`ratio` · `default_ratio` · `min_ratio` · `step` · `orientation` · `first_hidden` · `second_hidden` · `handle_size`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`toggle_first` · `toggle_second` · `toggle_panel` · `step_forward` · `increase` · `next` · `step_backward` · `decrease` · `previous` · `reset` · `center` · `set`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 设计章节

`DESIGN.md` §4.5.1

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/layout.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
