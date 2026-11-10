---
title: SelectPanel · 下拉选择
summary: 下拉面板：`surface` + `shadow_md` + `radius_md`，选项行高 32（hover `surface_alt` / 选中 `primary_soft`）。
---

# SelectPanel

下拉面板：`surface` + `shadow_md` + `radius_md`，选项行高 32（hover `surface_alt` / 选中 `primary_soft`）。

## 头与实现

- 头：`include/st/ui/components/select_panel.hpp`
- 实现：`src/ui/components/select.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`SelectPanel`

```cpp
#include "st/ui/components/select_panel.hpp"
// 构造：SelectPanel(std::vector<SelectOption> options, std::optional<std::size_t> selected)
auto w = std::make_unique<st::ui::SelectPanel>();
```

## 属性面（控制通道 `get` / `set`）

本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/select.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
