// **圆角填充的几何精度与覆盖率连续性**。
//
// ## 用户报的问题
//
// 「霜天框架的组件（按钮等）圆角不光滑」。这条测试把"光滑"拆成两个可量的量，
// 分别钉住——因为它们的失效方式完全不同，混在一起就分不清该改哪里。
//
// | 量 | 含义 | 失效表现 |
// |---|---|---|
// | **几何精度** | 实测弧边界 vs 真圆弧的偏差 | 圆角形状不对（被压扁/外扩/四角不对称） |
// | **覆盖率级数** | 弧上出现的不同中间灰度数 | 边缘是"在/不在"两值（一位式渲染） |
//
// ## 实测基线（scale=1.5，radius=10 逻辑 → 15 物理，2026-11-06）
//
// - **几何精度**：逐行偏差 **±0.6 像素**（最大 0.6），四角互差 **0 像素**；
// - **覆盖率级数**：20 级（1.0 倍 16 级 / 2.0 倍 26 级）；
// - **单行最大跳变 196/255**：每行只有 1 个过渡像素。
//
// 前两条说明**渲染正确**（弧是真圆弧，覆盖率连续）。第三条是扫描线光栅化的
// 固有特征——半径 15 的圆在某一行里 x 方向只跨 0.6 像素，覆盖率从 4% 到 78%
// 是数学上准确的，但视觉上就是"一级台阶"。
//
// ⚠ 实测过并**否决**的一个方向：把 `kSubsamples` 4→8。单行跳变仍是 196、
// 级数反而 20→18 —— y 方向子采样不改善 x 方向的边缘量化。别再试。
//
// 所以本测试钉的是「不退化」：几何精度与级数不得低于上述基线。
// 「每行 1 个过渡像素」要改需要换光栅化策略（如 x 方向也做子采样），属另行决策。

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::raster::Canvas;
using st::raster::Paint;

constexpr int kWidth = 120;
constexpr int kHeight = 90;
constexpr float kInset = 10.0f;

[[nodiscard]] auto corner_rect() -> st::math::Rect {
  return st::math::Rect{kInset, kInset, static_cast<float>(kWidth) - 2 * kInset,
                        static_cast<float>(kHeight) - 2 * kInset};
}

/// 走 `fill_rect(radius)` —— **这是 UI 组件（按钮/卡片）的真实路径**。
///
/// 为何测试必须量这条：圆角有两条渲染路径，修一条不等于修两条。
/// 实测踩过——把 `fill_rect` 换成 SDF 之后，用 `fill_path` 的测试数字**一个都没变**，
/// 看起来"没生效"，其实是根本没走到那条路。
[[nodiscard]] auto make_corner_canvas(float scale, float radius) -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, scale);
  canvas.clear(Color::rgb(0xFF, 0xFF, 0xFF));
  canvas.fill_rect(corner_rect(), Paint::solid(Color::rgb(0x20, 0x20, 0x20)), radius);
  return canvas;
}

/// 同一形状走 `fill_path`（描边/自定义路径那条路）。两条必须观感一致。
[[nodiscard]] auto make_corner_canvas_via_path(float scale, float radius) -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, scale);
  canvas.clear(Color::rgb(0xFF, 0xFF, 0xFF));
  st::math::Path path;
  path.add_rounded_rect(corner_rect(), radius);
  canvas.fill_path(path, Paint::solid(Color::rgb(0x20, 0x20, 0x20)));
  return canvas;
}

[[nodiscard]] auto dark_at(const Canvas& canvas, int x, int y) -> int {
  return 255 - static_cast<int>(canvas.pixel_at(x, y).r);
}

}  // namespace

