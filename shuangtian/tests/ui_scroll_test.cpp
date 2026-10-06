/// 滚动容器（`ScrollView`）的**方向与夹取**测试。
///
/// 为什么专门测方向：滚轮方向取错符号时**不会报任何错**——事件照样被处理
/// （`handled=true`）、偏移只是被夹在 0，表现为"往下滚滚不动、页面下半截永远看不到"。
/// 这种"静默失效"最容易在人工点几下时被当成"内容就这样"，实测确实在画廊里躺了很久。

#include "st/test/test.hpp"

#include "st/ui/components/basic.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Rect;

/// 造一个「内容高 1000 / 视口高 200」的滚动容器。
[[nodiscard]] auto make_tall_scroll(st::ui::ScrollView& view) -> st::ui::RenderContext {
  view.style().padding = st::math::Insets{};
  for (int index = 0; index < 5; ++index) {
    auto block = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    block->style().height = 200.0f;
    block->style().width = 300.0f;
    view.add_child(std::move(block));
  }
  st::ui::RenderContext context{st::ui::Theme::light(), nullptr, 0.0};
  view.measure(context, st::ui::Constraints{.max_width = 320.0f, .max_height = 200.0f});
  view.arrange(context, Rect{0.0f, 0.0f, 320.0f, 200.0f});
  return context;
}

[[nodiscard]] auto wheel(float delta) -> st::ui::Event {
  st::ui::Event event;
  event.kind = st::ui::EventKind::Wheel;
  event.position = st::math::Point{100.0f, 100.0f};
  event.wheel_delta = delta;
  return event;
}

}  // namespace

ST_TEST(scroll_view_max_scroll_matches_overflow) {
  st::ui::ScrollView view;
  (void)make_tall_scroll(view);
  ST_CHECK_EQ(static_cast<int>(view.content_height()), 1000);
  ST_CHECK_EQ(static_cast<int>(view.view_height()), 200);
  ST_CHECK_EQ(static_cast<int>(view.max_scroll()), 800);
}

ST_TEST(scroll_view_wheel_direction_is_natural) {
  st::ui::ScrollView view;
  const st::ui::RenderContext context = make_tall_scroll(view);

  // 滚轮向下 = 负 delta（Win32 的 WM_MOUSEWHEEL 向上给 +120，X11 按钮 4 也是上）
  st::ui::Event down = wheel(-1.0f);
  ST_CHECK(view.on_event(context, down));
  ST_CHECK(down.handled);
  const float after_down = view.scroll_offset();
  ST_CHECK(after_down > 0.0f);  // 往下滚必须真的动（曾经这里恒为 0）
  ST_CHECK_EQ(static_cast<int>(after_down), static_cast<int>(st::ui::ScrollView::kStep));

  // 滚轮向上 = 正 delta → 回到顶部
  st::ui::Event up = wheel(1.0f);
  ST_CHECK(view.on_event(context, up));
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 0);
}

ST_TEST(scroll_view_wheel_clamps_at_both_ends) {
  st::ui::ScrollView view;
  const st::ui::RenderContext context = make_tall_scroll(view);

  for (int index = 0; index < 40; ++index) {
    st::ui::Event down = wheel(-1.0f);
    view.on_event(context, down);
  }
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 800);  // 底部不越界

  for (int index = 0; index < 40; ++index) {
    st::ui::Event up = wheel(1.0f);
    view.on_event(context, up);
  }
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 0);  // 顶部不为负
}

ST_TEST(scroll_view_ignores_wheel_when_content_fits) {
  st::ui::ScrollView view;
  auto block = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  block->style().height = 40.0f;
  auto* tracked = block.get();
  view.add_child(std::move(block));
  st::ui::RenderContext context{st::ui::Theme::light(), nullptr, 0.0};
  view.measure(context, st::ui::Constraints{.max_width = 320.0f, .max_height = 200.0f});
  view.arrange(context, Rect{0.0f, 0.0f, 320.0f, 200.0f});
  (void)tracked;

  st::ui::Event down = wheel(-1.0f);
  ST_CHECK(!view.on_event(context, down));  // 无可滚动空间：不消费事件（让父级决定）
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 0);
}

