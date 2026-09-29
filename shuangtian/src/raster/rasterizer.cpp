#include "rasterize_internal.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace st::raster::detail {
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
};

/// 把水平区间 [span_start, span_end) 按覆盖率权重累加到逐像素覆盖数组（端点线性分摊）。
void add_span(std::vector<float>& coverage, int base_x, float span_start, float span_end,
              float weight) {
  if (span_end <= span_start) return;
  const auto total = static_cast<int>(coverage.size());
  if (total <= 0) return;
  const float low_limit = static_cast<float>(base_x);
  const float high_limit = low_limit + static_cast<float>(total);
  const float x0 = std::max(span_start, low_limit);
  const float x1 = std::min(span_end, high_limit);
  if (x1 <= x0) return;

  int first = static_cast<int>(std::floor(x0)) - base_x;
  int last = static_cast<int>(std::floor(x1)) - base_x;
  if (first < 0) first = 0;
  if (last >= total) last = total - 1;
  if (first > last) return;

  if (first == last) {
    coverage[static_cast<std::size_t>(first)] += weight * (x1 - x0);
    return;
  }
  coverage[static_cast<std::size_t>(first)] +=
      weight * (static_cast<float>(first + base_x + 1) - x0);
  for (int index = first + 1; index < last; ++index) {
    coverage[static_cast<std::size_t>(index)] += weight;
  }
  const float tail = x1 - static_cast<float>(last + base_x);
  if (tail > 0.0f) coverage[static_cast<std::size_t>(last)] += weight * tail;
}

/// 扫描线覆盖率光栅化核心：`sink(y, coverage)` 逐行消费覆盖率。
template <class Sink>
void rasterize_polylines(const std::vector<Polyline>& polylines, int base_x, int width, int first_y,
                         int last_y, Sink&& sink) {
  std::vector<Edge> edges;
  for (const auto& polyline : polylines) {
    if (polyline.points.size() < 2) continue;
    // **填充语义要求隐式闭合子路径**（PostScript/SVG/TrueType 一致）：字体轮廓通常不含
    // 显式 Close 且首尾点不重合，缺了这条闭合边环绕数就永不归零——填充会一路向右溢出，
    // 整个字形糊成黑块（CJK 里带 口 部首的字形最先中招）。
    const math::Point first_point = polyline.points.front();
    const math::Point last_point = polyline.points.back();
    const float dx = first_point.x - last_point.x;
    const float dy = first_point.y - last_point.y;
    const bool needs_closing_edge = (dx * dx + dy * dy) > 1.0e-8f;
    for (std::size_t index = 1; index <= polyline.points.size(); ++index) {
      if (index == polyline.points.size() && !needs_closing_edge) break;
      const math::Point from = polyline.points[index - 1];
      const math::Point to = index == polyline.points.size() ? first_point : polyline.points[index];
      if (from.y == to.y) continue;
      Edge edge;
      edge.x0 = from.x;
      edge.y0 = from.y;
      edge.x1 = to.x;
      edge.y1 = to.y;
      edge.ymin = from.y < to.y ? from.y : to.y;
      edge.ymax = from.y < to.y ? to.y : from.y;
      edge.direction = to.y > from.y ? 1 : -1;
      edges.push_back(edge);
    }
  }
  if (edges.empty() || width <= 0 || first_y >= last_y) return;

  std::vector<float> coverage(static_cast<std::size_t>(width), 0.0f);
  std::vector<std::pair<float, int>> crossings;
  crossings.reserve(edges.size());

  for (int y = first_y; y < last_y; ++y) {
    std::ranges::fill(coverage, 0.0f);
    for (int sample = 0; sample < kSubsamples; ++sample) {
      const float sample_y =
          static_cast<float>(y) + (static_cast<float>(sample) + 0.5f) * kSubsampleWeight;
      crossings.clear();
      for (const auto& edge : edges) {
        if (sample_y < edge.ymin || sample_y >= edge.ymax) continue;
        const float ratio = (sample_y - edge.y0) / (edge.y1 - edge.y0);
        crossings.emplace_back(edge.x0 + (edge.x1 - edge.x0) * ratio, edge.direction);
      }
          if (crossings.size() < 2) continue;
      std::ranges::sort(crossings, {}, &std::pair<float, int>::first);
      int winding = 0;
      for (std::size_t index = 0; index < crossings.size(); ++index) {
        winding += crossings[index].second;
        if (winding == 0 || index + 1 >= crossings.size()) continue;
        const float sign = winding > 0 ? 1.0f : -1.0f;
        add_span(coverage, base_x, crossings[index].first, crossings[index + 1].first,
                 sign * kSubsampleWeight);
      }
    }
    sink(y, coverage);
  }
}

