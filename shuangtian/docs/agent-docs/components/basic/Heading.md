---
title: Heading · 基础展示
summary: 标题（按层级取字号与字重）。
---

# Heading

标题（按层级取字号与字重）。

## 头与实现

- 头：`include/st/ui/components/heading.hpp`
- 实现：`src/ui/components/basic.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Heading`

```cpp
#include "st/ui/components/heading.hpp"
// 构造：Heading(std::string content = {}, std::uint32_t level = 1)
auto w = std::make_unique<st::ui::Heading>();
```

## 属性面（控制通道 `get` / `set`）

本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 设计章节

`DESIGN.md` §4.2.6

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/basic.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
