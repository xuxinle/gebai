/// UI 属性改名的等价性与告警测试。
///
/// ## 为什么这类测试必须有
///
/// 改名（`size` → `font_size` / `diameter` / `scale`）最容易出的问题是
/// **改了名字但没接上实现**：旧的还能用、新的没人认，而界面上看不出差别
/// （因为旧名还在生效）。只有"新名与旧名产出相同结果"这条能抓住它。
///
/// 另一类问题是**改错语义**：`size` 在 UI 里有四种含义（字号/直径/枚举），
/// 一律改成 `font_size` 会把图标直径也叫成字号——那等于造出一个新的语义错误。
/// 所以下面分别验证：字号归字号、直径归直径、枚举归枚举。
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/element.hpp"

ST_TEST(ui_text_font_size_alias_equivalence) {
  // `font_size` 与旧的 `size` 必须产出**相同字号**
  st::ui::Text a;
  st::ui::Text b;
  ST_CHECK(a.set_property("font_size", "18"));
  ST_CHECK(b.set_property("size", "18"));
  const auto size_a = a.get_property("font_size");
  const auto size_b = b.get_property("size");
  ST_REQUIRE(size_a.has_value());
  ST_REQUIRE(size_b.has_value());
  ST_CHECK_EQ(*size_a, *size_b);
  st::log::out("[ui-alias] font_size=%s size=%s\n", size_a->c_str(), size_b->c_str());
}

ST_TEST(ui_icon_diameter_is_not_font_size) {
  // ## 关键：图标/头像的 `size` 是**直径**，不是字号
  //
  // 若改名时一律改成 `font_size`，这条会红——它会证明"直径"这条路径
  // 不再接受 `font_size`（因为它压根不是字号）。
  st::ui::IconView icon_a;
  st::ui::IconView icon_b;
  ST_CHECK(icon_a.set_property("diameter", "24"));
  ST_CHECK(icon_b.set_property("size", "24"));
  const auto diameter_a = icon_a.get_property("diameter");
  const auto diameter_b = icon_b.get_property("size");
  ST_REQUIRE(diameter_a.has_value());
  ST_REQUIRE(diameter_b.has_value());
  ST_CHECK_EQ(*diameter_a, *diameter_b);

  // 图标**不接受** `font_size`（它不是文本）
  st::ui::IconView icon_c;
  ST_CHECK(!icon_c.set_property("font_size", "24"));
}

ST_TEST(ui_button_scale_enum_alias) {
  // 按钮的 `size` 是枚举 small/medium/large，新名 `scale`
  st::ui::Button button_a{"ok"};
  st::ui::Button button_b{"ok"};
  ST_CHECK(button_a.set_property("scale", "large"));
  ST_CHECK(button_b.set_property("size", "large"));
  const auto scale_a = button_a.get_property("scale");
  const auto scale_b = button_b.get_property("size");
  ST_REQUIRE(scale_a.has_value());
  ST_REQUIRE(scale_b.has_value());
  ST_CHECK_EQ(*scale_a, *scale_b);
  ST_CHECK_EQ(*scale_a, std::string("large"));

  // 非法枚举值必须被拒（而不是静默接受）
  st::ui::Button button_c{"ok"};
  ST_CHECK(!button_c.set_property("scale", "huge"));
}

ST_TEST(ui_new_names_are_advertised) {
  // `property_names()` 必须**同时**报新名（否则 `apply_properties` 认不出新名）
  st::ui::Text text;
  const auto names = text.property_names();
  bool has_font_size = false;
  for (const auto name : names) {
    if (name == "font_size") has_font_size = true;
  }
  ST_CHECK(has_font_size);

  st::ui::IconView icon;
  bool has_diameter = false;
  for (const auto name : icon.property_names()) {
    if (name == "diameter") has_diameter = true;
  }
  ST_CHECK(has_diameter);

  st::ui::Button button{"ok"};
  bool has_scale = false;
  for (const auto name : button.property_names()) {
    if (name == "scale") has_scale = true;
  }
  ST_CHECK(has_scale);
}
