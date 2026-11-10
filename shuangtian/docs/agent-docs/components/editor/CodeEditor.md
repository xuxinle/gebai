---
title: CodeEditor · 编辑器与文档视图
summary: CodeEditor 的构造、属性面、动作面与源码位置。
---

# CodeEditor

## 头与实现

- 头：`include/st/ui/components/code_editor.hpp`
- 实现：`src/ui/components/code_editor.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`CodeEditor`

```cpp
#include "st/ui/components/code_editor.hpp"
// 构造：CodeEditor()
auto w = std::make_unique<st::ui::CodeEditor>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`text` · `language` · `cursor` · `line` · `column` · `lines` · `selection` · `selected_text` · `read_only` · `highlight` · `show_line_numbers` · `tab_width` · `indent_guides` · `auto_pairs` · `scroll_beyond_last_line` · `font_size` · `font_scale` · `scroll` · `first_visible_line` · `visible_lines` · `goto_line` · `find_needle` · `find_matches` · `find_active` · `find_case` · `find_word` · `line_height` · `text_offset` · `caret_top` · `ink_height` · `baseline` · `caret_blink` · `diagnostics` · `diagnostic_errors` · `hover_diagnostic` · `end_annotations`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`focus` · `activate` · `select_all` · `undo` · `redo` · `clear_selection` · `insert` · `copy` · `cut` · `paste` · `comment` · `select_next_occurrence` · `move_line_up` · `move_line_down` · `move_lines` · `indent` · `dedent` · `goto_line` · `scroll_to_line` · `scroll_by` · `reveal_line` · `set_text` · `find` · `clear_find` · `find_next` · `find_prev` · `replace` · `replace_all`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/editor.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
