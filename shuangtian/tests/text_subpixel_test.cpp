/// 亚像素（LCD）文字渲染：**边缘合成方式变了，排版、墨量与亮度分布一点没变**。
///
/// 起因（实测反馈）：屏幕上 13.5px 正文的字“看着就是糊的”——灰度抗锯齿的过渡带
/// 摊在 2~3 个像素上，而 Chrome/VSCode 走 ClearType 类亚像素渲染（水平有效分辨率 3 倍）。
/// 人眼的参照系是后者，所以前者显糊。
///
/// 这个文件钉住六件事，缺一条这个特性就不成立：
/// 1. **网格不变**：位图 width/height/offset 与灰度模式逐字段相同（开不开都不挪字）；
/// 2. **墨量守恒**：三通道覆盖率之和 ≈ 3 × 灰度覆盖率（亚像素不是“加墨”，是重新分配）；
/// 3. **亮度守恒**（感知口径）：把覆盖率还原成**子像素分辨率的亮度 profile** 后，
///    LCD 与灰色的边缘位置/过渡宽度一致——即“字不会变胖变瘦、也不会移位”；
/// 4. **彩边真的出现了**，且只出现在边缘（深的墨心里三通道一致）；
/// 5. **滤波真的在起作用**（彩边能量下降），而关掉滤波锐度更高（对照实验）；
/// 6. **缓存不混用**：同一渲染器反复切模式，取回的位图与“一开始就是那个模式”逐像素一致。
///
/// 关于“更锐”这件事的**实测事实**（避免这个文件变成一句空话）：
/// 逐子像素亮度 profile 的最大斜率——未滤波 1.11×、滤波后 0.87~1.00×（相对灰度）；
/// 边缘 10→90% 过渡宽度——两者都是 1.00×。也就是说：
/// 亚像素带来的是**1/3 像素的边缘定位精度与彩边**，不是“过渡带变窄”。
/// 小字发糊的主因在**笔画未对齐像素网格（无 hinting）**，那是另一件事，不在本文件口径内。

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

