#pragma once

/// @file component_registry.hpp
/// @brief 组件注册表：**登记**与**查询**的唯一入口。
///
/// ## 一条实测过的硬约束（先说清，免得下一个人再试一遍）
///
/// `dsl::make_element(name)` 是**按字符串查表**——它对任何具体组件都没有符号引用。
/// 而"组件的代码在不在产物里"由链接器按**引用**决定。于是：
///
/// - **静态库里没人引用的 `.o` 不进产物**（这正是"按需"的机制本身）；
/// - 所以"只 include 头、不引用任何符号"的组件不会进产物，
///   它的**任何**静态初始化都不会运行；
/// - 想让注册"自动"发生就得引用它，那又让"用不用"变成编译期常量，按需失效。
///
/// 实测过三条死路（全都**编译链接全绿、运行时注册表是空的**）：
///
/// | 写法 | 断在哪 |
/// |---|---|
/// | 头里 `inline const bool x = [](){...}();` | 没有 TU 对它 ODR-use → 链接器可整块丢弃 |
/// | 同上 + 实现文件里取一次地址 | 只保住**被取地址的那一份**；"哪个 `.o` 进产物"仍由别处决定 |
/// | 头里声明非 inline 函数、`.cpp` 里定义 + 静态登记 | `.cpp` 不进产物，定义与登记都不存在 |
///
/// ## 本框架的选择：**显式导入**
///
/// 与 Qt 的 `qmlRegisterType` / Flutter 的 `registerViewFactory` 同一形态——
/// "我要这个组件"必须**被写出来一次**：
///
/// ```cpp
/// #include "st/ui/components/button.hpp"      // ① 类型可见
/// st::ui::import_components<st::ui::Button>(); // ② 登记（并把它链进产物）
/// ```
///
/// 或者一步到位（开发期最省事，XML 应用的推荐做法）：
///
/// ```cpp
/// #include "st/ui/components/all.hpp"
/// st::ui::import_all_components();
/// ```
///
/// 这一步**不是形式主义**：`import_components<T>()` 会取
/// `component_registrar<T>::run` 的地址——那定义在组件的 `.cpp` 里，于是链接器
/// 必须把那个目标文件拉进来。**"导入"与"链进来"因此是同一件事**，
/// 而没导入的组件整块不进产物（这才是按需）。
///
/// 依赖关系仍然写在组件自己身上（注册名与构造方式都在组件头里），
/// 不再是"一张别人不看就会忘的集中清单"。
///
/// ## XML 应用
///
/// - 用 `all.hpp` + `import_all_components()`：全部标签可用；
/// - 自己按需 include：逐个 `import_components<T...>()`。
///
/// 写了没导入的标签**不会静默消失**：`XmlBuildReport::unknown_tags` 会如实报出
/// （见 `docs/xml-ui.md`）。

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {

class Element;

namespace component_registry {

/// 一个已注册组件的条目。
struct Entry {
  /// 注册名（**对外契约**：XML 标签别名表、属性面、控制通道 `ui.create` 都用它）。
  std::string name;
  /// 构造器（无参：需要构造参数的组件在登记处填默认表达式）。
  std::function<std::unique_ptr<Element>()> create;
};

/// 登记一个组件。返回 false = 名字/构造器为空，或重名（**保留先登记的**）。
///
/// 为何重名要拒绝而不是覆盖：注册名是外部契约（脚本/XML/控制通道按它寻址），
/// 两个组件抢同一个名字时谁生效将取决于调用顺序——那是最难查的一类不一致。
[[nodiscard]] auto add(Entry entry) -> bool;

/// 按注册名造元素（未登记返回 `nullptr`，**不静默造假元素**）。
[[nodiscard]] auto create(std::string_view name) -> std::unique_ptr<Element>;

/// 全部注册名（按登记顺序；诊断与一致性单测用）。
[[nodiscard]] auto names() -> std::vector<std::string>;

/// 是否已登记该名字。
[[nodiscard]] auto contains(std::string_view name) -> bool;

/// 每个类型一个**进程唯一** token（inline 函数内 static：COMDAT 折叠保证跨 TU 同址）。
///
/// 为何不用 `typeid` / `type_index`：动态库各自持一份 `type_info` 时同一类型会有多个键，
/// 那会让"登记了却查不到"以最难查的形式出现。地址折叠不受此影响。
template <class T>
[[nodiscard]] auto type_token() -> const void* {
  static const char tag = 0;
  return &tag;
}

/// 登记「类型 token → 注册名」（`custom<T>` / `type_name<T>` 靠它把编译期类型
/// 换成字符串）。
void add_type_name(const void* token, std::string_view name);

/// 按 token 查注册名（未登记返回空串）。
[[nodiscard]] auto type_name_of(const void* token) -> std::string;

/// **已登记的导入清单**（每个类型一个函数，由组件头登记、由 `.cpp` 定义）。
///
/// 为何需要一张清单：`make_element` 是**按字符串**查表的，它不知道有哪些组件；
/// 而如果连"有哪些组件"都要应用自己列，那就退回了集中清单的老路。
/// 这张清单的条目由**组件头**登记（include 了就在），于是"有哪些组件可用"
/// 依然写在组件自己身上。
///
/// 为何清单里只存函数**指针**：存指针不会引用组件符号本身，所以不会把组件的
/// `.o` 拉进产物——按需仍然成立。
void add_import(void (*run)());

/// 跑全部已登记的导入（幂等）。
void run_imports();

}  // namespace component_registry

namespace detail {

/// 组件的**身份**（注册名 + 默认构造）：**特化在组件头里**。
///
/// 为什么身份放头里、导入放 `.cpp` 里——
///
/// - 身份（名字、怎么造）是**组件自己的事**，写在它自己的头里，
///   于是"这个组件叫什么、怎么建"在代码里一看就是它自己的，不在某张集中表里；
/// - 而"它进不进产物"是**应用的选择**，写在 `.cpp` 的一次导入里。
///
/// `component_meta<T>` 是**类模板全特化、定义在头里**，这与"一个 `.cpp` 一个
/// 函数定义"不冲突：类模板特化的定义可以出现在多个 TU（要求完全一致，而它们
/// 来自同一个头）。
template <class T>
struct component_meta;

/// 组件的**导入锚点**：只有主模板声明（不定义）。
///
/// 组件的 `.cpp` 用 `ST_UI_COMPONENT_IMPL` 声明特化类并**在类外**定义 `run`——
/// 类外定义才是外部链接的强符号（类内定义会被隐式设为 `inline`，取不到地址，
/// 实测报：`undefined symbol ... run()`）。
///
/// 于是 `import_components<T>()` 取它的地址就对组件 `.o` 产生真实引用，
/// 链接器必须把它拉进来。没被导入的类型没有定义 → 链接期报未定义符号
/// （**直接指出是哪个类型**），不会变成"编译过了但注册表里没有"的静默失败。
template <class T>
struct component_importer {
  static void run();
};

}  // namespace detail

/// 导入一个组件：**这一句就是"我要它"的声明**。
///
/// 它引用 `component_importer<T>::run`（定义在那个组件的 `.cpp` 里），
/// 所以调用它 = 把那目标文件拉进产物。
template <class T>
inline void import_one() {
  detail::component_importer<T>::run();
}

/// 导入若干组件。
///
/// ```cpp
/// st::ui::import_components<st::ui::Button, st::ui::Text>();
/// ```
template <class... Ts>
void import_components() {
  (import_one<Ts>(), ...);
}

}  // namespace st::ui

