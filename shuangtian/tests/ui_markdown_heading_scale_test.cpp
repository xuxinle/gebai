/// 标题字号阶梯：**跟随正文基准、且跨度压紧**。
///
/// ## 缺陷现场（用户：“markdown 组件渲染的标题比正文字体层级差距过大”）
///
/// 旧实现有两个独立问题，任一都足以让标题显得突兀：
///
/// 1. **标题字号是主题绝对值**（`font_3xl=34 / font_2xl=26 …`），不随调用方的正文字号变化。
///    聊天类界面常把正文调到 11~13px，于是：
///
///    | 场景 | 正文 | 旧 H1 | 倍数 |
///    |---|---|---|---|
///    | 小白气泡 | 13 | 34 | **2.6×** |
///    | 思考块 | 12 | 34 | **2.8×** |
///    | 工具输出 | 11 | 34 | **3.1×** |
///
/// 2. **`set_base_font_size` 只改正文、不重算标题**：`apply_theme` 只在挂树/换主题时跑，
///    所以“把正文改成 13px”之后标题一直停在 34px 上——问题 1 因此永远不会自愈。
///
/// ## 本文件钉什么
///
/// 用**内容高差**做判据（`FixedAdvanceTextPort` 的行高 = 字号 × 倍数，几何可比）：
/// · 上界：H1 与正文的内容高差 **≤ 18px**（旧实现在 13px 正文下是 21.2px）；
/// · 下界：仍要有可辨识的层次，差 **≥ 4px**（不许压平到完全一样）；
/// · 跟随：正文变小，标题也必须跟着变小；
/// · 单调：H1 ≥ H2 ≥ H3 ≥ H4。
///
/// 改 `kHeadingScale` / `kHeadingMargins` 后这四条会一起给出反馈。
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

constexpr float kWidth = 800.0f;
/// 各场景的真实正文字号（气泡 13 / 思考块 12 / 工具卡 11）。
constexpr float kBubbleBase = 13.0f;

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

/// 单个标题的内容高（含上间距 + 行高 + 下间距；H1/H2 还含分隔线夹距）。
[[nodiscard]] auto heading_height(Fixture& fixture, int level, float base) -> float {
  MarkdownView view;
  view.set_shrink_to_fit(false);
  view.set_base_font_size(base);
  view.set_markdown(std::string(static_cast<std::size_t>(level), '#') + " 标题");
  view.measure(fixture.context(), constraints());
  return view.content_height();
}

/// 单行正文的内容高（同口径的对照量）。
[[nodiscard]] auto body_height(Fixture& fixture, float base) -> float {
  MarkdownView view;
  view.set_shrink_to_fit(false);
  view.set_base_font_size(base);
  view.set_markdown("一行正文");
  view.measure(fixture.context(), constraints());
  return view.content_height();
}

}  // namespace

ST_TEST(md_heading_gap_to_body_is_bounded) {
  // **核心回归**：三个真实正文字号下，H1 与正文的内容高差都必须 ≤ 18px。
  //
  // 旧实现：base13 差 21.2px / base11 差 20.3px → 红。
  Fixture fixture;
  for (const float base : {20.0f, kBubbleBase, 12.0f, 11.0f}) {
    const float gap = heading_height(fixture, 1, base) - body_height(fixture, base);
    // 上界：跨度不许过大（这就是用户报的那件事）。
    ST_CHECK(gap <= 18.0f);
    // 下界：仍要有层次感，不许压平。
    ST_CHECK(gap >= 4.0f);
  }
}

ST_TEST(md_heading_scale_follows_base_font) {
  // **标题必须跟着正文字号走**：正文变小，标题也跟着变小。
  //
  // 旧实现里标题是不随 base 变化的绝对值 → 两档量出来一样高，红。
  Fixture fixture;
  for (const int level : {1, 2, 3}) {
    const float big = heading_height(fixture, level, 20.0f);
    const float small = heading_height(fixture, level, 12.0f);
    ST_CHECK(small < big);
    ST_CHECK(big - small > 6.0f);   // 20→12 是 0.6 倍，必须量得出差异
  }
}

ST_TEST(md_heading_levels_stay_monotonic) {
  // 层次不许被压平：H1 ≥ H2 ≥ H3 ≥ H4（单调不降）。
  Fixture fixture;
  const float h1 = heading_height(fixture, 1, kBubbleBase);
  const float h2 = heading_height(fixture, 2, kBubbleBase);
  const float h3 = heading_height(fixture, 3, kBubbleBase);
  const float h4 = heading_height(fixture, 4, kBubbleBase);

  ST_CHECK(h1 >= h2 - 0.5f);
  ST_CHECK(h2 >= h3 - 0.5f);
  ST_CHECK(h3 >= h4 - 0.5f);
  // H1 与 H4 之间必须拉开可辨识的距离（不是全部一样高）。
  ST_CHECK(h1 - h4 > 4.0f);
}

ST_TEST(md_heading_upper_levels_shrink_at_smaller_base) {
  // 工具卡（11px）里 H1 与正文的差同样受约束——小字号下最容易失控。
  Fixture fixture;
  const float gap = heading_height(fixture, 1, 11.0f) - body_height(fixture, 11.0f);
  ST_CHECK(gap <= 18.0f);
  ST_CHECK(gap >= 4.0f);
}
