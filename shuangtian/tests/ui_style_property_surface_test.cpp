/// 「`Style` 的每个字段都能通过控制通道读写」——框架层的**能力回归**。
///
/// ## 为什么需要这条
///
/// `Style` 有一份权威键表（`sheet_key_names()`，见 `stylesheet.hpp`）与一条统一
/// 解析层（`apply_stylesheet`）。但**读**没有对应通路：调用方 `set` 之后只能凭
/// 写入值猜“到底生效了没有”，而这些值多数会被主题回放与 `apply_declared_paint`
/// 改写——于是“设了、回读也对、画面不动”成了这类问题的固定形态。
///
/// 实测代价（2026-11-20）：调一个玻璃边缘参数，每档都要改代码、重编、重启、截图，
/// 一轮 20~30 s，而真正需要的只是“改个数字看看”。
///
/// ## 这条测试钉住两件事
///
/// 1. **字段名 ≠ 样式串键名**。`Style::backdrop_blur` 在样式串里叫 `blur`、
///    `highlight_top` 叫 `edge_top`、`highlight_width` 叫 `ring`。
///    用字段名直接拼样式串会得到“未知键”——实测 11 个键里 9 个静默不生效
///    （`set` 返回 changed 为空、读回还是旧值）。
/// 2. **写进去要能读回来**，且读到的是**实际生效值**。
#include <memory>

#include "st/test/test.hpp"

#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/style.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Card;
using st::ui::Element;

/// 拿一个带 `Style` 的元素（`Card` 会自行填默认值，正是真实工况）。
struct Fixture {
  st::ui::UiRoot root;
  Card* card{nullptr};

  Fixture() {
    root.set_viewport({400.0f, 300.0f});
    auto owned = std::make_unique<Card>();
    card = owned.get();
    card->set_id("probe");
    root.set_content(std::move(owned));
    root.layout(true);
  }
};

/// 写一个属性，再读回来。
auto roundtrip(Element& element, std::string_view name, std::string_view value)
    -> std::optional<std::string> {
  if (!element.set_property(name, value)) return std::nullopt;
  return element.get_property(name);
}

}  // namespace

/// ① **字段名与样式串键名不同**的那批必须都能写。
///
/// 这条是逆向验证的靶子：把 `sheet_key` 改回字段名，下面每一条都会红。
ST_TEST(style_fields_writable_via_control_channel) {
  Fixture fixture;
  Element& element = *fixture.card;

  // 名字 → 期望值（都是**样式串里的键名**对应的字段）
  const std::pair<std::string_view, std::string_view> cases[] = {
      {"backdrop_blur", "22"},        // 键名 blur
      {"backdrop_tint", "0.55"},      // 键名 tint
      {"backdrop_refraction", "4"},   // 键名 refraction
      {"backdrop_dispersion", "0.1"}, // 键名 dispersion
      {"highlight_top", "0.8"},       // 键名 edge_top
      {"highlight_bottom", "0.3"},    // 键名 edge_bottom
      {"highlight_left", "0.15"},     // 键名 edge_left
      {"highlight_right", "0.15"},    // 键名 edge_right
      {"highlight_width", "1.6"},     // 键名 ring
      {"opacity", "0.95"},            // 键名同名
  };
  for (const auto& [name, value] : cases) {
    const auto read_back = roundtrip(element, name, value);
    // std::string(name) + " 应当可写
    ST_CHECK(read_back.has_value());
    if (!read_back.has_value()) continue;
    const float actual = std::strtof(read_back->c_str(), nullptr);
    const float expected = std::strtof(std::string(value).c_str(), nullptr);
    // std::string(name) + " 写 " + std::string(value) + " 读回 " + *read_back
    ST_CHECK(std::abs(actual - expected) < 0.001f);
  }
}

