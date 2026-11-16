/// `ScrollView` 的内容高：**必须取去重后的尺寸，不能读子元素样式的 `height`**。
///
/// ## 为什么这条是 `List::measure` 的配套契约
///
/// `List` 的显式 `height` 是**单行高**（见 `list_explicit_height_is_row_height_not_container_height`），
/// 它的容器高由 `measure()` 按行数累加得出。
///
/// 于是 `ScrollView` 若按 `child.style().height` 算内容高，就拿到那个 22
/// ——**滚动宿主以为内容只有 22px**：`max_scroll = 0`、滚动条不出现、
/// 后面 163 行被当成溢出丢掉（实测 gbcode 大纲：164 个符号只显示 1 行）。
///
/// 这两条是**同一个缺陷的两半**：只修 `List::measure` 而 `ScrollView` 仍读
/// `style().height`，画面上依然只有一行。所以两者必须各自有用例。
///
/// ## 判据为什么用“容器里真的能滚”
///
/// 单看 `content_height()` 也能断言，但那只是中间量。用户可见的事实是
/// **`max_scroll_y() > 0`（滚动条该出现）** 与 **滚动后视口真的动了**。
/// 前者能抓住“内容高算错”，后者能抓住“算了但没接进滚动量”。

#include "st/test/test.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/scroll_view.hpp"
#include "st/ui/theme.hpp"

namespace {

[[nodiscard]] auto context_of() -> st::ui::RenderContext {
  static const st::ui::Theme theme = st::ui::Theme::light();
  return st::ui::RenderContext{theme, nullptr, 0.0};
}

/// 造一个「List 装 N 行、行高 22」的滚动宿主（与 gbcode 大纲同参数）。
[[nodiscard]] auto make_scrolling_list(std::size_t rows, float viewport_height)
    -> std::unique_ptr<st::ui::ScrollView> {
  auto view = std::make_unique<st::ui::ScrollView>();
  auto* list = dynamic_cast<st::ui::List*>(view->add_child(std::make_unique<st::ui::List>()));
  std::vector<st::ui::List::Entry> entries;
  entries.reserve(rows);
  for (std::size_t index = 0; index < rows; ++index) {
    entries.push_back(st::ui::List::Entry{.key = "row-" + std::to_string(index),
                                          .label = "行 " + std::to_string(index)});
  }
  list->sync_items(entries);
  list->set_item_gap(0.0f);
  // 关键：显式 height 是**单行高**，不是容器高。
  list->style().height = 22.0f;

  const auto context = context_of();
  st::ui::Constraints constraints;
  constraints.max_width = 240.0f;
  constraints.max_height = viewport_height;
  constraints.available_width = 240.0f;
  constraints.available_height = viewport_height;
  view->measure(context, constraints);
  view->arrange(context, st::math::Rect{0.0f, 0.0f, 240.0f, viewport_height});
  return view;
}

}  // namespace

/// 内容高按 **行数×行高** 算，滚动条因此出现（不是“内容只有 22px”）。
ST_TEST(scroll_content_height_uses_measured_size_not_style_height) {
  constexpr std::size_t kRows = 164;     // gbcode 大纲当年的真实规模
  constexpr float kRowHeight = 22.0f;
  constexpr float kViewport = 511.0f;

  auto view = make_scrolling_list(kRows, kViewport);
  const float content_height = view->content_height();
  st::log::out("[scroll] {} 行 × {} = {}；宿主 content_height = {}\n", kRows, kRowHeight,
               static_cast<double>(kRows * kRowHeight), static_cast<double>(content_height));

  ST_CHECK_NEAR(content_height, static_cast<float>(kRows) * kRowHeight, 1.0f);

  // **反向护栏**：旧实现会得到 22（style().height）。若真如此，这里会直接抓到。
  ST_CHECK(content_height > kRowHeight * 2.0f);

  // 用户可见的事实：内容远超视口 ⇒ 必须有滚动量、滚动条该出来。
  ST_CHECK(view->max_scroll() > 0.0f);
  st::log::out("[scroll] max_scroll = {}\n", static_cast<double>(view->max_scroll()));
}

/// 滚到底时视口**真的动了**（防“算了内容高但没接进滚动量”）。
ST_TEST(scroll_reaches_last_row_when_content_is_tall) {
  constexpr std::size_t kRows = 100;
  constexpr float kViewport = 200.0f;

  auto view = make_scrolling_list(kRows, kViewport);
  const float max_scroll = view->max_scroll();
  ST_REQUIRE(max_scroll > 0.0f);

  view->scroll_to(max_scroll * 2.0f);   // 超出上界：必须夹到 max
  st::log::out("[scroll] 请求滚动 {} → 实际 {}\n", static_cast<double>(max_scroll * 2.0f),
               static_cast<double>(view->scroll_offset()));
  ST_CHECK_NEAR(view->scroll_offset(), max_scroll, 0.5f);

  // 内容高 − 实际滚动 = 视口高（滚到底的口径）。
  ST_CHECK_NEAR(view->content_height() - view->scroll_offset(), kViewport, 1.0f);
}
