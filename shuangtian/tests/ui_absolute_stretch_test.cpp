/// 绝对定位元素的「两边缘锚定 = 拉伸」契约测试。
///
/// 缺陷（2026-11-15，st_desktop 任务栏发现）：`place_absolute` 只支持两种定位——
/// `left/top` 定左上、或 `right/bottom` 按**自身测量宽度**贴边。缺少 CSS 的
/// `left:0; right:0` 语义（宽度由父宽反解），于是"铺满父容器的底栏/遮罩"写不出来：
/// 显式给 `width` 得把窗口尺寸抄一份到样式里（窗口一变就错），
/// 不给就收缩成内容宽（实测桌面 Dock 本该 1280 宽，实际 436 宽、贴右边）。
///
/// 三条契约：
/// 1. **`left + right` 且 `width = kAuto` → 宽度 = 父宽 − left − right**；
/// 2. **显式给 `width` 时仍按 `width` 贴右缘**（拉伸不得抢走显式尺寸）；
/// 3. **只给 `left`**（老用法）行为不变：宽度取测量值。
#include <cmath>
#include <memory>

#include "st/test/test.hpp"

#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Panel;

/// 父容器 1280×800，子元素按给定样式绝对定位；结果写进 `out`。
auto arrange_child(const st::ui::Theme& theme, void (*configure)(st::ui::Style&),
                   st::math::Rect* out) -> void {
  const st::ui::RenderContext context{theme, nullptr, 0.0};
  auto parent = std::make_unique<Panel>(FlexDirection::Column);
  parent->set_id("parent");
  auto child = std::make_unique<Panel>(FlexDirection::Row);
  child->set_id("child");
  child->style().absolute = true;
  child->style().height = 72.0f;
  configure(child->style());
  Element* raw = child.get();
  parent->add_child(std::move(child));
  parent->measure(context, st::ui::Constraints{});
  parent->arrange(context, st::math::Rect{0.0f, 0.0f, 1280.0f, 800.0f});
  *out = raw->bounds();
}

}  // namespace

ST_TEST(absolute_stretches_between_left_and_right_when_width_auto) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  st::math::Rect bounds{};
  arrange_child(
      theme,
      [](st::ui::Style& style) {
        style.left = 0.0f;
        style.right = 0.0f;
        style.bottom = 0.0f;
      },
      &bounds);
  // 全宽、贴底：x=0、宽 = 父宽（无 padding）、底缘贴父底。
  ST_CHECK(std::abs(bounds.x - 0.0f) < 0.5f);
  ST_CHECK(std::abs(bounds.width - 1280.0f) < 0.5f);
  ST_CHECK(std::abs(bounds.bottom() - 800.0f) < 0.5f);
}

ST_TEST(absolute_stretch_respects_left_right_insets) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  st::math::Rect bounds{};
  arrange_child(
      theme,
      [](st::ui::Style& style) {
        style.left = 24.0f;
        style.right = 24.0f;
      },
      &bounds);
  // 两侧各留 24：宽 = 1280 − 48。
  ST_CHECK(std::abs(bounds.x - 24.0f) < 0.5f);
  ST_CHECK(std::abs(bounds.width - 1232.0f) < 0.5f);
}

ST_TEST(absolute_explicit_width_wins_over_right_anchor) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  st::math::Rect bounds{};
  arrange_child(
      theme,
      [](st::ui::Style& style) {
        style.width = 200.0f;
        style.left = 0.0f;
        style.right = 20.0f;
      },
      &bounds);
  // 显式宽度优先：宽 200、贴右缘 20（x = 1280 − 20 − 200 = 1060）。
  ST_CHECK(std::abs(bounds.width - 200.0f) < 0.5f);
  ST_CHECK(std::abs(bounds.x - 1060.0f) < 0.5f);
}

ST_TEST(absolute_left_only_keeps_measured_width) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  st::math::Rect bounds{};
  arrange_child(
      theme,
      [](st::ui::Style& style) {
        style.left = 40.0f;
        style.top = 30.0f;
      },
      &bounds);
  // 老用法：位置按 left/top，宽度取测量值（Panel 空内容 → 0），**不被拉满**。
  ST_CHECK(std::abs(bounds.x - 40.0f) < 0.5f);
  ST_CHECK(std::abs(bounds.y - 30.0f) < 0.5f);
  ST_CHECK(bounds.width < 1.0f);
}
