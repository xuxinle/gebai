---
title: Icon · 基础展示
summary: 图标。
---

# Icon

图标。

## 头与实现

- 头：`include/st/ui/components/icon_view.hpp`
- 实现：`src/ui/components/basic.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Icon`

```cpp
#include "st/ui/components/icon_view.hpp"
// 构造：IconView(std::string name = {}, float size = 18.0f)
auto w = std::make_unique<st::ui::IconView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`icon` · `diameter` · `tone`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 设计章节

`DESIGN.md` §4.2.6

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/basic.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
