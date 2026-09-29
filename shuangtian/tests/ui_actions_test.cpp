/// 「属性面」与控制通道 `set` 的**一致性契约**测试。
///
/// 背景（实测踩到的静默缺陷）：`apply_properties` 曾自己维护一份可写属性的白名单，
/// 而各组件另有 `property_names()` 声明自己的属性面。两者一旦不一致，就出现
/// **读得到、改不了**的分裂：`Input` 的 `placeholder` 能被 `get` 读出占位文字，
/// 用 `set` 改却返回一个"成功"的 `changed: []`——调用方看到空数组，
/// 很容易当成"改过了"，实际什么都没发生。
///
/// 现在属性名以**元素自己的声明**为准（白名单只作并集兜底），这里把该契约钉住：
/// 凡是 `property_names()` 里声明的名字，`set` 都必须真的生效。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::Json;
using st::ui::apply_properties;
using st::ui::Checkbox;
using st::ui::Element;
using st::ui::Input;
using st::ui::Panel;
using st::ui::Slider;
using st::ui::Switch;
using st::ui::UiRoot;

/// 把元素装进一棵真实的树：`apply_properties` 需要 `UiRoot`（焦点等框架级属性要它参与）。
/// 这里不上树也没关系——被测的全是"元素自己的属性"；
/// `focused` 这类需要树内元素的属性不在此测（它由 `ui_root_lifetime_test` 覆盖）。
struct Fixture {
  UiRoot root{};
  Input input{};
  Switch toggle{};
  Checkbox box{};
  Slider slider{};

  Fixture() { root.set_theme(st::ui::Theme::light()); }
};

[[nodiscard]] auto as_array(const Json& value) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& item : value) out.push_back(st::json_as_string(item));
  return out;
}

[[nodiscard]] auto contains(const std::vector<std::string>& items, std::string_view needle)
    -> bool {
  for (const auto& item : items) {
    if (item == needle) return true;
  }
  return false;
}

}  // namespace

ST_TEST(ui_actions_set_changes_placeholder) {
  // 精确回归：这一条曾经失败（`changed: []`，占位文字纹丝不动）
  Fixture fixture;
  Json props = Json::object();
  props["placeholder"] = "已改：回读确认";
  const auto changed = as_array(apply_properties(fixture.root, fixture.input, props));
  ST_CHECK(contains(changed, "placeholder"));
  ST_CHECK_EQ(fixture.input.placeholder(), std::string("已改：回读确认"));
}

ST_TEST(ui_actions_set_changes_all_declared_string_properties) {
  // 通用护栏：`property_names()` 里声明的名字，`set` 必须生效——
  // 白名单一旦再与属性面分裂，这条会立刻亮红。
  Fixture fixture;
  const auto declared = fixture.input.property_names();
  ST_CHECK(!declared.empty());
  for (const auto& name : declared) {
    if (name == "enabled" || name == "visible") continue;  // 框架级，值语义不同
    Json props = Json::object();
    props[std::string(name)] = "测试值";
    const auto changed = as_array(apply_properties(fixture.root, fixture.input, props));
    ST_CHECK(contains(changed, name));
  }
}

ST_TEST(ui_actions_set_toggles_checked_and_reports_it) {
  Fixture fixture;
  fixture.toggle.set_checked(false);
  Json props = Json::object();
  props["checked"] = true;
  const auto changed = as_array(apply_properties(fixture.root, fixture.toggle, props));
  ST_CHECK(contains(changed, "checked"));
  ST_CHECK(fixture.toggle.checked());
}

ST_TEST(ui_actions_set_changes_slider_value) {
  Fixture fixture;
  Json props = Json::object();
  props["value"] = 0.25;
  const auto changed = as_array(apply_properties(fixture.root, fixture.slider, props));
  ST_CHECK(contains(changed, "value"));
  ST_CHECK_NEAR(fixture.slider.value(), 0.25f, 0.001f);
}

ST_TEST(ui_actions_unknown_property_is_ignored_silently) {
  // 批量下发时个别属性不被该组件支持是正常的：忽略而不报错，
  // 但**不能**把已知属性也一起吞掉（`changed` 要如实反映"哪些生效了"）
  Fixture fixture;
  Json props = Json::object();
  props["definitely_not_a_property"] = "x";
  props["placeholder"] = "兼有已知与未知";
  const auto changed = as_array(apply_properties(fixture.root, fixture.input, props));
  ST_CHECK(!contains(changed, "definitely_not_a_property"));
  ST_CHECK(contains(changed, "placeholder"));
}

ST_TEST(ui_actions_framework_level_properties_are_applied) {
  // `enabled` / `visible` 不经 `set_property`（需要框架参与），但必须照常可写
  Fixture fixture;
  Json props = Json::object();
  props["enabled"] = false;
  props["visible"] = false;
  const auto changed = as_array(apply_properties(fixture.root, fixture.input, props));
  ST_CHECK(contains(changed, "enabled"));
  ST_CHECK(contains(changed, "visible"));
  ST_CHECK(!fixture.input.enabled());
  ST_CHECK(!fixture.input.visible());
}
