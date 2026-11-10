---
title: Spacer · 框架容器
summary: Spacer 的构造、属性面、动作面与源码位置。
---

# Spacer

## 头与实现

- 头：`include/st/ui/components/panel.hpp`
- 实现：无独立 `.cpp`（定义在其他组件文件或 `element.hpp`）
- 注册名（XML / `ui.create` / 声明式按名建）：`Spacer`

```cpp
#include "st/ui/components/panel.hpp"
auto w = std::make_unique<st::ui::Spacer>();  // 构造参数见头文件
```

## 属性面（控制通道 `get` / `set`）

本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/panel.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
