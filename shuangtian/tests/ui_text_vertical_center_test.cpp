/// 单行文本在盒子里的**垂直居中**（`st::ui::centered_line_top`）。
///
/// ## 为什么需要这组用例
///
/// 用户报的原始现象是「按钮文字没有居中」。它属于最难自查的一类：
/// 文字**确实画出来了、也没画错位置**，只是整体偏下两三像素——
/// 单看截图"看着有点怪"，只有量了才知道。
///
/// ## 两个坑（都是本组用例要钉住的）
///
/// 1. **按行盒居中 ≠ 看起来居中**。`line_height` 是字体的**行盒**（含大把头尾预留），
///    按它居中会把文字系统性推下。实测 DejaVu Sans 的 `hhea.ascender = 0.928em`，
///    而大写字母的墨迹只有 `0.729em`。→ 对齐依据必须是**墨迹**（`ink_metrics`）。
///
/// 2. **基线位不能用 `ascent(size)` 反推**。`TextPort::draw` 排版时取的是
///    **跨 run 的最大 ascender**（汉字回退到 CJK 面时它明显大于主面），
///    而 `ascent(size)` 只反映主面。两者不一致时居中含**系统性残差**——
///    实测第一版改动"改了跟没改差不多"（偏差反而从 +2.50 变 +3.50），
///    就是因为拿 `ascent()` 代替了排版的真实基线位。
///
/// 判据选「墨迹中心相对盒中心的偏移」：这是用户肉眼看的那个量，
/// 而不是任何中间度量。

#include "st/test/test.hpp"

#include <cmath>

#include "st/ui/components/basic.hpp"
#include "st/ui/text_port.hpp"
#include "tests/support/text_port_fixtures.hpp"
#include "st/ui/line_layout.hpp"

namespace {

using st::ui::centered_line_top;
using st::ui::TextPort;

/// 只报墨迹的桩：把「行盒」与「墨迹」做成**明显不等**，否则居中的错误口径
/// 也能碰巧对（那样用例就是恒绿的假护栏）。
///
/// 数值取自真实字体（DejaVu Sans：`ascender = 0.928em`、`descender = 0.236em`、
/// 大写墨迹 `cap = 0.729em`）——用真实比例才有说服力。
class InkStubPort final : public TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view, float size) const -> st::math::Size override {
    return st::math::Size{size * 2.0f, line_height(size)};
  }
  [[nodiscard]] auto measure_width(std::string_view, float size, st::text::FontRole) const
      -> float override {
    return size * 2.0f;
  }
  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.164f; }
  [[nodiscard]] auto ascent(float size) const -> float override { return size * 0.928f; }
  [[nodiscard]] auto descent(float size) const -> float override { return size * 0.236f; }
  /// 与 `line_height`/`ascent` **故意不同**：这里是"排版实际用的基线位"，
  /// 取一个更大的值模拟「文本里含回退到 CJK 面的汉字」。
  [[nodiscard]] auto shaped_ascent(std::string_view, float size, st::text::FontRole) const
      -> float override {
    return size * 1.100f;
  }
  [[nodiscard]] auto ink_metrics(std::string_view, float size, st::text::FontRole) const
      -> std::optional<InkMetrics> override {
    return InkMetrics{size * 0.729f, 0.0f};  // 大写字母：坐在基线上
  }
  void draw(st::raster::Surface&, std::string_view, st::math::Point, float, st::math::Color,
            st::text::FontRole, float, bool) const override {}
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float) const -> std::string override {
    return std::string{utf8};
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float, std::size_t) const
      -> std::vector<std::string> override {
    return {std::string{utf8}};
  }
};

}  // namespace