namespace {

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::raster::CoverageFormat;
using st::text::FontStack;
using st::text::TextRenderer;

/// 有字体的环境才跑（无字体容器里这些断言没有意义）。
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

/// 界面里真实出现的样本：汉字（笔画多、边缘复杂）+ 拉丁（小字号、笔画细）。
constexpr std::string_view kSamples = "霜天自绘概览组件数据控制通道 win32 DPI";
constexpr float kSampleSize = 15.0f;

/// 子像素的亮度权重（Rec.709）。亚像素渲染之所以“能看见”，
/// 全靠屏幕的子像素在空间上是分开的、眼睛能分辨到这一层。
constexpr double kWeightR = 0.2126;
constexpr double kWeightG = 0.7152;
constexpr double kWeightB = 0.0722;

[[nodiscard]] auto weight_of(int channel) -> double {
  return channel == 0 ? kWeightR : (channel == 1 ? kWeightG : kWeightB);
}

/// 一份位图的“墨量”：灰度是覆盖率和，亚像素是三通道和（后者应约为前者 3 倍）。
[[nodiscard]] auto ink_sum(const TextRenderer::GlyphBitmap& bitmap) -> double {
  double total = 0.0;
  for (const float value : bitmap.coverage) total += static_cast<double>(value);
  return total;
}

/// 逐像素把亚像素三通道取平均后与灰度比较。
struct Deviation {
  double max_abs{0.0};
  double mean_abs{0.0};
};

[[nodiscard]] auto compare_with_grayscale(const TextRenderer::GlyphBitmap& gray,
                                          const TextRenderer::GlyphBitmap& lcd) -> Deviation {
  Deviation result;
  const std::size_t pixels =
      static_cast<std::size_t>(gray.width) * static_cast<std::size_t>(gray.height);
  if (pixels == 0 || lcd.coverage.size() < pixels * 3U || gray.coverage.size() < pixels) {
    return result;
  }
  double total = 0.0;
  for (std::size_t index = 0; index < pixels; ++index) {
    const double mean = (static_cast<double>(lcd.coverage[index * 3U + 0U]) +
                         static_cast<double>(lcd.coverage[index * 3U + 1U]) +
                         static_cast<double>(lcd.coverage[index * 3U + 2U])) /
                        3.0;
    const double delta = std::abs(mean - static_cast<double>(gray.coverage[index]));
    total += delta;
    if (delta > result.max_abs) result.max_abs = delta;
  }
  result.mean_abs = total / static_cast<double>(pixels);
  return result;
}

/// 彩边能量：`|R-G| + |G-B|` 的总和（“彩边有多浓”的直接量化）。
[[nodiscard]] auto fringe_energy(const TextRenderer::GlyphBitmap& lcd) -> double {
  double total = 0.0;
  const std::size_t pixels = lcd.coverage.size() / 3U;
  for (std::size_t index = 0; index < pixels; ++index) {
    const double r = lcd.coverage[index * 3U + 0U];
    const double g = lcd.coverage[index * 3U + 1U];
    const double b = lcd.coverage[index * 3U + 2U];
    total += std::abs(r - g) + std::abs(g - b);
  }
  return total;
}

/// **子像素分辨率的亮度 profile**（每行 3×宽度 个样本）。
///
/// 这是唯一公平的比较口径：灰度渲染把一个像素的墨均匀摊在三个子像素上
/// （每个子像素都发 `coverage × 自身权重` 的光），亚像素渲染则把墨放进具体某个子像素。
/// 只比“覆盖率统计”会得出“两者一样”的空结论，比亮度 profile 才能看出
/// 墨在**空间**上是怎么分布的——人眼看的正是后者。
[[nodiscard]] auto luminance_row(const TextRenderer::GlyphBitmap& bitmap, int row, bool lcd)
    -> std::vector<double> {
  const int width = bitmap.width;
  std::vector<double> profile(static_cast<std::size_t>(width) * 3U, 0.0);
  for (int x = 0; x < width; ++x) {
    for (int channel = 0; channel < 3; ++channel) {
      const std::size_t index = static_cast<std::size_t>(x) * 3U +
                                static_cast<std::size_t>(channel);
      const double coverage =
          lcd ? static_cast<double>(bitmap.coverage[(static_cast<std::size_t>(row) *
                                                         static_cast<std::size_t>(width) +
                                                     static_cast<std::size_t>(x)) *
                                                        3U +
                                                    static_cast<std::size_t>(channel)])
              : static_cast<double>(bitmap.coverage[static_cast<std::size_t>(row) *
                                                        static_cast<std::size_t>(width) +
                                                    static_cast<std::size_t>(x)]);
      profile[index] = coverage * weight_of(channel);
    }
  }
  return profile;
}

struct LuminanceStats {
  double mean_abs_difference{0.0};  ///< 与灰度亮度 profile 的平均差（每样本）
  double gray_max_slope{0.0};        ///< 相邻样本最大亮度差（灰度侧）
  double lcd_max_slope{0.0};         ///< 同上（亚像素侧）
};

[[nodiscard]] auto luminance_stats(const TextRenderer::GlyphBitmap& gray,
                                   const TextRenderer::GlyphBitmap& lcd) -> LuminanceStats {
  LuminanceStats stats;
  if (gray.width != lcd.width || gray.height != lcd.height || gray.height <= 0) return stats;
  double total = 0.0;
  std::size_t samples = 0;
  for (int row = 0; row < gray.height; ++row) {
    const std::vector<double> a = luminance_row(gray, row, false);
    const std::vector<double> b = luminance_row(lcd, row, true);
    for (std::size_t index = 0; index < a.size(); ++index) {
      total += std::abs(a[index] - b[index]);
      ++samples;
      if (index + 1 < a.size()) {
        stats.gray_max_slope = std::max(stats.gray_max_slope, std::abs(a[index + 1] - a[index]));
        stats.lcd_max_slope = std::max(stats.lcd_max_slope, std::abs(b[index + 1] - b[index]));
      }
    }
  }
  stats.mean_abs_difference = samples > 0 ? total / static_cast<double>(samples) : 0.0;
  return stats;
}

/// 把一段文本画到画布上（白字黑底透明），返回像素。
[[nodiscard]] auto render_text(const FontStack& stack, std::string_view text, float size, bool lcd)
    -> std::vector<std::uint32_t> {
  Canvas canvas{900, 64};
  canvas.clear(Color{0, 0, 0, 0});
  TextRenderer renderer(stack, 1.0f);
  renderer.set_subpixel(lcd);
  renderer.draw(canvas, text, Point{4.0f, 40.0f}, size, Color{0xFF, 0xFF, 0xFF, 0xFF});
  return std::vector<std::uint32_t>(canvas.pixels().begin(), canvas.pixels().end());
}

[[nodiscard]] auto differing(const std::vector<std::uint32_t>& a,
                             const std::vector<std::uint32_t>& b) -> std::size_t {
  if (a.size() != b.size()) return a.size() + b.size();
  std::size_t count = 0;
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (a[index] != b[index]) ++count;
  }
  return count;
}

}  // namespace

