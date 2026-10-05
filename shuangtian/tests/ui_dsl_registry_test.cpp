/// 组件注册表一致性（结构评审 A1 的防退护栏）。
///
/// ## 这组用例守的是什么
///
/// 组件的「类型身份」原先散在**两处**：`type_name<T>()` 特化表（给 `custom<T>` 用）与
/// `make_element` 的 if 链（给协议/脚本/属性填装用）。两张表谁都不认识对方——
/// 新增组件漏改一边就是**静默失效**：
///
/// - 漏改 `type_name` 特化 → `custom<T>` 走到「未注册」分支；
/// - 漏改 `make_element` → 返回空 → 声明式节点**凭空消失**（界面缺块而不报错）。
///
/// 两者都不会被编译器发现。现在两处都由同一份清单 `ST_COMPONENT_LIST` 展开，
/// 下面几条用例把「清单 → 两张表」的一致性钉死，让任何未来的手工偏离立刻红灯。

#include "st/test/test.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "st/ui/dsl.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"

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
  ST_CHECK(names.size() >= 38U);
  const std::set<std::string> unique(names.begin(), names.end());
  ST_CHECK_EQ(unique.size(), names.size());
}

/// ② 清单里的每个名字都能真的构造出元素，且**元素的 `type()` 就是该注册名**。
///
/// 这条是「两处派生一致」的核心断言：`kComponents` 给的注册名必须与组件自己报的
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

/// ④ `custom<T>` 走的那条路（编译期类型 → 注册名）与运行时表一致。
///
/// 抽查覆盖三类形态：与类名同名的（`Text`）、注册名异于类名的（`IconView` → `"Icon"`）、
/// 以及需要构造参数的（`Button` 注册名与类名同名但构造签名带参）。
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
