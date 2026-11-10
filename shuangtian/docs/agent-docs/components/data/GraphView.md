---
title: GraphView · 数据展示
summary: 关系图视图。
---

# GraphView

关系图视图。

## 头与实现

- 头：`include/st/ui/components/graph_view.hpp`
- 实现：`src/ui/components/graph_view.cpp`
- **不进注册表**：XML / `ui.create` / 声明式按名建**不会生效**，由 C++ 直接 `new` 挂树（理由见头文件末尾）

```cpp
#include "st/ui/components/graph_view.hpp"
// 构造：GraphView()
auto w = std::make_unique<st::ui::GraphView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`backend` · `render_scale`

**只读**（能 `get`，`set` 不生效）：`nodes` · `edges` · `visible_nodes` · `paint_frames` · `objects` · `labels` · `error` · `camera`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`reset_view` · `reset_camera` · `show` · `hide` · `frame` · `select`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 头注释里的逐条取舍（读源码前先看这里）

- 「它自己持有相机（而不是让 ThreeView 决定）」— `include/st/ui/components/graph_view.hpp:109`

> 这些是**为什么这样做**的记录（多半是踩过的坑）。改行为之前逐条读，否则会重新踩一遍。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/data.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
