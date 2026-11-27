---
title: Divider · 基础展示
summary: 水平分隔线（`vertical = true` 时为竖线）。
---

# Divider

水平分隔线（`vertical = true` 时为竖线）。

## 头与实现

- 头：`include/st/ui/components/divider.hpp`
- 实现：`src/ui/components/basic.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Divider`

```cpp
#include "st/ui/components/divider.hpp"
// 构造：Divider(bool vertical = false)
auto w = std::make_unique<st::ui::Divider>();
```

## 属性面（控制通道 `get` / `set`）

本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 头注释里的逐条取舍（读源码前先看这里）

- 「量法：铺满“有上界”的那个方向，另一个方向固定线厚」— `include/st/ui/components/divider.hpp:17`

> 这些是**为什么这样做**的记录（多半是踩过的坑）。改行为之前逐条读，否则会重新踩一遍。

## 设计章节

`DESIGN.md` §4.2.6

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/basic.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
