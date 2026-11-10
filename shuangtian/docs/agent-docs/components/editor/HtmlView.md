---
title: HtmlView · 编辑器与文档视图
summary: HTML 预览组件。
---

# HtmlView

HTML 预览组件。

## 头与实现

- 头：`include/st/ui/components/html_view.hpp`
- 实现：`src/ui/components/html_view.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`HtmlView`

```cpp
#include "st/ui/components/html_view.hpp"
// 构造：HtmlView()
auto w = std::make_unique<st::ui::HtmlView>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`html` · `padding` · `font_size` · `max_content_width` · `follow_theme` · `scroll`

**只读**（能 `get`，`set` 不生效）：`title` · `plain` · `rules` · `lines` · `height`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`scroll_to` · `set_scroll` · `scroll_top` · `scroll_bottom` · `reload` · `refresh`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/editor.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
