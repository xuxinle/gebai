/// 多行 `Text` 的**视口剔除 + 排版缓存**护栏。
///
/// ## 这组用例守的是什么
///
/// 多行 `Text` 过去在 `paint_content` 里每帧 `port.wrap(content_)` **重新折行全部内容**、
/// 逐行 `measure_width`、再逐行 `draw`——**完全不看视口**。把整份日志交给一个多行 `Text`
/// 的场景（`Terminal` 行模式就是这么做的）于是每帧成本直接等于内容长度。实测
/// （`tools/ui_big_editors_bench.cpp`，Terminal 行模式 400px 视口）：
///
/// | 行数 | 改前稳态帧 | 改后 |
/// |---|---|---|
/// | 500 | 47.488 ms | 0.332 ms |
/// | 2000 | 705.416 ms | 0.334 ms |
/// | 20000 | 3298.367 ms | 0.438 ms |
///
/// 修法两条：① 折行结果 + 行宽按 `(内容, 可用宽度, 字号)` 缓存；
/// ② `paint_content` 按裁剪区只画可见那几行。
///
/// ## 为什么断言"调用次数"而不是耗时
///
/// 同其它缓存护栏：耗时机器相关；而这里的真风险是**"改了内容/宽度还读回旧排版"**
/// ——那是正确性问题（显示错行、选错字）。数 `wrap`/`draw` 次数可以同时证明
/// "缓存生效"与"失效正确"。

#include "st/test/test.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Rect;
using st::raster::Canvas;
using st::test::FixedAdvanceTextPort;
using st::ui::Constraints;
using st::ui::RenderContext;
using st::ui::Text;
using st::ui::Theme;

inline constexpr int kCanvasWidth = 300;
inline constexpr int kCanvasHeight = 400;

/// 包装定宽端口：**计数 `wrap` 调用次数**（其余转发，`draw` 记录用内层的 `drawn()`）。
class CountingWrapPort final : public st::ui::TextPort {
 public:
  FixedAdvanceTextPort inner{};
  mutable std::size_t wrap_calls{0};

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
    inner.draw(canvas, utf8, origin, size, color, role, embolden, bold);
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return inner.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    ++wrap_calls;
    return inner.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    ++wrap_calls;
    return inner.wrap_limited(utf8, size, max_width, max_lines);
  }
};

/// `count` 行、每行一个字符（定宽桩下不会二次折行 → 显示行数 == `count`）。
[[nodiscard]] auto many_lines(std::size_t count) -> std::string {
  std::string text;
  text.reserve(count * 2U);
  for (std::size_t index = 0; index < count; ++index) text += "a\n";
  return text;
}

struct Fixture {
  Theme theme{Theme::light()};
  CountingWrapPort port{};
  Canvas canvas{kCanvasWidth, kCanvasHeight};

  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }

  /// 布局到 `bounds`，绘制一次，返回本次新画的文本条数。
  [[nodiscard]] auto paint(Text& text, Rect bounds) -> std::size_t {
    text.measure(context(), Constraints{});
    text.arrange(context(), bounds);
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    const std::size_t before = port.inner.drawn().size();
    text.paint(context(), canvas);
    return port.inner.drawn().size() - before;
  }
};

}  // namespace

/// 视口只容十几行时，**只画视口内的行**（不是全部 2000 行）。
ST_TEST(text_paint_draws_only_visible_lines) {
  Fixture fixture;
  Text text{many_lines(2000)};
  text.set_multiline(true);
  // bounds 高到内容全高（ScrollView 子元素的姿态）。
  const float tall = 2000.0F * fixture.port.inner.line_height(15.0F) + 40.0F;
  const std::size_t drawn = fixture.paint(text, Rect{0.0F, 0.0F, 300.0F, tall});

  // 400px 视口 / 行高 ≈ 21.75px → 约 18 行。
  ST_CHECK(drawn >= 16U);
  ST_CHECK(drawn <= 24U);  // 关键：**不是** 2000
}

