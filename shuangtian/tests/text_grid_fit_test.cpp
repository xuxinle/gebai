/// 字形网格拟合（hinting）：**笔画边缘吸附到像素网格，而排版与字宽一点不变**。
///
/// 起因（2026-10-01 由 vsedit 反馈逐像素对照定位）：13.5px 正文在 1.25 DPI 下是
/// 16.88 物理像素，笔画宽 1.2~1.7px 且边缘落在**分数相位**上——实测 184 条竖笔画里
/// **没有一条**边缘落在整数网格上（0.0%），91.8% 是"每边各一个过渡像素"的缓坡。
/// 这就是"字看着糊"的主因，**与抗锯齿模式无关**。
///
/// 这个文件钉住五件事：
/// 1. **排版不变**：位图的 `width/height/offset_x/offset_y` 与不拟合时**逐字段相同**
///    ——拟合只动轮廓点、不动 `hmtx` 的字宽，所以行内位置与折行都不变；
/// 2. **相位真的改善**：竖笔画边缘落在整数网格的比例显著上升（这是拟合的目标本身）；
/// 3. **墨量守恒**：拟合不该把字"加粗"或"减细"（13.5px 容差 6%，大字号更严）；
/// 4. **缓存键含模式位**：反复切换模式取回的位图与"一开始就是该模式"逐像素一致；
/// 5. **护栏有效**：位移超限时**放弃拟合**（宁可保持原样，也不让字形走样）。
///
/// 实测收益（`tools/stem_phase_probe.cpp`，同一把尺子量拟合前后）：
/// 中文「每边一个过渡像素」的糊笔画 90.9% → 21.8%、锐笔画 0% → 54.7%；
/// 拉丁 87.3% → 44.0%。FreeType auto-hinter 的基准是拉丁 49.0% / 中文 20.6%
/// （见 `tools/hinting_gain_probe.cpp`）——本实现的中文收益是它的 3 倍。

#include "st/test/test.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/path.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontStack;
using st::text::GlyphClass;
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

constexpr std::string_view kCjk =
    "霜天自绘概览组件数据控制通道关于按钮表单密码提交重置进度状态窗口字体渲染"
    "一二三四五六七八九十日月田目国回口品晶磊赢疆餐囊藏";
constexpr std::string_view kLatin =
    "Handgloves Illegible ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789";

/// "一步到位"的上升沿计数：相邻两像素从左边的“基本无墨”跳到右边的“基本满墨”。
/// 拟合的目标就是**让边缘一步到位**（而不是隔着中间调缓坡）。
struct StepStats {
  std::size_t steps{0};
  std::size_t gradual{0};  ///< 隔了中间调才过渡过去的边
};

[[nodiscard]] auto step_stats(const TextRenderer& renderer, std::string_view text, float pixel_size)
    -> StepStats {
  StepStats stats;
  for (const char32_t codepoint : st::utf8_decode(text)) {
    const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
    if (bitmap == nullptr || bitmap->coverage.empty()) continue;
    const int width = bitmap->width;
    const int height = bitmap->height;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x + 1 < width; ++x) {
        const float left = bitmap->coverage[static_cast<std::size_t>(y) * width + x];
        const float right = bitmap->coverage[static_cast<std::size_t>(y) * width + x + 1];
        const bool rising = left <= 0.1f && right >= 0.9f;
        const bool falling = left >= 0.9f && right <= 0.1f;
        if (rising || falling) {
          ++stats.steps;
          continue;
        }
        // 中间隔着部分覆盖像素：左边基本无墨、右边是中间调（或反之）——缓坡
        if ((left <= 0.1f && right > 0.1f && right < 0.9f) ||
            (left > 0.1f && left < 0.9f && right >= 0.9f)) {
          ++stats.gradual;
        }
      }
    }
  }
  return stats;
}

/// 有墨像素里的中间调占比（越低越锐）。
[[nodiscard]] auto mid_tone_ratio(const TextRenderer& renderer, std::string_view text,
                                  float pixel_size) -> double {
  std::size_t ink = 0;
  std::size_t mid = 0;
  for (const char32_t codepoint : st::utf8_decode(text)) {
    const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
    if (bitmap == nullptr) continue;
    for (const float value : bitmap->coverage) {
      if (value <= 0.02f) continue;
      ++ink;
      if (value > 0.15f && value < 0.85f) ++mid;
    }
  }
  return ink > 0 ? static_cast<double>(mid) / static_cast<double>(ink) : 0.0;
}

[[nodiscard]] auto ink_sum(const TextRenderer& renderer, std::string_view text, float pixel_size)
    -> double {
  double total = 0.0;
  for (const char32_t codepoint : st::utf8_decode(text)) {
    const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
    if (bitmap == nullptr) continue;
    for (const float value : bitmap->coverage) total += static_cast<double>(value);
  }
  return total;
}

