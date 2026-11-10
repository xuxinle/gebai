---
title: LabelOverlay · 三维与连线
summary: 节点的“锚点”是怎么画的（写在这里是因为它决定了整体观感）
---

# LabelOverlay

节点的“锚点”是怎么画的（写在这里是因为它决定了整体观感）

## 头与实现

- 头：`include/st/ui/components/label_overlay.hpp`
- 实现：`src/ui/components/label_overlay.cpp`
- **不进注册表**：XML / `ui.create` / 声明式按名建**不会生效**，由 C++ 直接 `new` 挂树（理由见头文件末尾）

```cpp
#include "st/ui/components/label_overlay.hpp"
// 构造：LabelOverlay()
auto w = std::make_unique<st::ui::LabelOverlay>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`label_count` · `edge_count` · `painted` · `painted_edges` · `font_scale`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 头注释里的逐条取舍（读源码前先看这里）

- 「三版的失败链与最终形态」— `include/st/ui/components/label_overlay.hpp:89`

> 这些是**为什么这样做**的记录（多半是踩过的坑）。改行为之前逐条读，否则会重新踩一遍。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/3d.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
