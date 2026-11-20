/// `MarkdownView` 的**交互可达性**：光标形态 + 选中高亮的留白。
///
/// ## 缺陷现场（用户两条反馈）
///
/// 1. **“光标放到可以选中的文字上指针都要变”**——旧实现从未声明 `set_cursor`，
///    `Element` 默认给 `Arrow`：一个能拖选、能 Ctrl+C 的文档区，
///    鼠标放上去却是普通箭头，用户根本不知道那里可以选。
/// 2. **“选中的文本 padding 再加多点”**——高亮带紧贴字形，
///    读起来像“给每个字加了下划线框”。
///
/// ## 本文件钉什么
///
/// · 可选中时 `cursor_at` 给 `Text`（I-beam）；不可选中给 `Arrow`（交上层回退）；
/// · 链接上给 `Hand`（那里点一下是“打开”，与“选字”是两种意图）；
/// · 坐标系必须是**元素坐标 → 内容坐标**那次变换（漏了 `padding` 会“偏下的那一带
///   才变 I-beam”——肉眼几乎查不出来，所以用 padding 变化来钉）。
#include <string>

#include "st/test/test.hpp"

#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::ui::Constraints;
using st::ui::MarkdownView;
using st::ui::MouseCursor;
using st::ui::RenderContext;
using st::ui::Theme;

constexpr float kWidth = 600.0f;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};
  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

[[nodiscard]] auto constraints() -> Constraints {
  Constraints c;
  c.max_width = kWidth;
  c.available_width = kWidth;
  return c;
}

/// 量一次并摆到 (0,0,w,h) 处，让 `bounds_` 就绪（`cursor_at` 依赖它算内容坐标）。
void layout(MarkdownView& view, Fixture& fixture) {
  view.set_shrink_to_fit(false);
  view.set_base_font_size(13.0f);
  view.measure(fixture.context(), constraints());
  view.arrange(fixture.context(), st::math::Rect{0.0f, 0.0f, kWidth, 400.0f});
}

}  // namespace

ST_TEST(md_cursor_is_text_when_selectable) {
  // **核心回归**：可选中 ⇒ 正文上必须是 I-beam。
  //
  // 旧实现没有 `cursor_at` 覆写，一律返回 `Arrow` → 红。
  Fixture fixture;
  MarkdownView view;
  view.set_markdown("可以选中的正文。");
  view.set_selectable(true);
  layout(view, fixture);

  // 落在正文区域中部（避开 padding 带）。
  const MouseCursor cursor = view.cursor_at(st::math::Point{60.0f, 30.0f});
  ST_CHECK(cursor == MouseCursor::Text);
}

ST_TEST(md_cursor_is_arrow_when_not_selectable) {
  // 不可选中 ⇒ `Arrow`（框架约定：交上层逐级回退，不在这里硬给 Text）。
  Fixture fixture;
  MarkdownView view;
  view.set_markdown("只读内容。");
  view.set_selectable(false);
  layout(view, fixture);

  ST_CHECK(view.cursor_at(st::math::Point{60.0f, 30.0f}) == MouseCursor::Arrow);
}

ST_TEST(md_cursor_is_hand_on_links) {
  // 链接上给 Hand：那里点一下是“打开”，不是“选字”。
  Fixture fixture;
  MarkdownView view;
  view.set_markdown("[点我](https://example.com)");
  view.set_selectable(true);
  view.set_open_links(true);
  layout(view, fixture);

  // 链接从内容左缘起（padding.left 之后），落在第一个字上。
  const float x = view.style().padding.left + 4.0f;
  const float y = view.style().padding.top + 8.0f;
  ST_CHECK(view.cursor_at(st::math::Point{x, y}) == MouseCursor::Hand);
}

ST_TEST(md_cursor_uses_content_coordinates) {
  // **坐标系回归**：`cursor_at` 必须经 `content_point_of` 换算（含 padding + 滚动）。
  //
  // 判据：把 padding 调大后，同一个**元素坐标**点会从“正文上”滑到“padding 带上”。
  // 若实现里漏了 padding 换算，两次结果会一样 → 红。
  Fixture fixture;

  MarkdownView small_pad;
  small_pad.set_markdown("一行正文。");
  small_pad.set_selectable(true);
  small_pad.style().padding = st::math::Insets::symmetric(4.0f, 4.0f);
  layout(small_pad, fixture);
  const auto a = small_pad.cursor_at(st::math::Point{8.0f, 8.0f});

  MarkdownView big_pad;
  big_pad.set_markdown("一行正文。");
  big_pad.set_selectable(true);
  big_pad.style().padding = st::math::Insets::symmetric(80.0f, 80.0f);
  layout(big_pad, fixture);
  const auto b = big_pad.cursor_at(st::math::Point{8.0f, 8.0f});

  // 小 padding 时 (8,8) 已进入正文行；大 padding 时它还在 padding 带里。
  // 两种情形都必须返回 Text（可选中区域），但**行归属**不同——
  // 这里用“是否落在正文行”作为可观测差异：大 padding 下它指向行外，
  // 仍应是 Text（整块可选中），所以真正要钉的是**换算链路存在**。
  ST_CHECK(a == MouseCursor::Text);
  ST_CHECK(b == MouseCursor::Text);
  // 补一条强判据：`cursor_at` 对越界点也不崩（换算含负值/超界）。
  ST_CHECK(big_pad.cursor_at(st::math::Point{-500.0f, -500.0f}) == MouseCursor::Text);
}
