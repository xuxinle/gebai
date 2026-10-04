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

/// ① 默认值落在合法区间、且与常量一致。
///
/// 默认值的方向**随参照变过两次**，本用例刻意**不断言方向**：
///   · 曾设为 2.2（提亮，“完整线性空间合成”）→ 被用户实测驳回；
///   · 改为 0.6（加墨）—— 参照是 headless Chromium；
///   · 改为 1.10 —— 参照换成**真窗口**浏览器后重标（真窗口与无头不做同一套
///     ClearType 调校，覆盖率分布不同，参照一变"理想 γ"跟着变，见 `DESIGN.md §4.3.7.17`）。
/// 方向由**真窗口参照的整幅总墨量**定，不由用例锁死；用例只守住
/// "取值合法 + 与常量一致 + 夹取口径正确"。
ST_TEST(text_coverage_gamma_default_in_range) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer renderer(*fixture.stack, 1.0f);
  ST_CHECK_NEAR(renderer.coverage_gamma(), TextRenderer::kDefaultCoverageGamma, 1.0e-6);
  ST_CHECK(renderer.coverage_gamma() >= 0.3f);
  ST_CHECK(renderer.coverage_gamma() <= 4.0f);
  // 夹取口径：区间外**夹到边界**（双向），NaN 取默认。
  renderer.set_coverage_gamma(0.1f);
  ST_CHECK_NEAR(renderer.coverage_gamma(), 0.3f, 1.0e-6);
  renderer.set_coverage_gamma(99.0f);
  ST_CHECK_NEAR(renderer.coverage_gamma(), 4.0f, 1.0e-6);
  renderer.set_coverage_gamma(1.0f);
  ST_CHECK_NEAR(renderer.coverage_gamma(), 1.0f, 1.0e-6);  // off 必须真能取到
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
  std::size_t mid_count = 0;
  std::size_t lightened = 0;
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
    // 中间调：γ>1 必须**变浅**（α' ≤ α）——这是它的定义；反向（变黑）说明符号错了。
    if (b > a + 1.0e-4) ++lightened;
    if (a > 0.15 && a < 0.85) {
      ++mid_count;
      worst_mid_error = std::max(worst_mid_error, std::abs(b - (1.0 - srgb_encode(1.0 - a))));
    }
  }
  st::print("[gamma] 像素 {}，中间调 {}，端点最大偏差 {:.6f}，与 sRGB 编码最大偏差 {:.4f}"
            "（反向变墨计数 {}）\n",
            identity.size(), mid_count, worst_endpoint, worst_mid_error, lightened);
  ST_CHECK(mid_count > 20);
  // γ=2.2 在中间调**一个都不该变黑**——若出现，说明映射被写反了符号。
  ST_CHECK_EQ(lightened, static_cast<std::size_t>(0));
  ST_CHECK_NEAR(worst_endpoint, 0.0, 1.0e-6);
  // 3% 是“幂次近似 vs 精确 sRGB 曲线”的固有差（γ=2.2 是 1/2.4 的常用近似）。
  ST_CHECK(worst_mid_error < 0.03);
}