/// ② 每个写进去的字段都必须**在属性面上可见**（`property_names()` 声明过）。
///
/// 只写不声明，快照里就没有这个键——调用方（尤其自动化）靠快照判断
/// “改成了什么”，缺一个名就是一次静默失明。
ST_TEST(style_fields_are_declared_in_property_names) {
  Fixture fixture;
  const auto declared = fixture.card->property_names();
  const std::string_view expected[] = {
      "backdrop_blur", "backdrop_tint", "backdrop_refraction", "backdrop_dispersion",
      "highlight_top", "highlight_bottom", "highlight_left", "highlight_right",
      "highlight_width", "top_highlight", "opacity",
      // 短名别名（与样式串同口径，报账好认）
      "glass_edge_top", "glass_ring", "glass_highlight", "glass_highlight_color",
  };
  for (const auto name : expected) {
    const bool found = std::ranges::find(declared, name) != declared.end();
    // std::string(name) + " 未在 property_names() 里声明
    ST_CHECK(found);
  }
}

/// ③ 四条边的**符号语义**：正 = 亮边、负 = 暗边、0 = 不画。
///
/// 这条钉住的是“压暗必须能用负值表达”——暗色主题要亮边，亮色主题要暗边，
/// 而半透明白叠在灰底上永远是“变亮”（实测侧边 +21/+42），所以暗边
/// 只能靠负权重走黑色叠加。
///
/// **存取层不夹取**：写 2 就存 2，夹取只发生在绘制时的 alpha 映射
///（`std::clamp(|weight|, 0, 1)`）。这里断言的就是这个分工——若是存取层
/// 静默夹取，调用方就再也拿不回自己写的值，而“读回值 == 写入值”是
/// 自动化断言的前提。
ST_TEST(style_edge_weights_accept_negative_and_zero) {
  Fixture fixture;
  Element& element = *fixture.card;
  const std::pair<std::string_view, std::string_view> cases[] = {
      {"highlight_left", "-0.45"},
      {"highlight_right", "-1"},
      {"highlight_top", "0"},
      {"highlight_bottom", "2"},   // 超出 ±1 也存原值，绘制时才封顶
  };
  for (const auto& [name, value] : cases) {
    const auto read_back = roundtrip(element, name, value);
    // 每个名字都应接受写入并回读
    ST_CHECK(read_back.has_value());
    if (!read_back.has_value()) continue;
    const float actual = std::strtof(read_back->c_str(), nullptr);
    const float expected = std::strtof(std::string(value).c_str(), nullptr);
    // 存取层**不夹取**：读回值必须等于写入值（含负数与超界）
    ST_CHECK(std::abs(actual - expected) < 0.001f);
  }
  // 负值必须真的留住（不被夹成 0）
  const auto negative = roundtrip(element, "highlight_left", "-0.45");
  ST_CHECK(negative.has_value());
  if (negative.has_value()) {
    ST_CHECK(std::strtof(negative->c_str(), nullptr) < -0.4f);
  }
}

/// ④ 高光**色相**必须能改，且改色不把强度清零。
///
/// 亮色主题下卡片近乎透明（卡内 244 vs 背景 239），加亮没有空间，
/// 只能靠色相拉开差异——这条通路断了，亮色就调不动。
ST_TEST(style_highlight_color_keeps_strength) {
  Fixture fixture;
  Element& element = *fixture.card;
  // 先设一个已知强度
  ST_CHECK(element.set_property("glass_highlight", "0.5"));
  const auto before = element.get_property("glass_highlight");
  ST_CHECK(before.has_value());
  // 再换色相
  ST_CHECK(element.set_property("glass_highlight_color", "#8FB4FF"));
  const auto color = element.get_property("glass_highlight_color");
  ST_CHECK(color.has_value());
  if (color.has_value()) ST_CHECK(color->find("8F") != std::string::npos ||
                                  color->find("143") != std::string::npos);
  // 强度不应被清零
  const auto after = element.get_property("glass_highlight");
  ST_CHECK(after.has_value());
  if (after.has_value()) {
    // 换色相不应该把强度清零
    ST_CHECK(std::strtof(after->c_str(), nullptr) > 0.4f);
  }
}
