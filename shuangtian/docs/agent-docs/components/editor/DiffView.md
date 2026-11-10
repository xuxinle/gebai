---
title: DiffView · 编辑器与文档视图
summary: 差异视图：把一份 `text::FileDiff` 渲染成可导航的两栏/内联视图。
---

# DiffView

差异视图：把一份 `text::FileDiff` 渲染成可导航的两栏/内联视图。

## 头与实现

- 头：`include/st/ui/components/diff_view.hpp`
- 实现：`src/ui/components/diff_view.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`DiffView`

```cpp
#include "st/ui/components/diff_view.hpp"
// 构造：DiffView()
auto w = std::make_unique<st::ui::DiffView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`hunks` · `current_hunk` · `additions` · `deletions` · `layout` · `editable` · `binary` · `summary` · `old_lines` · `new_lines` · `logical_left` · `logical_right` · `numbers_old` · `numbers_new` · `left_text` · `right_text` · `scroll` · `disabled_old_lines` · `disabled_new_lines` · `current_hunk_line`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`next_hunk` · `previous_hunk` · `goto_hunk` · `set_layout` · `clear`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 设计章节

`DESIGN.md` §4.2.11

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/editor.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
