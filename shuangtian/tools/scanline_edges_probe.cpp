// 扫描线「边扫描量」统计（性能轮）：搞清楚 `fill_path_aa` 的成本是不是花在**逐行扫边**上。
//
// ## 背景
//
// `rasterize_polylines` 用「按 ymin 排序的边表 + 逐行游标」代替"每行重扫全部边"。
// 但游标只推进**前缀**：边按 ymin 排序，一条 ymin 小、ymax 大的边会**挡住**游标，
// 使它后面的"已结束"边继续被逐行扫到。对于**子路径沿整条高度铺开**的路径
// （描边的逐段四边形就是这样），前缀收益接近于零。
//
// 本探针把两种方案的**边测试次数**数出来：
//   · 现状：`for index = first_candidate; index < edges.size(); ++index`
//   · AET ：活动边表（按 ymin 入、按 ymax 出），每行只迭代**活动边**
//
// 输出两者的边测试总数与比值——这是「改光栅器能不能真省下来」的直接依据。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "rasterize_internal.hpp"

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace {

inline constexpr int kSubsamples = 4;
inline constexpr float kSubsampleWeight = 1.0f / static_cast<float>(kSubsamples);

struct Edge {
  float x0{0.0f};
  float y0{0.0f};
  float x1{0.0f};
  float y1{0.0f};
  float ymin{0.0f};
  float ymax{0.0f};
  int direction{1};
  std::size_t source{0};   ///< 来自哪条折线（AET 统计用）
};

struct Stats {
  std::size_t edges{0};
  std::size_t current_tests{0};   ///< 现状方案：逐行扫到的边数
  std::size_t aet_tests{0};       ///< 活动边表：逐行真正检查的边数
  std::size_t rows{0};
  std::size_t peak_active{0};
};

auto collect_edges(const std::vector<st::raster::Polyline>& polylines) -> std::vector<Edge> {
  std::vector<Edge> edges;
  for (std::size_t source = 0; source < polylines.size(); ++source) {
    const auto& points = polylines[source].points;
    if (points.size() < 2) continue;
    const st::math::Point first = points.front();
    const st::math::Point last = points.back();
    const float dx = first.x - last.x;
    const float dy = first.y - last.y;
    const bool closing = (dx * dx + dy * dy) > 1.0e-8f;
    for (std::size_t index = 1; index <= points.size(); ++index) {
      if (index == points.size() && !closing) break;
      const st::math::Point from = points[index - 1];
      const st::math::Point to = index == points.size() ? first : points[index];
      if (from.y == to.y) continue;
      Edge edge;
      edge.x0 = from.x;
      edge.y0 = from.y;
      edge.x1 = to.x;
      edge.y1 = to.y;
      edge.ymin = from.y < to.y ? from.y : to.y;
      edge.ymax = from.y < to.y ? to.y : from.y;
      edge.direction = to.y > from.y ? 1 : -1;
      edge.source = source;
      edges.push_back(edge);
    }
  }
  return edges;
}

auto analyse(const std::vector<st::raster::Polyline>& polylines, int first_y, int last_y)
    -> Stats {
  std::vector<Edge> edges = collect_edges(polylines);
  Stats stats;
  stats.edges = edges.size();
  if (edges.empty() || first_y >= last_y) return stats;
  stats.rows = static_cast<std::size_t>(last_y - first_y);

  // —— 现状：按 ymin 排序 + 前缀游标 ——
  std::vector<Edge> sorted = edges;
  std::ranges::sort(sorted, {}, &Edge::ymin);
  std::size_t first_candidate = 0;
  for (int y = first_y; y < last_y; ++y) {
    const float row_top = static_cast<float>(y);
    while (first_candidate < sorted.size() && sorted[first_candidate].ymax <= row_top) {
      ++first_candidate;
    }
    for (int sample = 0; sample < kSubsamples; ++sample) {
      const float sample_y = static_cast<float>(y) + (static_cast<float>(sample) + 0.5f) *
                                                          kSubsampleWeight;
      for (std::size_t index = first_candidate; index < sorted.size(); ++index) {
        if (sorted[index].ymin > sample_y) break;
        ++stats.current_tests;
      }
    }
  }

  // —— AET：按 ymin 入表、按 ymax 出表 ——
  std::vector<std::size_t> by_ymin(edges.size());
  for (std::size_t index = 0; index < by_ymin.size(); ++index) by_ymin[index] = index;
  std::ranges::sort(by_ymin, [&edges](std::size_t a, std::size_t b) {
    return edges[a].ymin < edges[b].ymin;
  });
  std::vector<std::size_t> active;
  std::size_t next = 0;
  for (int y = first_y; y < last_y; ++y) {
    const float row_top = static_cast<float>(y);
    const float row_bottom = static_cast<float>(y) + 1.0f;
    while (next < by_ymin.size() && edges[by_ymin[next]].ymin < row_bottom) {
      active.push_back(by_ymin[next]);
      ++next;
    }
    std::erase_if(active, [&edges, row_top](std::size_t index) {
      return edges[index].ymax <= row_top;
    });
    stats.peak_active = std::max(stats.peak_active, active.size());
    for (int sample = 0; sample < kSubsamples; ++sample) {
      const float sample_y = static_cast<float>(y) +
                             (static_cast<float>(sample) + 0.5f) * kSubsampleWeight;
      for (const std::size_t index : active) {
        if (edges[index].ymin > sample_y) continue;
        ++stats.aet_tests;
      }
    }
  }
  return stats;
}

auto report(const char* name, const std::vector<st::raster::Polyline>& polylines) -> void {
  const st::math::Rect bounds = [&] {
    bool first = true;
    float minx = 0.0f, miny = 0.0f, maxx = 0.0f, maxy = 0.0f;
    for (const auto& line : polylines) {
      for (const auto& point : line.points) {
        if (first) {
          minx = maxx = point.x;
          miny = maxy = point.y;
          first = false;
        }
        minx = std::min(minx, point.x);
        miny = std::min(miny, point.y);
        maxx = std::max(maxx, point.x);
        maxy = std::max(maxy, point.y);
      }
    }
    return first ? st::math::Rect{} : st::math::Rect::from_ltrb(minx, miny, maxx, maxy);
  }();
  const int first_y = static_cast<int>(std::floor(bounds.y));
  const int last_y = static_cast<int>(std::ceil(bounds.bottom())) + 1;
  const Stats stats = analyse(polylines, first_y, last_y);
  st::print("  {:<34} 折线 {:>4} · 边 {:>5} · 行 {:>3} · 峰值活动 {:>4} · 现状边测试 {:>8} · "
            "AET {:>7} · 省 {:.1f}×\n",
            name, polylines.size(), stats.edges, stats.rows, stats.peak_active,
            stats.current_tests, stats.aet_tests,
            static_cast<double>(stats.current_tests) / std::max(1.0, static_cast<double>(stats.aet_tests)));
}

}  // namespace

