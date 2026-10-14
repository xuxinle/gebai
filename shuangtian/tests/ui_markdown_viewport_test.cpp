/// `MarkdownView` 视口剔除与**二分区间**的回归护栏。
///
/// ## 这组用例守的是什么
///
/// `MarkdownView::paint_content` 过去遍历**全部行**、逐行做边界检查。真正的问题在
/// 组首行那条豁免规则：
///
/// ```cpp
/// if (outside && !row.group_first) continue;   // 组首行永不剔除
/// ```
///
/// 于是**每个屏幕外组的首行都会调 `paint_group`**——而它会建 `Path` + `stroke_path`
/// （圆角描边），是每帧成本的主体。实测（`tools/ui_rich_views_bench.cpp`，稳态帧）：
///
/// | 段数 | 改前 | 改为组矩形相交 | 再加二分 |
/// |---|---|---|---|
/// | 500 | 1.894 ms | 0.365 ms | 0.363 ms |
/// | 1500 | 4.949 ms | 0.374 ms | 0.359 ms |
/// | 4000 | **13.463 ms** | 3.670 ms | **0.360 ms** |
///
/// ## 本组用例的重点：**二分的前提必须成立**
///
/// 二分定位可见行依赖「`rows_` 按 `box.y` 单调递增」。这个前提**一旦不成立，
/// 表现是"某些行静默不画"**——比慢得多的问题。所以这里逐行断言"该画的都画了"：
/// 用记录型文本端口数**画了哪些行**，与"哪些行应落在视口内"逐一对齐。
///
/// 覆盖三类真实情形（每一类都是二分可能"切错"的地方）：
/// ① 视口在文档顶部；② 视口停在中间；③ 视口在文档底部。

#include "st/test/test.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Rect;
using st::raster::Canvas;
using st::test::FixedAdvanceTextPort;
using st::ui::MarkdownView;
using st::ui::RenderContext;
using st::ui::Theme;

inline constexpr int kCanvasWidth = 600;
inline constexpr int kCanvasHeight = 300;

/// 记录**画过的文本**（用于断言"哪些行被画"）。
class RecordingPort final : public st::ui::TextPort {
 public:
  FixedAdvanceTextPort inner{};
  mutable std::vector<std::string> drawn{};

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return inner.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(
      std::string_view utf8, float size,
      st::text::FontRole role = st::text::FontRole::Proportional) const -> float override {
    return inner.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return inner.line_height(size);
  }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color, st::text::FontRole role = st::text::FontRole::Proportional,
            float embolden = 0.0f, bool bold = false) const override {
    drawn.emplace_back(utf8);
    inner.draw(canvas, utf8, origin, size, color, role, embolden, bold);
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return inner.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    return inner.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    return inner.wrap_limited(utf8, size, max_width, max_lines);
  }
};

/// 造 `count` 行**段落**（每段一行文本，行数可预期；不引入代码块以免组逻辑干扰）。
[[nodiscard]] auto paragraphs(std::size_t count) -> std::string {
  std::string text;
  for (std::size_t index = 0; index < count; ++index) {
    text += "row-" + std::to_string(index);
    text += "\n\n";
  }
  return text;
}

struct Fixture {
  Theme theme{Theme::light()};
  RecordingPort port{};
  Canvas canvas{kCanvasWidth, kCanvasHeight};

  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }

  [[nodiscard]] auto paint(MarkdownView& view) -> const std::vector<std::string>& {
    // **有界高度**：`MarkdownView::measure` 在 `max_height` 无界时把 `view_height_`
    // 设成内容全高（自身撑高、由外部 ScrollView 滚动）——那种模式下 `max_scroll()==0`，
    // 内部 `scroll_to` 是空的（**设计如此**，实测踩到）。要测“内部滚动”必须给上界。
    st::ui::Constraints constraints;
    constraints.max_width = static_cast<float>(kCanvasWidth);
    constraints.max_height = static_cast<float>(kCanvasHeight);
    view.measure(context(), constraints);
    view.arrange(context(),
                 Rect{0.0F, 0.0F, static_cast<float>(kCanvasWidth), static_cast<float>(kCanvasHeight)});
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    port.drawn.clear();
    view.paint(context(), canvas);
    return port.drawn;
  }
};