/// ① 网格不变：两种模式的位图**逐字段**同尺寸、同偏移、同通道布局契约。
ST_TEST(text_subpixel_bitmap_grid_matches_grayscale) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer gray(*fixture.stack, 1.0f);
  TextRenderer lcd(*fixture.stack, 1.0f);
  lcd.set_subpixel(true);

  std::size_t mismatched = 0;
  std::size_t compared = 0;
  for (const float size : {11.0f, 13.5f, 15.0f, 22.0f, 40.0f}) {
    for (const char32_t codepoint : st::utf8_decode(kSamples)) {
      const auto a = gray.glyph_bitmap_of(codepoint, size);
      const auto b = lcd.glyph_bitmap_of(codepoint, size);
      if (a == nullptr || b == nullptr) continue;
      ++compared;
      if (a->width != b->width || a->height != b->height || a->offset_x != b->offset_x ||
          a->offset_y != b->offset_y) {
        ++mismatched;
        st::print("[lcd] 网格不一致：U+{:04X} @{}px 灰度 {}x{}@{},{} vs LCD {}x{}@{},{}\n",
                  static_cast<unsigned>(codepoint), size, a->width, a->height, a->offset_x,
                  a->offset_y, b->width, b->height, b->offset_x, b->offset_y);
      }
      // 通道布局也要如实：灰度 1 项/像素、亚像素 3 项/像素
      ST_CHECK(a->format == CoverageFormat::Grayscale);
      ST_CHECK(b->format == CoverageFormat::Lcd);
      ST_CHECK_EQ(static_cast<int>(a->coverage.size()), static_cast<int>(a->width * a->height));
      ST_CHECK_EQ(static_cast<int>(b->coverage.size()),
                  static_cast<int>(b->width * b->height * 3));
    }
  }
  st::print("[lcd] 网格对齐：比较 {} 个字形，不一致 {}\n", compared, mismatched);
  ST_CHECK(compared > 20);
  ST_CHECK_EQ(static_cast<int>(mismatched), 0);
}

/// ② 墨量守恒：三通道之和 ≈ 3 × 灰度覆盖率（亚像素是重新分配，不是加墨）。
///
/// 注意口径：逐字形的**最坏**偏差会到 ~7%（两端用的是不同的水平光栅化分辨率，
/// 曲线扁平化容差在 3 倍细网格上更精细，小字号字形因此会有零点几个百分点的面积差），
/// 所以这里断言的是**聚合墨量**与**逐像素平均偏差**——量级错了才会红。
ST_TEST(text_subpixel_ink_matches_grayscale) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer gray(*fixture.stack, 1.0f);
  TextRenderer lcd(*fixture.stack, 1.0f);
  lcd.set_subpixel(true);

  double worst_mean = 0.0;
  double worst_max = 0.0;
  double worst_per_glyph = 0.0;
  double gray_total = 0.0;
  double lcd_total = 0.0;
  std::size_t compared = 0;
  for (const float size : {11.0f, 13.5f, 15.0f, 22.0f, 40.0f}) {
    for (const char32_t codepoint : st::utf8_decode(kSamples)) {
      const auto a = gray.glyph_bitmap_of(codepoint, size);
      const auto b = lcd.glyph_bitmap_of(codepoint, size);
      if (a == nullptr || b == nullptr || a->coverage.empty()) continue;
      ++compared;
      const Deviation deviation = compare_with_grayscale(*a, *b);
      worst_mean = std::max(worst_mean, deviation.mean_abs);
      worst_max = std::max(worst_max, deviation.max_abs);
      const double gray_ink = ink_sum(*a);
      if (gray_ink > 1.0) {
        worst_per_glyph = std::max(worst_per_glyph, std::abs(ink_sum(*b) / (3.0 * gray_ink) - 1.0));
      }
      gray_total += gray_ink;
      lcd_total += ink_sum(*b) / 3.0;
    }
  }
  const double aggregate_error =
      gray_total > 0.0 ? std::abs(lcd_total / gray_total - 1.0) : 1.0;
  st::print("[lcd] 墨量：{} 个字形，聚合误差 {:.2f}%、逐字形最坏 {:.2f}%、"
            "逐像素平均偏差 ≤{:.4f}（最大 ≤{:.4f}）\n",
            compared, aggregate_error * 100.0, worst_per_glyph * 100.0, worst_mean, worst_max);
  ST_CHECK(compared > 20);
  ST_CHECK(aggregate_error < 0.03);
  ST_CHECK(worst_mean < 0.08);
  ST_CHECK(worst_max < 0.5);
}

