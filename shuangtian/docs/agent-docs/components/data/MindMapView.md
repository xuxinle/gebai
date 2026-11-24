---
title: MindMapView · 数据展示
summary: 思维导图视图（查看 + 编辑 + 保存）。
---

# MindMapView

思维导图视图（查看 + 编辑 + 保存）。

## 头与实现

- 头：`include/st/ui/components/mind_map_view.hpp`
- 实现：`src/ui/components/mind_map_view.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`MindMapView`

```cpp
#include "st/ui/components/mind_map_view.hpp"
// 构造：MindMapView()
auto w = std::make_unique<st::ui::MindMapView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`file` · `direction` · `font_size` · `zoom` · `fit` · `pan_x` · `pan_y` · `selected` · `editable` · `pan_enabled` · `wheel_zoom` · `show_collapse_badges` · `source`

**只读**（能 `get`，`set` 不生效）：`node_count` · `visible_count` · `depth` · `selected_text` · `selected_path` · `error` · `content_width` · `content_height` · `scale` · `dragging` · `editing` · `can_undo` · `can_redo` · `dirty`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`reset_view` · `fit` · `zoom_in` · `zoom_out` · `select_root` · `load` · `save` · `undo` · `redo` · `expand_all` · `collapse_all` · `new` · `add_child` · `add_sibling` · `remove` · `remove_subtree` · `rename` · `toggle_collapse` · `move_up` · `move_down`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/data.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