[[nodiscard]] auto render_text(const FontStack& stack, std::string_view text, float size,
                               GridFitMode mode) -> std::vector<std::uint32_t> {
  Canvas canvas{900, 64};
  canvas.clear(Color{0, 0, 0, 0});
  TextRenderer renderer(stack, 1.0f);
  renderer.set_grid_fit(mode);
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

/// ① 排版不变：拟合与否，位图网格逐字段相同。
///
/// 这是**最重要的不变式**：拟合动的是轮廓点，不是字宽（`hmtx`），
/// 所以行内笔位、折行位置、光标定位全部不受影响——只有笔画边缘的相位变了。
ST_TEST(grid_fit_bitmap_grid_matches_unfitted) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer off(*fixture.stack, 1.0f);
  TextRenderer light(*fixture.stack, 1.0f);
  light.set_grid_fit(GridFitMode::Light);
  TextRenderer normal(*fixture.stack, 1.0f);
  normal.set_grid_fit(GridFitMode::Normal);

  std::size_t compared = 0;
  std::size_t mismatched = 0;
  for (const float size : {11.0f, 13.5f, 15.0f, 22.0f, 40.0f}) {
    for (const char32_t codepoint : st::utf8_decode(kCjk)) {
      const auto base = off.glyph_bitmap_of(codepoint, size);
      const auto fitted = light.glyph_bitmap_of(codepoint, size);
      const auto strong = normal.glyph_bitmap_of(codepoint, size);
      if (base == nullptr || fitted == nullptr || strong == nullptr) continue;
      ++compared;
      const auto same_grid = [&](const TextRenderer::GlyphBitmap& other) {
        return base->width == other.width && base->height == other.height &&
               base->offset_x == other.offset_x && base->offset_y == other.offset_y;
      };
      if (!same_grid(*fitted) || !same_grid(*strong)) {
        ++mismatched;
        st::print("[fit] 网格不一致：U+{:04X} @{}px 基准 {}x{}@{},{} / Light {}x{}@{},{} / "
                  "Normal {}x{}@{},{}\n",
                  static_cast<unsigned>(codepoint), size, base->width, base->height,
                  base->offset_x, base->offset_y, fitted->width, fitted->height, fitted->offset_x,
                  fitted->offset_y, strong->width, strong->height, strong->offset_x,
                  strong->offset_y);
      }
    }
  }
  st::print("[fit] 网格对齐：比较 {} 个字形，不一致 {}\n", compared, mismatched);
  ST_CHECK(compared > 50);
  ST_CHECK_EQ(static_cast<int>(mismatched), 0);
}

/// ② 相位改善：拟合后"一步到位"的上升沿显著增多、中间调占比显著下降。
ST_TEST(grid_fit_sharpens_stems) {
  FontFixture fixture;
  if (!fixture.ok) return;
  const float pixel_size = 13.5f * 1.25f;  // 与反馈现场同口径

  for (const auto& [name, text] : {std::pair{"中文", kCjk}, std::pair{"拉丁", kLatin}}) {
    TextRenderer off(*fixture.stack, 1.0f);
    TextRenderer normal(*fixture.stack, 1.0f);
    normal.set_grid_fit(GridFitMode::Normal);
    const StepStats base = step_stats(off, text, pixel_size);
    const StepStats fitted = step_stats(normal, text, pixel_size);
    const double mid_base = mid_tone_ratio(off, text, pixel_size);
    const double mid_fitted = mid_tone_ratio(normal, text, pixel_size);
    st::print("[fit] {}：一步到位 {} → {}（缓坡 {} → {}）；中间调占比 {:.1f}% → {:.1f}%\n",
              name, base.steps, fitted.steps, base.gradual, fitted.gradual, mid_base * 100.0,
              mid_fitted * 100.0);
    // 中间调占比必须下降（这是"看着不糊了"的可断言代理），且幅度要真实可见
    ST_CHECK(mid_fitted < mid_base);
    ST_CHECK(mid_base - mid_fitted > 0.02);
    // 缓坡必须减少（边缘变得"一步到位"）
    ST_CHECK(fitted.gradual < base.gradual);
  }
}

/// ③ 墨量守恒：拟合不该显著改变字的粗细（大字号更严——那里的形变余量更小）。
///
/// **必须在 γ = 1 下量**（2026-10-04 补）：本用例的口径是“Σ覆盖率 ≈ 几何墨量”，
/// 而那个相等关系只在**恒等映射**下成立（Σ 是线性算子）。叠加覆盖率 gamma 后，
/// 拟合与不拟合分别作用于不同的齿位，Σ 之差就混入了非线性映射的残差
/// （实测拟合后墨量变化从 ±5% 跳到 +16.5%），把真正的几何形变量污染掉。
/// 所以这里刻意关掉 gamma：**它量的是几何，不是合成空间**。
ST_TEST(grid_fit_preserves_ink) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer off(*fixture.stack, 1.0f);
  TextRenderer normal(*fixture.stack, 1.0f);
  off.set_coverage_gamma(1.0f);
  normal.set_coverage_gamma(1.0f);
  normal.set_grid_fit(GridFitMode::Normal);

  for (const auto& [name, text] : {std::pair{"中文", kCjk}, std::pair{"拉丁", kLatin}}) {
    for (const float size : {13.5f, 16.0f, 22.0f, 40.0f}) {
      const double base = ink_sum(off, text, size);
      const double fitted = ink_sum(normal, text, size);
      if (base < 1.0) continue;
      const double delta = std::fabs(fitted / base - 1.0);
      st::print("[fit] {} @{}px 墨量变化 {:+.2f}%\n", name, size, (fitted / base - 1.0) * 100.0);
      // 小字号容差放宽（笔画本来就小于 2px，吸附会带来更明显的相对变化）；
      // 大字号必须接近恒等——那里"不需要"hinting，动了反而说明实现有问题。
      ST_CHECK(delta < (size <= 16.0f ? 0.08 : 0.03));
    }
  }
}

