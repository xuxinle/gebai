// 绘制优化的**像素等价性**门禁（2026-10 性能轮）。
//
// ## 它守的是什么
//
// 本轮把两处绘制换成了更省的实现（见 `docs/PAINT_DIAGNOSIS.md`）：
//   ① 卡片边框：`stroke_path(圆角矩形)` → `fill_path(make_rounded_border_ring(...))`
//      —— 6.9 ms/帧 → ≈2.5 ms/帧；
//   ② 两层投影：两次 `draw_shadow` → `draw_shadow_layered`（预合成一张缓存贴图）
//      —— 14.0 ms/帧 → ≈6.9 ms/帧。
//
// 两者**都不逐像素相同**（抗锯齿逼近不同），因此不能靠"截图哈希相等"来守。
// 本文件把可接受的口径显式写下来，让"为了提速把边框画错"这类事故当场红灯：
//
// | 断言 | 口径 | 为什么是这个口径 |
// |---|---|---|
// | 覆盖范围 | 边框仍然完全落在 `rect` 之内、厚度等于 `width` | 走位的边框是最典型的退化 |
// | 边缘等价 | 与旧实现比，**多数像素完全一致**，其余只差抗锯齿 | 0.1% 级差异是逼近差异，不是错位 |
// | 阴影等价 | 与两次 `draw_shadow` 在**铺上不透明底色后**逐像素一致 | `src-over` 结合律保证：合并两层再贴 ≡ 依次贴两次 |
//
// 阴影那条是本轮最关键的**正确性论证**：贴图存的是"阴影自身的预乘 RGBA"
// （从全透明开始合成），不是"合成到背景上的结果"——后者会把底色烘进贴图，
// 元素底下一有渐变/别的元素就错。所以断言要**在非纯色底上**也成立。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/raster/surface.hpp"
#include "st/test/test.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::raster::Canvas;
using st::raster::Paint;
using st::raster::Path;

inline constexpr int kWidth = 320;
inline constexpr int kHeight = 240;

[[nodiscard]] auto make_canvas(float scale = 1.0f) -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, scale);
  canvas.clear(Color::rgb(0xF7, 0xF8, 0xFA));
  return canvas;
}

struct PixelDiff {
  std::size_t differing{0};
  std::size_t total{0};
  int max_delta{0};
  int worst_x{0};
  int worst_y{0};
  [[nodiscard]] auto ratio() const -> double {
    return total == 0 ? 0.0 : static_cast<double>(differing) / static_cast<double>(total);
  }
};

[[nodiscard]] auto diff(const Canvas& left, const Canvas& right) -> PixelDiff {
  PixelDiff result;
  for (int y = 0; y < left.physical_height(); ++y) {
    for (int x = 0; x < left.physical_width(); ++x) {
      const Color a = left.pixel_at(x, y);
      const Color b = right.pixel_at(x, y);
      const int delta = std::max({std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
                                  std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
                                  std::abs(static_cast<int>(a.b) - static_cast<int>(b.b)),
                                  std::abs(static_cast<int>(a.a) - static_cast<int>(b.a))});
      ++result.total;
      if (delta > 0) ++result.differing;
      if (delta > result.max_delta) {
        result.max_delta = delta;
        result.worst_x = x;
        result.worst_y = y;
      }
    }
  }
  return result;
}

/// 旧实现（描边）——**保留一份供对照**，不要因为"代码里没人用"就删：
/// 它是本文件所有边框断言的参考基准。
void draw_border_by_stroke(Canvas& canvas, st::math::Rect rect, float radius, Color color,
                           float width) {
  const float half = width * 0.5f;
  Path outline;
  outline.add_rounded_rect(rect.inset(st::math::Insets::all(half)),
                           radius > half ? radius - half : 0.0f);
  canvas.stroke_path(outline, Paint::solid(color), width);
}

void draw_border_by_ring(Canvas& canvas, st::math::Rect rect, float radius, Color color,
                         float width) {
  canvas.fill_path(st::raster::make_rounded_border_ring(rect, radius, width),
                   Paint::solid(color));
}

