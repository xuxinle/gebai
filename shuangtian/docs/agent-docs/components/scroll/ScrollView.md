---
title: ScrollView · 滚动
summary: 垂直滚动容器：子节点按列堆叠，超出视口部分被裁剪，右侧自动出现 `ScrollBar`。 滚动不改变子节点尺寸，只平移其 `bounds`（因此会触发重排）。
---

# ScrollView

垂直滚动容器：子节点按列堆叠，超出视口部分被裁剪，右侧自动出现 `ScrollBar`。 滚动不改变子节点尺寸，只平移其 `bounds`（因此会触发重排）。

## 头与实现

- 头：`include/st/ui/components/scroll_view.hpp`
- 实现：`src/ui/components/markdown_view.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`ScrollView`

```cpp
#include "st/ui/components/scroll_view.hpp"
// 构造：ScrollView()
auto w = std::make_unique<st::ui::ScrollView>();
```

## 属性面（控制通道 `get` / `set`）

本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 设计章节

`DESIGN.md` §5

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/scroll.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