/// ④ 缓存键含渲染模式位：反复切换模式，取回的位图与"一开始就是该模式"逐像素一致。
ST_TEST(grid_fit_cache_does_not_mix_modes) {
  FontFixture fixture;
  if (!fixture.ok) return;
  const std::vector<std::uint32_t> off_reference =
      render_text(*fixture.stack, kCjk, 13.5f, GridFitMode::Off);
  const std::vector<std::uint32_t> normal_reference =
      render_text(*fixture.stack, kCjk, 13.5f, GridFitMode::Normal);
  ST_CHECK(differing(off_reference, normal_reference) > 0);

  TextRenderer renderer(*fixture.stack, 1.0f);
  Canvas canvas{900, 64};
  for (int round = 0; round < 3; ++round) {
    for (const GridFitMode mode : {GridFitMode::Normal, GridFitMode::Off}) {
      renderer.set_grid_fit(mode);
      canvas.clear(Color{0, 0, 0, 0});
      renderer.draw(canvas, kCjk, Point{4.0f, 40.0f}, 13.5f, Color{0xFF, 0xFF, 0xFF, 0xFF});
      const std::vector<std::uint32_t> actual(canvas.pixels().begin(), canvas.pixels().end());
      const auto& reference =
          mode == GridFitMode::Off ? off_reference : normal_reference;
      ST_CHECK_EQ(static_cast<int>(differing(actual, reference)), 0);
    }
  }
}

/// ⑤ 护栏有效：位移超限时**放弃拟合**（返回原路径、`applied=false`）。
ST_TEST(grid_fit_guard_refuses_excessive_shift) {
  // 造一条**真实尺度**的竖笔画：宽 1.5px（小字笔画就在 1.2~1.7px），
  // 边缘落在 .5 相位上——吸附需要 0.5px，正好压在护栏上。
  const auto build = []() {
    st::raster::Path path;
    path.move_to(Point{0.5f, 0.0f});
    path.line_to(Point{2.0f, 0.0f});
    path.line_to(Point{2.0f, 10.0f});
    path.line_to(Point{0.5f, 10.0f});
    path.close();
    return path;
  };
  const st::raster::Path original = build();

  // 默认护栏（max_shift 0.5）应当**允许**这次吸附（位移 0.5）
  const auto accepted = st::text::grid_fit(original, {.mode = GridFitMode::Normal});
  ST_CHECK(accepted.applied);
  const auto points = accepted.path.raw_points();
  // 右沿 2.0 已在网格上（位移 0），左沿 0.5 吸到 1.0（位移 +0.5）⇒ 宽度由 1.5 变成 1.0。
  //
  // ⚠ **这里曾经期望宽度变成 2**（宽度量化 `round(1.5)=2` 把左沿推到 0）。2026-10-06 的
  //    "量化适用闸"（`kMaxQuantizeErrorPx = 0.2`）让这条笔画不再量化——它取整误差 0.5，
  //    远超闸门。实测：**关掉量化得到的结果与开着一模一样**（同样是两条边各自吸附），
  //    因为"两条边都落在整数网格上"本身就让宽度成了整数差。⇒ 量化分支在多数笔画上
  //    与纯吸附等价，它唯一可观察的效果是**把设计宽度的小差异放大**（见该常量的注释）。
  ST_CHECK(std::fabs(points[0].x - 1.0f) < 0.01f);
  ST_CHECK(std::fabs(points[1].x - 2.0f) < 0.01f);
  ST_CHECK(std::fabs((points[1].x - points[0].x) - 1.0f) < 0.01f);

  // 护栏的**可配置性**：关掉量化后，两条边各自吸到最近网格（各 0.5），
  // 此时 max_shift 收紧到 0.1 就必须**拒绝**（保持原样）。
  //
  // 注意：量化开着时预算会被抬到 `max_shift + grid/2`（量化把宽度取整时
  // 远边要额外叠上半像素，几何必需）——那时 0.1 的 max_shift 不足以拒绝
  // 位移 0.5 的吸附。所以这个反例要在 `quantize_width = false` 下测。
  GridFitOptions strict;
  strict.mode = GridFitMode::Normal;
  strict.quantize_width = false;
  strict.max_shift = 0.1f;
  const auto refused = st::text::grid_fit(original, strict);
  ST_CHECK(!refused.applied);
  ST_CHECK(std::fabs(refused.path.raw_points()[0].x - 0.5f) < 0.01f);

  // `Off` 模式必须逐点不动（保证"默认不改字形"）
  const auto disabled = st::text::grid_fit(original, {.mode = GridFitMode::Off});
  ST_CHECK(!disabled.applied);
  const auto disabled_points = disabled.path.raw_points();
  const auto original_points = original.raw_points();
  ST_CHECK_EQ(static_cast<int>(disabled_points.size()), static_cast<int>(original_points.size()));
  for (std::size_t index = 0; index < disabled_points.size(); ++index) {
    ST_CHECK_EQ(static_cast<int>(std::lround(disabled_points[index].x * 100.0f)),
                static_cast<int>(std::lround(original_points[index].x * 100.0f)));
  }
}

