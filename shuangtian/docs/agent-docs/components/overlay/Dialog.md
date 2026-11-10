---
title: Dialog · 浮层
summary: 模态对话框：遮罩 + 居中卡片 + 标题/正文 + 底部按钮行。 交互：Esc 与点击卡片外区域触发 `on_dismiss`；按钮点击触发 `on_action(index)`。
---

# Dialog

模态对话框：遮罩 + 居中卡片 + 标题/正文 + 底部按钮行。 交互：Esc 与点击卡片外区域触发 `on_dismiss`；按钮点击触发 `on_action(index)`。

## 头与实现

- 头：`include/st/ui/components/dialog.hpp`
- 实现：`src/ui/components/completion_popup.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Dialog`

```cpp
#include "st/ui/components/dialog.hpp"
// 构造：Dialog(std::string title = {}, std::string body = {})
auto w = std::make_unique<st::ui::Dialog>();
```

## 属性面（控制通道 `get` / `set`）

本组件**没有自己的属性面**——只能读写通用属性（`id`/`visible`/几何/样式）与它自己的 C++ 接口。要断言内容，用 `tree` 的 `text`/`value` 字段或截图。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/overlay.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
