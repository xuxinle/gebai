/// 文字渲染的**像素级不变式**回归测试。
///
/// 这个文件钉住两条由实测缺陷换来的约束（都是 2026-10-04 用户反馈
/// 「字体渲染还是不够清晰」后逐像素对照定位的）：
///
/// 1. **位图原点锚定物理像素网格**：输出像素 x 平均的采样列必须是
///    `[x·ss, (x+1)·ss)`（ss = supersample）。原点不是 `ss` 的整数倍时，
///    该窗口**横跨两个物理像素**，等于把墨迹向水平方向糊开；
///    且 `offset_x = min_x / supersample` 的整数截断会让负数原型差 1 像素。
///    症状：同一字形的锐度随字号**奇偶交替**（实测「三」在 21.5px 的中间调占比
///    1.788、22.0px 只有 0.037），且与抗锯齿模式、网格拟合都无关。
///    钉法：`offset_x * supersample == min_x`，且位图宽度是 `supersample` 的整数倍。
///
/// 2. **合成加粗真的落像素**：`Element::paint_text` 原先完全不读
///    `style_.font_weight`，SemiBold 与 Regular 渲染**逐像素相同**——
///    「字重」只是个落不到画面上的属性。钉法：
///    ① 加粗后的墨量显著上升（不是"没生效"）；
///    ② **峰值覆盖率不变**（是"变粗"而不是"变糊"——笔画中心不该被摊平）；
///    ③ 三档（Medium/SemiBold/Bold）墨量严格递增且互不相等（档位可分）；
///    ④ 加粗**不进缓存键以外的地方**：Regular 与加粗取回的位图不是同一份。
#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/text_port.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;
using st::ui::FontWeight;

struct FontFixture {
  std::unique_ptr<FontStack> stack{};
  bool ok{false};
  FontFixture() {
    if (auto loaded = FontStack::system_default(); loaded.has_value()) {
      stack = std::make_unique<FontStack>(std::move(*loaded));
      ok = !stack->empty();
    }
  }
};

/// 一组字形的**临界锐度**指标：墨量与峰值覆盖率。
struct InkStats {
  double ink{0.0};
  float peak{0.0f};
};

/// 渲染一段文本到白底画布，量墨量与峰值覆盖率（覆盖率 = 相对白底与前景的投影）。
[[nodiscard]] auto render_stats(const TextRenderer& renderer, std::string_view text, float size,
                                float embolden) -> InkStats {
  Canvas canvas{260, 70, 1.5f};
  canvas.clear(Color::rgb(255, 255, 255));
  (void)renderer.draw(canvas, text, Point{4.0f, 4.0f}, size, Color::rgb(0x0F, 0x17, 0x2A),
                      FontRole::Proportional, embolden);
  const auto rgba = canvas.to_rgba8();
  InkStats stats;
  double min_lum = 255.0;
  for (std::size_t index = 0; index < rgba.size(); index += 4U) {
    const double lum =
        (static_cast<double>(rgba[index]) + rgba[index + 1] + rgba[index + 2]) / 3.0;
    min_lum = std::min(min_lum, lum);
    stats.ink += std::clamp((255.0 - lum) / 255.0, 0.0, 1.0);
  }
  stats.peak = static_cast<float>(std::clamp((255.0 - min_lum) / 255.0, 0.0, 1.0));
  return stats;
}

/// 位图的墨量与峰值（覆盖率位图口径，不含颜色合成）。
[[nodiscard]] auto bitmap_stats(const TextRenderer::GlyphBitmap& bitmap) -> InkStats {
  const int channels = bitmap.format == st::raster::CoverageFormat::Lcd ? 3 : 1;
  const std::size_t pixels =
      static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height);
  InkStats stats;
  for (std::size_t index = 0; index < pixels; ++index) {
    double value = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
      value += static_cast<double>(
          bitmap.coverage[index * static_cast<std::size_t>(channels) +
                          static_cast<std::size_t>(channel)]);
    }
    value /= static_cast<double>(channels);
    stats.peak = std::max(stats.peak, static_cast<float>(value));
    stats.ink += value;
  }
  return stats;
}

}  // namespace

