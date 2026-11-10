---
title: SlowContent · 布局与窗口
summary: 慢内容壳：把「排版慢」的内容组件包起来，排版移出主线程。
---

# SlowContent

慢内容壳：把「排版慢」的内容组件包起来，排版移出主线程。

## 头与实现

- 头：`include/st/ui/components/slow_content.hpp`
- 实现：`src/ui/components/slow_content.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`SlowContent`

```cpp
#include "st/ui/components/slow_content.hpp"
// 构造：SlowContent(std::unique_ptr<Element> content, std::shared_ptr<SlowLayoutTarget> target, app::Tasks* tasks = nullptr)
auto w = std::make_unique<st::ui::SlowContent>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`layout_pending` · `layout_ms` · `measure_ms` · `source_bytes` · `generation`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

本组件没有自定义动作。通用动作（`focus` 等）由 `Element` 提供。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/layout.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
