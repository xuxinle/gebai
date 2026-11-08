/// 组件注册表一致性（结构评审 A1 的防退护栏；2026-11-08 改为自注册后重写）。
///
/// ## 这组用例守的是什么
///
/// 组件的「类型身份」原先散在**两处**：`type_name<T>()` 特化表（给 `custom<T>` 用）与
/// `make_element` 的 if 链（给协议/脚本/属性填装用）。两张表谁都不认识对方——
/// 新增组件漏改一边就是**静默失效**。
///
/// 2026-11-08 之后两张表合成一张（`st/ui/component_registry.hpp`），组件在自己头里
/// 用 `ST_UI_COMPONENT(TYPE, "名字")` 一次登记工厂 + 类型名。这组用例守的是：
///
/// ① 注册名不重复（按名寻址不能有歧义）；
/// ② 每个注册名都能造出元素，且元素的 `type()` 就是该名（两处派生一致）；
/// ③ 未注册的名字返回空（**不静默造假元素**）；
/// ④ `type_name<T>` 与运行时注册表一致（`custom<T>` 走的那条路）。
///
/// ## ⚠ 这组用例**必须显式 include 它要测的组件头**
///
/// 这正是"按需导入"生效的证据：注册表的内容 = **被 include 的组件头**的集合。
/// 不 include 就没有注册（`make_element` 返回空）——所以下面这条 include 清单
/// 不是"测试的准备工作"，而是**被测对象本身**。
///
/// 反过来说：本文件测的是**机制**，不是"框架里到底有多少组件"；后者由
/// `tests/ui_component_contract_test.cpp` 与 `tests/gallery_component_parity_test.cpp`
/// 通过 include `all.hpp` 来覆盖。

#include "st/test/test.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"

// 被测的组件（一个组件一个头 —— 想测哪些就 include 哪些）。
#include "st/ui/components/button.hpp"
#include "st/ui/components/icon_view.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/components/text.hpp"

namespace {
// 触发 `type_name<T>()` 的实例化：编译期类型 → 注册名，必须与运行时表一致。
template <class T>
[[nodiscard]] auto declared_name() -> std::string {
  return st::ui::dsl::type_name<T>();
}
}  // namespace

/// ① 注册名唯一（否则「按名寻址」有歧义：先命中的赢，后者静默不可达）。
ST_TEST(dsl_registry_names_are_unique) {
  const auto names = st::ui::dsl::registered_element_types();
  // 下界只约束"至少注册了本文件 include 的那几个"——**不写死总数**：
  // 总数取决于 include 了哪些头，写死会让"多 include 一个组件"变成假红。
  ST_CHECK(names.size() >= 4U);
  const std::set<std::string> unique(names.begin(), names.end());
  ST_CHECK_EQ(unique.size(), names.size());
}

/// ② 注册表里的每个名字都能真的构造出元素，且**元素的 `type()` 就是该注册名**。
///
/// 这条是「两处派生一致」的核心断言：注册时给的 `NAME` 必须与组件自己报的
/// `type()` 相同——否则协议按注册名建出来的元素，在 `tree`/选择器里是另一种身份。
ST_TEST(dsl_registry_every_entry_builds_and_matches_type) {
  for (const std::string& name : st::ui::dsl::registered_element_types()) {
    auto element = st::ui::dsl::make_element(name);
    ST_REQUIRE(element != nullptr);
    ST_CHECK_EQ(std::string(element->type()), name);
  }
}

/// ③ 未知类型返回空（**不静默造一个假元素**）。
ST_TEST(dsl_registry_unknown_type_returns_null) {
  ST_CHECK(st::ui::dsl::make_element("NoSuchComponent") == nullptr);
  ST_CHECK(st::ui::dsl::make_element("") == nullptr);
}

/// ③′ 注册表与 `dsl` 的关系：**dsl 能建的 ⊇ 注册表能建的**，且两边都不造假元素。
///
/// ## 为什么不再断言"没导入的组件查不到"
///
/// 那条断言曾经成立（组件头里放静态注册时，没 include 就没注册）。改成
/// **显式导入**之后它不再成立，原因是一条无法绕开的链路：
///
/// 1. 组件的实现文件里有一句自导入（`import_components<T>()`），保证它自己的
///    登记器在运行时被调——否则"代码在产物里但注册表查不到"会成为常态；
/// 2. 测试二进制把全部组件实现链在一起；
/// 3. 于是进程启动后，**凡实现进了产物的组件都已在注册表里**。
///
/// 所以"按需"的边界在**产物**上，不在"同一进程内的查表"上：
/// `tools/import_probe.cpp` 用**只导三个组件的独立产物**证明了这件事
/// （那个产物里 `Terminal` 的符号根本不存在）。
///
/// 本用例保留的是在这层能守的东西：注册表与 dsl 不矛盾、不造假元素。
ST_TEST(dsl_registry_and_dsl_layer_stay_consistent) {
  namespace registry = st::ui::component_registry;

  // ① 未注册的名字，两层都返回空（不静默造一个假元素）。
  ST_CHECK(registry::create("NoSuchComponent") == nullptr);
  ST_CHECK(st::ui::dsl::make_element("NoSuchComponent") == nullptr);
  ST_CHECK(registry::create("") == nullptr);

  // ② 注册表里的每个名字，dsl 层也能建（dsl 是注册表的**超集视角**：它还含外部注册）。
  for (const std::string& name : registry::names()) {
    auto built = st::ui::dsl::make_element(name);
    ST_REQUIRE(built != nullptr);
    ST_CHECK_EQ(std::string(built->type()), name);
  }

  // ③ 本文件只 include 了四个组件头，但产物里链着全部组件——所以注册表里
  //    会有本文件没用到的组件。这不是缺陷，而是"测试二进制包含全部实现"的必然结果；
  //    真正的按需证据在 `tools/import_probe.cpp`（独立产物）。
  ST_CHECK(registry::names().size() >= 4U);
}

/// ④ `custom<T>` 走的那条路（编译期类型 → 注册名）与运行时表一致。
///
/// 抽查覆盖三类形态：与类名同名的（`Text`）、注册名异于类名的（`IconView` → `"Icon"`）、
/// 以及需要构造参数的（`Button` 构造签名带参）。
ST_TEST(dsl_registry_type_name_matches_runtime_table) {
  const auto names = st::ui::dsl::registered_element_types();
  const auto contains = [&names](const std::string& wanted) {
    return std::ranges::find(names, wanted) != names.end();
  };

  ST_CHECK(contains(declared_name<st::ui::Panel>()));
  ST_CHECK(contains(declared_name<st::ui::Text>()));
  ST_CHECK(contains(declared_name<st::ui::Button>()));
  ST_CHECK(contains(declared_name<st::ui::IconView>()));

  // 注册名是对外契约：`IconView` 的注册名是 `"Icon"`（不是笔误，改名属破坏性变更）。
  ST_CHECK_EQ(declared_name<st::ui::IconView>(), std::string("Icon"));
  ST_CHECK_EQ(declared_name<st::ui::Button>(), std::string("Button"));

  // `custom<T>` 真的能按这个名字建出 T（端到端，而不只是字符串相等）。
  auto built = st::ui::dsl::make_element(declared_name<st::ui::IconView>());
  ST_REQUIRE(built != nullptr);
  auto* as_icon = dynamic_cast<st::ui::IconView*>(built.get());
  ST_CHECK(as_icon != nullptr);
}
