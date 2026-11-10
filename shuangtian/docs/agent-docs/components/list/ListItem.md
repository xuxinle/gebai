---
title: ListItem · 列表
summary: 列表项：单行标签 + 可选次行说明（仍固定 40px 高，说明用 `font_xs` 小字）。
---

# ListItem

列表项：单行标签 + 可选次行说明（仍固定 40px 高，说明用 `font_xs` 小字）。

## 头与实现

- 头：`include/st/ui/components/list_item.hpp`
- 实现：`src/ui/components/command_palette.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`ListItem`

```cpp
#include "st/ui/components/list_item.hpp"
// 构造：ListItem(std::string label, std::string subtitle = {})
auto w = std::make_unique<st::ui::ListItem>();
```

## 属性面（控制通道 `get` / `set`）

本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/list.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