/// 边框墨量（与背景的通道差之和）——用来断言"厚度没变"：
/// 环形与描边的抗锯齿分布不同，但**总墨量应当接近**（两者覆盖同一块几何区域）。
[[nodiscard]] auto border_ink(const Canvas& canvas, const Color& background) -> double {
  double total = 0.0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      total += std::abs(static_cast<double>(pixel.r) - static_cast<double>(background.r));
      total += std::abs(static_cast<double>(pixel.g) - static_cast<double>(background.g));
      total += std::abs(static_cast<double>(pixel.b) - static_cast<double>(background.b));
    }
  }
  return total;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// ① 边框：环形填充 vs 描边
// ————————————————————————————————————————————————————————————————————————————

/// 覆盖范围：环形边框**不越界**，且确实画出了东西（两者的基本正确性）。
ST_TEST(border_ring_covers_same_area_as_stroke) {
  const Color background = Color::rgb(0xF7, 0xF8, 0xFA);
  const Color border = Color::rgb(0xD3, 0xDC, 0xE9);
  const st::math::Rect box{60.0f, 40.0f, 140.0f, 48.0f};
  constexpr float kRadius = 12.0f;
  constexpr float kBorderWidth = 1.0f;

  Canvas ring_canvas = make_canvas();
  draw_border_by_ring(ring_canvas, box, kRadius, border, kBorderWidth);
  Canvas stroke_canvas = make_canvas();
  draw_border_by_stroke(stroke_canvas, box, kRadius, border, kBorderWidth);

  const double ring_ink = border_ink(ring_canvas, background);
  const double stroke_ink = border_ink(stroke_canvas, background);
  st::print("[border-ink] 环形 {:.0f} · 描边 {:.0f} · 比值 {:.3f}\n", ring_ink, stroke_ink,
            ring_ink / stroke_ink);
  ST_CHECK(ring_ink > 0.0);
  ST_CHECK(stroke_ink > 0.0);
  // 两者覆盖同一块几何：墨量差异必须很小（不同抗锯齿逼近的固有差别）
  ST_CHECK(ring_ink > stroke_ink * 0.9);
  ST_CHECK(ring_ink < stroke_ink * 1.1);

  // 越界检查：环形边框绝不应该画到 `box` 之外（外扩一个像素容抗锯齿）
  const st::math::IntRect outside = ring_canvas.to_physical(box.inflate(1.0f)).intersect(
      st::math::IntRect{0, 0, kWidth, kHeight});
  bool outside_clean = true;
  for (int y = 0; y < ring_canvas.physical_height(); ++y) {
    for (int x = 0; x < ring_canvas.physical_width(); ++x) {
      const bool inside = x >= outside.x && x < outside.right() && y >= outside.y &&
                          y < outside.bottom();
      if (inside) continue;
      if (ring_canvas.pixel_at(x, y) != background) outside_clean = false;
    }
  }
  ST_CHECK(outside_clean);
}

/// 边缘等价：与描边比，**绝大多数像素完全一致**，其余差异只来自抗锯齿逼近。
ST_TEST(border_ring_edge_matches_stroke_within_tolerance) {
  const Color border = Color::rgb(0xD3, 0xDC, 0xE9);
  struct Case {
    float width_px;
    float radius;
    float border_width;
    float dpi;
  };
  const std::vector<Case> cases{{140.0f, 12.0f, 1.0f, 1.0f}, {140.0f, 12.0f, 2.0f, 1.0f},
                                {140.0f, 12.0f, 1.0f, 2.0f}, {200.0f, 6.0f, 1.5f, 1.0f}};
  for (const Case& item : cases) {
    const st::math::Rect box{60.0f, 40.0f, item.width_px, 80.0f};
    Canvas ring_canvas = make_canvas(item.dpi);
    draw_border_by_ring(ring_canvas, box, item.radius, border, item.border_width);
    Canvas stroke_canvas = make_canvas(item.dpi);
    draw_border_by_stroke(stroke_canvas, box, item.radius, border, item.border_width);
    const PixelDiff result = diff(ring_canvas, stroke_canvas);
    st::print("[border-diff] w={:.0f} r={:.0f} bw={:.1f} dpi={:.1f}：超差 {:.3f}%（Δ≤{}）\n",
              static_cast<double>(item.width_px), static_cast<double>(item.radius),
              static_cast<double>(item.border_width), static_cast<double>(item.dpi),
              result.ratio() * 100.0, result.max_delta);
    // 超差像素是**边框边缘**上的抗锯齿分布差异，量级 <1%
    ST_CHECK(result.ratio() < 0.01);
    ST_CHECK(result.max_delta <= 64);
  }
}

