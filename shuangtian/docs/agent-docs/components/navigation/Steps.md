---
title: Steps · 导航与组织
summary: 步骤条。
---

# Steps

步骤条。

## 头与实现

- 头：`include/st/ui/components/steps.hpp`
- 实现：`src/ui/components/steps.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`Steps`

```cpp
#include "st/ui/components/steps.hpp"
// 构造：Steps()
auto w = std::make_unique<st::ui::Steps>();
```

## 属性面（控制通道 `get` / `set`）

**可读写**：`current` · `steps` · `orientation`

**只读**（能 `get`，`set` 不生效）：`count` · `all_done` · `states`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`next` · `step_forward` · `prev` · `previous` · `step_backward` · `goto` · `set` · `done`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/navigation.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
