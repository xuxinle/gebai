---
title: WebView · 布局与窗口
summary: 可编程嵌入浏览器组件。见文件头注释。
---

# WebView

可编程嵌入浏览器组件。见文件头注释。

## 头与实现

- 头：`include/st/ui/components/webview.hpp`
- 实现：`src/ui/components/webview.cpp`
- 注册名（XML / `ui.create` / 声明式按名建）：`WebView`

```cpp
#include "st/ui/components/webview.hpp"
// 构造：WebView()
auto w = std::make_unique<st::ui::WebView>();
```

## 属性面（控制通道 `get` / `set`）


**只读**（能 `get`，`set` 不生效）：`attached` · `supported` · `shown` · `ready` · `loading` · `url` · `title` · `source` · `can_back` · `can_forward` · `zoom` · `rasterization_scale` · `assets` · `status` · `last_result` · `unsupported_reason` · `placeholder_visible`

> 只列组件自己声明的属性；`id` / `visible` / `x` / `width` / `background` 这类**通用属性**由 `Element` 统一提供，任何组件都能读写（见 `overview/properties.md`）。

## 动作面（控制通道 `invoke`）

`navigate` · `load_html` · `reload` · `stop` · `back` · `forward` · `post` · `zoom` · `set_script` · `exec`

> `invoke` 是**绕过鼠标**的可靠触发路径：它直接调组件的动作入口，与真实点击走同一段业务代码但不过命中测试。要验证点击路径本身再用 `click`。

## 更细

- 族页（同类控件怎么选、组合套路）：`components/layout.md`
- 属性/动作/通用属性三张表的口径：`overview/properties.md`
- 用控制通道驱动它跑一遍：`overview/control-protocol.md`
