---
title: Table · 数据展示
summary: 数据表格：列定义 + 行数据，表头/斑马纹/hover/横向滚动全部自绘，无子节点。
---

# Table

数据表格：列定义 + 行数据，表头/斑马纹/hover/横向滚动全部自绘，无子节点。

## 头与实现

- 头：`include/st/ui/components/table.hpp`
- 实现：`src/ui/components/table.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Table`

```cpp
#include "st/ui/components/table.hpp"
// 构造：Table(std::vector<TableColumn> columns = {})
auto w = std::make_unique<st::ui::Table>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`columns` · `rows` · `row_height` · `zebra` · `scroll` · `max_scroll` · `scroll_y` · `max_scroll_y` · `selected_row` · `value` · `first_visible` · `visible_rows`

**只读**（能 `get`，`set` 不生效）：`column_widths` · `lane_inset`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`add_row` · `clear_rows` · `scroll_to` · `scroll_by`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/data.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