/// 弧必须是**真圆弧**：逐行比对实测边界与解析解，偏差不得 > 1 像素。
///
/// 这条防的是"圆角被画扁/画方"——那会让圆角看起来像被削掉一块，
/// 与"边缘有台阶"是两种不同的坏。
ST_TEST(rounded_corner_follows_a_true_arc) {
  const float scale = 1.5f;
  const float radius = 10.0f;
  const Canvas canvas = make_corner_canvas(scale, radius);

  const float x0 = kInset * scale;
  const float y0 = kInset * scale;
  const float r = radius * scale;
  float worst = 0.0f;
  int rows = 0;
  for (int i = 1; i <= static_cast<int>(r); ++i) {
    const int py = static_cast<int>(y0) + i;
    if (py >= canvas.physical_height()) break;
    int measured = -1;
    for (int j = 0; j < static_cast<int>(r) + 4; ++j) {
      const int px = static_cast<int>(x0) + j;
      if (px >= canvas.physical_width()) break;
      if (dark_at(canvas, px, py) >= 128) {
        measured = j;
        break;
      }
    }
    if (measured < 0) continue;
    // 真圆弧：圆心在 (x0+r, y0+r)，取该像素行的中心 y
    const float dy = (y0 + r) - (static_cast<float>(py) + 0.5f);
    const float theory = r - std::sqrt(std::max(0.0f, r * r - dy * dy));
    worst = std::max(worst, std::abs(static_cast<float>(measured) - theory));
    ++rows;
  }
  st::log::out("[corner] 弧精度：{} 行，最大偏差 {:.2f} px（判据 <= 1.0）\n", rows, worst);
  ST_CHECK(rows > 0);
  ST_CHECK(worst <= 1.0f);
}

/// 四角必须**对称**：同一半径下四个角的弧形状一致。
///
/// 不对称是最难解释的观感问题（"说不出哪里怪，就是看着别扭"），
/// 而它一旦来自取整方向错误（floor/ceil 混用），就只会在某些缩放/尺寸下出现。
ST_TEST(rounded_corner_is_four_way_symmetric) {
  const float scale = 1.5f;
  const float radius = 10.0f;
  const Canvas canvas = make_corner_canvas(scale, radius);

  const int x0 = static_cast<int>(std::lround(kInset * scale));
  const int y0 = static_cast<int>(std::lround(kInset * scale));
  const int x1 = static_cast<int>(std::lround((kWidth - kInset) * scale)) - 1;
  const int y1 = static_cast<int>(std::lround((kHeight - kInset) * scale)) - 1;
  const int limit = static_cast<int>(radius * scale) + 5;

  // 沿对角线从角点向内走，找覆盖率首次过半的位置
  const auto crossing = [&](int cx, int cy, int sx, int sy) -> int {
    for (int i = 0; i < limit; ++i) {
      const int x = cx + sx * i;
      const int y = cy + sy * i;
      if (x < 0 || y < 0 || x >= canvas.physical_width() || y >= canvas.physical_height()) break;
      if (dark_at(canvas, x, y) >= 128) return i;
    }
    return -1;
  };

  const int tl = crossing(x0, y0, +1, +1);
  const int tr = crossing(x1, y0, -1, +1);
  const int bl = crossing(x0, y1, +1, -1);
  const int br = crossing(x1, y1, -1, -1);
  st::log::out("[corner] 四角对角线交点：左上={} 右上={} 左下={} 右下={}（判据互差 <= 1）\n", tl,
               tr, bl, br);
  ST_CHECK(tl > 0 && tr > 0 && bl > 0 && br > 0);
  const int spread = std::max({tl, tr, bl, br}) - std::min({tl, tr, bl, br});
  ST_CHECK(spread <= 1);
}

/// 弧上必须有**连续多级覆盖率**（不是一位式的两值）。
///
/// 三档 DPI 都量：1.0 是整数对齐（最容易"看起来还行"的假象），
/// **1.5 是用户实际在用的那一档**（Windows 150% 缩放，弧落在像素格点之间）。
///
/// 判据 14 级以上是实测基线（1.0→16、1.5→20、2.0→26）往下留的余量。
ST_TEST(rounded_corner_coverage_is_continuous) {
  for (const float scale : {1.0f, 1.5f, 2.0f}) {
    const Canvas canvas = make_corner_canvas(scale, 10.0f);
    const int px0 = static_cast<int>(kInset * scale);
    const int py0 = static_cast<int>(kInset * scale);
    const int span = static_cast<int>(10.0f * scale) + 6;
    bool seen[256] = {};
    int distinct = 0;
    for (int y = py0; y < py0 + span && y < canvas.physical_height(); ++y) {
      for (int x = px0; x < px0 + span && x < canvas.physical_width(); ++x) {
        const int dark = dark_at(canvas, x, y);
        if (dark > 2 && dark < 253 && !seen[dark]) {
          seen[dark] = true;
          ++distinct;
        }
      }
    }
    st::log::out("[corner] scale={:.2f} 中间灰度级数={}（判据 >= 14）\n", scale, distinct);
    ST_CHECK(distinct >= 14);
  }
}