/// 行数 100 → 4000，**每帧绘制量不跟着涨**——剔除的结构性证据。
ST_TEST(text_paint_cost_is_independent_of_line_count) {
  const auto drawn_for = [](std::size_t lines) {
    Fixture fixture;
    Text text{many_lines(lines)};
    text.set_multiline(true);
    const float tall = static_cast<float>(lines) * 21.75F + 40.0F;
    return fixture.paint(text, Rect{0.0F, 0.0F, 300.0F, tall});
  };
  ST_CHECK_EQ(drawn_for(100U), drawn_for(4000U));
}

/// **重复布局不重折行**（排版缓存生效）——这直接对应 705 ms → 0.33 ms 那条。
ST_TEST(text_layout_cache_avoids_repeat_wrap) {
  Fixture fixture;
  Text text{many_lines(500)};
  text.set_multiline(true);

  Constraints constraints;
  constraints.max_width = 300.0F;  // 必须与下面 paint 的 bounds 宽一致——否则折行宽度不同，
                                   // 缓存自然不命中（measure 用 `constraints`、paint 用 `bounds_`
                                   // 是两个既有口径；真实布局管道里两者相等）。
  text.measure(fixture.context(), constraints);
  const std::size_t first = fixture.port.wrap_calls;
  ST_CHECK(first > 0U);  // 首轮必须折行

  text.measure(fixture.context(), constraints);
  text.arrange(fixture.context(), Rect{0.0F, 0.0F, 300.0F, 400.0F});
  fixture.canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
  text.paint(fixture.context(), fixture.canvas);
  ST_CHECK_EQ(fixture.port.wrap_calls, first);  // 次轮零折行（measure 与 paint 都不折）
}

/// **内容变了**必须重折（否则显示的是旧内容）。
ST_TEST(text_layout_cache_invalidates_on_content_change) {
  Fixture fixture;
  Text text{many_lines(10)};
  text.set_multiline(true);
  text.measure(fixture.context(), Constraints{});
  const std::size_t after_first = fixture.port.wrap_calls;

  text.set_content(many_lines(20));
  text.measure(fixture.context(), Constraints{});
  ST_CHECK(fixture.port.wrap_calls > after_first);
}

/// **可用宽度变了**必须重折（窗口缩放后折行位置会变）。
ST_TEST(text_layout_cache_invalidates_on_width_change) {
  Fixture fixture;
  // 长行（会随宽度二次折行）：定宽桩下 40 字符在 100px 宽里折成 5 行、300px 里折成 2 行。
  std::string wide;
  for (std::size_t index = 0; index < 40; ++index) wide += 'a';
  wide += '\n';

  Text text{wide};
  text.set_multiline(true);
  Constraints narrow;
  narrow.max_width = 100.0F;
  text.measure(fixture.context(), narrow);
  const std::size_t after_narrow = fixture.port.wrap_calls;
  const float narrow_height = text.measured_size().height;

  Constraints wide_constraints;
  wide_constraints.max_width = 300.0F;
  text.measure(fixture.context(), wide_constraints);
  ST_CHECK(fixture.port.wrap_calls > after_narrow);  // 必须重折
  ST_CHECK(text.measured_size().height < narrow_height);  // 行少了 → 高度变小
}

/// 缓存**不改变结果**：多次 measure 的量出尺寸一致。
ST_TEST(text_layout_cache_preserves_measured_size) {
  Fixture fixture;
  Text text{many_lines(50)};
  text.set_multiline(true);
  text.measure(fixture.context(), Constraints{});
  const st::math::Size first = text.measured_size();
  for (int round = 0; round < 3; ++round) text.measure(fixture.context(), Constraints{});
  ST_CHECK_EQ(text.measured_size().width, first.width);
  ST_CHECK_EQ(text.measured_size().height, first.height);
}
