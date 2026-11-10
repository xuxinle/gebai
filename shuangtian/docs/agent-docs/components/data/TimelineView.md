---
title: TimelineView · 数据展示
summary: TimelineView 的构造、属性面、动作面与源码位置。
---

# TimelineView

## 头与实现

- 头：`include/st/ui/components/timeline_view.hpp`
- 实现：`src/ui/components/timeline_view.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`TimelineView`

```cpp
#include "st/ui/components/timeline_view.hpp"
// 构造：TimelineView()
auto w = std::make_unique<st::ui::TimelineView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`playhead` · `pixels_per_second` · `scroll` · `tracks` · `duration` · `snap` · `hover_clip` · `trim_handles` · `track_headers`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`zoom_in` · `zoom_out` · `play` · `scroll_to`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/data.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