ST_TEST(scroll_view_keyboard_page_and_home_end) {
  st::ui::ScrollView view;
  const st::ui::RenderContext context = make_tall_scroll(view);

  st::ui::Event page_down;
  page_down.kind = st::ui::EventKind::KeyDown;
  page_down.key = "PageDown";
  ST_CHECK(view.on_event(context, page_down));
  const int after_page = static_cast<int>(view.scroll_offset());
  ST_CHECK(after_page > 0);
  ST_CHECK(after_page <= static_cast<int>(view.max_scroll()));

  st::ui::Event end;
  end.kind = st::ui::EventKind::KeyDown;
  end.key = "End";
  ST_CHECK(view.on_event(context, end));
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 800);

  st::ui::Event home;
  home.kind = st::ui::EventKind::KeyDown;
  home.key = "Home";
  ST_CHECK(view.on_event(context, home));
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 0);
}

ST_TEST(scroll_view_scroll_to_clamps_and_notifies) {
  st::ui::ScrollView view;
  (void)make_tall_scroll(view);
  float reported = -1.0f;
  view.set_on_scroll([&reported](float offset) { reported = offset; });

  view.scroll_to(120.0f);
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 120);
  ST_CHECK_EQ(static_cast<int>(reported), 120);

  view.scroll_to(99999.0f);  // 越界被夹取
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 800);
  ST_CHECK_EQ(static_cast<int>(reported), 800);

  view.scroll_to(-50.0f);
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 0);
}

ST_TEST(scroll_view_follow_end_pins_after_content_grows) {
  // 回归（2026-10-06）：日志/终端/输出窗要的是“新内容出现就在底部”。
  // 旧的提议做法（调用方自己 `scroll_to`）会**失败得很隐蔽**：
  // `max_scroll()` 取的是**上一次布局**的 `content_height_`，追加后立即滚会被旧上限夹住
  // （实测：内容已 331.8、上限还是 175.6 → 停在 175.6；等布局重新夹取时它已是合法偏移，
  // 没人知道它本该在底部——表现为滚动条永远差两行到底）。
  // `set_follow_end(true)` 把“在底部”变成持续意图：`arrange` 里量完新几何、
  // 归一化偏移后立即回到底。这条测试钉的就是这个时刻。
  st::ui::ScrollView view;
  (void)make_tall_scroll(view);
  view.set_follow_end(true);
  ST_CHECK(view.follow_end());

  // 再加一个高块：内容变长，重排后应自动在底部
  auto extra = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  extra->style().height = 300.0f;
  extra->style().width = 300.0f;
  view.add_child(std::move(extra));
  st::ui::RenderContext context{st::ui::Theme::light(), nullptr, 0.0};
  view.measure(context, st::ui::Constraints{.max_width = 320.0f, .max_height = 200.0f});
  view.arrange(context, Rect{0.0f, 0.0f, 320.0f, 200.0f});
  ST_CHECK(view.at_end());
  // 内容 1300 / 视口 200 → 底部偏移 1100
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 1100);

  // 关掉跟随（用户往上翻的场景）→ 不应再被拉回底部
  view.set_follow_end(false);
  view.scroll_to(0.0f);
  auto more = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  more->style().height = 300.0f;
  more->style().width = 300.0f;
  view.add_child(std::move(more));
  view.measure(context, st::ui::Constraints{.max_width = 320.0f, .max_height = 200.0f});
  view.arrange(context, Rect{0.0f, 0.0f, 320.0f, 200.0f});
  ST_CHECK_EQ(static_cast<int>(view.scroll_offset()), 0);
  ST_CHECK(!view.at_end());
}