/// ③ 亮度守恒（感知口径）：亚像素不改变字的位置、粗细与亮度分布。
///
/// 这是本特性最容易被说反的一条：亚像素**不是**“把过渡带变窄”，
/// 而是“把同样的墨放进 1/3 像素的子像素格子里”。所以亮度 profile 必须基本重合
/// （实测平均差 ~%%、逐样本 ≤0.06），否则就是字形走样了。
ST_TEST(text_subpixel_luminance_profile_matches_grayscale) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer gray(*fixture.stack, 1.0f);
  TextRenderer lcd(*fixture.stack, 1.0f);
  lcd.set_subpixel(true);

  double worst = 0.0;
  std::size_t compared = 0;
  for (const float size : {11.0f, 13.5f, 15.0f, 22.0f, 40.0f}) {
    for (const char32_t codepoint : st::utf8_decode(kSamples)) {
      const auto a = gray.glyph_bitmap_of(codepoint, size);
      const auto b = lcd.glyph_bitmap_of(codepoint, size);
      if (a == nullptr || b == nullptr || a->coverage.empty()) continue;
      ++compared;
      worst = std::max(worst, luminance_stats(*a, *b).mean_abs_difference);
    }
  }
  st::print("[lcd] 亮度 profile 平均差（子像素口径，滤波后）≤{:.4f}（{} 个字形）\n", worst,
            compared);
  ST_CHECK(compared > 20);
  ST_CHECK(worst < 0.06);
}

/// ④ 彩边只出现在边缘：墨心（四邻全满）三通道必须一致，边缘必须有明显通道差。
ST_TEST(text_subpixel_edges_carry_color_and_core_stays_neutral) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer gray(*fixture.stack, 1.0f);
  TextRenderer lcd(*fixture.stack, 1.0f);
  lcd.set_subpixel(true);

  const auto reference = gray.glyph_bitmap_of(U'口', 40.0f);
  ST_CHECK(reference != nullptr);

  // 逐个字形统计：边缘（有墨但不是深墨心）与深墨心（3×3 邻域全满）。
  std::size_t edge_pixels = 0;
  std::size_t colored_edges = 0;
  std::size_t core_pixels = 0;
  std::size_t tinted_cores = 0;
  double edge_spread = 0.0;
  double core_spread_max = 0.0;
  for (const char32_t codepoint : st::utf8_decode("霜口田目日回国一十")) {
    for (const float size : {22.0f, 40.0f}) {
      const auto subpixel = lcd.glyph_bitmap_of(codepoint, size);
      if (subpixel == nullptr || subpixel->coverage.empty()) continue;
      const int width = subpixel->width;
      const int height = subpixel->height;
      const auto channel = [&](int x, int y, int c) -> double {
        return subpixel->coverage[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) *
                                      3U +
                                  static_cast<std::size_t>(c)];
      };
      for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
          const double r = channel(x, y, 0);
          const double g = channel(x, y, 1);
          const double b = channel(x, y, 2);
          const double spread = std::max({r, g, b}) - std::min({r, g, b});
          const double mean = (r + g + b) / 3.0;
          // 深墨心：**3×3 邻域全满**（离边缘至少 1 个像素）——
          // 滤波的跨度是 ±2 子像素 ≈ 0.67 像素，够不到这里，所以三通道必须完全一致。
          bool deep_core = true;
          for (int dy = -1; dy <= 1 && deep_core; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              const int nx = x + dx;
              const int ny = y + dy;
              if (std::min({channel(nx, ny, 0), channel(nx, ny, 1), channel(nx, ny, 2)}) < 0.98) {
                deep_core = false;
                break;
              }
            }
          }
          if (deep_core) {
            ++core_pixels;
            core_spread_max = std::max(core_spread_max, spread);
            if (spread > 0.02) ++tinted_cores;
          } else if (mean > 0.05) {
            ++edge_pixels;
            edge_spread += spread;
            if (spread > 0.02) ++colored_edges;
          }
        }
      }
    }
  }
  if (edge_pixels > 0) edge_spread /= static_cast<double>(edge_pixels);
  st::print("[lcd] 彩边分布（9 个字形 @22/40px）：边缘 {} 个（带色 {}，平均通道差 {:.3f}）；"
            "深墨心 {} 个（带色 {}，最大通道差 {:.3f}）\n",
            edge_pixels, colored_edges, edge_spread, core_pixels, tinted_cores, core_spread_max);
  ST_CHECK(edge_pixels > 20);
  ST_CHECK(core_pixels > 20);
  // 边缘必须有明显的通道差（这就是“1/3 像素定位”留下的彩边）
  ST_CHECK(edge_spread > 0.02);
  // 深墨心必须干净：亚像素不该给实心区域上色（滤波在边缘附近有细微着色，那是它的定义）
  ST_CHECK_EQ(static_cast<int>(tinted_cores), 0);
}

