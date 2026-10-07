/// **行排版**（`st::ui::layout_line`）的回归测试。
///
/// 背景（2026-10-07，用户三次反馈后的重构）：
/// 「行间距怎么全在行下方」→「背景和光标还是偏下」→「上部的背景和光标怎么没了」。
/// 三报暴露的不是某个算错，而是**框架缺了一个低级组件**：13 个组件各自调
/// `TextPort::draw` 并各自算垂直定位，同一件事有三条实现，修一处不动另一处。
///
/// 重构后统一到 `layout_line`：**一次给出全部几何**（文字落位 / 带的范围 / 基线），
/// 且**全部相对盒子顶**（与绝对位置无关，多行控件可缓存复用）。
///
/// ## 两个量各回答不同的问题（第三报踩的就是把它俩混为一谈）
///
/// | 量 | 回答 | 口径 |
/// |---|---|---|
/// | `origin_y`（文字落位） | 文字整体在盒子里**居中在哪** | 按**参考墨迹**居中 |
/// | `ink_top` / `ink_height`（带的范围） | 带要多大才**装得下**这一行 | 按**字体 em 盒** |
///
/// 用参考墨迹去定带的高是错的：实测第 6 行 `auto fill_path_aa(Surface&, ...)` 的
/// 文字墨迹是 y 375..395，而按参考样本算出的带只画在 381..402——**文字顶露出带外 6px**。
/// 样本只适合回答“居中在哪”（要各组件一致），回答不了“带要多大”（必须按字体上界）。
///
/// 本组用例钉住四件事（每个都是踩过的坑）：
/// ① 文字按**墨迹**居中（不按行盒——行盒含字体预留空间）；
/// ② 中心公式**不许多减一个 `below`**（曾把汉字系统性压下 2px）；
/// ③ 行距放大时，多出来的高度**两侧分摊**（不是全堆下方）；
/// ④ 带必须**装得下**墨迹（`ink_top ≤ 墨迹顶` 且 `带底 ≥ 墨迹底`），且几何自洽。
#include "st/test/test.hpp"

#include <cmath>
#include <optional>
#include <string_view>
#include <vector>

#include "st/text/text.hpp"
#include "st/app/text_port.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"

namespace {

using st::ui::LineGeometry;
using st::ui::TextPort;

/// 桩端口：行高固定，把几何关系与真字体解耦（真字体数值见 `tools/line_box_probe.cpp`）。
///
/// ⚠ `below` 必须**非零**：`ui_text_vertical_center_test.cpp` 记过一个假绿——
/// 桩把 `below` 恒报 0 时，“正确公式”与“多减一个 below 的错误公式”**恰好相等**，
/// 于是缺陷在桩上永远测不出来。
class FixedPort final : public TextPort {
 public:
  FixedPort(float line, float above, float below, float ascent_ratio)
      : line_(line), above_(above), below_(below), ascent_ratio_(ascent_ratio) {}

  [[nodiscard]] auto measure(std::string_view, float) const -> st::math::Size override {
    return st::math::Size{10.0F, line_};
  }
  [[nodiscard]] auto measure_width(std::string_view, float, st::text::FontRole) const
      -> float override {
    return 10.0F;
  }
  [[nodiscard]] auto line_height(float) const -> float override { return line_; }
  [[nodiscard]] auto ascent(float) const -> float override { return line_ * ascent_ratio_; }
  [[nodiscard]] auto shaped_ascent(std::string_view, float, st::text::FontRole) const
      -> float override {
    return line_ * ascent_ratio_;
  }
  [[nodiscard]] auto ink_metrics(std::string_view, float, st::text::FontRole) const
      -> std::optional<InkMetrics> override {
    return InkMetrics{above_, below_};
  }
  void draw(st::raster::Surface&, std::string_view, st::math::Point, float, st::math::Color,
            st::text::FontRole, float, bool) const override {}
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float) const
      -> std::string override {
    return std::string(utf8);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float,
                                  std::size_t) const -> std::vector<std::string> override {
    return {std::string(utf8)};
  }

 private:
  float line_{20.0F};
  float above_{12.0F};
  float below_{3.0F};
  float ascent_ratio_{0.8F};
};

constexpr float kTolerance = 0.01F;

/// 参考样本的墨迹在“位移后”的落位（相对盒顶）。
struct InkSpan {
  float top{0.0F};
  float bottom{0.0F};
};

[[nodiscard]] auto ink_span(const TextPort& port, const LineGeometry& g, float size)
    -> InkSpan {
  const auto ink = port.ink_metrics(st::ui::kInkReferenceSample, size,
                                    st::text::FontRole::Monospace);
  const float ascent = port.shaped_ascent(st::ui::kInkReferenceSample, size,
                                          st::text::FontRole::Monospace);
  const float baseline = g.origin_y + ascent;
  return InkSpan{baseline - (ink ? ink->above : 0.0F), baseline + (ink ? ink->below : 0.0F)};
}

}  // namespace

ST_TEST(line_layout_centres_ink_in_the_box) {
  // ① 文字按**墨迹**居中：墨迹中心必须落在盒子中心。
  const FixedPort port{20.0F, 12.0F, 3.0F, 0.8F};
  for (const float box : {20.0F, 22.77F, 30.0F}) {
    const LineGeometry g = st::ui::layout_line(port, st::ui::kInkReferenceSample, 15.0F, box);
    const InkSpan span = ink_span(port, g, 15.0F);
    const float centre = (span.top + span.bottom) * 0.5F;
    ST_CHECK(std::abs(centre - box * 0.5F) <= kTolerance);
  }
}

