// 圆角边框环路径的几何诊断探针。
//
// 目的：把 `make_rounded_border_ring` 产出的路径**逐条命令 + 展平折线**打出来，
// 用来判定"圆角弧上墨量缺失"是几何问题还是光栅化问题。
// 判据是「内圈是不是一条独立的闭合子路径」——内圈若从外圈起点起步，
// 四个圆角弧上会成段无墨（现象：卡片圆角是断的）。
//
// 用法： ring_path_probe

#include <cstddef>
#include <string>

#include "st/core/print.hpp"
#include "st/raster/path.hpp"

namespace {

using st::raster::Path;
using st::raster::PathCommand;

[[nodiscard]] auto kind_name(PathCommand::Kind kind) -> std::string_view {
  switch (kind) {
    case PathCommand::Kind::MoveTo: return "M";
    case PathCommand::Kind::LineTo: return "L";
    case PathCommand::Kind::QuadTo: return "Q";
    case PathCommand::Kind::CubicTo: return "C";
    case PathCommand::Kind::Close: return "Z";
  }
  return "?";
}

[[nodiscard]] auto fl(float value) -> double { return static_cast<double>(value); }

void dump_commands(const Path& path) {
  const auto commands = path.commands();
  st::print("命令数 = {}\n", commands.size());
  for (std::size_t index = 0; index < commands.size(); ++index) {
    const PathCommand& command = commands[index];
    switch (command.kind) {
      case PathCommand::Kind::MoveTo:
      case PathCommand::Kind::LineTo:
        st::print("  {:2} {} ({:.3f}, {:.3f})\n", index, kind_name(command.kind),
                  fl(command.p1.x), fl(command.p1.y));
        break;
      case PathCommand::Kind::QuadTo:
        st::print("  {:2} {} end({:.3f}, {:.3f}) ctrl({:.3f}, {:.3f})\n", index,
                  kind_name(command.kind), fl(command.p1.x), fl(command.p1.y),
                  fl(command.p2.x), fl(command.p2.y));
        break;
      case PathCommand::Kind::CubicTo:
        st::print("  {:2} {} end({:.3f}, {:.3f}) c1({:.3f}, {:.3f}) c2({:.3f}, {:.3f})\n", index,
                  kind_name(command.kind), fl(command.p1.x), fl(command.p1.y),
                  fl(command.p2.x), fl(command.p2.y), fl(command.p3.x), fl(command.p3.y));
        break;
      case PathCommand::Kind::Close:
        st::print("  {:2} {}\n", index, kind_name(command.kind));
        break;
    }
  }
}

void dump_polylines(const Path& path, float tolerance) {
  const auto polylines = path.flatten(tolerance);
  st::print("展平(tol={:.2f})后折线数 = {}\n", fl(tolerance), polylines.size());
  for (std::size_t index = 0; index < polylines.size(); ++index) {
    const auto& polyline = polylines[index];
    st::print("  折线[{}] 点数={} closed={}  首({:.3f},{:.3f}) 尾({:.3f},{:.3f})\n", index,
              polyline.points.size(), polyline.closed ? 1 : 0,
              fl(polyline.points.front().x), fl(polyline.points.front().y),
              fl(polyline.points.back().x), fl(polyline.points.back().y));
  }
}

/// 内圈各点相对**理想圆**的偏差（半径方向）——判定「圆弧比直壁薄」是几何问题还是感知问题。
///
/// 内圈圆弧的圆心 = (box.x + radius, box.y + radius)，理想半径 = radius - width。
/// 打印内圈圆弧中段各点的实际半径与理想半径之差（>0 表示外撇 = 环变薄）。
void check_inner_arc_geometry(const Path& ring, const st::math::Rect& box, float radius,
                              float width) {
  const auto polylines = ring.flatten(0.25f);
  if (polylines.size() < 2) {
    st::print("内圈子路径缺失（折线数 {}）\n", polylines.size());
    return;
  }
  // 左上圆弧：圆心在 (box.x+radius, box.y+radius)
  const double ccx = static_cast<double>(box.x) + static_cast<double>(radius);
  const double ccy = static_cast<double>(box.y) + static_cast<double>(radius);
  const double ideal = static_cast<double>(radius) - static_cast<double>(width);
  double worst = 0.0;
  std::size_t counted = 0;
  double sum_radius = 0.0;
  for (const auto& point : polylines[1].points) {
    const double dx = static_cast<double>(point.x) - ccx;
    const double dy = static_cast<double>(point.y) - ccy;
    // 只取左上四分之一弧（x<=ccx 且 y<=ccy）
    if (dx > 0.0 || dy > 0.0) continue;
    const double actual = std::sqrt(dx * dx + dy * dy);
    const double deviation = actual - ideal;
    sum_radius += actual;
    ++counted;
    if (std::abs(deviation) > std::abs(worst)) worst = deviation;
  }
  st::print("内圈左上弧：采样 {} 点，理想半径 {:.3f}，实际均值 {:.3f}，最大偏差 {:+.4f} 逻辑px\n",
            counted, ideal, counted > 0 ? sum_radius / static_cast<double>(counted) : 0.0, worst);
  st::print("（偏差 >0 = 内圈外撇 = 环变薄；纯几何实现应 ≈ 0）\n");
}

/// 从**展平后的多边形**量壁厚：对每个内圈点，求它到外圈多边形的最近距离。
///
/// 这是「圆弧比直壁细」的关键判据：环的几何由两条轮廓围成，
/// 若某处的多边形壁厚不等于 width，则该处的墨量必然不对（与光栅化无关）。
void check_wall_thickness(const Path& ring, float radius, float width) {
  const auto polylines = ring.flatten(0.1f);
  if (polylines.size() < 2) {
    st::print("壁厚检查：折线数 {}（需 2）\n", polylines.size());
    return;
  }
  const auto& outer = polylines[0].points;
  const auto& inner = polylines[1].points;

  // 点到多边形（闭合）的距离
  const auto dist_to_polygon = [&outer](double px, double py) -> double {
    double best = 1e9;
    for (std::size_t i = 0; i < outer.size(); ++i) {
      const double ax = static_cast<double>(outer[i].x);
      const double ay = static_cast<double>(outer[i].y);
      const double bx = static_cast<double>(outer[(i + 1) % outer.size()].x);
      const double by = static_cast<double>(outer[(i + 1) % outer.size()].y);
      const double dx = bx - ax;
      const double dy = by - ay;
      const double len2 = dx * dx + dy * dy;
      double t = len2 > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
      t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
      const double qx = ax + t * dx;
      const double qy = ay + t * dy;
      const double d = std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
      if (d < best) best = d;
    }
    return best;
  };

  // 按“上/右/下/左”四段直边 + 四个弧分区统计
  const double cx = static_cast<double>(radius);
  double straight_min = 1e9;
  double straight_max = 0.0;
  double arc_min = 1e9;
  double arc_max = 0.0;
  double arc_sum = 0.0;
  std::size_t arc_count = 0;
  for (const auto& point : inner) {
    const double x = static_cast<double>(point.x);
    const double y = static_cast<double>(point.y);
    const double d = dist_to_polygon(x, y);
    // 角区判定：内外轮廓的圆心是 (radius, radius) / (rect.right()-radius, ...) 等
    // 只要有一个坐标离最近的圆心小于 radius 就归入弧区；这里简化：
    // 直段上的内圈点，其到基准圆的切向坐标为“中段”
    const bool near_corner_x = x < cx + 0.5 || x > (0.0 + 0.0);
    (void)near_corner_x;
    if (d < straight_min) straight_min = d;
    if (d > straight_max) straight_max = d;
    arc_sum += d;
    ++arc_count;
    if (d < arc_min) arc_min = d;
    if (d > arc_max) arc_max = d;
  }
  st::print("壁厚（内圈点到外圈多边形的最短距离，理想 {:.3f}）：\n", static_cast<double>(width));
  st::print("  全部内圈点：min {:.4f}  max {:.4f}  均值 {:.4f}\n", straight_min, straight_max,
            arc_count > 0 ? arc_sum / static_cast<double>(arc_count) : 0.0);
}

}  // namespace

auto main() -> int {
  const st::math::Rect box{0.0f, 0.0f, 100.0f, 60.0f};
  const Path ring = st::raster::make_rounded_border_ring(box, 14.0f, 1.0f);
  st::print("=== 环路径（100x60, r=14, w=1）===\n");
  dump_commands(ring);
  dump_polylines(ring, 0.25f);
  check_inner_arc_geometry(ring, box, 14.0f, 1.0f);
  check_wall_thickness(ring, 14.0f, 1.0f);
  // 同一组几何在 2x 下的形态（内圈半径应恰好 = (14-1)*2 = 26 物理px）
  const Path ring2 = st::raster::make_rounded_border_ring(
      st::math::Rect{0.0f, 0.0f, 200.0f, 120.0f}, 28.0f, 2.0f);
  check_inner_arc_geometry(ring2, st::math::Rect{0.0f, 0.0f, 200.0f, 120.0f}, 28.0f, 2.0f);
  check_wall_thickness(ring2, 28.0f, 2.0f);
  return 0;
}