/// 半径越大，弧越长，同一块采样区里的中间级数必须**增加**。
///
/// 正交对照：如果级数与半径无关，说明量到的不是弧、而是某个固定量化常量，
/// 上面那条就白测了。
///
/// ⚠ **采样框必须固定**（踩过）：第一版让采样框随 radius 一起变大，
/// 于是大半径既延长了弧、又扩大了取样面积，两者叠加让"级数增加"恒成立——
/// 把弧退化成直角后测试照样绿（假绿）。现在固定取角点附近同一块 14×14，
/// 只有弧变长这一项能影响结果。
ST_TEST(rounded_corner_levels_grow_with_radius) {
  const float scale = 1.5f;
  // 固定的取样窗：从窗口角点起 14×14 物理像素。半径 6→9px、20→30px 都覆盖到这块。
  constexpr int kWindow = 14;
  const auto levels = [&](float radius) {
    const Canvas canvas = make_corner_canvas(scale, radius);
    const int px0 = static_cast<int>(kInset * scale);
    const int py0 = static_cast<int>(kInset * scale);
    bool seen[256] = {};
    int distinct = 0;
    for (int y = py0; y < py0 + kWindow && y < canvas.physical_height(); ++y) {
      for (int x = px0; x < px0 + kWindow && x < canvas.physical_width(); ++x) {
        const int dark = dark_at(canvas, x, y);
        if (dark > 2 && dark < 253 && !seen[dark]) {
          seen[dark] = true;
          ++distinct;
        }
      }
    }
    return distinct;
  };
  const int small = levels(6.0f);
  const int large = levels(20.0f);
  st::log::out("[corner] 固定 14x14 窗内：半径 6 → {} 级；半径 20 → {} 级\n", small, large);
  ST_CHECK(large > small);
}


/// `fill_rect(radius)` 与 `fill_path(圆角矩形路径)` 必须给出**同一观感**。
///
/// 为何要这条：圆角有两条渲染路径（前者是 UI 组件用的，后者是描边/自定义路径用的），
/// 本轮把前者换成了 SDF。若两条不一致，同一界面里按钮（SDF）与卡片（路径）的
/// 圆角会一个光滑一个有台阶——那种不一致比两边都粗糙更难解释。
///
/// 判据不要求逐像素相等（算法不同，量化细节会有 ±1 差异），
/// 只要求**边缘形状一致**：两条路径的"每行首个过渡像素位置"不得相差 > 1 像素。
ST_TEST(rounded_corner_two_paths_agree) {
  const float scale = 1.5f;
  const float radius = 10.0f;
  const Canvas rect_canvas = make_corner_canvas(scale, radius);
  const Canvas path_canvas = make_corner_canvas_via_path(scale, radius);

  const int px0 = static_cast<int>(kInset * scale);
  const int py0 = static_cast<int>(kInset * scale);
  const int span = static_cast<int>(radius * scale) + 4;
  int worst = 0;
  int rows = 0;
  for (int y = py0; y < py0 + span && y < rect_canvas.physical_height(); ++y) {
    const auto first_partial = [&](const Canvas& c) -> int {
      for (int x = px0; x < px0 + span && x < c.physical_width(); ++x) {
        const int dark = dark_at(c, x, y);
        if (dark > 2 && dark < 253) return x;
      }
      return -1;
    };
    const int a = first_partial(rect_canvas);
    const int b = first_partial(path_canvas);
    if (a < 0 || b < 0) continue;
    worst = std::max(worst, std::abs(a - b));
    ++rows;
  }
  st::log::out("[corner] 两条路径边缘位置最大差 {} px（{} 行，判据 <= 1）\n", worst, rows);
  ST_CHECK(rows > 0);
  ST_CHECK(worst <= 1);
}