ST_TEST(line_layout_does_not_over_subtract_below) {
  // ② 中心公式不许多减一个 `below`。
  //
  // 若写成 `ascent − (above + below)/2`（错），墨迹中心会偏下 `below`。
  // 用一个 below 明显不为 0 的桩把它分开。
  const FixedPort port{20.0F, 12.0F, 5.0F, 0.8F};
  const LineGeometry g = st::ui::layout_line(port, st::ui::kInkReferenceSample, 15.0F, 24.0F);
  const InkSpan span = ink_span(port, g, 15.0F);
  const float centre = (span.top + span.bottom) * 0.5F;
  ST_CHECK(std::abs(centre - 12.0F) <= kTolerance);   // 盒 24 → 中心 12
}

ST_TEST(line_layout_band_spans_the_whole_box) {
  // ③ **行带 = 整个行盒**（用户第三报的量化形式）。
  //
  // 带若只包住墨迹，**行距就变成了带与带之间的空白缝隙**——“背景被切短了”。
  // 反向的错误（带比行盒还高）会叠到邻行上。两个方向都钉住。
  const FixedPort port{20.0F, 12.0F, 3.0F, 0.8F};
  for (const float box : {20.0F, 22.77F, 30.0F}) {
    const LineGeometry g = st::ui::layout_line(port, st::ui::kInkReferenceSample, 15.0F, box);
    ST_CHECK(std::abs(g.ink_top - 0.0F) <= kTolerance);
    ST_CHECK(std::abs(g.ink_height - box) <= kTolerance);
  }
}

ST_TEST(line_layout_band_never_cuts_off_the_text) {
  // ③b 带必须包得住**当前字体的实际字形**——这是第三报的核心诉求。
  //
  // 用真字体量几段真实文本的墨迹范围：它必须落在带内（带上沿不高于墨迹顶、下沿不低于墨迹底）。
  // 这一条把“带该多大”与“字体实际字形”绑在一起，不再依赖任何固定样本的巧合。
  auto stack = st::text::FontStack::system_default();
  if (!stack) return;   // 无字体环境无法量字形
  // ⚠ 渲染器必须**具名**存活：`RendererTextPort` 持的是它的引用，
  // 写成 `RendererTextPort port{TextRenderer{*stack}}` 会让临时对象在本行末销毁，
  // 端口随即悬垂（实测：整个测试进程当场退出，exit=5 而非断言失败）。
  const st::text::TextRenderer renderer{*stack};
  const st::app::RendererTextPort port{renderer};
  const float size = 15.0F;
  for (const float spacing : {1.0F, 1.15F, 1.5F}) {
    const float box = port.line_height(size) * spacing;
    const LineGeometry g = st::ui::layout_line(port, st::ui::kInkReferenceSample, size, box);
    const float ascent = port.shaped_ascent(st::ui::kInkReferenceSample, size,
                                            st::text::FontRole::Monospace);
    const float baseline = g.origin_y + ascent;
    // 逐行量真实文本的墨迹（下划线/括号/中文都在里面）
    for (const std::string_view text : {"auto fill_path_aa(Surface& canvas) -> void {",
                                        "// 霜天光栅器：扫描线覆盖率抗锯齿", "#include <vector>",
                                        "const auto polyline = path.flatten(0.25f);"}) {
      const auto ink = port.ink_metrics(text, size, st::text::FontRole::Monospace);
      if (!ink) continue;
      const float ink_top = baseline - ink->above;
      const float ink_bottom = baseline + ink->below;
      // 带（0..box）必须覆盖墨迹
      ST_CHECK(ink_top >= -kTolerance);
      ST_CHECK(ink_bottom <= box + kTolerance);
    }
  }
}

ST_TEST(line_layout_splits_extra_leading_between_both_sides) {
  // ④ 行距放大：多出来的高度**两侧分摊**（不是全堆下方）。
  //
  // 这就是用户第一次报的「行间距全在行下方」：修复前上留白恒定、增量 100% 进下方。
  // 现在墨迹居中，盒子每长 3，墨迹上沿应上移 1.5（上侧拿到一半）。
  const FixedPort port{20.0F, 12.0F, 3.0F, 0.8F};
  float previous_top = -1.0F;
  for (const float box : {20.0F, 23.0F, 26.0F, 29.0F}) {   // 等距，否则断言的步长对不上
    const LineGeometry g = st::ui::layout_line(port, st::ui::kInkReferenceSample, 15.0F, box);
    const InkSpan span = ink_span(port, g, 15.0F);
    const float top_gap = span.top;
    const float bottom_gap = box - span.bottom;
    ST_CHECK(std::abs(top_gap - bottom_gap) <= kTolerance);   // 上下留白相等
    if (previous_top >= 0.0F) {
      ST_CHECK(std::abs((top_gap - previous_top) - 1.5F) <= kTolerance);
    }
    previous_top = top_gap;
  }
}

ST_TEST(line_layout_geometry_is_self_consistent) {
  // ⑤ 几何量自洽：高亮带 / 光标 / 文字用的必须是同一套数。
  // `origin_y` 是交给 `draw` 的值，`draw` 内部按 `baseline = origin_y + shaped_ascent`
  // 落基线——所以 `baseline` 必须与它一致。
  const FixedPort port{20.0F, 12.0F, 3.0F, 0.8F};
  const LineGeometry g = st::ui::layout_line(port, st::ui::kInkReferenceSample, 15.0F, 26.0F);
  const float ascent = port.shaped_ascent(st::ui::kInkReferenceSample, 15.0F,
                                          st::text::FontRole::Monospace);
  ST_CHECK(std::abs(g.baseline - (g.origin_y + ascent)) <= kTolerance);
  // 带 = 整个行盒
  ST_CHECK(std::abs(g.ink_top - 0.0F) <= kTolerance);
  ST_CHECK(std::abs(g.ink_height - 26.0F) <= kTolerance);
}