/// 退化边界：非正厚度、超大厚度、放不下内圈的窄矩形都不能崩、不能画错。
ST_TEST(border_ring_handles_degenerate_geometry) {
  const Color border = Color::rgb(0xD3, 0xDC, 0xE9);
  const Color background = Color::rgb(0xF7, 0xF8, 0xFA);
  // 非正厚度 = 实心圆角矩形
  {
    Canvas canvas = make_canvas();
    draw_border_by_ring(canvas, st::math::Rect{60.0f, 40.0f, 100.0f, 60.0f}, 10.0f, border, 0.0f);
    ST_CHECK(canvas.pixel_at(110, 70) == border);   // 中心被填满
  }
  // 厚度大于半宽 → 退化实心，不产生空洞、不越界
  {
    Canvas canvas = make_canvas();
    draw_border_by_ring(canvas, st::math::Rect{60.0f, 40.0f, 20.0f, 20.0f}, 8.0f, border, 40.0f);
    ST_CHECK(canvas.pixel_at(70, 50) == border);
  }
  // 半径为 0（直角环）
  {
    Canvas canvas = make_canvas();
    draw_border_by_ring(canvas, st::math::Rect{60.0f, 40.0f, 100.0f, 60.0f}, 0.0f, border, 2.0f);
    ST_CHECK(canvas.pixel_at(110, 70) == background);   // 直角环中心是空的
  }
}

/// 圆角环的**内圈是真的反向**：中心是空的，四角也是空的（不是"退化成一个圆"）。
ST_TEST(border_ring_is_hollow) {
  const Color border = Color::rgb(0xD3, 0xDC, 0xE9);
  Canvas canvas = make_canvas();
  const st::math::Rect box{60.0f, 40.0f, 140.0f, 80.0f};
  draw_border_by_ring(canvas, box, 12.0f, border, 2.0f);
  // 几何中心
  ST_CHECK(canvas.pixel_at(static_cast<int>(box.x + box.width * 0.5f),
                           static_cast<int>(box.y + box.height * 0.5f)) != border);
  // 上边中间（应当有边框）
  ST_CHECK(canvas.pixel_at(static_cast<int>(box.x + box.width * 0.5f),
                           static_cast<int>(box.y) + 1) == border);
}

// ————————————————————————————————————————————————————————————————————————————
// ② 两层投影：预合成贴图 vs 两次 draw_shadow
// ————————————————————————————————————————————————————————————————————————————

