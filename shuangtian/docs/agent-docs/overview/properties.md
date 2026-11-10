---
title: 属性面 · get / set / invoke 的三张表
summary: 组件只声明自己的名字并包一层基类名；通用属性、只读诊断名、基类可写名各走哪条路。
---

# 属性面（`get` / `set` / `invoke`）

## 三层结构

| 层 | 是什么 | 在哪 |
|---|---|---|
| **组件自己的属性** | 只列这个组件认得的名字（`value`/`items`/`scroll`…） | 各组件 `property_names()` |
| **通用属性** | 任何元素都有（`id`/`visible`/几何/样式…） | `Element` 的 `with_universal(...)` |
| **只读诊断名** | 能 `get`、`set` 不生效（动画五项、`hovered`、`selection`…） | `with_universal_readonly(...)` |

组件实现里**必须**把自己的表包一层基类名（`with_universal`），否则"这个元素明明有 `id`，
`get` 却说没有"。实测代价：66 个组件覆写 `property_names` 时**一个都没并上基类名**，
合计 75 个组件缺 462 项——症状是"属性名拼对了也读不到"。

## `set` 的三种拒绝（读 `rejected`）

| `reason` | 意思 | 典型形态 |
|---|---|---|
| `unknown_name` | 名字不属于该元素 | 拼错属性名（`lable`） |
| `invalid_value` | 名字认得但值不合法 | 枚举拼错、非法颜色串、宽度写成 `"abc"` |
| `unfocusable` | 名字值都合法，但能力未开 | 对不可聚焦元素设 `focused=true` |

调用方**必须读 `rejected`**：只 diff `changed` 会把"拼错被静默跳过"看成"设置成功但值没变"。

## 三个"基类无条件支持"的可写名

`color` / `font_size` / `weight` **不下发给组件**（值必须合法，而"声明可写"意味着"填任意字符串必生效"，
两者矛盾），但它们在任何会画字的元素上都**可写**——由 `Element::is_base_writable_property()`
单独认。这份名单必须与 `Element::set_property` 里的 `base_accepts` **同一份**，
分散就会回到"认了但落不上"或"能写但协议不认"。

## 通用属性一览（写在任何元素上都生效）

| 类别 | 名字 | 备注 |
|---|---|---|
| 身份 | `id` | 不写则自动生成 |
| 可见/启用 | `visible` `enabled` | |
| 几何 | `x` `y` `width` `height` | 逻辑像素；在 flex 布局里可能被父级覆盖 |
| 布局 | `flex_grow` `flex_shrink` `padding*` `margin*` `gap` `align` `justify` | 见 `style.hpp` |
| 外观 | `background` `border` `radius` `opacity` | 颜色/尺寸字符串 |
| 文字 | `color` `font_size` `weight` | 值必须合法 |
| 诊断（只读） | 动画五项 · `hovered` · `hover_progress` · `hover_effect` · `transition` | 只 `get` |

> 名字冲突时**各自解释**：`Button` 的 `size` 是尺寸档位，`Text` 的 `size` 是字号（历史名，
> 新写法 `font_size`）。写代码时用新名字，读属性时按组件文档确认。

## `invoke` 与真实点击的区别

- `invoke` 直接调组件的动作入口：**不过命中测试、不要求可见**，是自动化的可靠路径；
- `click` 走真实命中与焦点路由：验证"用户点得着吗""点击落到谁身上"时必须用它。
- 两者调用**同一段业务代码**，所以业务断言用 `invoke`、交互正确性断言用 `click`。

## 声明式 / XML 的属性名与协议名是**同一套**

`xml-ui.cpp` 的 `normalize_property` 与协议 `set` 认同一批名字（含 `#id`、`Type[attr]` 等选择器
与属性别名）。给组件加属性时三处要一起动：`property_names()`、`set_property()`、
以及（若要在 XML/JS 里直接写）`normalize_property` 的别名。

## 更细

- 每个组件认哪些名字：`components/<族>/<组件>.md`（从源码生成，事实即代码）
- 属性描述契约（一份元数据驱动面板/校验/文档）：`DESIGN.md` §4.5.0
- 基类与通用名的实现：`include/st/ui/element.hpp`（`with_universal` / `kUniversalReadonlyNames`）
