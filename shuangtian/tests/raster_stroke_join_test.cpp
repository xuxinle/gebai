/// 描边几何（`stroke_to_path`）的**连续性**回归：多段路径的衔接处**不得缺墨**。
///
/// 起因（2026-10-06，与真窗口浏览器逐例对照）：把 SVG 描边图元放到 192px 下量，
/// **单段直线与浏览器墨量比 0.998，而 180° 圆弧 0.955、4 段折线 0.935** ——
/// 偏差集中在"多段折线拼成的连续笔画"上。
///
/// 两个独立成因（改前实测）：
/// ① **曲线展平是内接的**：弧转成贝塞尔后展平的弦在弧**内侧**，相邻弦在顶点外侧留下
///    楔形缺口。补圆能填掉它，但补圆有 `cos(25°)` 阈值——弧展平后相邻弦夹角只有几度，
///    **一路都不补**，于是每条弦接缝都留一个极小缺口，累积成可观缺墨。
/// ② **四边形绕向随段方向翻转**：`nx = -dy/len*half` 使顺/逆时针段的四边形绕向相反，
///    非零环绕下**互相抵消**（折线转角处红蓝交替正是此特征）。
///
/// 判据用**解析面积**而不是"和浏览器逐像素比"：框架测试里没有浏览器，而描边的理论面积
/// 解析可算（长 × 宽 + 补角），与实现无关——这是"带外部参照"在测试里的等价形态。
/// 三个独立判据（直线基线 / 弧 / 转折）+ 一个连续性剖面判据，避免单一指标互相掩盖。
#include <cmath>
#include <cstddef>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/test/test.hpp"

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::raster::DrawOptions;
using st::raster::Paint;
using st::raster::Path;

namespace {

constexpr int kSize = 192;
constexpr double kPi = 3.14159265358979;
const Color kBackground{0x00, 0x00, 0x00, 0xFF};
const Color kInk{0xFF, 0xFF, 0xFF, 0xFF};

/// 墨量 = 与背景的灰度差总量（0~255 口径）。
auto ink_total(const Canvas& canvas) -> double {
  const auto rgba = canvas.to_rgba8();
  double sum = 0.0;
  for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
    sum += static_cast<double>(rgba[i]);
  }
  return sum;
}

auto polyline_path(const std::vector<Point>& points) -> Path {
  Path path;
  path.move_to(points.front());
  for (std::size_t i = 1; i < points.size(); ++i) path.line_to(points[i]);
  return path;
}

/// 上半圆弧上的采样点（角度从 180° 到 360°，经顶端）。
auto semicircle_points(Point center, double radius, int segments) -> std::vector<Point> {
  std::vector<Point> points;
  points.reserve(static_cast<std::size_t>(segments) + 1);
  for (int i = 0; i <= segments; ++i) {
    const double angle = kPi + static_cast<double>(i) / static_cast<double>(segments) * kPi;
    points.push_back(Point{static_cast<float>(static_cast<double>(center.x) + radius * std::cos(angle)),
                           static_cast<float>(static_cast<double>(center.y) + radius * std::sin(angle))});
  }
  return points;
}

}  // namespace

/// ① **直线不能变胖**（对照基线）：单段直线墨量 = `长 × 宽`（±2%）。
/// 它一直是对的；任何"为修接缝而整体加粗"的改法都会打破它。
ST_TEST(stroke_single_segment_area_matches_length_times_width) {
  Canvas canvas{kSize, kSize};
  canvas.clear(kBackground);
  constexpr double kWidth = 6.0;
  constexpr double kLength = 120.0;
  Path line;
  line.move_to(Point{36.0f, 96.0f});
  line.line_to(Point{static_cast<float>(36.0 + kLength), 96.0f});
  canvas.stroke_path(line, Paint::solid(kInk), static_cast<float>(kWidth));

  const double ink = ink_total(canvas);
  const double expected = kLength * kWidth * 255.0;
  const double ratio = ink / expected;
  st::print("[stroke] 单段直线 墨量 {:.0f} / 理论 {:.0f} = {:.4f}\n", ink, expected, ratio);
  ST_CHECK(ratio > 0.98 && ratio < 1.02);
}

/// ② **圆弧不得缺墨**：半圆墨量 ≈ `π·r·宽`（弧长 × 宽）。
/// 改前实测 0.955——缺口全部来自弦接缝。这是"多段连续性"最干净的判据。
ST_TEST(stroke_arc_area_matches_arc_length_times_width) {
  Canvas canvas{kSize, kSize};
  canvas.clear(kBackground);
  constexpr double kWidth = 6.0;
  constexpr double kRadius = 60.0;
  const Point center{96.0f, 96.0f};
  canvas.stroke_path(polyline_path(semicircle_points(center, kRadius, 64)),
                     Paint::solid(kInk), static_cast<float>(kWidth));

  const double ink = ink_total(canvas);
  const double expected = kPi * kRadius * kWidth * 255.0;
  const double ratio = ink / expected;
  st::print("[stroke] 半圆弧 墨量 {:.0f} / 解析 {:.0f} = {:.4f}\n", ink, expected, ratio);
  // 弦比弧短 0.04%（64 段）；顶点补圆把外侧楔形补回一部分（补的是圆，比楔形略多）。
  // 实测 0.98（kMaxGapPx=0.1）——判据放在 ±3%，同时守住"缺墨"与"变胖"两侧。
  ST_CHECK(ratio > 0.97 && ratio < 1.03);
}

