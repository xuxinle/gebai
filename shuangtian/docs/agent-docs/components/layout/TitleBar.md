---
title: TitleBar · 布局与窗口
summary: 自绘标题栏：图标 + 标题 + 右侧最小化/最大化/关闭。
---

# TitleBar

自绘标题栏：图标 + 标题 + 右侧最小化/最大化/关闭。

## 头与实现

- 头：`include/st/ui/components/title_bar.hpp`
- 实现：`src/ui/components/title_bar.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`TitleBar`

```cpp
#include "st/ui/components/title_bar.hpp"
// 构造：TitleBar(std::string title = {})
auto w = std::make_unique<st::ui::TitleBar>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`title` · `text` · `icon` · `show_controls` · `maximized` · `window_control_available` · `hovered_button` · `hovered_caption`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 设计章节

`DESIGN.md` §4.5

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/layout.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