auto main() -> int {
  const st::math::Rect box{100.0f, 100.0f, 140.0f, 48.0f};
  constexpr float kRadius = 12.0f;

  st::raster::Path rounded;
  rounded.add_rounded_rect(box, kRadius);

  st::raster::Path outline;
  outline.add_rounded_rect(box.inset(st::math::Insets::all(0.5f)), kRadius - 0.5f);
  const st::raster::Path expanded = st::raster::detail::stroke_to_path(outline, 1.0f, 0.25f);

  st::print("扫描线边扫描量（同一份几何，两种方案）：\n");
  report("圆角矩形填充（单子路径）", rounded.flatten(0.25f));
  report("1px 边框 描边（现状实现）", expanded.flatten(0.25f));
  report("1px 边框 环形（候选实现）",
         [&] {
           st::raster::Path ring;
           ring.add_rounded_rect(box, kRadius);
           st::raster::Path inner;
           inner.add_rounded_rect(box.inset(st::math::Insets::all(1.0f)), kRadius - 1.0f);
           for (const auto& line : inner.flatten(0.2f)) {
             if (line.points.size() < 2) continue;
             ring.move_to(line.points.back());
             for (std::size_t index = line.points.size() - 1; index-- > 0;) {
               ring.line_to(line.points[index]);
             }
             ring.close();
           }
           return ring.flatten(0.25f);
         }());

  // 曲线多的形态：圆形（图标场景）
  st::raster::Path circle;
  circle.add_circle(st::math::Point{200.0f, 200.0f}, 40.0f);
  report("圆形填充 r=40", circle.flatten(0.25f));
  return 0;
}
