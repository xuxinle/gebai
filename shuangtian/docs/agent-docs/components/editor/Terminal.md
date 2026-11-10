---
title: Terminal · 编辑器与文档视图
summary: Terminal 的构造、属性面、动作面与源码位置。
---

# Terminal

## 头与实现

- 头：`include/st/ui/components/terminal.hpp`
- 实现：`src/ui/components/terminal.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Terminal`

```cpp
#include "st/ui/components/terminal.hpp"
// 构造：Terminal()
auto w = std::make_unique<st::ui::Terminal>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`sessions` · `active_session` · `session_title` · `working_directory` · `busy` · `output` · `screen` · `cursor` · `cursor_rect` · `pty` · `alt_screen` · `scroll` · `user_scrolled` · `cursor_shape` · `screen_cursor_shape` · `monospace` · `tabs_visible` · `add_button` · `font_scale` · `scrollbar` · `running` · `running_name` · `idle_ms` · `command_issued`

**只读**（能 `get`，`set` 不生效）：`selection`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`open_shell` · `shell` · `run` · `send` · `send_line` · `select_all` · `clear_selection` · `copy` · `stop` · `clear` · `collapse` · `new_session` · `next_session` · `prev_session` · `previous_session` · `paste` · `scroll_to_end` · `scroll_bottom` · `close_session` · `select_session` · `submit`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/editor.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
