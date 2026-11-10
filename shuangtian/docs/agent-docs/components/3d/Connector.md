---
title: Connector · 三维与连线
summary: 连一条线（端点、路由、箭头、虚线、命中）。
---

# Connector

连一条线（端点、路由、箭头、虚线、命中）。

## 头与实现

- 头：`include/st/ui/components/connector.hpp`
- 实现：`src/ui/components/connector.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Connector`

```cpp
#include "st/ui/components/connector.hpp"
// 构造：Connector()
auto w = std::make_unique<st::ui::Connector>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`route` · `from` · `to` · `arrow` · `arrow_size` · `dash` · `width` · `smooth` · `curvature` · `min_segment` · `tone` · `stroke_color` · `hit_tolerance` · `label` · `source_side` · `target_side`

**只读**（能 `get`，`set` 不生效）：`source_ok` · `target_ok` · `path_length`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/3d.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
