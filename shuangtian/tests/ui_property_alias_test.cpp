/// UI 属性名严格性测试。
///
/// ## 为什么这条现在是"严格性"而不是"兼容性"
///
/// 2026-10-31 起 `size` 兼容别名**已删除**：UI 的"大小"按语义拆成三个名字
/// （`font_size` / `diameter` / `scale`）。删除的理由是 `size` 与三维场景的
/// `size`（全长）同名不同义——那是模型最容易踩的坑（名字一样、含义无关、两边都不报错）。
///
/// 所以本文件现在验证的是**旧名必须被拒绝**，而不是"旧名仍然能用"。
/// 一个还能用的别名会让作者继续写错，而错误不会报出来。
///
/// ## 另一类问题：改错语义
///
/// `size` 在 UI 里有四种含义（字号/直径/枚举）。一律改成 `font_size`
/// 会把图标直径也叫成字号——那等于造出一个新的语义错误。
/// 所以下面分别验证：字号归字号、直径归直径、枚举归枚举。
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/element.hpp"

ST_TEST(ui_font_size_is_the_only_text_size_name) {
  // 字号：`font_size` 生效，`size` 被拒
  st::ui::Text text;
  ST_CHECK(text.set_property("font_size", "18"));
  const auto value = text.get_property("font_size");
  ST_REQUIRE(value.has_value());
  ST_CHECK_EQ(*value, std::string("18"));

  // 旧名 `size` 必须**失败**（不是静默忽略，也不是兼容）
  st::ui::Text legacy;
  ST_CHECK(!legacy.set_property("size", "18"));
  ST_CHECK(!legacy.get_property("size").has_value());
}

ST_TEST(ui_icon_diameter_is_not_font_size) {
  // ## 图标/头像的"大小"是**直径**，不是字号
  //
  // 若当初图省事把 `size` 一律改成 `font_size`，这条会红——
  // 它会证明"直径"这条路径不接受 `font_size`（因为它压根不是字号）。
  st::ui::IconView icon;
  ST_CHECK(icon.set_property("diameter", "24"));
  const auto diameter = icon.get_property("diameter");
  ST_REQUIRE(diameter.has_value());
  ST_CHECK_EQ(*diameter, std::string("24"));

  // 图标不接受字号，也不接受旧名
  st::ui::IconView other;
  ST_CHECK(!other.set_property("font_size", "24"));
  ST_CHECK(!other.set_property("size", "24"));
}

ST_TEST(ui_button_scale_is_an_enum_not_a_length) {
  // 按钮的"大小"是**枚举**（small/medium/large），名字是 `scale`
  st::ui::Button button{"ok"};
  ST_CHECK(button.set_property("scale", "large"));
  const auto scale = button.get_property("scale");
  ST_REQUIRE(scale.has_value());
  ST_CHECK_EQ(*scale, std::string("large"));

  // 非法枚举值必须被拒
  st::ui::Button invalid{"ok"};
  ST_CHECK(!invalid.set_property("scale", "huge"));

  // 旧名被拒
  st::ui::Button legacy{"ok"};
  ST_CHECK(!legacy.set_property("size", "large"));
}

ST_TEST(ui_new_names_are_advertised) {
  // `property_names()` 必须报新名（`apply_properties` 靠它认名字）
  st::ui::Text text;
  bool has_font_size = false;
  for (const auto name : text.property_names()) {
    if (name == "font_size") has_font_size = true;
    // 旧名不应再出现在声明清单里
    ST_CHECK(name != "size");
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
