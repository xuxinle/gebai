---
title: Select · 下拉选择
summary: 下拉单选控件：与 `Input` 同高（`metrics.control_height`），右侧自绘 `chevron-down`。
---

# Select

下拉单选控件：与 `Input` 同高（`metrics.control_height`），右侧自绘 `chevron-down`。

## 头与实现

- 头：`include/st/ui/components/select_box.hpp`
- 实现：`src/ui/components/overlay.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Select`

```cpp
#include "st/ui/components/select_box.hpp"
// 构造：Select()
auto w = std::make_unique<st::ui::Select>();
```

## 属性面（控制通道 `get` / `set`）

本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/select.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
