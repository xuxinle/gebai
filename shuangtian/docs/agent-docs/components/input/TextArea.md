---
title: TextArea · 输入与取值
summary: 多行文本域：自动换行（经 `TextPort::wrap`）+ 内部竖向滚动（光标始终可见）。
---

# TextArea

多行文本域：自动换行（经 `TextPort::wrap`）+ 内部竖向滚动（光标始终可见）。

## 头与实现

- 头：`include/st/ui/components/text_area.hpp`
- 实现：`src/ui/components/input.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`TextArea`

```cpp
#include "st/ui/components/text_area.hpp"
// 构造：TextArea()
auto w = std::make_unique<st::ui::TextArea>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`value` · `text` · `placeholder` · `cursor_index` · `scroll` · `read_only`

**只读**（能 `get`，`set` 不生效）：`selection`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`clear` · `submit` · `activate` · `select_all` · `clear_selection` · `copy`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/input.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
