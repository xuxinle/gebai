---
title: NativeHost · 布局与窗口
summary: 把外部程序的窗口嵌进界面的一个矩形里。
---

# NativeHost

把外部程序的窗口嵌进界面的一个矩形里。

## 头与实现

- 头：`include/st/ui/components/native_host.hpp`
- 实现：`src/ui/components/native_host.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`NativeHost`

```cpp
#include "st/ui/components/native_host.hpp"
// 构造：NativeHost()
auto w = std::make_unique<st::ui::NativeHost>();
```

## 属性面（控制通道 `get` / `set`）


**只读**（能 `get`，`set` 不生效）：`attached` · `shown` · `title_bar_height` · `content_x` · `content_y` · `content_width` · `content_height` · `host_width` · `host_height` · `placeholder_visible`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 设计章节

`DESIGN.md` §1

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/layout.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
