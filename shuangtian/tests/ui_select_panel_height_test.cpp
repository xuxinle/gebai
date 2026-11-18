/// `SelectPanel` 的**限高与滚动**（下拉列表不许溢出窗口）。
///
/// ## 缺陷现场
///
/// 旧实现按选项数硬算高度：`height = options × row_height + padding`。
/// 而浮层宿主传给面板的约束是**整个窗口矩形**（`OverlayHost::arrange` 用满幅重测），
/// 面板只知道“窗口多高”，不知道**从锚点往下还剩多少**。
/// 于是靠近窗口底部打开的下拉会直接画到屏幕外：底部选项既看不见、也滚不到
/// （`index_at` 还会把光标命中的行算成屏幕外那些看不见的行）。
///
/// ## 本文件钉什么
///
/// - 面板高度必须被“锚点以下的剩余空间”限制住；
/// - 超出部分可以滚动看到（滚轮改变偏移，且被夹在 `[0, content - viewport]`）；
/// - 可见区之外的选项**不可命中**（否则点击会选中看不见的那一项）。
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "st/test/test.hpp"

#include "st/ui/components/select_panel.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Point;
using st::math::Rect;
using st::ui::Constraints;
using st::ui::RenderContext;
using st::ui::SelectOption;
using st::ui::SelectPanel;
using st::ui::Theme;

/// 面板行高（与实现同口径：`select.cpp` 的 `k_row_height`）。
constexpr float kRowHeight = 32.0f;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};
  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

[[nodiscard]] auto make_options(std::size_t count) -> std::vector<SelectOption> {
  std::vector<SelectOption> options;
  options.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    options.push_back(SelectOption{std::to_string(index), "选项 " + std::to_string(index)});
  }
  return options;
}

/// 摆一次（measure + arrange），返回面板实际矩形。
[[nodiscard]] auto layout_panel(Fixture& fixture, SelectPanel& panel, float viewport_width,
                                float viewport_height) -> Rect {
  Constraints constraints;
  constraints.max_width = viewport_width;
  constraints.max_height = viewport_height;
  constraints.available_width = viewport_width;
  constraints.available_height = viewport_height;
  panel.measure(fixture.context(), constraints);
  panel.arrange(fixture.context(), Rect{0.0f, 0.0f, viewport_width, viewport_height});
  return panel.bounds();
}

}  // namespace

ST_TEST(select_panel_height_clamped_by_remaining_space) {
  // **核心回归**：锚点在窗口底部附近、选项很多时，面板必须被限制高度，
  // 而不是按选项数一路撑出窗口。
  Fixture fixture;
  SelectPanel panel(make_options(20), std::nullopt);
  panel.set_panel_width(200.0f);
  // 锚点刻意贴近窗口底部（视口 600，锚点 y=520）。
  panel.set_anchor(Point{0.0f, 520.0f});

  const Rect bounds = layout_panel(fixture, panel, 800.0f, 600.0f);

  // 面板底边不得越过窗口底边（留 1px 容差）。
  ST_CHECK(bounds.bottom() <= 600.0f + 1.0f);
  // 且确实被压矮了（20 个选项的自然高远不止这点）。
  ST_CHECK(bounds.height < 20.0f * kRowHeight);
}

ST_TEST(select_panel_scrolls_when_content_exceeds_viewport) {
  // 被压矮之后，滚动偏移必须能移动，且被夹在合法范围内。
  Fixture fixture;
  SelectPanel panel(make_options(20), std::nullopt);
  panel.set_panel_width(200.0f);
  panel.set_anchor(Point{0.0f, 100.0f});

  (void)layout_panel(fixture, panel, 800.0f, 400.0f);

  // 内容高于可见区 ⇒ 存在可滚空间。
  const float content = panel.content_height();
  ST_CHECK(content > panel.bounds().height + 0.5f);
  ST_CHECK_EQ(static_cast<int>(panel.scroll_offset()), 0);

  // 向下滚一大步：偏移必须前进，但不超过“内容减视口”。
  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.wheel_delta = -120.0f;   // 向下（系统约定：向上为正）
  const bool handled = panel.on_event(fixture.context(), wheel);
  ST_CHECK(handled);
  ST_CHECK(panel.scroll_offset() > 0.0f);
  ST_CHECK(panel.scroll_offset() <= content - panel.bounds().height + 0.5f);
}

ST_TEST(select_panel_short_list_does_not_scroll) {
  // 选项少（装得下）时**不该**被限高，也不该有滚动空间。
  Fixture fixture;
  SelectPanel panel(make_options(3), std::nullopt);
  panel.set_panel_width(200.0f);
  panel.set_anchor(Point{0.0f, 100.0f});

  const Rect bounds = layout_panel(fixture, panel, 800.0f, 600.0f);
  ST_CHECK(bounds.height > 3.0f * kRowHeight - 1.0f);
  ST_CHECK(panel.content_height() <= bounds.height + 0.5f);

  // 滚轮在“装得下”时必须**让给外层**（返回 false），不静默消费。
  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.wheel_delta = -120.0f;
  ST_CHECK(!panel.on_event(fixture.context(), wheel));
}

ST_TEST(select_panel_invisible_rows_are_not_clickable) {
  // 滚出可见区的行不能命中——否则点下去会选中看不见的那一项。
  Fixture fixture;
  SelectPanel panel(make_options(20), std::nullopt);
  panel.set_panel_width(200.0f);
  panel.set_anchor(Point{0.0f, 100.0f});

  const Rect bounds = layout_panel(fixture, panel, 800.0f, 300.0f);
  // 高度被限制 ⇒ 选项装不下。
  ST_CHECK(bounds.height < 20.0f * kRowHeight);

  // 取最后一个选项的矩形：它应当落在面板可见区之外。
  const Rect last = panel.option_rect(19);
  ST_CHECK(last.y >= bounds.bottom() - 0.5f || last.bottom() <= bounds.y + 0.5f);

  // 在该行中心点击：不能被当作有效命中（返回 true = 已处理但未选中任何项）。
  st::ui::Event click;
  click.kind = st::ui::EventKind::Click;
  click.position = Point{last.x + last.width * 0.5f, last.y + last.height * 0.5f};
  bool picked = false;
  panel.on_pick = [&picked](std::size_t) { picked = true; };
  (void)panel.on_event(fixture.context(), click);
  ST_CHECK(!picked);
}