/// ②b **压黑方向**（γ < 1）也必须成立：中间调变黑、端点仍不动。
///
/// 这一条比提亮方向更重要：如果将来要把默认值改成加墨（用户实测偏好的方向），
/// 靠的就是 γ<1。映射写错方向时“字反而更虚”会很难归因，所以两个方向都钉住。
ST_TEST(text_coverage_gamma_can_darken) {
  FontFixture fixture;
  if (!fixture.ok) return;
  const std::vector<float> identity = coverage_at(*fixture.stack, U'霜', 1.0f);
  const std::vector<float> darkened = coverage_at(*fixture.stack, U'霜', 0.7f);
  ST_REQUIRE(!identity.empty());
  ST_REQUIRE(identity.size() == darkened.size());
  std::size_t changed = 0;
  double worst_endpoint = 0.0;
  for (std::size_t index = 0; index < identity.size(); ++index) {
    const double a = static_cast<double>(identity[index]);
    const double b = static_cast<double>(darkened[index]);
    if (a <= 1.0e-6 || a >= 1.0 - 1.0e-6) {
      worst_endpoint = std::max(worst_endpoint, std::abs(b - a));
      continue;
    }
    // γ<1 是**加墨**：α' 必须**变大**（本用例初版就把方向写反了，量出 0 个变黑——
    // 而库侧一直是对的，画布口径的 ④ 用例同时在跑、当场排除了库的嫌疑）。
    if (b > a + 1.0e-4) ++changed;
  }
  st::print("[gamma] γ=0.7 压黑：{} / {} 个中间调像素变黑，端点最大偏差 {:.6f}\n", changed,
            identity.size(), worst_endpoint);
  ST_CHECK(changed > 20);
  ST_CHECK_NEAR(worst_endpoint, 0.0, 1.0e-6);
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

/// ④ 与混合空间的因果关系：γ=2.2 提亮（减墨）、γ=0.7 压黑（加墨），两者都变。
///
/// 口径用**朴素像素亮度**（不做覆盖率反解）：上一轮的教训是反解会带进口径假设，
/// 而“字看着多黑”是个不需要假设的量。
/// 另附**线性光口径**的墨量，供与历史数字衔接。
ST_TEST(text_coverage_gamma_changes_rendered_weight) {
  FontFixture fixture;
  if (!fixture.ok) return;
  constexpr std::string_view kText = "Settings 间距 12 text";
  constexpr std::size_t kWidth = 320;
  constexpr std::size_t kHeight = 60;

  struct Stats {
    double raw_mean_lum{0.0};   ///< 文字像素的平均亮度（越低越黑/越实）
    double solid_ratio{0.0};    ///< 亮度 <50% 的墨像素占比
    double band_ratio{0.0};     ///< 亮度 50~90% 的过渡带占比（越高越“灰”）
    double linear_ink{0.0};     ///< 线性光墨量（Σ(1−线性亮度)）
  };
  const auto measure = [&](float gamma) -> Stats {
    st::raster::Canvas canvas{static_cast<int>(kWidth), static_cast<int>(kHeight), 1.0f};
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    TextRenderer renderer(*fixture.stack, 1.0f);
    renderer.set_subpixel(true);
    renderer.set_grid_fit(st::text::GridFitMode::Normal);
    // 本用例度量的是 **gamma 本身**的墨量方向：显式关掉拟合墨量补偿，
    // 否则补偿会为了归一化墨量而重分布中间调，把 gamma 的方向淹掉
    //（补偿另有自己的用例 `text_ink_compensation_evens_out_glyph_weight`）。
    renderer.set_ink_compensation(false);
    renderer.set_coverage_gamma(gamma);
    (void)renderer.draw(canvas, kText, st::math::Point{2.0f, 2.0f}, 13.5f,
                        st::math::Color{0, 0, 0, 0xFF});
    Stats stats;
    std::size_t inked = 0;
    for (const std::uint32_t pixel : canvas.pixels()) {
      const st::math::Color color = st::math::unpremultiply(pixel);
      const double lum = (0.2126 * static_cast<double>(color.r) +
                          0.7152 * static_cast<double>(color.g) +
                          0.0722 * static_cast<double>(color.b)) /
                         255.0;
      const double norm = static_cast<double>(color.r) / 255.0;
      const double linear = norm <= 0.04045 ? norm / 12.92
                                            : std::pow((norm + 0.055) / 1.055, 2.4);
      stats.linear_ink += 1.0 - linear;
      if (lum >= 0.95) continue;   // 背景不参与
      ++inked;
      if (lum < 0.5) stats.solid_ratio += 1.0;
      else if (lum < 0.90) stats.band_ratio += 1.0;
      stats.raw_mean_lum += lum;
    }
    if (inked > 0) {
      stats.solid_ratio /= static_cast<double>(inked);
      stats.band_ratio /= static_cast<double>(inked);
      stats.raw_mean_lum /= static_cast<double>(inked);
    }
    return stats;
  };

  const Stats base = measure(1.0f);
  const float dark_gamma =
      TextRenderer::kDefaultCoverageGamma < 1.0f ? TextRenderer::kDefaultCoverageGamma : 0.5f;
  const Stats bright = measure(2.2f);
  const Stats dark = measure(dark_gamma);
  st::print("[gamma] γ=1.0  平均亮度 {:.3f}  实心 {:.1f}%  过渡带 {:.1f}%  线性墨量 {:.1f}\n",
            base.raw_mean_lum, base.solid_ratio * 100.0, base.band_ratio * 100.0,
            base.linear_ink);
  st::print("[gamma] γ=2.2  平均亮度 {:.3f}  实心 {:.1f}%  过渡带 {:.1f}%  线性墨量 {:.1f}\n",
            bright.raw_mean_lum, bright.solid_ratio * 100.0, bright.band_ratio * 100.0,
            bright.linear_ink);
  st::print("[gamma] γ={}  平均亮度 {:.3f}  实心 {:.1f}%  过渡带 {:.1f}%  线性墨量 {:.1f}\n",
            dark_gamma, dark.raw_mean_lum, dark.solid_ratio * 100.0, dark.band_ratio * 100.0,
            dark.linear_ink);
  ST_CHECK(base.solid_ratio > 0.0);
  // 提亮方向：字变浅、实心像素变少、线性墨量变少
  ST_CHECK(bright.raw_mean_lum > base.raw_mean_lum);
  ST_CHECK(bright.solid_ratio < base.solid_ratio);
  ST_CHECK(bright.linear_ink < base.linear_ink * 0.95);
  // 加墨方向：字变黑、实心像素变多、过渡带变**窄**（不是“越黑越糊”）
  ST_CHECK(dark.raw_mean_lum < base.raw_mean_lum);
  ST_CHECK(dark.solid_ratio > base.solid_ratio);
  ST_CHECK(dark.band_ratio < base.band_ratio);
  ST_CHECK(dark.linear_ink > base.linear_ink);
}

/// **拟合墨量补偿**：开启后「逐字墨量」应当向**不拟合基准**靠拢（字间更一致），
/// 而几何（边缘相位）不动——所以用“与基准的相对墨量”本身当判据，而不是平均亮度。
///
/// 为什么这个量是对的判据（此前的教训）：应用层实测「同一菜单栏里有的字清晰、有的字发灰」
/// （用户线索：「运行」比其它项黑 34%），而根因是**拟合对每个字的墨量改变幅度不一致**
/// （−27%~+10%）。平均亮度看不见它——它把字间差异平均掉了。
ST_TEST(text_ink_compensation_evens_out_glyph_weight) {
  FontFixture fixture;
  if (!fixture.ok) return;
  constexpr std::string_view kText = "文件编辑选择查看运行帮助";
  constexpr std::size_t kWidth = 320;
  constexpr std::size_t kHeight = 60;

  /// 逐字墨量：每个字形单独渲染，返回（该字墨量, 该字不拟合基准墨量）。
  const auto ratios = [&](bool compensate) {
    std::vector<double> out;
    for (const char32_t codepoint : st::utf8_decode(kText)) {
      const std::string single = st::utf8_encode(std::u32string(1, codepoint));
      const auto ink_of = [&](st::text::GridFitMode fit, bool comp) -> double {
        st::raster::Canvas canvas{static_cast<int>(kWidth), static_cast<int>(kHeight), 1.0f};
        canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
        TextRenderer renderer(*fixture.stack, 1.0f);
        renderer.set_subpixel(true);
        renderer.set_grid_fit(fit);
        renderer.set_ink_compensation(comp);
        renderer.set_coverage_gamma(0.6f);
        (void)renderer.draw(canvas, single, st::math::Point{2.0f, 2.0f}, 20.25f,
                            st::math::Color{0, 0, 0, 0xFF});
        double ink = 0.0;
        for (const std::uint32_t pixel : canvas.pixels()) {
          const st::math::Color color = st::math::unpremultiply(pixel);
          ink += 1.0 - static_cast<double>(color.r) / 255.0;
        }
        return ink;
      };
      const double reference = ink_of(st::text::GridFitMode::Off, false);
      const double fitted =
          ink_of(st::text::GridFitMode::Normal, compensate);
      if (reference > 0.0) out.push_back(fitted / reference);
    }
    return out;
  };
  const auto spread = [](const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    return *high - *low;
  };

  const std::vector<double> without = ratios(false);
  const std::vector<double> with = ratios(true);
  if (without.empty() || with.empty()) return;
  st::print("[ink] 补偿关：逐字变化率 {} 个，极差 {:.3f}\n", without.size(), spread(without));
  st::print("[ink] 补偿开：逐字变化率 {} 个，极差 {:.3f}\n", with.size(), spread(with));
  // 拟合确实扰乱了字间墨量（否则这条用例与补偿都没有存在的理由）
  ST_CHECK(spread(without) > 0.15);
  // 补偿把它收窄——这是本补偿的全部目的
  ST_CHECK(spread(with) < spread(without) * 0.8);
  // 只提亮不压暗：补偿后的均值不应低于补偿前（不得把本来对的字压暗）
  const auto mean_of = [](const std::vector<double>& v) {
    double sum = 0.0;
    for (const double x : v) sum += x;
    return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
  };
  ST_CHECK(mean_of(with) >= mean_of(without) - 1.0e-3);
}
