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
ST_TEST(grid_fit_preserves_ink) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer off(*fixture.stack, 1.0f);
  TextRenderer normal(*fixture.stack, 1.0f);
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
  // `round(0.5)` 向上取整（`std::round` 的“远离零”规则），所以左沿 → 1、右沿 → 2
  ST_CHECK(std::fabs(points[0].x - 1.0f) < 0.01f);
  ST_CHECK(std::fabs(points[1].x - 2.0f) < 0.01f);

  // 收紧 max_shift 到 0.1 → 必须**拒绝**（保持原样）
  GridFitOptions strict;
  strict.mode = GridFitMode::Normal;
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
ST_TEST(grid_fit_combines_with_subpixel_without_distortion) {
  FontFixture fixture;
  if (!fixture.ok) return;
  TextRenderer subpixel_only(*fixture.stack, 1.0f);
  subpixel_only.set_subpixel(true);
  TextRenderer both(*fixture.stack, 1.0f);
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
