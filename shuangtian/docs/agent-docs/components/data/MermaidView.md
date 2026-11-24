---
title: MermaidView · 数据展示
summary: Mermaid 图视图。
---

# MermaidView

Mermaid 图视图。

## 头与实现

- 头：`include/st/ui/components/mermaid_view.hpp`
- 实现：`src/ui/components/mermaid_view.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`MermaidView`

```cpp
#include "st/ui/components/mermaid_view.hpp"
// 构造：MermaidView()
auto w = std::make_unique<st::ui::MermaidView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`source` · `zoom` · `fit` · `font_size` · `pan_x` · `pan_y` · `pan_enabled` · `wheel_zoom`

**只读**（能 `get`，`set` 不生效）：`kind` · `error` · `diagnostics` · `node_count` · `edge_count` · `content_width` · `content_height` · `scale` · `dragging`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`zoom_in` · `zoom_out` · `reset_view` · `fit`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/data.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
