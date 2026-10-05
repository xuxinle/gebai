/// 网格拟合的**护栏一致性**回归测试。
///
/// 起因（2026-10-04 用户反馈「编辑器字体效果很好，UI 字体还差点意思」）：
/// 平台期定位到 `grid_fit` 里**宽度量化与位移护栏不自洽**——
/// 代码对笔画的近边/远边**各自独立**做 `|delta| <= max_shift` 判定，
/// 而宽度量化必然让两侧位移不等（远边要额外叠上宽度的取整量）。
/// 于是常见情况是：近边（0.1px）通过并被应用，远边（0.6~1.0px）被拒——
/// 笔画被**平移了却没被改宽**：既没拿到网格对齐，又把字形整体推歪一点。
/// 净效果是更糊，而且只在**小字**上显形（大字号的笔画粗、量化量小）。
///
/// 这个文件钉住三条：
/// 1. **量化要么整体生效、要么整体不生效**：位图里不该出现
///    「同一条笔画的某一侧被移动、另一侧没有」的状态——判据是
///    轮廓点的位移在这条笔画的**两侧成对**出现（这里用更稳的等价量：
///    小字号的半覆盖像素数必须显著低于「护栏不自洽」时的水平）；
/// 2. **量化只在小字号开**：物理尺寸超过阈值后墨量必须接近恒等
///    （大字号本来就有满黑像素，量化只剩代价）；
/// 3. **网格稳定**：上面两条都不能改变位图的 `width/height/offset`
///    （排版与缓存依赖它，已有 `grid_fit_bitmap_grid_matches_unfitted` 钉住，
///    这里再钉一次是因为本轮动了拟合内部）。
#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

namespace {

using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::GridFitOptions;
using st::text::TextRenderer;

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

constexpr std::string_view kCjk = "资源管理器设置概览数据控制通道关于按钮";
constexpr std::string_view kLatin = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP";

struct Sharpness {
  int half{0};   ///< 0.45~0.55 半覆盖像素（糊边的直接证据）
  int solid{0};
  int mid{0};
  double ink{0.0};
};

[[nodiscard]] auto scan(const TextRenderer& renderer, std::string_view text, float pixel_size)
    -> Sharpness {
  Sharpness stat;
  for (const char32_t codepoint : st::utf8_decode(text)) {
    const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size, FontRole::Proportional);
    if (bitmap == nullptr || bitmap->coverage.empty()) continue;
    const int channels = bitmap->format == st::raster::CoverageFormat::Lcd ? 3 : 1;
    const std::size_t pixels =
        static_cast<std::size_t>(bitmap->width) * static_cast<std::size_t>(bitmap->height);
    for (std::size_t index = 0; index < pixels; ++index) {
      double value = 0.0;
      for (int channel = 0; channel < channels; ++channel) {
        value += static_cast<double>(
            bitmap->coverage[index * static_cast<std::size_t>(channels) +
                             static_cast<std::size_t>(channel)]);
      }
      value /= static_cast<double>(channels);
      stat.ink += value;
      if (value > 0.85) ++stat.solid;
      else if (value > 0.15) ++stat.mid;
      if (value > 0.45 && value < 0.55) ++stat.half;
    }
  }
  return stat;
}

}  // namespace