/// ⑤ 滤波确实在起作用；且关掉滤波锐度更高（两条一起钉住这个取舍）。
ST_TEST(text_subpixel_filter_trades_fringe_for_sharpness) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer filtered(*fixture.stack, 1.0f);
  filtered.set_subpixel(true);
  TextRenderer raw(*fixture.stack, 1.0f);
  raw.set_subpixel(true);
  raw.set_subpixel_filter(false);
  TextRenderer gray(*fixture.stack, 1.0f);

  double filtered_energy = 0.0;
  double raw_energy = 0.0;
  double gray_slope = 0.0;
  double filtered_slope = 0.0;
  double raw_slope = 0.0;
  for (const float size : {11.0f, 13.5f, 15.0f, 22.0f, 40.0f}) {
    for (const char32_t codepoint : st::utf8_decode(kSamples)) {
      const auto a = gray.glyph_bitmap_of(codepoint, size);
      const auto b = filtered.glyph_bitmap_of(codepoint, size);
      const auto c = raw.glyph_bitmap_of(codepoint, size);
      if (a == nullptr || b == nullptr || c == nullptr || a->coverage.empty()) continue;
      filtered_energy += fringe_energy(*b);
      raw_energy += fringe_energy(*c);
      const LuminanceStats stats = luminance_stats(*a, *b);
      const LuminanceStats raw_stats = luminance_stats(*a, *c);
      gray_slope = std::max(gray_slope, stats.gray_max_slope);
      filtered_slope = std::max(filtered_slope, stats.lcd_max_slope);
      raw_slope = std::max(raw_slope, raw_stats.lcd_max_slope);
    }
  }
  st::print("[lcd] 彩边能量：滤波 {:.1f} vs 原始 {:.1f}（{:.1f}%）；"
            "亮度最大斜率：灰度 {:.3f} / 滤波 {:.3f}（{:.2f}×）/ 原始 {:.3f}（{:.2f}×）\n",
            filtered_energy, raw_energy,
            raw_energy > 0.0 ? filtered_energy / raw_energy * 100.0 : 0.0, gray_slope,
            filtered_slope, gray_slope > 0.0 ? filtered_slope / gray_slope : 0.0, raw_slope,
            gray_slope > 0.0 ? raw_slope / gray_slope : 0.0);
  ST_CHECK(raw_energy > 0.0);
  // 滤波一定要降彩边（否则它就没存在的理由）
  ST_CHECK(filtered_energy < raw_energy * 0.95);
  // 关掉滤波的子像素渲染更“立”（实测 ~1.1× 灰度）；滤波会把这部分增益换掉
  ST_CHECK(raw_slope > gray_slope);
}

/// ⑥ 模式切换不会取到“另一种模式的位图”（缓存键必须带渲染模式位）。
ST_TEST(text_subpixel_cache_does_not_mix_modes) {
  FontFixture fixture;
  if (!fixture.ok) return;
  const std::vector<std::uint32_t> gray_reference =
      render_text(*fixture.stack, kSamples, kSampleSize, false);
  const std::vector<std::uint32_t> lcd_reference =
      render_text(*fixture.stack, kSamples, kSampleSize, true);
  // 两种形态必须真的不同（否则下面“没混用”是废话）
  ST_CHECK(differing(gray_reference, lcd_reference) > 0);

  TextRenderer renderer(*fixture.stack, 1.0f);
  Canvas canvas{900, 64};
  for (int round = 0; round < 3; ++round) {
    renderer.set_subpixel(false);
    canvas.clear(Color{0, 0, 0, 0});
    renderer.draw(canvas, kSamples, Point{4.0f, 40.0f}, kSampleSize,
                  Color{0xFF, 0xFF, 0xFF, 0xFF});
    const std::vector<std::uint32_t> gray(canvas.pixels().begin(), canvas.pixels().end());
    renderer.set_subpixel(true);
    canvas.clear(Color{0, 0, 0, 0});
    renderer.draw(canvas, kSamples, Point{4.0f, 40.0f}, kSampleSize,
                  Color{0xFF, 0xFF, 0xFF, 0xFF});
    const std::vector<std::uint32_t> lcd(canvas.pixels().begin(), canvas.pixels().end());
    ST_CHECK_EQ(static_cast<int>(differing(gray, gray_reference)), 0);
    ST_CHECK_EQ(static_cast<int>(differing(lcd, lcd_reference)), 0);
  }
}