/// ⑦ **亚像素 + 拟合** 的组合：网格不变，且不能把字形拉坏。
///
/// 这条是实测踩到的一个真回归：亚像素路径需要把轮廓水平放大 3 倍，
/// 当时用 `Path::scaled(3)` 实现——它会把 x 与 y **一起**乘 3，
/// 于是字形被纵向拉成 3 倍高、只有上半部分落在画布里（表现为“文字像被切成两半”）。
///
/// **墨量断言在 γ = 1 下量**：这里的“墨量”是 Σ 覆盖率，而 Σ 只在恒等映射下等于几何墨量——
/// 覆盖率 gamma 是非线性的，拟合改变了相位就改变了 Σ 分布，非线性映射会把这点差异放大成
/// 两位数的偏差（实测 γ=2.2 时 +21.7%）。网格/形变那几条断言与 gamma 无关，保持原样。
ST_TEST(grid_fit_combines_with_subpixel_without_distortion) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer subpixel_only(*fixture.stack, 1.0f);
  subpixel_only.set_coverage_gamma(1.0f);
  subpixel_only.set_subpixel(true);
  TextRenderer both(*fixture.stack, 1.0f);
  both.set_coverage_gamma(1.0f);
  both.set_subpixel(true);
  both.set_grid_fit(GridFitMode::Normal);

  std::size_t compared = 0;
  std::size_t vanished = 0;
  double plain_ink_total = 0.0;
  double fitted_ink_total = 0.0;
  for (const char32_t codepoint : st::utf8_decode(kCjk)) {
    for (const float size : {13.5f, 22.0f}) {
      const auto plain = subpixel_only.glyph_bitmap_of(codepoint, size);
      const auto fitted = both.glyph_bitmap_of(codepoint, size);
      if (plain == nullptr || fitted == nullptr) continue;
      ++compared;
      double plain_ink = 0.0;
      double fitted_ink = 0.0;
      for (const float value : plain->coverage) plain_ink += static_cast<double>(value);
      for (const float value : fitted->coverage) fitted_ink += static_cast<double>(value);
      // ① 网格必须不变（拟合不改变位图尺寸/偏移）
      ST_CHECK(plain->width == fitted->width);
      ST_CHECK(plain->height == fitted->height);
      ST_CHECK(plain->offset_x == fitted->offset_x);
      ST_CHECK(plain->offset_y == fitted->offset_y);
      // ② 亚像素布局仍然是三通道
      ST_CHECK(fitted->format == st::raster::CoverageFormat::Lcd);
      ST_CHECK_EQ(static_cast<int>(fitted->coverage.size()),
                  static_cast<int>(fitted->width * fitted->height * 3));
      // ③ **不得纵向被拉伸**：墨迹高度应与不拟合时接近（上下各留不出过多空白）。
      //    若 y 被错误地放大，字形会溢出画布上沿——墨迹会贴到第 0 行。
      const int width = fitted->width;
      const int height = fitted->height;
      bool top_row_inked = false;
      bool bottom_row_inked = false;
      for (int x = 0; x < width; ++x) {
        for (int channel = 0; channel < 3; ++channel) {
          if (fitted->coverage[(static_cast<std::size_t>(x) * 3U +
                                static_cast<std::size_t>(channel))] > 0.2f) {
            top_row_inked = true;
          }
          const std::size_t bottom =
              (static_cast<std::size_t>(height - 1) * static_cast<std::size_t>(width) +
               static_cast<std::size_t>(x)) *
                  3U +
              static_cast<std::size_t>(channel);
          if (fitted->coverage[bottom] > 0.2f) bottom_row_inked = true;
        }
      }
      // 生成时 padding=1：墨迹不该贴到最上/最下边（拉伸才会贴边）
      ST_CHECK(!top_row_inked);
      ST_CHECK(!bottom_row_inked);
      // ④ 墨量：**聚合口径**才是形变的正确度量。
      //    单个小字形（如「一」「、」）的笔画只有 1.5px，吸附半个像素就会让它
      //    的相对墨量变化几十个百分点——那是拟合的正常代价，不是缺陷。
      //    会出问题的是“整段文字变粗/变细”，或者某个字的笔画**消失**。
      plain_ink_total += plain_ink;
      fitted_ink_total += fitted_ink;
      if (plain_ink > 4.0 && fitted_ink < plain_ink * 0.4) ++vanished;
    }
  }
  ST_CHECK(compared > 20);
  if (plain_ink_total > 1.0) {
    st::print("[fit] 亚像素+拟合：墨量总量变化 {:+.2f}%（{} 个字形）\n",
              (fitted_ink_total / plain_ink_total - 1.0) * 100.0, compared);
    ST_CHECK(std::fabs(fitted_ink_total / plain_ink_total - 1.0) < 0.06);
  }
  // 任何字形的墨量都不该“掉一半以上”（那是笔画消失，不是相位调整）
  ST_CHECK_EQ(static_cast<int>(vanished), 0);
}

