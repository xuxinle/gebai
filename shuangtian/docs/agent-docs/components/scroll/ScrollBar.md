---
title: ScrollBar · 滚动
summary: 垂直滚动条：轨道 + 圆角 pill 滑块（宽 8px，hover/拖拽时 10px）。 几何由 `set_scroll_geometry` 注入（内容高 / 视口高 / 偏移），滚动条本身不持有内容。
---

# ScrollBar

垂直滚动条：轨道 + 圆角 pill 滑块（宽 8px，hover/拖拽时 10px）。 几何由 `set_scroll_geometry` 注入（内容高 / 视口高 / 偏移），滚动条本身不持有内容。

## 头与实现

- 头：`include/st/ui/components/scroll_bar.hpp`
- 实现：`src/ui/components/scroll.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`ScrollBar`

```cpp
#include "st/ui/components/scroll_bar.hpp"
// 构造：ScrollBar()
auto w = std::make_unique<st::ui::ScrollBar>();
```

## 属性面（控制通道 `get` / `set`）

本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/scroll.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