[[nodiscard]] auto polyline_bounds(const std::vector<Polyline>& polylines) -> math::Rect {
  bool has_point = false;
  float min_x = 0.0f;
  float min_y = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
  for (const auto& polyline : polylines) {
    for (const auto& point : polyline.points) {
      if (!has_point) {
        min_x = max_x = point.x;
        min_y = max_y = point.y;
        has_point = true;
        continue;
      }
      min_x = point.x < min_x ? point.x : min_x;
      min_y = point.y < min_y ? point.y : min_y;
      max_x = point.x > max_x ? point.x : max_x;
      max_y = point.y > max_y ? point.y : max_y;
    }
  }
  if (!has_point) return math::Rect{};
  return math::Rect::from_ltrb(min_x, min_y, max_x, max_y);
}

}  // namespace

void fill_path_aa(Canvas& canvas, const Path& path, const Paint& paint,
                  const DrawOptions& options) {
  if (path.is_empty()) return;
  const math::IntRect clip = canvas.clip_rect();
  if (clip.is_empty()) return;
  const auto polylines = path.flatten(options.antialias ? 0.25f : 0.5f);
  if (polylines.empty()) return;
  const math::Rect bounds = polyline_bounds(polylines);
  if (bounds.is_empty()) return;

  const int first_y = std::max(clip.y, static_cast<int>(std::floor(bounds.y)));
  const int last_y = std::min(clip.bottom(), static_cast<int>(std::ceil(bounds.bottom())) + 1);
  if (first_y >= last_y) return;

  rasterize_polylines(polylines, clip.x, clip.width, first_y, last_y,
                      [&canvas, &paint, &options, &clip](int y, const std::vector<float>& coverage) {
                        canvas.blend_coverage_row(
                            y, clip.x, std::span<const float>(coverage.data(), coverage.size()),
                            paint, options.opacity, options.blend);
                      });
}

void rasterize_mask(Mask& mask, const Path& path, float origin_x, float origin_y) {
  if (mask.empty() || path.is_empty()) return;
  Path shifted;
  shifted.add_path(path, -origin_x, -origin_y);
  const auto polylines = shifted.flatten(0.25f);
  if (polylines.empty()) return;
  const math::Rect bounds = polyline_bounds(polylines);
  if (bounds.is_empty()) return;

  const int first_y = std::max(0, static_cast<int>(std::floor(bounds.y)));
  const int last_y = std::min(mask.height(), static_cast<int>(std::ceil(bounds.bottom())) + 1);
  auto values = mask.values();
  const int width = mask.width();

  rasterize_polylines(polylines, 0, width, first_y, last_y,
                      [&values, width](int y, const std::vector<float>& coverage) {
                        auto* row = values.data() + static_cast<std::size_t>(y) *
                                                        static_cast<std::size_t>(width);
                        for (std::size_t index = 0; index < coverage.size(); ++index) {
                          const float value = std::abs(coverage[index]);
                          const float clamped = value > 1.0f ? 1.0f : value;
                          row[index] = static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
                        }
                      });
}

auto stroke_to_path(const Path& path, float width, float flatten_tolerance) -> Path {
  const float half = width * 0.5f;
  const auto polylines = path.flatten(flatten_tolerance);
  Path outline;
  for (const auto& polyline : polylines) {
    const auto& points = polyline.points;
    for (std::size_t index = 1; index < points.size(); ++index) {
      const math::Point from = points[index - 1];
      const math::Point to = points[index];
      const float dx = to.x - from.x;
      const float dy = to.y - from.y;
      const float length = std::sqrt(dx * dx + dy * dy);
      if (length <= 0.0001f) continue;
      const float nx = -dy / length * half;
      const float ny = dx / length * half;
      outline.move_to(math::Point{from.x + nx, from.y + ny});
      outline.line_to(math::Point{to.x + nx, to.y + ny});
      outline.line_to(math::Point{to.x - nx, to.y - ny});
      outline.line_to(math::Point{from.x - nx, from.y - ny});
      outline.close();
    }
    // 圆头/圆角连接：所有顶点补圆（同绕向，非零填充下与线段四边形求并）
    for (const auto& point : points) {
      outline.add_circle(point, half);
    }
  }
  return outline;
}

}  // namespace st::raster::detail