/// **核心等价性**：合并贴图与两次 `draw_shadow` 的差**只有 8 位取整噪声**。
///
/// 这条是 `src-over` 结合律的可执行形式：贴图从**全透明**开始合成（存的是阴影
/// 自身的预乘色），所以贴到任何底色上都与"两次依次贴"等价。
///
/// ## 为什么口径是 Δ≤1 而不是"逐像素相等"
///
/// 预乘 8 位缓冲上，`(a over b) over c` 与 `a over (b over c)` **数学上相等**，
/// 但每次合成都做一次 `x/255` 取整；合并方案多了一次中间取整，于是尾差 1/255。
/// 实测：超差像素 100% 满足 Δ≤1，且**没有一处是结构性的**（不是错位、不是漏画）。
/// 把这条口径写下来比笼统说"近似"重要——Δ>1 就说明不是取整、而是真错了。
ST_TEST(layered_shadow_matches_two_draws) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow shadow = st::ui::shadow_md(theme);
  const st::math::Rect box{100.0f, 80.0f, 120.0f, 48.0f};
  constexpr float kRadius = 10.0f;
  const Color background = Color::rgb(0xF7, 0xF8, 0xFA);
  const Color surface = Color::rgb(0xFF, 0xFF, 0xFF);

  Canvas layered = make_canvas();
  layered.draw_shadow_layered(box, kRadius, shadow.color, shadow.blur,
                              st::math::Point{shadow.offset_x, shadow.offset_y}, shadow.color2,
                              shadow.blur2,
                              st::math::Point{shadow.offset2_x, shadow.offset2_y});
  Canvas twice = make_canvas();
  twice.draw_shadow(box, kRadius, shadow.blur2, shadow.color2,
                    st::math::Point{shadow.offset2_x, shadow.offset2_y});
  twice.draw_shadow(box, kRadius, shadow.blur, shadow.color,
                    st::math::Point{shadow.offset_x, shadow.offset_y});

  // 未铺底色时差异来自"多一次取整"——必须全部落在 Δ≤1。
  // ⚠ 若这里出现 Δ 明显大于 1 的差异，说明贴图烘进了底色（那才是最危险的错法）。
  const PixelDiff raw = diff(layered, twice);
  st::print("[shadow-diff] 未铺底色：超差 {:.3f}%（Δ≤{}）\n", raw.ratio() * 100.0,
            raw.max_delta);
  ST_CHECK(raw.max_delta <= 1);

  // 铺上不透明底色后（用户看到的画面）：同样只允许取整噪声。
  layered.fill_rect(box, Paint::solid(surface), kRadius);
  twice.fill_rect(box, Paint::solid(surface), kRadius);
  const PixelDiff covered = diff(layered, twice);
  st::print("[shadow-diff] 铺底色后：超差 {} px（Δ≤{}）\n", covered.differing, covered.max_delta);
  ST_CHECK(covered.max_delta <= 1);
  (void)background;
}

/// **贴图不许烘底色**（比 Δ≤1 更强的一条）：把同一份阴影贴在**两种颜色截然不同**
/// 的底上，与"两次 `draw_shadow`"在同一底上的结果都必须只差取整噪声。
///
/// 若贴图是从"合成到某个背景上的结果"里抽出来的，深底那一次就会明显偏离
/// （阴影被烘成了"浅底 + 阴影"的固定色）。这条正是为那个错法设的。
///
/// ## 阈值为什么是 2 而不是 1
///
/// 累积误差正比于 **α·(1-α)·底色跨度**：浅底上的阴影 α 很小、底色跨度也小；
/// 而饱和蓝（0x3B82F6）在 R 通道上有 200 级跨度，两次 8 位取整就能到 2/255。
/// 实测 Δ≤2 且**没有任何一处结构差异**；Δ≥3 就说明不是取整了。
/// 先把它量出来再定阈值，比"看着差不多就放过"可靠。
ST_TEST(layered_shadow_is_background_independent) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow shadow = st::ui::shadow_md(theme);
  const st::math::Rect box{100.0f, 80.0f, 120.0f, 48.0f};
  constexpr float kRadius = 10.0f;

  const auto render = [&](Color background, bool merged) {
    Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    canvas.clear(background);
    if (merged) {
      canvas.draw_shadow_layered(box, kRadius, shadow.color, shadow.blur,
                                 st::math::Point{shadow.offset_x, shadow.offset_y}, shadow.color2,
                                 shadow.blur2,
                                 st::math::Point{shadow.offset2_x, shadow.offset2_y});
    } else {
      canvas.draw_shadow(box, kRadius, shadow.blur2, shadow.color2,
                         st::math::Point{shadow.offset2_x, shadow.offset2_y});
      canvas.draw_shadow(box, kRadius, shadow.blur, shadow.color,
                         st::math::Point{shadow.offset_x, shadow.offset_y});
    }
    return canvas;
  };

  for (const Color background : {Color::rgb(0xF7, 0xF8, 0xFA), Color::rgb(0x10, 0x14, 0x1B),
                                 Color::rgb(0x3B, 0x82, 0xF6)}) {
    const PixelDiff result = diff(render(background, true), render(background, false));
    st::print("[shadow-bg] 底色 #{:02X}{:02X}{:02X}：超差 {} px（Δ≤{}）\n", background.r,
              background.g, background.b, result.differing, result.max_delta);
    ST_CHECK(result.max_delta <= 2);
  }
}