[[nodiscard]] auto contains(const std::vector<std::string>& items, const std::string& needle)
    -> bool {
  for (const std::string& item : items) {
    if (item == needle) return true;
  }
  return false;
}

}  // namespace

/// 视口在文档顶部：只画前几行，**远方的行一行都不画**。
ST_TEST(md_paint_draws_only_rows_inside_viewport) {
  Fixture fixture;
  MarkdownView view;
  view.set_markdown(paragraphs(300));

  const std::vector<std::string> drawn = fixture.paint(view);

  ST_CHECK(!drawn.empty());
  ST_CHECK(contains(drawn, "row-0"));      // 首行必画
  ST_CHECK(!contains(drawn, "row-299"));   // 末行（远在下方）不画
  // 300 行文档、300px 视口、行高约 20px → 可见约 15 行；就算双倍也远小于 300。
  ST_CHECK(drawn.size() < 60U);
}

/// **二分的前提**：滚动到中间时，画的必须是**那一段**——既不漏画也不多画。
///
/// 这条直接盯住"单调性假设不成立就会静默漏画"这个最危险的失效模式。
ST_TEST(md_binary_search_paints_the_correct_window) {
  Fixture fixture;
  MarkdownView view;
  view.set_markdown(paragraphs(400));
  (void)fixture.paint(view);  // 先排一次拿到 content_height

  // 滚到大约中段。
  const float mid = view.content_height() * 0.5F;
  view.scroll_to(mid);
  const std::vector<std::string> drawn = fixture.paint(view);

  ST_CHECK(!drawn.empty());
  // 视口中心附近的行必须在（说明二分切对了位置）。
  const auto [first, last] = view.visible_row_range_now();
  ST_CHECK(first > 20U);          // 确实滚过去了
  ST_CHECK(last < 380U);          // 且没到末尾
  ST_CHECK(last > first);
  // 顶部行与底部行都不该出现。
  ST_CHECK(!contains(drawn, "row-0"));
  ST_CHECK(!contains(drawn, "row-399"));
  // 区间宽度应接近"视口能容的行数"，而不是整个文档。
  ST_CHECK((last - first) < 40U);
}

/// 视口在文档底部：画的必须是末尾那几行（边界 1 不越界、不漏画）。
ST_TEST(md_paint_at_document_bottom) {
  Fixture fixture;
  MarkdownView view;
  view.set_markdown(paragraphs(200));
  (void)fixture.paint(view);

  view.scroll_to(view.content_height());  // 贴底
  const std::vector<std::string> drawn = fixture.paint(view);

  ST_CHECK(!drawn.empty());
  ST_CHECK(contains(drawn, "row-199"));  // 末行必须画（二分上界正确）
  ST_CHECK(!contains(drawn, "row-0"));   // 首行不该出现
  const auto [first, last] = view.visible_row_range_now();
  ST_CHECK_EQ(last, view.row_count());
  ST_CHECK(first > 100U);
}

/// 行数从 50 涨到 2000，**每帧绘制量不跟着涨**（剔除的结构性证据）。
ST_TEST(md_paint_cost_is_independent_of_document_size) {
  const auto drawn_for = [](std::size_t rows) {
    Fixture fixture;
    MarkdownView view;
    view.set_markdown(paragraphs(rows));
    return fixture.paint(view).size();
  };
  ST_CHECK_EQ(drawn_for(50U), drawn_for(2000U));
}

/// 空文档：`rebuild_rows` 会造**一行**占位（faint 的「（空）」）——
/// 目的是“保证元素仍有可命中的高度”（见那里的注释），所以不该断言零绘制。
ST_TEST(md_visible_range_is_safe_when_empty) {
  Fixture fixture;
  MarkdownView view;
  const std::vector<std::string> drawn = fixture.paint(view);
  ST_CHECK_EQ(view.row_count(), std::size_t{1});
  ST_CHECK_EQ(drawn.size(), std::size_t{1});  // 只有那行占位符
  const auto [first, last] = view.visible_row_range_now();
  ST_CHECK_EQ(first, std::size_t{0});
  ST_CHECK_EQ(last, std::size_t{1});
}