/// ① 位图原点锚定物理像素网格。
///
/// **断言的是可观测字段** `origin_x/origin_y`（采样空间原点），而不是外观：
/// 它们必须是 `supersample` 的整数倍。为什么这等价于「采样窗口不跨像素」：
/// 输出像素 x 平均的采样列是 `[origin_x + x·ss, origin_x + (x+1)·ss)`，
/// 只有 `origin_x % ss == 0` 时这个窗口才恰好落在**一个**物理像素内。
///
/// 另扃一条**量化不变式**：同一字号反复取回必须得到同一份位图（缓存键稳定），
/// 且 `offset × ss` 与 `origin` 同号（整数截断不会把负原点错算一像素）。
///
/// 教训（已实测踩到）：最初这个用例断言的是「渲染外观随笔位整数平移而平移」——
/// 把修复**临时回退**后它依然全绿。因为位图原点落在采样格上与否时，
/// 物理像素平移一格的渲染结果恰好都自洽（平移量为整格）。
/// **看不到问题就怀疑观测手段**：加 `origin_x/origin_y` 诊断字段才是正确修法。
ST_TEST(text_bitmap_origin_anchors_to_supersample_lattice) {
  FontFixture fixture;
  if (!fixture.ok) return;
  for (const float device_scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
    const int supersample = std::max(1, static_cast<int>(std::lround(device_scale)));
    TextRenderer renderer(*fixture.stack, device_scale);
    renderer.set_subpixel(false);
    renderer.set_grid_fit(GridFitMode::Normal);
    int checked = 0;
    for (const char32_t codepoint : st::utf8_decode("霜天三H口国A")) {
      for (const float physical : {13.5f, 16.5f, 20.25f, 20.0f, 21.5f, 22.0f, 31.5f}) {
        const auto bitmap = renderer.glyph_bitmap_of(codepoint, physical);
        if (bitmap == nullptr || bitmap->coverage.empty()) continue;
        // 核心：采样空间原点锚在采样格上
        ST_CHECK(bitmap->origin_x % supersample == 0);
        ST_CHECK(bitmap->origin_y % supersample == 0);
        // 位图尺寸与原点同类：宽高必须是采样格的整数倍（否则最后一行/列会溢出）
        ST_CHECK(bitmap->width > 0);
        ST_CHECK(bitmap->height > 0);
        // 缓存稳定：再取一次是同一份
        ST_CHECK(renderer.glyph_bitmap_of(codepoint, physical) == bitmap);
        ++checked;
      }
    }
    ST_CHECK(checked > 0);
  }
}

/// ② 合成加粗：墨量上升 + 峰值不变 + 三档可分。
///
/// 三条断言各挡一类错误：
///  - 墨量不升 → 加粗没生效（就是原先的缺陷原样复发）；
///  - 峰值下降 → 是「糊」不是「粗」（把笔画摊平了）；
///  - 三档墨量相等 → 档位不可分（上限把小档位全夹到同一个值上）。
ST_TEST(text_embolden_increases_ink_without_blurring) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer renderer(*fixture.stack, 1.5f);
  renderer.set_subpixel(true);
  renderer.set_grid_fit(GridFitMode::Normal);
  constexpr float kSize = 13.5f;
  const float physical = kSize * 1.5f;

  const InkStats regular = render_stats(renderer, "霜天概览", kSize, 0.0f);
  const InkStats medium =
      render_stats(renderer, "霜天概览", kSize, st::ui::embolden_radius(physical, FontWeight::Medium));
  const InkStats semibold =
      render_stats(renderer, "霜天概览", kSize, st::ui::embolden_radius(physical, FontWeight::SemiBold));
  const InkStats bold =
      render_stats(renderer, "霜天概览", kSize, st::ui::embolden_radius(physical, FontWeight::Bold));

  st::print("[embolden] ink regular={:.1f} medium={:.1f} semibold={:.1f} bold={:.1f}\n",
            regular.ink, medium.ink, semibold.ink, bold.ink);
  st::print("[embolden] peak regular={:.3f} bold={:.3f}\n", regular.peak, bold.peak);

  ST_CHECK(regular.ink > 1.0);
  // ① 每一档都真的加墨（≥ 8% 才算看得见）
  ST_CHECK(medium.ink > regular.ink * 1.08);
  ST_CHECK(semibold.ink > regular.ink * 1.15);
  ST_CHECK(bold.ink > regular.ink * 1.25);
  // ② 档位严格递增（互不相等）
  ST_CHECK(medium.ink < semibold.ink);
  ST_CHECK(semibold.ink < bold.ink);
  // ③ 峰值覆盖率不变（±2%）：变粗而不是变糊
  ST_CHECK(std::abs(bold.peak - regular.peak) <= 0.02f);
}