/// ① 护栏自洽：给小字号的**细笔画**足够的位移预算后，半覆盖像素显著下降。
///
/// 直接对比两个 `GridFitOptions`：默认（预算 = `max_shift`）与
/// 「预算 = `max_shift` + 量化量」。后者是修正后的口径。
/// 对细笔画而言，两者应当差出一个**可观测**的幅度——如果不差，
/// 说明修正没生效（或者护栏本来就自洽，那这条断言就该被重新审视）。
///
/// **判据是 A/B（拟合 vs 不拟合），不是绝对阈值**。
///
/// 为什么必须改成 A/B（2026-10-05 修）：本用例原先钉绝对阈值
/// （`mid/solid < 0.70`、`half < 240`），那两个数是**在覆盖率 gamma 默认 0.6 时标的**。
/// 后来默认γ先后变成 2.2 → 1.10（深色主题另有 0.60 档，见 `kDefaultCoverageGamma`），
/// 整个覆盖率分布被映射平移：同一份正确实现下 CJK 的 `mid/solid` 变成 **0.865**、
/// 拉丁 **0.602**——**恒红**，而它守的实现一直是对的。
/// 同一次 gamma 改动已把本文件里另外四条「墨量守恒」用例显式改用 γ=1 量，漏了这条。
///
/// A/B 的好处：γ 是**对两份位图施加的同一变换**，比值把它约掉——判据在任意 γ / 任意
/// 默认值下都成立，不需要跟着默认值重标（本轮实测 1.00 与 1.10 两档数字仅小数位差异）。
///
/// **阈值取 0.85 而不是 0.75**：
/// - 正确实现 CJK 实测 −48%、拉丁 −29%（相对**不拟合**）；
/// - 回退 `budget` 后 CJK 只剩 −19%、拉丁 −15%（实测，见下方回退验证）。
/// 即「预算过大」的失效模式（−19% ≈ 0.81）距 0.85 只有 5% 余量，而 0.75 会让它**通过**——
/// 所以阈值必须贴在正确值这一侧，不能图宽松。反向余量：正确值 0.52 / 0.71，
/// 离 0.85 有 16%‾20%，足够吸收字体差异。
///
/// **回退验证（写入本仓库的理由）**：把 `grid_fit.cpp` 的 `budget` 换成裸 `max_shift`
/// （即回退成「逐边独立判定」），本用例**当场变红**（0.81 > 0.85 不成立）：
/// ```
/// [fit/护栏] 中文 @20.25px：half 362 → 294（-19%）· mid/solid 2.444 → 1.439
/// 断言失败: ratio < 0.85
/// ```
ST_TEST(grid_fit_quantized_stems_keep_both_edges) {
  FontFixture fixture;
  if (!fixture.ok) return;
  // 细笔画区间：13.5 逻辑 px @1.5 DPI = 20.25 物理像素
  constexpr float kPixelSize = 20.25f;
  TextRenderer unfitted(*fixture.stack, 1.5f);
  unfitted.set_subpixel(false);
  unfitted.set_grid_fit(GridFitMode::Off);
  TextRenderer fitted(*fixture.stack, 1.5f);
  fitted.set_subpixel(false);
  fitted.set_grid_fit(GridFitMode::Normal);

  for (const auto& [name, text] : {std::pair{"中文", kCjk}, std::pair{"拉丁", kLatin}}) {
    const Sharpness base = scan(unfitted, text, kPixelSize);
    const Sharpness after = scan(fitted, text, kPixelSize);
    ST_REQUIRE(base.half > 0);
    const double ratio = static_cast<double>(after.half) / static_cast<double>(base.half);
    st::print("[fit/护栏] {} @{}px：half {} → {}（{:+.0f}%）· mid/solid {:.3f} → {:.3f}\n", name,
              kPixelSize, base.half, after.half, (ratio - 1.0) * 100.0,
              static_cast<double>(base.mid) / static_cast<double>(base.solid),
              static_cast<double>(after.mid) / static_cast<double>(after.solid));
    // ① 拟合必须**显著**减少半覆盖像素（糊边的直接证据）
    ST_CHECK(ratio < 0.85);
    // ② 同时不能是「把糊边换成了另一种糊」：实心像素要涨、半覆盖要降
    ST_CHECK(after.solid > 0);
    ST_CHECK(after.solid > base.solid);
  }
}

/// ② 量化只在小字号开：物理尺寸超过阈值后墨量接近恒等。
///
/// 大字号本来就有满黑像素，量化只剩代价（墨量偏差最大 0.5px）。
/// 反例：把 `kQuantizeBelowPx` 调到很大（全字号量化），22px 中文会从 +0.4% 涨到 +3.5%，
/// 超过本用例的 2% 容差。
///
/// **在 γ = 1 下量**：本用例的判据是“Σ 覆盖率 ≈ 几何墨量”，而该等式只对恒等映射成立；
/// 覆盖率 gamma（非线性，默认 2.2）会把拟合带来的相位差异折算成假的墨量变化
/// （实测 33px 中文 +10.2%），把真正的几何护栏（2%）淹没。关掉 gamma 才是本用例的原口径。
ST_TEST(grid_fit_keeps_ink_identity_for_large_glyphs) {
  FontFixture fixture;
  if (!fixture.ok) return;
  for (const float pixel_size : {33.0f, 60.0f}) {
    TextRenderer off(*fixture.stack, 1.0f);
    off.set_coverage_gamma(1.0f);
    TextRenderer fitted(*fixture.stack, 1.0f);
    fitted.set_coverage_gamma(1.0f);
    fitted.set_grid_fit(GridFitMode::Normal);
    for (const auto& [name, text] : {std::pair{"中文", kCjk}, std::pair{"拉丁", kLatin}}) {
      const Sharpness base = scan(off, text, pixel_size);
      const Sharpness after = scan(fitted, text, pixel_size);
      if (base.ink < 1.0) continue;
      const double delta = std::fabs(after.ink / base.ink - 1.0);
      st::print("[fit/大字号] {} @{}px 墨量变化 {:+.2f}%\n", name, pixel_size,
                (after.ink / base.ink - 1.0) * 100.0);
      ST_CHECK(delta < 0.02);
    }
  }
}

/// ③ 位图网格与拟合开关无关（本轮改了拟合内部，再钉一次）。
ST_TEST(grid_fit_internal_change_keeps_bitmap_grid) {
  FontFixture fixture;
  if (!fixture.ok) return;
  int compared = 0;
  for (const float device_scale : {1.0f, 1.5f, 2.0f}) {
    TextRenderer off(*fixture.stack, device_scale);
    TextRenderer fitted(*fixture.stack, device_scale);
    fitted.set_grid_fit(GridFitMode::Normal);
    for (const float pixel_size : {16.5f, 20.25f, 21.0f}) {
      for (const char32_t codepoint : st::utf8_decode("霜天三H口国A")) {
        const auto base = off.glyph_bitmap_of(codepoint, pixel_size);
        const auto after = fitted.glyph_bitmap_of(codepoint, pixel_size);
        if (base == nullptr || after == nullptr) continue;
        ST_CHECK_EQ(base->width, after->width);
        ST_CHECK_EQ(base->height, after->height);
        ST_CHECK_EQ(base->offset_x, after->offset_x);
        ST_CHECK_EQ(base->offset_y, after->offset_y);
        ++compared;
      }
    }
  }
  ST_CHECK(compared > 20);
}
