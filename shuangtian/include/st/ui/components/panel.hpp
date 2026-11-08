#pragma once

/// @file panel.hpp
/// @brief 两个**基类级便捷容器**：`Panel`（行/列布局面板）与 `Spacer`（弹性/固定占位）。
///
/// ## 为什么它们单独成一个头，而不是继续住在 `element.hpp` 里
///
/// 它们原先定义在 `element.hpp`（元素基类的那个头）里。问题是：`element.hpp` 是
/// **每个界面翻译单元都必然包含**的根头——把两个"可声明组件"放在那里，等于
/// 让"支持 `<Panel>` 标签"成为一条**永远无法裁掉**的依赖。
///
/// 而 `Panel` 恰好是最常用的容器（XML 的 `<Row>` / `<Column>` 都翻译成它），
/// 所以它其实不需要靠位置取巧：放在自己的头里、由需要它的地方 include 即可。
///
/// ## 这里为什么是"复述声明"而不是"搬走定义"
///
/// `Panel` / `Spacer` 的定义仍留在 `element.hpp`（基类的布局代码要用它们，
/// 搬走会形成循环包含）。本头只做**自注册**：让"include 这个头"= "`Panel` 可声明"。
/// 于是 `dsl::make_element("Panel")` 在没有 include 本头的翻译单元里查不到——
/// 那正是按需导入的语义。

#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

ST_UI_COMPONENT(::st::ui::Panel, "Panel")
ST_UI_COMPONENT(::st::ui::Spacer, "Spacer")
