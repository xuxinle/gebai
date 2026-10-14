/// 多行 `Text` **折行缓存键**的回归护栏（2026-10-14）。
///
/// ## 守的是什么
///
/// `Text::display_lines_` 的缓存判据原为 `(content_, available, size)`。但折行结果
/// **也取决于字宽**，而字宽来自当时的 `TextPort`。漏掉这一维的后果是：
/// 任何"只换端口、其余全不变"的路径会**静默复用旧字体折的行**——换行位置/行数错，
/// 且**不报错**。
///
/// 为何当时没暴露：生产代码只在启动时 `set_text_port` 一次
///（`app.cpp`），DPI 切换也不换端口 ⇒ 潜伏脆弱点而非活缺陷。
/// `CodeEditor` 早已把端口纳入自己的缓存判据（`layout_port_`），这里是**对齐遗漏**。
///
/// ## 这条用例为什么必须"换端口"而不是"换内容"
///
/// 换内容会走缓存未命中分支、掩盖问题；只有**只换端口**才能复现"缓存该失效而没失效"。
/// 反向验证：把 `basic.cpp` 判据里的 `layout_cache_port_ != context.text` 去掉，
/// 本用例立刻变红。
///
/// ## 端口桩
///
/// 标准夹具的两个桩字宽固定（`FixedAdvanceTextPort` 每码点 8px、
/// `ProportionalTextPort` 按 0.55/1.0 计），无法表达"同一个端口之外还有别的字体"。
/// 所以这里内联一个**可配缩放**的桩：`ScaledPort(scale)` 把字宽整体乘 `scale`——
/// 等价于"字更宽/更窄的另一种字体"。

#include "st/test/test.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::math::Size;

/// 可配字宽缩放的端口桩：语义上等价于"另一种字体"。
class ScaledTextPort final : public st::ui::TextPort {
 public:
  explicit ScaledTextPort(float advance) : advance_(advance) {}

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> Size override {
    return Size{measure_width(utf8, size), line_height(size)};
  }
  /// 字宽**随字号缩放**（真实字体就是如此）——否则"字号变大"这类用例量不出变化，
  /// 桩就会把一个假象当成通过（第一版正是这么写的，被本文件的第二条用例抓住）。
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                                   st::text::FontRole = st::text::FontRole::Proportional) const
      -> float override {
    return advance_ * (size / 16.0F) * static_cast<float>(st::utf8_length(utf8));
  }
  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.45F; }
  void draw(st::raster::Surface&, std::string_view, Point, float, Color,
            st::text::FontRole = st::text::FontRole::Proportional, float = 0.0F,
            bool = false) const override {}
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float max_width) const
      -> std::string override {
    return std::string(utf8.substr(0, static_cast<std::size_t>(max_width / advance_)));
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    // 按**词**折行（与真实端口的语义一致：不切词内）。
    const float advance = advance_ * (size / 16.0F);
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    float width = 0.0F;
    std::size_t index = 0;
    while (index < utf8.size()) {
      const std::size_t word_end = utf8.find(' ', index);
      const std::size_t stop = word_end == std::string_view::npos ? utf8.size() : word_end;
      const float word_width = advance * static_cast<float>(stop - index);
      if (width > 0.0F && width + word_width > max_width) {
        lines.push_back(utf8.substr(begin, index - begin - 1U));
        begin = index;
        width = 0.0F;
      }
      width += word_width;
      index = stop + 1U;
    }
    if (begin < utf8.size()) lines.push_back(utf8.substr(begin));
    return lines;
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const -> std::vector<std::string> override {
    std::vector<std::string> out;
    for (const auto& line : wrap(utf8, size, max_width)) {
      if (out.size() == max_lines) break;
      out.emplace_back(line);
    }
    return out;
  }

 private:
  float advance_{8.0F};
};

/// 折行行数（行高 = size * 1.45，故 `measured.height / line_height` 即行数）。
[[nodiscard]] auto wrapped_line_count(const st::ui::Text& text, float size) -> std::size_t {
  const float line_height = size * 1.45F;
  return static_cast<std::size_t>(text.measured_size().height / line_height + 0.5F);
}

}  // namespace

/// **换端口必须让折行缓存失效**：同一 `Text`，只在"另一个字宽的端口"下重排，
/// 折行行数要跟着变（而不是复用旧端口的折行）。
ST_TEST(text_wrap_cache_follows_text_port) {
  static const st::ui::Theme theme = st::ui::Theme::light();
  constexpr float kSize = 16.0F;
  constexpr float kWidth = 120.0F;

  ScaledTextPort narrow{6.0F};   // 字窄 → 一行装得下更多
  ScaledTextPort wide{16.0F};    // 字宽 → 换行更多
  const char* content = "alpha beta gamma delta epsilon zeta";

  st::ui::Text text{content};
  text.set_multiline(true);
  text.apply_theme(theme);

  st::ui::RenderContext ctx_narrow{theme, &narrow, 0.0};
  st::ui::Constraints constraints{.max_width = kWidth};
  text.measure(ctx_narrow, constraints);
  const std::size_t narrow_lines = wrapped_line_count(text, kSize);
  ST_CHECK(narrow_lines >= 2U);  // 桩的有效性前提：窄字体下也真的折了行

  // **只换端口**（内容/宽度/字号全不变）——这正是缓存该失效而没失效的场景。
  st::ui::RenderContext ctx_wide{theme, &wide, 0.0};
  text.measure(ctx_wide, constraints);
  const std::size_t wide_lines = wrapped_line_count(text, kSize);

  ST_CHECK(wide_lines > narrow_lines);  // 字更宽 → 行更多

  // 换回去也必须重新算（不是"单向记得第一次"）。
  text.measure(ctx_narrow, constraints);
  ST_CHECK_EQ(wrapped_line_count(text, kSize), narrow_lines);
}

/// 缩放/主题变化（走 `apply_theme`）后，缓存同样要失效——防止"只覆盖端口"的补丁。
ST_TEST(text_wrap_cache_follows_theme_font_size) {
  static const st::ui::Theme theme = st::ui::Theme::light();
  ScaledTextPort port{8.0F};
  st::ui::Text text{"alpha beta gamma delta epsilon zeta"};
  text.set_multiline(true);

  st::ui::RenderContext context{theme, &port, 0.0};
  st::ui::Constraints constraints{.max_width = 120.0F};
  text.measure(context, constraints);
  const std::size_t small_lines = wrapped_line_count(text, 16.0F);

  // 字号变大 → 可用宽度不变但字更宽 → 行数应变多。
  text.set_font_size(28.0F);
  text.apply_theme(theme);
  text.measure(context, constraints);
  const std::size_t big_lines = wrapped_line_count(text, 28.0F);

  ST_CHECK(big_lines > small_lines);
}