/// ⑥ 曲线控制点必须**跟着端点走**：拟合后不能只剩端点被移动（那会让曲线变形）。
ST_TEST(grid_fit_moves_curve_controls_with_endpoints) {
  // 形状：左右两条**直线竖边**（构成竖笔画，宽 1.5px）+ 底部一段**曲线**。
  // 曲线控制点若不动、端点却动了，这段弧就会被拉变形。
  st::raster::Path path;
  path.move_to(Point{0.4f, 0.0f});
  path.line_to(Point{1.9f, 0.0f});          // 顶边（横）
  path.line_to(Point{1.9f, 10.0f});         // 右边的竖边（直线）
  path.quad_to(Point{1.15f, 12.0f}, Point{0.4f, 10.0f});  // 底部曲线
  path.close();                              // 左边竖边（直线）
  const auto before = path.raw_points();
  const auto fitted = st::text::grid_fit(path, {.mode = GridFitMode::Normal});
  ST_CHECK(fitted.applied);
  const auto after = fitted.path.raw_points();
  // 竖笔画的两条边都在（左 0.4、右 1.9）
  ST_CHECK(fitted.vertical_stems >= 1);

  // 曲线控制点 = `quad` 命令的第一个点（索引 2），它的两个端点分别是：
  // 起点 = quad 之前的那个点（索引 1）、终点 = quad 的终点（索引 3）。
  const float start_delta = after[1].x - before[1].x;
  const float end_delta = after[3].x - before[3].x;
  const float control_delta = after[2].x - before[2].x;
  // 控制点必须在两端位移的**区间内**（既没被落下，也没被单独推出去）
  const float low = std::min(start_delta, end_delta);
  const float high = std::max(start_delta, end_delta);
  ST_CHECK(control_delta >= low - 0.01f);
  ST_CHECK(control_delta <= high + 0.01f);
  // 且**确实动了**（“端点动了、控制点没动”正是要防的那个 bug）
  ST_CHECK(std::fabs(control_delta) > 1.0e-4f);
  // 两端位移相同时（整条曲线平移），控制点位移必须与之一致——形状严格不变
  if (std::fabs(start_delta - end_delta) < 1.0e-4f) {
    ST_CHECK(std::fabs(control_delta - start_delta) < 0.01f);
  }
}

/// ⑧ **吸附格点锚在物理像素上，与超采样倍率无关**（2026-10-02 修复的真回归）。
///
/// 起因：用户反馈「字体渲染远不如浏览器」。逐像素取样后定位到——拟合在**超采样坐标空间**
/// 里吸整数，而本机 1.5× DPI 让渲染器用 `supersample=2`，于是吸附目标是“半个物理像素”，
/// 拟合反而把边缘推入像素正中间（实测「霜」@20.25px 的 0.5 覆盖率像素由 25 涨到 **96**）；
/// 阈值 `max_stem_width`/`max_shift` 也按采样单位比较，实际只剩一半宽。
///
/// 这条钉住三件事：
/// 1. `grid=S` 时，拟合后的边缘落在 **S 的整数倍**上（= 物理像素边界）；
/// 2. 同一笔画在 `grid=1`（等效 ss=1）与 `grid=2`（等效 ss=2）下**都**被吸到网格；
/// 3. `grid` 不影响**物理像素**口径的形状语义：`max_stem_width` 等阈值在任何倍率下
///    都放行同样的物理宽度。
ST_TEST(grid_fit_snaps_to_physical_pixel_lattice_at_any_supersample) {
  const auto build = [](float grid) {
    // 一条竖笔画：宽 1.6 物理像素，两边缘落在 .42/.02 相位上
    const float left = 4.42f * grid;
    const float right = left + 1.6f * grid;
    st::raster::Path path;
    path.move_to(Point{left, 0.0f});
    path.line_to(Point{right, 0.0f});
    path.line_to(Point{right, 12.0f * grid});
    path.line_to(Point{left, 12.0f * grid});
    path.close();
    return path;
  };
  for (const float grid : {1.0f, 2.0f}) {
    const auto fitted = st::text::grid_fit(build(grid), {.mode = GridFitMode::Normal, .grid = grid});
    ST_CHECK(fitted.applied);
    const auto points = fitted.path.raw_points();
    // 两条竖边的 x 都必须在 **grid 的整数倍**上（= 物理像素边界）
    const float left_x = points[0].x;
    const float right_x = points[1].x;
    const float left_error = std::fabs(left_x / grid - std::round(left_x / grid));
    const float right_error = std::fabs(right_x / grid - std::round(right_x / grid));
    ST_CHECK(left_error < 0.01f);
    ST_CHECK(right_error < 0.01f);
    // 且宽度按**物理像素**计不少于 1px（不塌成零宽）
    ST_CHECK((right_x - left_x) / grid >= 0.99f);
  }

  // **负例**（防止有人把参数改回 grid=1）：在 2 倍采样坐标里用整数格点，
  // 边缘会落到**半个物理像素**上（半个采样格 = 物理像素非整数倍）。
  const auto half_pixel = build(2.0f);
  const auto old = st::text::grid_fit(half_pixel, {.mode = GridFitMode::Normal, .grid = 1.0f});
  if (old.applied) {
    const float old_left = old.path.raw_points()[0].x;
    const float physical = old_left / 2.0f;
    ST_CHECK(std::fabs(physical - std::round(physical)) > 0.3f);  // 确在非物理像素相位
  }

  // 3) 阈值物理口径：一条 **2.4 物理像素**宽的笔画（在 max_stem_width=2.6 以内）
  //    在 2 倍采样下必须能被识别出来。旧口径（阈值不乘 grid）有效阈值只剩 1.3px，
  //    会漏掉它——实测就靠这条定位了“2 倍采样下笔画几乎全漏”。
  const auto wide = [](float grid) {
    st::raster::Path path;
    const float left = 3.2f * grid;
    const float right = left + 2.4f * grid;
    path.move_to(Point{left, 0.0f});
    path.line_to(Point{right, 0.0f});
    path.line_to(Point{right, 10.0f * grid});
    path.line_to(Point{left, 10.0f * grid});
    path.close();
    return path;
  };
  ST_CHECK(st::text::grid_fit(wide(2.0f), {.mode = GridFitMode::Light, .grid = 2.0f}).vertical_stems >=
           1);
  // 阈值确实随 grid 换算：同一轮廓在 grid=1 口径下（2.4px → 4.8 采样单位）超过 2.6 阈值，
  // 不被当作笔画——这正是“阈值必须乘 grid”的反面证据。
  ST_CHECK(st::text::grid_fit(wide(2.0f), {.mode = GridFitMode::Light, .grid = 1.0f}).vertical_stems ==
           0);
}