/// 退化为单层：只有关键层时，行为与一次 `draw_shadow` 一致。
ST_TEST(layered_shadow_single_layer_matches_draw) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow shadow = st::ui::shadow_sm(theme);
  const st::math::Rect box{100.0f, 80.0f, 120.0f, 48.0f};
  Canvas single = make_canvas();
  single.draw_shadow_layered(box, 10.0f, shadow.color, shadow.blur,
                             st::math::Point{shadow.offset_x, shadow.offset_y}, Color{0, 0, 0, 0},
                             0.0f, {});
  Canvas reference = make_canvas();
  reference.draw_shadow(box, 10.0f, shadow.blur, shadow.color,
                        st::math::Point{shadow.offset_x, shadow.offset_y});
  const PixelDiff result = diff(single, reference);
  st::print("[shadow-single] 单层退化：超差 {} px（Δ≤{}）\n", result.differing, result.max_delta);
  ST_CHECK(result.differing == 0);
}

/// DPI：`draw_shadow_layered` 与 `draw_shadow` 同样按 `device_scale` 缩放几何。
ST_TEST(layered_shadow_respects_device_scale) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow shadow = st::ui::shadow_md(theme);
  const st::math::Rect box{40.0f, 30.0f, 100.0f, 40.0f};
  Canvas layered = make_canvas(2.0f);
  layered.draw_shadow_layered(box, 8.0f, shadow.color, shadow.blur,
                              st::math::Point{shadow.offset_x, shadow.offset_y}, shadow.color2,
                              shadow.blur2, st::math::Point{shadow.offset2_x, shadow.offset2_y});
  Canvas twice = make_canvas(2.0f);
  twice.draw_shadow(box, 8.0f, shadow.blur2, shadow.color2,
                    st::math::Point{shadow.offset2_x, shadow.offset2_y});
  twice.draw_shadow(box, 8.0f, shadow.blur, shadow.color,
                    st::math::Point{shadow.offset_x, shadow.offset_y});
  layered.fill_rect(box, Paint::solid(Color::rgb(0xFF, 0xFF, 0xFF)), 8.0f);
  twice.fill_rect(box, Paint::solid(Color::rgb(0xFF, 0xFF, 0xFF)), 8.0f);
  const PixelDiff result = diff(layered, twice);
  st::print("[shadow-dpi] scale=2 铺底色后：超差 {} px（Δ≤{}）\n", result.differing,
            result.max_delta);
  ST_CHECK(result.max_delta <= 1);   // 同样只允许 8 位取整噪声（见 matches_two_draws）
}

