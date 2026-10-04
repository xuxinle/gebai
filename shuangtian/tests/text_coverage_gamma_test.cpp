/// 覆盖率 gamma 预校正（`TextRenderer::set_coverage_gamma`）：**合成空间的物理正确性**。
///
/// 起因（2026-10-04，实测）：与 Edge 逐像素对照后发现，霜天的字在**线性光口径**下
/// 系统性偏重——出厂默认配置（LCD + 5-tap 滤波 + 网格拟合 normal）墨量 +13.8%、
/// 实心像素 +16.4%（四条真实界面行带均值）。根因不是几何而是**合成空间**：
/// 霜天在 sRGB **编码空间**直接做 alpha 混合（`out = bg + (fg−bg)·α`），
/// 而屏幕是 sRGB 非线性。黑字白底时 code 空间混合给出 `code = 1−α`、
/// 线性正确给出 `code = srgb(1−α)`，后者在中间调处码值更高 = **笔画更浅**。
///
/// 修法：把覆盖率重映射为 `α' = 1 − (1−α)^(1/γ)`（γ = 2.2 时即完整线性合成），
/// 在**位图生成时**施加——覆盖率位图是 CPU 逐行混合与 GPU 遮罩纹理唯一共同的输入，
/// 在这一处校正，软件与 GPU 天然同源。
///
/// 本文件钉住四件事：
/// 1. **默认开启**（回归护栏：默认值被悄悄改回 1.0 会当场变红——那正是本次要修的行为）；
/// 2. **单调、保端点、严格变浅**：α' ≤ α 且 0/1 不动（端点不动是硬约束——
///    满覆盖的墨心必须仍是满黑，否则“字变浅”会变成“字发灰”这种更糟的缺陷）；
/// 3. **公式口径**：`1 − (1−α)^(1/γ)`，与 `linear_to_srgb(1−α)` 在中间调吻合到 3% 内；
/// 4. **缓存分桶**：不同 γ 是两份不同位图（否则“改了参数看不出变化”）。
///
/// 为什么断言放在**位图覆盖率**而不是画布像素：画布像素还叠着混合公式与预乘量化，
/// 取值域不干净；而覆盖率就是本参数的作用对象，"映射对不对"应当直接量它。

#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace {

using st::raster::CoverageFormat;
using st::text::FontStack;
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

/// 取一个字形在指定 γ 下的覆盖率（灰度模式，取值域干净）。
[[nodiscard]] auto coverage_at(const FontStack& stack, char32_t codepoint, float gamma)
    -> std::vector<float> {
  TextRenderer renderer(stack, 1.0f);
  renderer.set_coverage_gamma(gamma);
  const auto bitmap = renderer.glyph_bitmap_of(codepoint, 15.0f);
  if (bitmap == nullptr) return {};
  return std::vector<float>(bitmap->coverage.begin(), bitmap->coverage.end());
}

/// sRGB 解码（与 `linear_to_srgb` 的幂次近似对照用）。
[[nodiscard]] auto srgb_encode(double linear) noexcept -> double {
  return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

}  // namespace

/// ① 默认值：出厂就该开着校正（这是本次修复的行为本身）。
///
/// 为什么值得单独立一条：默认值悄悄改回 1.0（= 回到 code 空间混合）是本缺陷的**原状**，
/// 而它不会有任何编译或运行错误——只有这条用例会变红。
ST_TEST(text_coverage_gamma_is_on_by_default) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer renderer(*fixture.stack, 1.0f);
  ST_CHECK_NEAR(renderer.coverage_gamma(), TextRenderer::kDefaultCoverageGamma, 1.0e-6);
  ST_CHECK(renderer.coverage_gamma() > 1.0f);
  // 夹取口径：区间外**夹到边界**、NaN 取默认（不是一路 NaN 传下去）。
  renderer.set_coverage_gamma(0.5f);
  ST_CHECK_NEAR(renderer.coverage_gamma(), 1.0f, 1.0e-6);
  renderer.set_coverage_gamma(99.0f);
  ST_CHECK_NEAR(renderer.coverage_gamma(), 4.0f, 1.0e-6);
  renderer.set_coverage_gamma(std::nanf(""));
  ST_CHECK_NEAR(renderer.coverage_gamma(), TextRenderer::kDefaultCoverageGamma, 1.0e-6);
}