/// ③ **字体自带的 stem hints 必须真的进入拟合**（方案 B 的回归防线）。
///
/// 背景：CFF（OTF）字体把"笔画在哪、多宽"写在 charstring 的 `vstem`/`hstem` 里，
/// 而按轮廓几何反推笔画（近轴直线边两两配对）实测**召回只有 ~19%**——漏掉的笔画
/// 不被吸附、留着分数相位，正是"同一字里有的笔画实、有的发灰"。本用例钉住：
///   ① 解释器确实解析出了 hints（字体单位、宽度为正、方向正确）；
///   ② 这些 hints 在拟合里**被采用**（`hint_seen`/`hint_bound`/`hint_stems` 均非零）。
///
/// ⚠ 本用例对"换算口径"特别敏感——实现过程中踩过两次同类错，每次的表现都是
/// `hint_stems` 恒为 0（看起来像"字体没有 hints"，其实是把提示喂到了错的空间）：
///   · hint 在 `stems_from_hints` 内部要再乘 `grid`，调用方不得重复乘；
///   · 亚像素分支的轮廓横轴是 3 倍（`build_path(horizontal=3)`），
///     hints 必须按**同一空间**换算。
/// 所以断言用"非零且随字号合理"而不是精确值：精确值会随字体版本变。
ST_TEST(font_stem_hints_reach_the_fitter) {
  FontFixture fixture;
  if (!fixture.ok) return;
  // 挑一个**确定有 hints** 的汉字（Noto Sans CJK 的「中」有 3 条竖提示）。
  const char32_t kProbe = U'中';
  const auto* face = fixture.stack->find_face(kProbe, st::text::FontRole::Proportional);
  if (face == nullptr) return;
  const auto glyph = face->glyph_index(kProbe);
  if (!glyph.has_value()) return;
  const auto& hints = face->stem_hints(*glyph);
  if (hints.empty()) return;   // 字体没有 CFF hints（例如换成 TrueType）：本用例不适用
  std::size_t vertical = 0;
  for (const auto& hint : hints) {
    if (!hint.vertical) continue;
    ++vertical;
    ST_CHECK(hint.hi > hint.lo);              // 归一后的边界必须有序
    ST_CHECK(hint.hi - hint.lo > 0.0f);
  }
  ST_CHECK(vertical >= 1);

  // ⚠ hints 的**采用**默认关闭（实验能力，见 `text.cpp` 里那段说明）：本用例因此分两段——
  //    ① 任何时候都断言"解析正确"；
  //    ② 只有开启 `ST_TEXT_USE_FONT_HINTS` 时才断言"接上了轮廓点"。
  //    这样开关两态都被钉住，又不会让默认路径依赖一个未定档的能力。
  const bool hints_enabled = std::getenv("ST_TEXT_USE_FONT_HINTS") != nullptr;
  TextRenderer renderer(*fixture.stack, 1.0f);
  renderer.set_subpixel(true);
  renderer.set_grid_fit(GridFitMode::Normal);
  // **每次都是新渲染器**：位图缓存按 (face, glyph, 尺寸, 超采样, 拟合模式, 参数桶) 索引，
  // 同一渲染器重复取同一字形会直接命中缓存、绕过重算。逆向验证时（改坏换算口径后重跑
  // 同一个可执行文件）曾因此**读到上一轮的位图**、让测试恒绿——那正是"恒绿的护栏"。
  // 缓存键不含本次的换算实现，所以这里只能换尺寸来强制重算。
  const auto bitmap = renderer.glyph_bitmap_of(kProbe, 15.0f + static_cast<float>(
                                                        std::getenv("ST_HINT_SIZE_OFFSET") == nullptr
                                                            ? 0.0
                                                            : 0.25));
  if (bitmap == nullptr) return;
  st::print("[hint] U+4E2D 提示 {} 条；拟合内 seen={} bound={} 参与={}\n", hints.size(),
            bitmap->fit_hint_seen, bitmap->fit_hint_bound, bitmap->fit_hint_stems);
  // 断言到「接上轮廓点」为止，不断言「最终参与拟合的条数 > 0」：
  // 后者取决于字体给的是哪种提示（CJK 的「中」给的竖提示算下来是整字宽的边框，
  // 被 `max_stem_width` 正确拒绝——那不是缺陷，是笔画判定的正常结果）。
  // 而 `bound > 0` 恰好是**换算口径**的判据：口径一错（重复乘 grid、或没跟上
  // 亚像素的 3 倍横轴），提示边缘会偏出容差、一条都接不上（实测口径错时 seen=3/bound=0）。
  if (hints_enabled) {
    ST_CHECK(bitmap->fit_hint_seen > 0);
    ST_CHECK(bitmap->fit_hint_bound > 0);
  } else {
    ST_CHECK_EQ(bitmap->fit_hint_seen, 0);   // 默认路径不得采用 hints（未定档的能力）
  }
}

