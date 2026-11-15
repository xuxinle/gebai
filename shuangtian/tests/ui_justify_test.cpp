/// `Justify::Center` / `End` 位移段的契约测试。
///
/// 缺陷（2026-11-15，st_desktop Dock 发现）：单行 `justify` 的二次位移循环把
/// **第一遍排布的位置**当基准、又按 `shift += width + gap` 累加了一遍步长——
/// 于是每个子项的落点 = 第一遍位置 + 偏移 + （已在第一遍里走过的步长），
/// 步长被加了两次。实测 7 个 30px 图标 + gap22：
/// 期望 418→1072（居中，步长 52），实际 418→1072 但**步长 104**（= 52×2），
/// 组整体右偏、且期望的居中起点恰好被「首项 x=0 + offset」掩盖。
///
/// 数学：第一遍 x = 0, 52, 104…；第二遍 moved.x += shift（shift 从 offset=418 起、
/// 每项后 += 52）→ 第二项 = 52 + (418+52) = 522 ≠ 52 + 418 = 470。
///
/// 修复判据：`justify` 位移必须**只加 offset**（每项同值），不重走步长；
/// 或第二遍从「第一遍结果 + offset」整体平移。本测试钉住正确语义。
#include <cmath>
#include <memory>

#include "st/test/test.hpp"

#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::IconView;
using st::ui::Justify;
using st::ui::Panel;

/// 造一个 Row 容器：`width` 宽、N 个 30px 图标、`justify` 与 `gap` 可指定。
[[nodiscard]] auto make_row(float width, std::size_t count, Justify justify, float gap)
    -> std::pair<std::unique_ptr<Panel>, std::vector<Element*>> {
  auto row = std::make_unique<Panel>(FlexDirection::Row);
  row->style().width = width;
  row->style().height = 64.0f;
  row->style().justify = justify;
  row->style().gap = gap;
  row->style().align_items = st::ui::Align::Center;
  std::vector<Element*> icons;
  for (std::size_t i = 0; i < count; ++i) {
    auto icon = std::make_unique<IconView>("folder", 30.0f);
    icon->set_id("item-" + std::to_string(i));
    icons.push_back(icon.get());
    row->add_child(std::move(icon));
  }
  return {std::move(row), icons};
}

}  // namespace

ST_TEST(justify_center_places_items_at_even_stride) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::RenderContext context{theme, nullptr, 0.0};

  auto [row, icons] = make_row(1178.0f, 7, Justify::Center, 22.0f);
  row->measure(context, st::ui::Constraints{});
  row->arrange(context, st::math::Rect{0.0f, 0.0f, 1178.0f, 64.0f});

  // 期望：组宽 7×30 + 6×22 = 342，offset = (1178-342)/2 = 418，步长 52。
  const float first_x = icons[0]->bounds().x;
  ST_CHECK(std::abs(first_x - 418.0f) < 0.5f);
  for (std::size_t i = 1; i < icons.size(); ++i) {
    const float stride = icons[i]->bounds().x - icons[i - 1]->bounds().x;
    ST_CHECK(std::abs(stride - 52.0f) < 0.5f);   // 30 + 22，不是 104
  }
  // 末项右缘 = 418 + 342 - 30 + 30 = 组右缘 760。
  const float last_right = icons.back()->bounds().right();
  ST_CHECK(std::abs(last_right - 760.0f) < 0.5f);
}

ST_TEST(justify_end_pins_last_item_to_right_edge) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::RenderContext context{theme, nullptr, 0.0};

  auto [row, icons] = make_row(1178.0f, 3, Justify::End, 22.0f);
  row->measure(context, st::ui::Constraints{});
  row->arrange(context, st::math::Rect{0.0f, 0.0f, 1178.0f, 64.0f});

  // 期望：末项右缘贴 content 右缘（1178），步长 52。
  ST_CHECK(std::abs(icons.back()->bounds().right() - 1178.0f) < 0.5f);
  const float stride = icons[1]->bounds().x - icons[0]->bounds().x;
  ST_CHECK(std::abs(stride - 52.0f) < 0.5f);
}
