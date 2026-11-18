/// `MarkdownView` 的**排版宽口径一致性**（小白气泡裁行/宽度不随内容走的回归）。
///
/// ## 缺陷现场
///
/// 气泡链：`Row` → `Card(max_width=520)` → `MarkdownView(shrink_to_fit)`。
/// `Element::measure` 算「可用宽」时只取父级 `constraints.max_width`，
/// 没把本元素自己的 `max_width` 算进去，于是无界（1e9）一路传到下游：
/// `measure` 按 1106 排版（行少），`arrange` 只拿到真实的 488（行多）。
///
/// 同一份内容两个口径 ⇒ 末行被裁、`max_scroll > 0`，且**多轮采样一模一样**
/// （每轮都错得一致，看起来像“稳定的渲染 bug”）。
///
/// ## 本文件钉什么
///
/// 缺陷出现在**约束层**（`Element::measure` / `Element::arrange`），
/// 那里的直接回归在 `ui_measure_width_constraint_test.cpp`。
/// 这里钉的是**组件侧的表现**：给定同一个宽，`measure` 报出的内容高
/// 必须与 `arrange` 按该宽排出的内容高一致——不许出现“内部还要滚一段”。
#include <algorithm>
#include <cmath>
#include <string>

#include "st/test/test.hpp"

#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Rect;
using st::ui::Constraints;
using st::ui::MarkdownView;
using st::ui::RenderContext;
using st::ui::Theme;

/// 气泡的真实框宽（逻辑 px）。
constexpr float kBubbleWidth = 488.0f;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};
  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

/// 有界宽约束：宽给 `kBubbleWidth`，高无界（气泡在纵向可自由长高）。
[[nodiscard]] auto bubble_constraints() -> Constraints {
  Constraints constraints;
  constraints.max_width = kBubbleWidth;
  constraints.available_width = kBubbleWidth;
  return constraints;
}

/// 足够长的正文：在 488 宽下必然折行成多行，
/// 这样“按不同宽排”才会得到**不同**的高度（短句时两个宽不折行、高度相等，
/// 那样的测试回退修复后依然全绿——逆向验证抓出来的）。
[[nodiscard]] auto long_body() -> std::string {
  std::string text;
  for (int index = 0; index < 8; ++index) {
    text += "第 " + std::to_string(index) +
            " 段：向量数据库把文本、图片、音频这类非结构化数据交给嵌入模型编码成一串"
            "高维浮点数，用这串数字代表数据的内容与含义，于是检索就从字面匹配变成"
            "在向量空间里找最近的邻居。\\n\\n";
  }
  return text;
}

}  // namespace

ST_TEST(md_view_measure_height_matches_arrange) {
  // **核心回归**：同一个宽下，`measure` 报的内容高必须与 `arrange` 排出来的一致。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);
  view.set_markdown(long_body());

  view.measure(fixture.context(), bubble_constraints());
  const float measured_height = view.measured_size().height;
  const float content_at_measure = view.content_height();
  ST_CHECK(measured_height > 0.0f);
  ST_CHECK(measured_height >= content_at_measure - 0.5f);

  // 按自报尺寸摆位（父容器就是这么干的）。
  view.arrange(fixture.context(), Rect{0.0f, 0.0f, view.measured_size().width, measured_height});

  // 排完之后：内容高不得因换宽而暴涨（说明两处用的是同一个宽）。
  ST_CHECK(view.content_height() <= measured_height + 1.0f);
  // 且视口与内容自洽（没有“内部还要滚一段”的残差）。
  // 走**属性面**读：`max_scroll()` 是私有实现细节。
  const std::string max_scroll = view.get_property("max_scroll").value_or("999");
  ST_CHECK(std::stof(max_scroll) <= 0.5f);
}

ST_TEST(md_view_width_shrinks_for_short_content) {
  // 短内容仍要能**收缩**（气泡随短消息变窄）。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);
  view.set_markdown("你好");

  view.measure(fixture.context(), bubble_constraints());
  const float narrow = view.measured_size().width;
  ST_CHECK(narrow > 0.0f);
  ST_CHECK(narrow < kBubbleWidth - 8.0f);
}

ST_TEST(md_view_width_expands_for_long_content) {
  // 会折行的长文必须**占满**框宽（不能停在短句那个窄宽上）。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);
  view.set_markdown(long_body());

  view.measure(fixture.context(), bubble_constraints());
  ST_CHECK(view.measured_size().width >= kBubbleWidth - 1.0f);
}

ST_TEST(md_view_measure_is_stable_across_repeat) {
  // 连续多轮 measure/arrange（模拟每帧重排）：尺寸必须稳定，不许越排越大/越小。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(13.0f);
  view.set_markdown(long_body());

  view.measure(fixture.context(), bubble_constraints());
  const float first_width = view.measured_size().width;
  const float first_height = view.measured_size().height;

  for (int round = 0; round < 5; ++round) {
    view.arrange(fixture.context(),
                 Rect{0.0f, 0.0f, view.measured_size().width, view.measured_size().height});
    view.measure(fixture.context(), bubble_constraints());
  }

  ST_CHECK(std::abs(view.measured_size().width - first_width) <= 0.5f);
  ST_CHECK(std::abs(view.measured_size().height - first_height) <= 0.5f);
  ST_CHECK(std::stof(view.get_property("max_scroll").value_or("999")) <= 0.5f);
}
