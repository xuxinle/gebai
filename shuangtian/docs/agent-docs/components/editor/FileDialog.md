---
title: FileDialog · 编辑器与文档视图
summary: FileDialog 的构造、属性面、动作面与源码位置。
---

# FileDialog

## 头与实现

- 头：`include/st/ui/components/file_dialog.hpp`
- 实现：`src/ui/components/file_dialog.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`FileDialog`

```cpp
#include "st/ui/components/file_dialog.hpp"
// 构造：FileDialog(Mode mode = Mode::Open, std::string title = {})
auto w = std::make_unique<st::ui::FileDialog>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`directory` · `filename` · `mode` · `error` · `selected` · `pending_path` · `pending_paths` · `multi_select` · `selected_paths` · `show_hidden` · `filters` · `places` · `scroll`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`confirm` · `cancel` · `up` · `select` · `activate` · `reload` · `new_folder` · `mkdir` · `toggle_hidden` · `goto_crumb` · `crumb` · `goto_place` · `place`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/editor.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