/// ② 映射本身：单调、保端点、在中间调严格变浅，且与 sRGB 编码吻合到 3% 内。
ST_TEST(text_coverage_gamma_maps_linear_light) {
  FontFixture fixture;
  if (!fixture.ok) return;
  constexpr char32_t kCodepoint = U'霜';
  const std::vector<float> identity = coverage_at(*fixture.stack, kCodepoint, 1.0f);
  const std::vector<float> corrected = coverage_at(*fixture.stack, kCodepoint, 2.2f);
  ST_REQUIRE(!identity.empty());
  ST_REQUIRE(identity.size() == corrected.size());

  double worst_endpoint = 0.0;
  double worst_mid_error = 0.0;
  double worst_decrease = 0.0;   // 中段“变浅”的最大逆差（应为 0）
  std::size_t mid_count = 0;
  std::size_t brightened = 0;
  for (std::size_t index = 0; index < identity.size(); ++index) {
    // 全程用 double 做算术：`-Wdouble-promotion` 在这个工程是错误（本文件初版就因此编不过），
    // 而且这里的判据精度本来就该离 float 远一点（端点要精确到 1e-6）。
    const double a = static_cast<double>(identity[index]);
    const double b = static_cast<double>(corrected[index]);
    ST_CHECK(b >= -1.0e-6 && b <= 1.0 + 1.0e-6);
    // 单调：α 越大，α' 越大。
    if (index > 0) {
      const double prev = static_cast<double>(identity[index - 1]);
      const double prev_out = static_cast<double>(corrected[index - 1]);
      if (prev <= a) ST_CHECK(b + 1.0e-6 >= prev_out);
    }
    // 端点不动：0 与 1 必须**精确**保持（1 代表满覆盖的墨心，动了就是“字发灰”）。
    if (a <= 1.0e-6) worst_endpoint = std::max(worst_endpoint, std::abs(b - a));
    if (a >= 1.0 - 1.0e-6) worst_endpoint = std::max(worst_endpoint, std::abs(b - a));
    if (a <= 1.0e-6 || a >= 1.0 - 1.0e-6) continue;
    // 中间调：必须变浅（α' ≤ α），且与 sRGB 编码口径吻合。
    if (b > a + 1.0e-4) {
      ++brightened;
      worst_decrease = std::max(worst_decrease, b - a);
    }
    if (a > 0.15 && a < 0.85) {
      ++mid_count;
      worst_mid_error = std::max(worst_mid_error, std::abs(b - (1.0 - srgb_encode(1.0 - a))));
    }
  }
  st::print("[gamma] 像素 {}，中间调 {}，端点最大偏差 {:.6f}，与 sRGB 编码最大偏差 {:.4f}"
            "（反向变墨最大 {:.6f}）\n",
            identity.size(), mid_count, worst_endpoint, worst_mid_error, worst_decrease);
  ST_CHECK(mid_count > 20);
  ST_CHECK_EQ(brightened, static_cast<std::size_t>(0));   // 一个都不许反向变黑
  ST_CHECK_NEAR(worst_endpoint, 0.0, 1.0e-6);
  // 3% 是“幂次近似 vs 精确 sRGB 曲线”的固有差（γ=2.2 是 1/2.4 的常用近似）。
  ST_CHECK(worst_mid_error < 0.03);
}

