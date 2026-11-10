---
title: MarkdownView · 编辑器与文档视图
summary: Markdown 渲染视图：`set_markdown` 全量替换 / `append_chunk` 流式追加，可滚动。
---

# MarkdownView

Markdown 渲染视图：`set_markdown` 全量替换 / `append_chunk` 流式追加，可滚动。

## 头与实现

- 头：`include/st/ui/components/markdown_view.hpp`
- 实现：`src/ui/components/markdown_view.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`MarkdownView`

```cpp
#include "st/ui/components/markdown_view.hpp"
// 构造：MarkdownView(std::string markdown = {})
auto w = std::make_unique<st::ui::MarkdownView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`markdown` · `text` · `block_count` · `content_height` · `scroll` · `max_scroll` · `viewport_height` · `base_font_size` · `code_font_size` · `max_width` · `selectable` · `stick_to_bottom` · `pending` · `selection` · `layout_width` · `row_count` · `first_visible` · `last_visible` · `row_at_probe` · `draw_stats` · `painted_groups`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`append` · `clear` · `copy` · `select_all` · `set_markdown` · `scroll_to_bottom` · `scroll_to_top`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 设计章节

`DESIGN.md` §4.5 · `DESIGN.md` §5

> 章节号是稳定接口（源码注释按号寻址），按号 grep 不要按内容找。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/editor.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