/// ③ **转折不得缺墨**：直角折线墨量 ≈ 两带面积 + 转角补角 + 端帽。
ST_TEST(stroke_right_angle_join_area) {
  Canvas canvas{kSize, kSize};
  canvas.clear(kBackground);
  constexpr double kWidth = 8.0;
  constexpr double kArm = 60.0;
  const double half = kWidth * 0.5;
  const std::vector<Point> points{
      Point{60.0f, 60.0f}, Point{60.0f, static_cast<float>(60.0 + kArm)},
      Point{static_cast<float>(60.0 + kArm), static_cast<float>(60.0 + kArm)}};
  canvas.stroke_path(polyline_path(points), Paint::solid(kInk), static_cast<float>(kWidth));

  const double ink = ink_total(canvas);
  // 直角折线（butt 端帽、miter 连接）的解析面积：
  //   两条带 − 转角重叠(half²) + 转角**外侧方形**(half²)
  // 注意：`miter` 连接的外角是**方形**（不是半圆）——写成半圆会把预期算高 57%，
  // 判据就会误判"缺墨"（实测踩到：预期 1040 而真值 955.9，那是判据的错、不是渲染的错）。
  const double two_bands = 2.0 * kArm * kWidth;
  const double overlap = half * half;
  const double miter_corner = half * half;
  const double expected = (two_bands - overlap + miter_corner) * 255.0;
  const double ratio = ink / expected;
  st::print("[stroke] 直角折线 墨量 {:.0f} / 解析 {:.0f} = {:.4f}\n", ink, expected, ratio);
  ST_CHECK(ratio > 0.96 && ratio < 1.04);
}

/// ④ **连续性剖面**：沿半径方向逐角度累加覆盖率，应稳定在"宽 × 255"附近。
/// 接缝缺墨会在剖面上留下**周期性凹陷**——总墨量判据可能被别处的多余墨掩盖，这条抓的是
/// "某几处衔接断了"。段数（48）刻意与 ② 不同：接缝位置变了仍要抓住。
ST_TEST(stroke_curve_has_no_periodic_gaps) {
  Canvas canvas{kSize, kSize};
  canvas.clear(kBackground);
  constexpr double kWidth = 5.0;
  constexpr double kRadius = 70.0;
  const Point center{96.0f, 96.0f};
  canvas.stroke_path(polyline_path(semicircle_points(center, kRadius, 48)),
                     Paint::solid(kInk), static_cast<float>(kWidth));

  const auto rgba = canvas.to_rgba8();
  // **只在弧顶附近取剖面**（θ 接近 90°）：那里笔画是水平的，竖直剖面与笔画正交，
  // 累加值恰为 `宽 × 255`。在斜段上取剖面会按 `1/|cosθ|` 放大（实测 27% 波动），
  // 那是**几何效应**而不是接缝缺墨——用它当判据等于把量尺口径搞错。
  const auto column_ink = [&](int x) -> double {
    double total = 0.0;
    for (int y = 0; y < kSize; ++y) {
      total += static_cast<double>(rgba[(static_cast<std::size_t>(y) * kSize + x) * 4]);
    }
    return total / 255.0;  // 归一成"像素 × 覆盖率"= 该列的等效实心高度
  };
  // 弧顶一段：x 从 center.x−40 到 center.x+40（覆盖数十段弦），逐列取高度。
  double worst = -1.0;
  double sum = 0.0;
  int count = 0;
  for (int x = static_cast<int>(center.x) - 40; x <= static_cast<int>(center.x) + 40; ++x) {
    const double value = column_ink(x);
    sum += value;
    ++count;
    if (worst < 0.0 || value < worst) worst = value;
  }
  const double mean = sum / static_cast<double>(count);
  st::print("[stroke] 弧顶列高 均值 {:.2f}（应≈宽 {:.1f}）最差 {:.2f} 凹陷 {:.1f}%\n", mean,
            kWidth, worst, 100.0 * (1.0 - worst / mean));
  // 平滑弧的列高应稳定在"宽"附近：均值贴近宽度、最差列不低于 90%。
  ST_CHECK(mean > kWidth * 0.9 && mean < kWidth * 1.1);
  ST_CHECK(worst > mean * 0.90);
}