/// ③ 缓存分桶：不同 γ 必须拿到**不同**的位图。
///
/// 症状对照：漏掉键字段时，先取 γ=1.0 的字形会被 γ=2.2 的渲染器原样取回——
/// “改了参数看不出变化”，直到该字形被 LRU 淘汰才“突然生效”。
ST_TEST(text_coverage_gamma_is_part_of_cache_key) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer renderer(*fixture.stack, 1.0f);
  renderer.set_coverage_gamma(1.0f);
  const auto before = renderer.glyph_bitmap_of(U'A', 15.0f);
  ST_REQUIRE(before != nullptr);
  const std::uint64_t key_before = before->cache_key;
  const std::vector<float> coverage_before(before->coverage.begin(), before->coverage.end());

  renderer.set_coverage_gamma(2.2f);
  const auto after = renderer.glyph_bitmap_of(U'A', 15.0f);
  ST_REQUIRE(after != nullptr);
  ST_CHECK(after->cache_key != key_before);
  bool differs = after->coverage.size() == coverage_before.size();
  if (differs) {
    differs = false;
    for (std::size_t index = 0; index < after->coverage.size(); ++index) {
      if (std::abs(after->coverage[index] - coverage_before[index]) > 1.0e-6f) {
        differs = true;
        break;
      }
    }
  }
  ST_CHECK(differs);
  // 切回去必须回到**逐像素相同**的旧位图（键稳定，不是每次重新算一个新值）。
  renderer.set_coverage_gamma(1.0f);
  const auto again = renderer.glyph_bitmap_of(U'A', 15.0f);
  ST_REQUIRE(again != nullptr);
  ST_CHECK_EQ(again->cache_key, key_before);
  ST_CHECK_EQ(static_cast<int>(again->coverage.size()),
              static_cast<int>(coverage_before.size()));
}

/// ④ 与混合空间的因果关系：画到画布上时，校正后的字在**线性光口径**下更轻。
///
/// 口径：黑字白底、13.5px、LCD + 滤波 + 拟合（= 出厂默认那一套）。
/// 判据用**线性光下的墨量**（Σ 每像素的线性覆盖率），因为它与合成空间无关，
/// 是和浏览器可比的那个量（实测偏重 +13.8% → +3.3%）。
ST_TEST(text_coverage_gamma_lightens_rendered_text) {
  FontFixture fixture;
  if (!fixture.ok) return;
  constexpr std::string_view kText = "Settings 间距 12 text";
  constexpr std::size_t kWidth = 320;
  constexpr std::size_t kHeight = 60;

  const auto ink_of = [&](float gamma) -> double {
    st::raster::Canvas canvas{static_cast<int>(kWidth), static_cast<int>(kHeight), 1.0f};
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    TextRenderer renderer(*fixture.stack, 1.0f);
    renderer.set_subpixel(true);
    renderer.set_grid_fit(st::text::GridFitMode::Normal);
    renderer.set_coverage_gamma(gamma);
    (void)renderer.draw(canvas, kText, st::math::Point{2.0f, 2.0f}, 13.5f,
                        st::math::Color{0, 0, 0, 0xFF});
    // 线性光口径：code/255 的 sRGB 解码；黑字白底时“线性覆盖率”= 1 − 线性亮度。
    double sum = 0.0;
    for (const std::uint32_t pixel : canvas.pixels()) {
      const st::math::Color color = st::math::unpremultiply(pixel);
      const double linear_norm = static_cast<double>(color.r) / 255.0;
      const double linear = linear_norm <= 0.04045 ? linear_norm / 12.92
                                                    : std::pow((linear_norm + 0.055) / 1.055, 2.4);
      sum += 1.0 - linear;
    }
    return sum;
  };

  const double identity = ink_of(1.0f);
  const double corrected = ink_of(TextRenderer::kDefaultCoverageGamma);
  st::print("[gamma] 线性光墨量：γ=1.0 → {:.1f}，γ={} → {:.1f}（{:+.1f}%）\n", identity,
            TextRenderer::kDefaultCoverageGamma, corrected,
            (corrected / identity - 1.0) * 100.0);
  ST_CHECK(identity > 0.0);
  // 校正后必须显著更轻；且不能轻到“笔画被抽掉”（>-40% 的护栏）。
  ST_CHECK(corrected < identity * 0.9);
  ST_CHECK(corrected > identity * 0.6);
  // γ = 1.0 必须真是恒等（关掉校正 = 回归旧行为，这条保证“旧行为”真的可取得）。
  ST_CHECK_NEAR(ink_of(1.0f), identity, 1.0e-6);
}