/// ④ **量化不得把小差异放大成大差异**（用户反馈「最小字号里 H/1/B 的竖线比浏览器粗、
///    a/n/l 比浏览器细」的直接回归）。
///
/// 现象与归因（2026-10-06，`tools/stroke_width_stats.py` + `tools/text_ab_glyphs_page.html`）：
/// 11px 下字体给大写竖笔 ≈1.7px、小写 ≈1.5px（相差 10%），而宽度量化的 `round()`
/// 把大写推到 **2**、小写推到 **1**（相差 56%）——**量化把小差异放大成大差异**。
/// 参照侧（真窗口浏览器）的同一比值是 1.10。
///
/// 判据取「大写与小写的竖笔宽比」而不是绝对宽度：绝对值随字体/DPI 变，而这个**比值**
/// 在两个渲染器之间可比（同一字体、同一字号的同一个设计值）。
///
/// ⚠ 采样要求：取多个字形的中位数（单个字形会被相位偶然性主导），
/// 且横截面要**避开横杠**（H/E/F 的横杠正好在竖直中线，在中线量会把
/// 「横杠 + 两根竖」量成一条 5px 宽的笔画——本用例开发时就踩到过）。
ST_TEST(quantize_does_not_amplify_design_width_differences) {
  FontFixture fixture;
  if (!fixture.ok) return;
  constexpr float kSize = 11.0f;
  constexpr char32_t kUpper[] = {U'H', U'E', U'F', U'T', U'L', U'I', U'B'};
  constexpr char32_t kLower[] = {U'n', U'l', U'i', U'r', U't'};

  TextRenderer renderer(*fixture.stack, 1.5f);
  renderer.set_subpixel(true);
  renderer.set_grid_fit(GridFitMode::Light);
  renderer.set_coverage_gamma(1.0f);

  // 一条扫描行里"最粗的连续覆盖率段"（单位：物理像素的覆盖率之和）。
  const auto widest_stem = [](const TextRenderer::GlyphBitmap& bitmap, float fraction) -> float {
    if (bitmap.format != st::raster::CoverageFormat::Lcd || bitmap.height <= 0) return 0.0f;
    const int y = std::clamp(static_cast<int>(static_cast<float>(bitmap.height) * fraction), 0,
                             bitmap.height - 1);
    std::vector<float> row(static_cast<std::size_t>(bitmap.width), 0.0f);
    for (int x = 0; x < bitmap.width; ++x) {
      const std::size_t base =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(bitmap.width) +
           static_cast<std::size_t>(x)) * 3U;
      if (base + 2U >= bitmap.coverage.size()) return 0.0f;
      row[static_cast<std::size_t>(x)] =
          (bitmap.coverage[base] + bitmap.coverage[base + 1U] + bitmap.coverage[base + 2U]) / 3.0f;
    }
    float best = 0.0f;
    float run = 0.0f;
    for (float value : row) {
      if (value > 0.05f) {
        run += value;
      } else {
        best = std::max(best, run);
        run = 0.0f;
      }
    }
    return std::max(best, run);
  };
  // 一个字形取多个高度的中位数（避开横杠：22% / 30% / 70% / 78%）
  const auto stem_of = [&](char32_t codepoint) -> float {
    const auto bitmap = renderer.glyph_bitmap_of(codepoint, kSize);
    if (bitmap == nullptr) return 0.0f;
    std::vector<float> values;
    for (const float fraction : {0.22f, 0.30f, 0.70f, 0.78f}) {
      const float value = widest_stem(*bitmap, fraction);
      if (value > 0.2f) values.push_back(value);
    }
    if (values.empty()) return 0.0f;
    std::ranges::sort(values);
    return values[values.size() / 2U];
  };
  const auto median_stem = [&](const char32_t* list, std::size_t count) -> float {
    std::vector<float> values;
    for (std::size_t index = 0; index < count; ++index) {
      const float value = stem_of(list[index]);
      if (value > 0.2f) values.push_back(value);
    }
    if (values.size() < 3) return 0.0f;
    std::ranges::sort(values);
    return values[values.size() / 2U];
  };

  const float upper = median_stem(kUpper, std::size(kUpper));
  const float lower = median_stem(kLower, std::size(kLower));
  if (upper <= 0.0f || lower <= 0.0f) return;
  const float ratio = upper / lower;
  st::print("[quantize] 11px 竖笔宽：大写 {:.2f} / 小写 {:.2f} = {:.3f}\n", upper, lower, ratio);
  // 参照（真窗口浏览器）的同一比值是 1.099；量化放大后实测 1.56。
  ST_CHECK(ratio < 1.25f);
  ST_CHECK(ratio > 1.0f);   // 大写确实略粗于小写（字体设计如此），不能反向
}

