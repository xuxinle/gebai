/// 气泡宽度语义：**短内容随内容变窄，长内容铺满可用宽**。
///
/// ## 缺陷现场（用户报“还是限制了宽度，这是一行，应该撑开气泡的宽度”）
///
/// 应用里两处都收窄了气泡，任一处都足以让长回答被挤成细条：
///
/// 1. **固定像素上限**：`kMaxBubbleWidth = 520`——即使窗口有 1178 的可用宽，
///    长正文也只按 520 折行，右侧留一大片空白。
/// 2. **用 `spacer` 做对齐**：气泡旁边放一个 `grow` 的空元素把它推向一侧，
///    而 `grow` 会**吃掉行内全部剩余宽** ⇒ 气泡永远只能长到“行宽的一半”左右。
///    对齐是个位置问题，不该占用宽度预算。
///
/// ## 本文件钉什么
///
/// 气泡的宽度由 `MarkdownView` 的 `shrink_to_fit` 决定：
/// 短内容 → 收缩到内容宽；长内容（会折行）→ 铺满可用宽。
/// 上限交给父级约束（窗口宽减边距），**不设应用层魔数**。
#include <string>

#include "st/test/test.hpp"

#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::ui::Constraints;
using st::ui::MarkdownView;
using st::ui::RenderContext;
using st::ui::Theme;

/// 消息区可用宽（窗口 1178 减两侧 14px 边距再减气泡外边距后的量级）。
constexpr float kAvailableWidth = 1138.0f;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};
  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

[[nodiscard]] auto available_constraints() -> Constraints {
  Constraints constraints;
  constraints.max_width = kAvailableWidth;
  constraints.available_width = kAvailableWidth;
  return constraints;
}

/// 一段必然折行的长正文。
[[nodiscard]] auto long_body() -> std::string {
  std::string text;
  for (int index = 0; index < 8; ++index) {
    text += "第 " + std::to_string(index) +
            " 段：向量数据库把文本、图片、音频这类非结构化数据交给嵌入模型编码成一串"
            "高维浮点数，用这串数字代表数据的内容与含义。\\n\\n";
  }
  return text;
}

}  // namespace

ST_TEST(md_bubble_long_content_fills_available_width) {
  // **核心回归**：长正文（会折行）必须铺满可用宽——不是停在某个固定像素。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);
  view.set_markdown(long_body());

  view.measure(fixture.context(), available_constraints());
  ST_CHECK(view.measured_size().width >= kAvailableWidth - 1.0f);
}

ST_TEST(md_bubble_short_content_stays_narrow) {
  // 反向：短内容必须**收缩**（气泡贴合文字，不留大片空白）。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);
  view.set_markdown("你好");

  view.measure(fixture.context(), available_constraints());
  const float narrow = view.measured_size().width;
  ST_CHECK(narrow > 0.0f);
  // 两个字的气泡应当远窄于可用宽（桩是 8px/字符 + 内边距）。
  ST_CHECK(narrow < kAvailableWidth * 0.5f);
}

ST_TEST(md_bubble_width_tracks_content_growth) {
  // 流式增长：先窄、后铺满（宽度必须跟着内容走，不能一次定死）。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);

  view.set_markdown("你好");
  view.measure(fixture.context(), available_constraints());
  const float narrow = view.measured_size().width;

  view.set_markdown(long_body());
  view.measure(fixture.context(), available_constraints());
  const float full = view.measured_size().width;

  ST_CHECK(full > narrow + 8.0f);
  ST_CHECK(full >= kAvailableWidth - 1.0f);
}

ST_TEST(md_bubble_never_exceeds_available_width) {
  // 上限仍然存在（父级约束），只是**没有应用层的固定魔数**：
  // 可用宽变小时，气炮也必须跟着变小（窗口缩小的行为）。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);
  view.set_markdown(long_body());

  Constraints narrow_constraints;
  narrow_constraints.max_width = 400.0f;
  narrow_constraints.available_width = 400.0f;
  view.measure(fixture.context(), narrow_constraints);
  ST_CHECK(view.measured_size().width <= 400.0f + 1.0f);
}