// ── 组件头里的声明 ───────────────────────────────────────────────────────────

/// 在组件头末尾声明"本组件可被导入"。
///
/// 它给出组件的**身份**（注册名 + 默认构造）——放在头里，因为那是组件自己的事。
/// 真正的"导入"发生在应用侧（`import_components<T>()`），或被 `all.hpp` 一次做完。
///
/// 需要构造参数的组件（`Button(label)` / `SelectPanel(options, selected)`）用
/// `ST_UI_COMPONENT_WITH` 给出"不传参也能造一个"的表达式——`make_element` 要的
/// 就是那个默认形态。
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

// ── 组件实现文件里的定义 ────────────────────────────────────────────────────

/// 在组件实现的 `.cpp` 里生成**导入锚点**的定义（每组件一次）。
///
/// 这里只做两件事：从头的 `component_meta<T>` 取名字与构造方式，登记进注册表。
/// 名字**不在这里重复写**——它是组件头里的那一份（单一来源）。
#define ST_UI_COMPONENT_IMPL(TYPE)                                                 \
  namespace st::ui::detail {                                                       \
  /* 特化声明（类内**不**定义 run：否则隐式 inline，拿不到强符号） */              \
  template <>                                                                      \
  struct component_importer<TYPE> {                                                \
    static void run();                                                             \
  };                                                                               \
  /* 类外定义：不带 `template <>`——因为已经处于显式特化的作用域里。            \
     加上反而会报 "template-id 'run<>' ... does not match any template
     declaration"（实测）。 */                                                    \
  void component_importer<TYPE>::run() {                                           \
    /* 幂等：同一个组件被多处导入时只登记一次 */                                     \
    static const bool registered = []() {                                          \
      (void)::st::ui::component_registry::add(                                     \
          ::st::ui::component_registry::Entry{                                     \
              component_meta<TYPE>::name,                                          \
              []() { return component_meta<TYPE>::make(); }});                     \
      ::st::ui::component_registry::add_type_name(                                 \
          ::st::ui::component_registry::type_token<TYPE>(),                        \
          component_meta<TYPE>::name);                                             \
      return true;                                                                 \
    }();                                                                           \
    (void)registered;                                                              \
  }                                                                                \
  }                                                                                \
  static_assert(true, "ST_UI_COMPONENT_IMPL 需要分号终止");

/// 同上（保留旧名以兼容已有调用点）。
#define ST_UI_COMPONENT_IMPL_NAMED(TYPE, NAME) ST_UI_COMPONENT_IMPL(TYPE)