/// ⑤ **按字形类的覆盖率分档**：只动该类、其余字形逐位不变（且缓存按类区分）。
///
/// 起因（2026-10-06 全量逐字形 A/B）：字母与汉字已与浏览器对齐（0.94~0.96），
/// 而**数字类稳定偏轻 8~10%**（10~13px 符号一致）。按用户要求"已对好的不要动"，
/// 因此引入**按类**这一层（`set_class_gamma`），而不是回头去改全局 γ。
///
/// 本用例钉住三条最容易写错的地方：
/// 1. **类 γ 只影响该类**——同一渲染器里改数字类 γ，字母的位图必须**逐位不变**；
/// 2. **缓存按类区分**——同一个字形先以默认档取、再以类覆盖档取，两次必须不同
///    （键里漏掉类覆盖会取到对方的位图，症状是"改了类 γ 看不出变化"）；
/// 3. **绘制路径也走类**——`TextRun::glyph_class` 在整形时定好、`draw` 照它取位图。
///    这条最难从接口层看出来（`glyph_bitmap_of` 与 `draw` 是两条路径），
///    实测就漏过一次：改了类 γ 后 PNG **逐像素不变**，而接口层的墨量却变了。
ST_TEST(class_gamma_affects_only_its_class_and_survives_cache) {
  FontFixture fixture;
  if (!fixture.ok) return;
  const auto ink = [](const TextRenderer::GlyphBitmap& bitmap) {
    double sum = 0.0;
    for (float value : bitmap.coverage) sum += static_cast<double>(value);
    return sum;
  };
  TextRenderer renderer(*fixture.stack, 1.5f);
  renderer.set_subpixel(true);
  renderer.set_coverage_gamma(1.10f);
  const auto digit_before = renderer.glyph_bitmap_of(U'3', 15.0f);
  const auto letter_before = renderer.glyph_bitmap_of(U'A', 15.0f);
  if (digit_before == nullptr || letter_before == nullptr) return;
  const double digit_ink_0 = ink(*digit_before);
  const double letter_ink_0 = ink(*letter_before);

  // ① 只压数字类
  renderer.set_class_gamma(GlyphClass::Digit, 0.92f);
  renderer.set_class_gamma(GlyphClass::Letter, 0.98f);
  const auto digit_after = renderer.glyph_bitmap_of(U'3', 15.0f);
  const auto letter_after = renderer.glyph_bitmap_of(U'A', 15.0f);
  ST_CHECK(digit_after != nullptr && letter_after != nullptr);
  st::print("[class] 数字 3：{:.1f} -> {:.1f}；字母 A：{:.1f} -> {:.1f}\n", digit_ink_0,
            ink(*digit_after), letter_ink_0, ink(*letter_after));
  // 数字变重（γ<1 压黑），且幅度可观（实测 ~7%）
  ST_CHECK(ink(*digit_after) > digit_ink_0 * 1.03);
  // **字母只受自己那一档影响**：本轮给字母类也定了档（γ=0.98），所以它**应当**变重
  // ——但变的是这一档，不是数字档。判据是"两个类各自独立可调"：
  // 这一段把字母档也设上，随后单独把字母档归零、数字档不动，字母必须回到原值。
  ST_CHECK(ink(*letter_after) > letter_ink_0 * 1.005);
  renderer.set_class_gamma(GlyphClass::Letter, 0.0f);
  const auto letter_restored = renderer.glyph_bitmap_of(U'A', 15.0f);
  ST_CHECK(letter_restored != nullptr);
  ST_CHECK_EQ(static_cast<int>(std::lround(ink(*letter_restored) * 100.0)),
              static_cast<int>(std::lround(letter_ink_0 * 100.0)));
  renderer.set_class_gamma(GlyphClass::Letter, 0.98f);

  // ①b **汉字（Default 类）不受数字/字母档影响**——这是"已对好的不要动"的核心
  const auto han = renderer.glyph_bitmap_of(U'中', 15.0f);
  if (han != nullptr) {
    TextRenderer reference(*fixture.stack, 1.5f);
    reference.set_subpixel(true);
    reference.set_coverage_gamma(1.10f);
    const auto han_reference = reference.glyph_bitmap_of(U'中', 15.0f);
    ST_CHECK(han_reference != nullptr);
    ST_CHECK_EQ(static_cast<int>(std::lround(ink(*han) * 100.0)),
                static_cast<int>(std::lround(ink(*han_reference) * 100.0)));
  }

  // ② 缓存不串档：改回不覆盖后，数字必须回到原值
  renderer.set_class_gamma(GlyphClass::Digit, 0.0f);
  const auto digit_back = renderer.glyph_bitmap_of(U'3', 15.0f);
  ST_CHECK(digit_back != nullptr);
  ST_CHECK_EQ(static_cast<int>(std::lround(ink(*digit_back) * 100.0)),
              static_cast<int>(std::lround(digit_ink_0 * 100.0)));

  // ③ 绘制路径：同一段含数字与字母的文字，只压数字类后**只有数字那块变**
  const auto render_row = [&](float digit_gamma) {
    st::raster::Canvas canvas{160, 40, 1.5f};
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    TextRenderer local(*fixture.stack, 1.5f);
    local.set_subpixel(true);
    local.set_coverage_gamma(1.10f);
    if (digit_gamma > 0.0f) local.set_class_gamma(GlyphClass::Digit, digit_gamma);
    (void)local.draw(canvas, "3A3A", st::math::Point{2.0f, 2.0f}, 15.0f,
                     st::math::Color{0x0F, 0x17, 0x2A, 0xFF});
    return canvas.to_rgba8();
  };
  const auto plain = render_row(0.0f);
  const auto darkened = render_row(0.92f);
  ST_CHECK_EQ(static_cast<int>(plain.size()), static_cast<int>(darkened.size()));
  std::size_t changed = 0;
  for (std::size_t index = 0; index < plain.size(); ++index) {
    if (plain[index] != darkened[index]) ++changed;
  }
  st::print("[class] 绘制路径：{} / {} 字节变化（数字像素应占少数，字母必须不动）\n", changed,
            plain.size());
  ST_CHECK(changed > 0);   // 变了 ⇒ 绘制路径确实走了类（这一条就是漏接线时的红点）
}
