#pragma once

/// @file component_id.hpp
/// @brief 组件的**身份声明**：注册名 + 默认构造。极轻量（只依赖 `<memory>`）。
///
/// ## 为什么这件事要从 `component_registry.hpp` 里拆出来
///
/// 每个组件头末尾都要写一行 `ST_UI_COMPONENT(类型, "名字")`。那一行展开后**只做
/// 一件事**：给 `component_meta<T>` 提供一个类模板特化（名字 + 造法）。纯模板，
/// 完全不碰注册表的运行期接口。
///
/// 而 `component_registry.hpp` 里装着 `add` / `create` / `names` 的声明，还要
/// include `<functional>` / `<string>` / `<string_view>` / `<vector>` / `<cstddef>`——
/// 那些是**用到注册表**的地方才需要的（`dsl.cpp`、控制通道、诊断），
/// 跟"声明一个组件是什么"没有关系。
///
/// 实测（2026-11-08）：只 include `button.hpp` 展开 7.6 万行，其中
/// `element.hpp` 占绝对主体，`component_registry.hpp` 那几百行不是瓶颈——所以
/// 这一步**不是为了省编译时间**，而是为了把依赖关系说准：
///
/// - 组件头依赖的是"身份"这个概念（本头）；
/// - 只有真正登记/查表的地方才依赖注册表（`component_registry.hpp`）。
///
/// 一条依赖说错了地方，后面就会有人往错的地方加东西（实测：为了让
/// `ST_UI_COMPONENT` 能展开，一度把 `<functional>` 拉进了 60 个组件头）。
///
/// ## 与注册表的关系
///
/// `component_registry.hpp` 会 include 本头并把接口补齐；组件头只 include 本头。
/// 因此"组件头 + 一句 import"仍然成立（`import_one` 在注册表头里，应用侧本来
/// 就要 include 它）。

#include <memory>

namespace st::ui {

class Element;

namespace detail {

/// 组件的身份：注册名 + 默认构造。
///
/// 特化由组件头里的 `ST_UI_COMPONENT` 生成。"怎么造"留给组件作者写，因为
/// 构造签名各异（`Button(label)` / `SelectPanel(options, selected)`），
/// `std::make_unique<T>()` 并不总能成立。
template <class T>
struct component_meta;

/// 组件的**导入锚点**：只有主模板声明（不定义）。
///
/// 组件的 `.cpp` 用 `ST_UI_COMPONENT_IMPL` 给出特化，并在**类外**定义 `run`——
/// 类外定义才是外部链接的强符号（类内定义会被隐式设为 `inline`，取不到地址）。
///
/// 于是 `import_components<T>()` 取它的地址就对组件 `.o` 产生真实引用，
/// 链接器必须把它拉进来。没被导入的类型没有定义 → 链接期报未定义符号
/// （**直接指出是哪个类型**），不会变成"编译过了但注册表里没有"的静默失败。
template <class T>
struct component_importer {
  static void run();
};

}  // namespace detail

}  // namespace st::ui

// ── 组件头里的声明 ───────────────────────────────────────────────────────────

/// 在组件头末尾声明"本组件是什么"（注册名 + 默认构造）。**不产生任何代码**。
///
/// 需要构造参数的组件用 `ST_UI_COMPONENT_WITH` 给出"不传参也能造一个"的表达式——
/// `make_element` 要的就是那个默认形态。
#define ST_UI_COMPONENT(TYPE, NAME) \
  ST_UI_COMPONENT_WITH(TYPE, NAME, std::make_unique<TYPE>())

#define ST_UI_COMPONENT_WITH(TYPE, NAME, MAKE)                                     \
  namespace st::ui::detail {                                                       \
  template <>                                                                      \
  struct component_meta<TYPE> {                                                    \
    static constexpr const char* name = NAME;                                      \
    static auto make() -> std::unique_ptr<::st::ui::Element> { return MAKE; }      \
  };                                                                               \
  }                                                                                \
  static_assert(true, "ST_UI_COMPONENT 需要分号终止");