/// 缓存边界：多种几何/颜色交替绘制，缓存上限（48）淘汰后仍然正确。
ST_TEST(layered_shadow_cache_eviction_stays_correct) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow shadow = st::ui::shadow_md(theme);
  // 60 种不同宽度 → 超出 48 的缓存上限，强制淘汰
  for (int index = 0; index < 60; ++index) {
    const st::math::Rect box{20.0f, 20.0f, 60.0f + static_cast<float>(index), 40.0f};
    Canvas layered = make_canvas();
    layered.draw_shadow_layered(box, 8.0f, shadow.color, shadow.blur,
                                st::math::Point{shadow.offset_x, shadow.offset_y}, shadow.color2,
                                shadow.blur2,
                                st::math::Point{shadow.offset2_x, shadow.offset2_y});
    Canvas twice = make_canvas();
    twice.draw_shadow(box, 8.0f, shadow.blur2, shadow.color2,
                      st::math::Point{shadow.offset2_x, shadow.offset2_y});
    twice.draw_shadow(box, 8.0f, shadow.blur, shadow.color,
                      st::math::Point{shadow.offset_x, shadow.offset_y});
    layered.fill_rect(box, Paint::solid(Color::rgb(0xFF, 0xFF, 0xFF)), 8.0f);
    twice.fill_rect(box, Paint::solid(Color::rgb(0xFF, 0xFF, 0xFF)), 8.0f);
    const PixelDiff result = diff(layered, twice);
    // 淘汰前后都必须只差取整噪声——**淘汰若写错（如返回悬垂指针）会直接崩或大面积超差**。
    if (result.max_delta > 1) {
      st::print("[shadow-cache] 第 {} 种宽度超差：{} px（Δ≤{}）\n", index, result.differing,
                result.max_delta);
    }
    ST_CHECK(result.max_delta <= 1);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// ③ 性能护栏（归一化，抗机器差异）——守住"别把省下来的又花回去"
// ————————————————————————————————————————————————————————————————————————————

/// 环形填充必须**不慢于**描边（这台机器上实测 2.8×，这里只守住"不倒退"）。
ST_TEST(border_ring_is_not_slower_than_stroke) {
  const Color border = Color::rgb(0xD3, 0xDC, 0xE9);
  const st::math::Rect box{60.0f, 40.0f, 140.0f, 48.0f};
  constexpr int kRuns = 400;

  const auto measure = [&](bool ring) {
    Canvas canvas = make_canvas();
    double best = 1e9;
    for (int index = 0; index < kRuns; ++index) {
      canvas.clear(Color::rgb(0xF7, 0xF8, 0xFA));
      const auto start = std::chrono::steady_clock::now();
      if (ring) {
        draw_border_by_ring(canvas, box, 12.0f, border, 1.0f);
      } else {
        draw_border_by_stroke(canvas, box, 12.0f, border, 1.0f);
      }
      best = std::min(best, std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start)
                                .count());
    }
    return best;
  };
  const double ring_ms = measure(true);
  const double stroke_ms = measure(false);
  st::print("[border-cost] 环形 {:.4f} ms · 描边 {:.4f} ms · 提速 {:.2f}×\n", ring_ms, stroke_ms,
            stroke_ms / std::max(ring_ms, 1e-9));
  // 余量给足（本机实测 2~3×）：只要求"环形严格更快"是稳妥的回归方向。
  ST_CHECK(ring_ms < stroke_ms);
}

/// 两层投影合一必须**不慢于**两次 `draw_shadow`（实测 ~2×）。
ST_TEST(layered_shadow_is_not_slower_than_two_draws) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow shadow = st::ui::shadow_md(theme);
  const st::math::Rect box{100.0f, 80.0f, 140.0f, 48.0f};
  constexpr int kRuns = 200;

  const auto measure = [&](bool merged) {
    Canvas canvas = make_canvas();
    double best = 1e9;
    for (int index = 0; index < kRuns; ++index) {
      canvas.clear(Color::rgb(0xF7, 0xF8, 0xFA));
      const auto start = std::chrono::steady_clock::now();
      if (merged) {
        canvas.draw_shadow_layered(box, 12.0f, shadow.color, shadow.blur,
                                   st::math::Point{shadow.offset_x, shadow.offset_y},
                                   shadow.color2, shadow.blur2,
                                   st::math::Point{shadow.offset2_x, shadow.offset2_y});
      } else {
        canvas.draw_shadow(box, 12.0f, shadow.blur2, shadow.color2,
                           st::math::Point{shadow.offset2_x, shadow.offset2_y});
        canvas.draw_shadow(box, 12.0f, shadow.blur, shadow.color,
                           st::math::Point{shadow.offset_x, shadow.offset_y});
      }
      best = std::min(best, std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start)
                                .count());
    }
    return best;
  };
  const double merged_ms = measure(true);
  const double twice_ms = measure(false);
  st::print("[shadow-cost] 合并 {:.4f} ms · 两次 {:.4f} ms · 提速 {:.2f}×\n", merged_ms, twice_ms,
            twice_ms / std::max(merged_ms, 1e-9));
  ST_CHECK(merged_ms < twice_ms);
}
