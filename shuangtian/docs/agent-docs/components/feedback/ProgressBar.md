---
title: ProgressBar · 反馈与状态
summary: 进度条：圆角轨道 + 渐变填充；`value` 取值 `[0,1]`，语义值输出百分比字符串。
---

# ProgressBar

进度条：圆角轨道 + 渐变填充；`value` 取值 `[0,1]`，语义值输出百分比字符串。

## 头与实现

- 头：`include/st/ui/components/progress_bar.hpp`
- 实现：`src/ui/components/feedback.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`ProgressBar`

```cpp
#include "st/ui/components/progress_bar.hpp"
// 构造：ProgressBar(float value = 0.0f)
auto w = std::make_unique<st::ui::ProgressBar>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`value` · `tone` · `text`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`set_value`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/feedback.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