/// 墨迹中心必须落在盒中心上（而不是行盒中心）。
ST_TEST(text_vertical_center_aligns_ink_not_line_box) {
  const InkStubPort port{};
  const float size = 14.0f;
  const float box_y = 100.0f;
  const float box_h = 34.0f;

  const float top = centered_line_top(port, "主要操作", size, box_y, box_h);

  // 复算墨迹中心：draw 把基线放在 `top + shaped_ascent`，墨迹中心在基线上方
  // `above - (above + below) / 2`。
  const float baseline = top + port.shaped_ascent("主要操作", size, st::text::FontRole::Proportional);
  const float ink_center = baseline - port.ink_metrics("主要操作", size, st::text::FontRole::Proportional)->above * 0.5f;
  const float box_center = box_y + box_h * 0.5f;

  ST_CHECK_NEAR(ink_center, box_center, 0.01);
}

/// **反向护栏**：如果实现退回"按行盒居中"，墨迹就会偏——本用例必须变红。
///
/// 这条断言的意义在于：它**只在墨迹与行盒不等时**才成立。桩里两者刻意不等
/// （`cap 0.729em` vs 行盒 `1.164em`），所以"按行盒居中"的实现必然失败。
ST_TEST(text_vertical_center_would_fail_if_line_box_centered) {
  const InkStubPort port{};
  const float size = 14.0f;
  const float box_y = 100.0f;
  const float box_h = 34.0f;

  const float correct = centered_line_top(port, "主要操作", size, box_y, box_h);
  const float box_only = box_y + (box_h - port.line_height(size)) * 0.5f;
  // 两者必须**明显不同**：相同就说明实现（或桩）退化了，下面那条断言也没意义了。
  ST_CHECK(std::fabs(correct - box_only) > 0.5f);

  const float baseline = correct + port.shaped_ascent("主要操作", size, st::text::FontRole::Proportional);
  const float ink_center = baseline - port.ink_metrics("主要操作", size, st::text::FontRole::Proportional)->above * 0.5f;
  ST_CHECK_NEAR(ink_center, box_y + box_h * 0.5f, 0.01);
}

/// 用的必须是**排版真实基线位**（`shaped_ascent`），不是主面的 `ascent`。
///
/// 桩里两者差 `0.172em`（≈2.4px @14px，量级与实测偏置一致）。
/// 若实现改用 `ascent()`，墨迹中心会偏 `0.172 * size * ...` 的量级，本用例即红。
ST_TEST(text_vertical_center_uses_the_shaped_baseline) {
  const InkStubPort port{};
  const float size = 14.0f;
  const float top = centered_line_top(port, "混排 Abc", size, 100.0f, 34.0f);

  const float with_shaped = top + port.shaped_ascent("混排 Abc", size, st::text::FontRole::Proportional);
  const float with_plain = top + port.ascent(size);
  // 两个基线位刻意不同——不然后面区分不出实现用了哪一个。
  ST_CHECK(std::fabs(with_shaped - with_plain) > 1.0f);

  const float ink_center = with_shaped - port.ink_metrics("混排 Abc", size, st::text::FontRole::Proportional)->above * 0.5f;
  ST_CHECK_NEAR(ink_center, 100.0f + 17.0f, 0.01);
}

/// 端口报不出墨迹时（无字体环境）**必须退化**成行盒居中，而不是崩/返回 NaN。
ST_TEST(text_vertical_center_degrades_to_line_box_without_ink) {
  // `FixedAdvanceTextPort` 是既有共享桩：它没实现 `ink_metrics` → 默认 `nullopt`。
  const st::test::FixedAdvanceTextPort port{};
  const float size = 14.0f;
  const float top = centered_line_top(port, "Abc", size, 100.0f, 34.0f);
  const float expected = 100.0f + (34.0f - port.line_height(size)) * 0.5f;
  ST_CHECK_NEAR(top, expected, 0.01);
}

/// 同一行字，**盒子越高越居中**——居中量与盒高成线性（回归"改了公式但忘了幅值"）。
ST_TEST(text_vertical_center_scales_with_box_height) {
  const InkStubPort port{};
  const float size = 14.0f;
  const float a = centered_line_top(port, "Abc", size, 0.0f, 30.0f);
  const float b = centered_line_top(port, "Abc", size, 0.0f, 50.0f);
  ST_CHECK_NEAR(b - a, 10.0f, 0.01);  // 盒高 +20 → 行盒顶 +10
}
