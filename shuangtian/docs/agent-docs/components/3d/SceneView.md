---
title: SceneView · 三维与连线
summary: SceneView 的构造、属性面、动作面与源码位置。
---

# SceneView

## 头与实现

- 头：`include/st/ui/components/scene_view.hpp`
- 实现：`src/ui/components/scene_view.cpp`
- **不进注册表**：XML / `ui.create` / 声明式按名建**不会生效**，由 C++ 直接 `new` 挂树（理由见头文件末尾）

```cpp
#include "st/ui/components/scene_view.hpp"
// 构造：SceneView(SceneShape shape = SceneShape::Cube)
auto w = std::make_unique<st::ui::SceneView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`shape` · `spin` · `spin_speed` · `scene_frames` · `scene_ready` · `mesh_vertices`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/3d.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
