/// 流式增长时 `MarkdownView` 的**高度自洽性**（小白气泡裁掉末行的回归）。
///
/// ## 缺陷现场
///
/// 小白的助手气泡里放一个 `MarkdownView`（`set_shrink_to_fit(true)`），
/// 流式每帧 `set_markdown(累积文本)`。内容长到超出父容器给的初始高度后，
/// 末几行会**被裁掉**（截图里最后一段话的下沿被切）。
///
/// ## 根因
///
/// `measure` 用**约束**（父容器建议的宽高）算尺寸；而 `arrange` 拿到的是
/// 父容器按上一轮实测结果给的矩形。内容变长后：`measure` 报出新的 `content_height_`，
/// 但父容器这一轮用的还是**旧高度** → `arrange` 给一个偏矮的矩形 →
/// `view_height_ = bounds_.height`（偏矮）→ 绘制按旧高裁剪 → 末行不可见。
///
/// 判据用**可量化的东西**：`measure` 报的高度必须等于内容的真实高度
/// （`content_height`），且在内容增长后**跟着增长**——不能停在上一轮的值。
#include <string>

#include "st/test/test.hpp"

#include "st/raster/canvas.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Rect;
using st::math::Size;
using st::ui::Constraints;
using st::ui::MarkdownView;
using st::ui::RenderContext;
using st::ui::Theme;

constexpr float kWidth = 420.0f;
constexpr float kCanvasHeight = 900.0f;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};

  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }

  /// 无界高度约束（气泡场景：MarkdownView 自己撑高，由外层 ScrollView 滚）。
  [[nodiscard]] static auto bubble_constraints() -> Constraints {
    Constraints constraints;
    constraints.max_width = kWidth;
    constraints.available_width = kWidth;
    constraints.max_height = st::ui::kUnbounded;
    constraints.available_height = st::ui::kUnbounded;
    return constraints;
  }
};

/// 生成 `lines` 行的正文（每行足够长，必然折行）。
[[nodiscard]] auto long_text(std::size_t lines) -> std::string {
  std::string text;
  for (std::size_t index = 0; index < lines; ++index) {
    text += "第 " + std::to_string(index) +
            " 行：这是一段足够长的说明文字，用来保证在 420 像素宽下必然折行。\n\n";
  }
  return text;
}

}  // namespace

ST_TEST(md_view_height_grows_with_streamed_content) {
  // 核心回归：**内容变长 ⇒ 实测高度必须跟着长**。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);

  view.set_markdown(long_text(1));
  view.measure(fixture.context(), Fixture::bubble_constraints());
  const float short_height = view.measured_size().height;

  // 流式追加到 6 行（模拟打字机式增长）。
  view.set_markdown(long_text(6));
  view.measure(fixture.context(), Fixture::bubble_constraints());
  const float tall_height = view.measured_size().height;

  ST_CHECK(short_height > 0.0f);
  ST_CHECK(tall_height > short_height + 1.0f);
  // 实测高度必须等于内容全高（无界高度下不该被裁）。
  ST_CHECK(tall_height >= view.content_height() - 0.5f);
}

ST_TEST(md_view_height_survives_stale_arrange_rect) {
  // 真实链路：父容器用**上一轮**的高度调 `arrange`，内容已经变长。
  // 期望：`arrange` 之后仍能报告正确的内容高度，绘制不裁掉末行。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);

  // 第一帧：一行内容 → 父容器记下这个高度。
  view.set_markdown(long_text(1));
  view.measure(fixture.context(), Fixture::bubble_constraints());
  const float stale_height = view.measured_size().height;
  view.arrange(fixture.context(), Rect{0.0f, 0.0f, view.measured_size().width, stale_height});

  // 第二帧：内容长到 6 行，但父容器仍按**旧高度**摆。
  view.set_markdown(long_text(6));
  view.measure(fixture.context(), Fixture::bubble_constraints());
  const float grown = view.measured_size().height;
  ST_CHECK(grown > stale_height + 1.0f);

  view.arrange(fixture.context(), Rect{0.0f, 0.0f, view.measured_size().width, stale_height});

  // 内容高度（排版的真值）必须体现新内容，而不是停在旧值。
  ST_CHECK(view.content_height() > stale_height + 1.0f);
}

ST_TEST(md_view_height_shrinks_back_when_content_shrinks) {
  // 反向：内容变短 ⇒ 高度必须回落（否则气泡会留一大片空白）。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);

  view.set_markdown(long_text(8));
  view.measure(fixture.context(), Fixture::bubble_constraints());
  const float tall = view.measured_size().height;

  view.set_markdown(long_text(1));
  view.measure(fixture.context(), Fixture::bubble_constraints());
  const float small = view.measured_size().height;

  ST_CHECK(small > 0.0f);
  ST_CHECK(small < tall - 1.0f);
}