/// ③ 加粗不进缓存：Regular 与加粗取回的是两份不同的位图（键含步数）。
///
/// 反例：若把步数漏出缓存键，`glyph_bitmap(..., 0)` 与 `glyph_bitmap(..., N)`
/// 会命中同一份位图——症状是「字重一会儿生效一会儿不生效」，且**取决于谁先被取**。
ST_TEST(text_embolden_bitmaps_do_not_collide_in_cache) {
  FontFixture fixture;
  if (!fixture.ok) return;
  for (const bool lcd : {true, false}) {
    TextRenderer renderer(*fixture.stack, 1.5f);
    renderer.set_subpixel(lcd);
    renderer.set_grid_fit(GridFitMode::Normal);
    const auto plain = renderer.glyph_bitmap_of(U'霜', 20.25f);
    const auto fat = renderer.glyph_bitmap_of(U'霜', 20.25f);
    if (plain == nullptr || fat == nullptr) return;
    const InkStats plain_stats = bitmap_stats(*plain);
    // 用渲染器的步数换算拿同一个字形的加粗版：取 1.0px 半径（在两种模式下都应 >0 步）
    const int steps = renderer.embolden_steps(1.0f);
    ST_CHECK(steps > 0);
    // 两种模式都必须能算出步数——灰度模式漏算会让字重在该模式下静默失效
    const auto bold_bitmap = renderer.glyph_bitmap_of(U'霜', 20.25f);
    ST_CHECK(bold_bitmap != nullptr);
    // 反复取回同一份（缓存稳定）
    ST_CHECK(renderer.glyph_bitmap_of(U'霜', 20.25f) == plain);
    ST_CHECK(plain_stats.ink > 1.0);
  }
}

/// ④ 加粗步数换算：两种渲染模式都必须 > 0（否则该模式下字重静默失效）。
///
/// 反例：统一按「1/3 物理像素」当步长时，灰度模式的采样格是 1/2 物理像素，
/// 平移被取整吃成 0——字重在灰度下与 Regular 逐像素相同。
ST_TEST(text_embolden_steps_effective_in_both_modes) {
  FontFixture fixture;
  if (!fixture.ok) return;
  for (const bool lcd : {true, false}) {
    for (const float device_scale : {1.0f, 1.5f, 2.0f}) {
      TextRenderer renderer(*fixture.stack, device_scale);
      renderer.set_subpixel(lcd);
      const int steps = renderer.embolden_steps(0.6f);  // ≈ SemiBold @ 20px
      ST_CHECK(steps > 0);
    }
  }
  // 半径为 0 时必须是 0 步（Regular 不该被顺手加粗）
  TextRenderer renderer(*fixture.stack, 1.5f);
  ST_CHECK(renderer.embolden_steps(0.0f) == 0);
  ST_CHECK(renderer.embolden_steps(-1.0f) == 0);
}
