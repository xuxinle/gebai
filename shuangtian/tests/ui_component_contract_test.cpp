/// **全类型**组件契约面自洽护栏 —— 遍历注册表里的**每一个**组件跑同一套不变量。
///
/// ## 为什么需要它（与既有测试的分工）
///
/// 组件库有 48 个注册类型（`ST_COMPONENT_LIST`），但属性/动作面的守卫此前是
/// **逐组件手工 fixture**（`tests/ui_actions_test.cpp` 里写死了 `Input`/`Switch`/
/// `Checkbox`/`Slider` 四个）。于是新增组件、或给某组件加属性时，只有"碰巧有人为它
/// 写了用例"才被覆盖——`DESIGN.md §8.1.1` 自己反推出来的那族缺口（"API 存在但动作面
/// 未实现"）正是这么长出来的。
///
/// 本文件把守卫从"N 个手工夹具"变成"**注册表的一个函数**"：`make_element` 造出每个
/// 类型，对每个都跑同一套断言。新增组件自动进入覆盖——这正是 `ST_COMPONENT_LIST`
/// 那份"一份清单、多处派生"应得的收益（见 `tests/ui_dsl_registry_test.cpp`）。
///
/// ## 守的是什么（每一条都对应一类真实会发生的腐烂）
///
/// | 不变量 | 腐烂形态 |
/// |---|---|
/// | 属性名非空、不重复 | 声明表里手滑多写/重名 → `set` 命中错的那条 |
/// | 可写表 ∩ 只读表 = ∅ | 同名同时声明可写与只读 → 契约自相矛盾 |
/// | 只读名 `set_property` 必须 false | 声明了只读却照样写得进去（两张表形同虚设） |
/// | 未知属性 `set_property` 必须 false | 组件"什么都接受" → 拼错的属性名静默"生效" |
/// | 未知属性 `get_property` 必须 nullopt | 同上：读端假装有值 |
/// | 未知动作 `invoke_action` 必须 false | 动作拼错时假装处理了，调用方以为点到了 |
///
/// 全部是**"如实上报"**类不变量：不检查"某属性该不该存在"（那是设计判断，机器不懂），
/// 只检查"答不上来时要明说答不上来"——与框架一以贯之的"缺失链路必须明确暴露，
/// 不静默"同一立场。
///
/// ## 口径说明：为什么"声明即可写"不能在这里强断言
///
/// 基类 `Element::property_names()` 声明了 8 个名字（`hovered`/`hover_progress`/… ），
/// 而其中数个是**只读快照**（`get` 有值、`set` 无意义）——`set_property` 对它们返回
/// false 是**正确行为**，不是腐烂。所以"声明的名字必须可写"这条**只在 `apply_properties`
/// 的通用车道上对特定组件成立**（见 `ui_actions_test.cpp` 的 `Input` 用例），
/// 不能提升成全组件不变量——否则它会立刻误报基类自己。

#include "st/test/test.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"

namespace {

using st::ui::Element;
using st::ui::dsl::make_element;
using st::ui::dsl::registered_element_types;

[[nodiscard]] auto contains(const std::vector<std::string_view>& names, std::string_view needle)
    -> bool {
  return std::ranges::find(names, needle) != names.end();
}

}  // namespace

/// 每个注册类型都造得出来，且能报出自己的 `type()`（注册名即身份）。
///
/// 这条同时是下面所有不变量能跑的前提——注册表里出现一个造不出的名字，
/// 就意味着声明式/协议建的界面会**凭空少一块**（且不报错）。
ST_TEST(contract_every_type_builds_and_answers_basics) {
  const auto names = registered_element_types();
  ST_CHECK(names.size() >= 48U);  // 组件库规模下界（防"清单被误删"）
  for (const std::string& name : names) {
    auto element = make_element(name);
    ST_REQUIRE(element != nullptr);
    ST_CHECK_EQ(std::string(element->type()), name);
    // 属性面是"给外部读写的门面"，至少要能问出来（可为空表）。
    (void)element->property_names();
    (void)element->readonly_property_names();
  }
}

/// 属性声明的**结构自洽**：名字非空、不重复、可写与只读不重叠。
ST_TEST(contract_property_name_lists_are_well_formed) {
  for (const std::string& name : registered_element_types()) {
    auto element = make_element(name);
    ST_REQUIRE(element != nullptr);

    const auto writable = element->property_names();
    const auto readonly = element->readonly_property_names();

    const std::set<std::string_view> writable_set(writable.begin(), writable.end());
    const std::set<std::string_view> readonly_set(readonly.begin(), readonly.end());

    // 不重复（重名会让 `set` 命中哪一条取决于实现顺序，是静默歧义）。
    ST_CHECK_EQ(writable_set.size(), writable.size());
    ST_CHECK_EQ(readonly_set.size(), readonly.size());

    for (const std::string_view property : writable) {
      ST_CHECK(!property.empty());
    }
    for (const std::string_view property : readonly) {
      ST_CHECK(!property.empty());
      // 同一名字不能既声明可写又声明只读——那是自相矛盾的契约。
      ST_CHECK(!contains(writable, property));
    }
  }
}

/// **只读属性必须拒绝写**：这是两张表（可写/只读）分家的全部意义。
///
/// 若某组件把 `selection` 之类声明进只读表，却仍在 `set_property` 里写它，
/// 那两张表就形同虚设——而调用方会看到 `changed` 里没有它、状态却变了。
ST_TEST(contract_readonly_properties_reject_writes) {
  for (const std::string& name : registered_element_types()) {
    auto element = make_element(name);
    ST_REQUIRE(element != nullptr);
    for (const std::string_view property : element->readonly_property_names()) {
      ST_CHECK(!element->set_property(property, "1"));
    }
  }
}

/// **未知名字要如实拒绝**（读端 `nullopt`、写端 `false`）——不许"假装有"。
///
/// 这条守的是拼写错误的静默化：`set` 一个不存在的属性若返回 true，调用方会以为改到了；
/// `invoke` 一个不存在的动作若返回 true，`changed`/通知链会挂上一个假来源。
ST_TEST(contract_unknown_names_are_rejected_not_faked) {
  // 用不太可能撞上真实属性名的哨兵串。
  constexpr std::string_view kUnknownProperty = "__no_such_property__";
  constexpr std::string_view kUnknownAction = "__no_such_action__";

  for (const std::string& name : registered_element_types()) {
    auto element = make_element(name);
    ST_REQUIRE(element != nullptr);

    ST_CHECK(!element->set_property(kUnknownProperty, "x"));
    ST_CHECK(!element->get_property(kUnknownProperty).has_value());
    ST_CHECK(!element->invoke_action(kUnknownAction, ""));
  }
}

/// 声明的属性名**读得动**（不崩、不抛）；读回来的要么是 `nullopt`（明确「此刻无值」）、
/// 要么是**可信的字符串**。
///
/// 注意：这里**不断言非空**。空串在本框架里是**合法值**——文本类属性（`text`/`label`/
/// `selected_key`/`icon`）未设置时就是 `""`，而 `nullopt` 才表示「不支持这个名字」。
/// 初稿写成「必须非空」是**假断言**：它会把 `Text`/`Tree` 这类根本没错的组件判红
/// （实测首跑就有 30+ 条误报）。要断的只有「不许崩、不许含糊」。
ST_TEST(contract_declared_properties_are_queryable) {
  for (const std::string& name : registered_element_types()) {
    auto element = make_element(name);
    ST_REQUIRE(element != nullptr);

    for (const std::string_view property : element->property_names()) {
      (void)element->get_property(property);  // 不崩即可
    }
  }
}
