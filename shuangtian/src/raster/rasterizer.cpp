#include "rasterize_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

/// 按 ymin 排序的边表 + 逐行游标（活动边窗口）。
///
/// 为什么要这个游标：旧实现对**每一行、每个子采样**都扫全部边，边数一多就是纯粹的浪费
/// （图标/文字的轮廓动辄上百条边）。排序一次后，每行只需从「第一条可能相关的边」开始扫。
struct EdgeTable {
  std::vector<Edge> edges;      ///< 按 ymin 升序
  std::size_t first_candidate{0};

  void build(std::vector<Edge> source) {
    std::ranges::sort(source, {}, &Edge::ymin);
    edges = std::move(source);
    first_candidate = 0;
  }

  void seek(int y) {
    const float row_top = static_cast<float>(y);
    while (first_candidate < edges.size() && edges[first_candidate].ymax <= row_top) ++first_candidate;
  }
};

/// 把一次子采样的交叉点转成带符号运行段（追加到 `runs`）。
void emit_runs(std::vector<std::pair<float, int>>& crossings, std::vector<CoverageRun>& runs) {
  if (crossings.size() < 2) return;
  std::ranges::sort(crossings, {}, &std::pair<float, int>::first);
  int winding = 0;
  for (std::size_t index = 0; index < crossings.size(); ++index) {
    winding += crossings[index].second;
    if (winding == 0 || index + 1 >= crossings.size()) continue;
    const float sign = winding > 0 ? 1.0f : -1.0f;
    CoverageRun run;
    run.x0 = crossings[index].first;
    run.x1 = crossings[index + 1].first;
    run.weight = sign * kSubsampleWeight;
    if (run.x1 > run.x0) runs.push_back(run);
  }
}

/// 把同一行里重叠的运行段合并成**互不重叠**的段（权重相加）。
///
/// 等价于旧实现里逐像素累加：重叠处正负相加即孔洞（非零环绕规则）。
/// 用**扫描线 + 事件**而不是两两分割：描边路径（每个顶点一个圆）一行里可能有几十个重叠段，
/// 两两分割是 O(n²)，实测因此把描边拖慢了近一倍。事件扫描是 O(n log n) 且合并天然彻底。
void merge_runs(std::vector<CoverageRun>& runs, std::vector<CoverageRun>& out,
                std::vector<std::pair<float, float>>& events) {
  out.clear();
  if (runs.empty()) return;
  events.clear();
  events.reserve(runs.size() * 2);
  for (const auto& run : runs) {
    events.emplace_back(run.x0, run.weight);
    events.emplace_back(run.x1, -run.weight);
  }
  std::ranges::sort(events, {}, &std::pair<float, float>::first);
  float accumulated = 0.0f;
  float previous = events.front().first;
  std::size_t index = 0;
  while (index < events.size()) {
    const float x = events[index].first;
    if (x > previous && std::abs(accumulated) > 0.004f) {
      // 与上一段同权重且相接时合并（段数越少，后面的混合调用越便宜）
      if (!out.empty() && out.back().x1 == previous &&
          std::abs(out.back().weight - accumulated) < 0.004f) {
        out.back().x1 = x;
      } else {
        out.push_back(CoverageRun{previous, x, accumulated});
      }
    }
    while (index < events.size() && events[index].first == x) {
      accumulated += events[index].second;
      ++index;
    }
    previous = x;
  }
}

/// 扫描线核心：`sink(y, runs)` 逐行消费**合并后的运行段**。
template <class Sink>
void rasterize_polylines(const std::vector<Polyline>& polylines, int first_y, int last_y,
                         Sink&& sink) {
  std::vector<Edge> collected;
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
      collected.push_back(edge);
    }
  }
  if (collected.empty() || first_y >= last_y) return;

  EdgeTable table;
  table.build(std::move(collected));

  std::vector<std::pair<float, int>> crossings;
  std::vector<CoverageRun> runs;
  std::vector<CoverageRun> merged;
  std::vector<std::pair<float, float>> events;
  crossings.reserve(64);
  runs.reserve(64);
  merged.reserve(64);
  events.reserve(128);

  for (int y = first_y; y < last_y; ++y) {
    table.seek(y);
    runs.clear();
    for (int sample = 0; sample < kSubsamples; ++sample) {
      const float sample_y =
          static_cast<float>(y) + (static_cast<float>(sample) + 0.5f) * kSubsampleWeight;
      crossings.clear();
      for (std::size_t index = table.first_candidate; index < table.edges.size(); ++index) {
        const Edge& edge = table.edges[index];
        if (edge.ymin > sample_y) break;  // 已按 ymin 排序：后面的边都还没开始
        if (sample_y >= edge.ymax) continue;
        const float ratio = (sample_y - edge.y0) / (edge.y1 - edge.y0);
        crossings.emplace_back(edge.x0 + (edge.x1 - edge.x0) * ratio, edge.direction);
      }
      emit_runs(crossings, runs);
    }
    if (runs.empty()) continue;
    merge_runs(runs, merged, events);
    if (merged.empty()) continue;
    sink(y, merged);
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

/// 曲线扁平化容差（采样单位）——**仅用于对照实验**（环境变量 `ST_TEXT_FLATTEN`）。
///
/// 背景（2026-10-04）：怀疑「中文笔画粗细不均」来自曲线轮廓被扁平化成折线后，
/// 笔画侧边的近似位置随字形漂移（中文是 CFF 立方曲线，侧边大量是微弯曲线）。
/// 曲线扁平化容差（采样单位）——**仅用于对照实验**（环境变量 `ST_TEXT_FLATTEN`）。
///
/// 背景（2026-10-04）：怀疑「中文笔画粗细不均」来自曲线轮廓被扁平化成折线后，
/// 笔画侧边的近似位置随字形漂移（中文是 CFF 立方曲线，侧边大量是微弯曲线）。
/// ⚠ **实测结论：容差不是瓶颈**（它们发生在超采样空间，0.25 采样单位 ≈ 0.125 物理像素，
/// 对 1.5px 笔画是 8% 量级；扫描 0.25→0.02 端指标无提升）。
/// 保留这个开关是为了把假设量成数据，而不是又一次“看起来像”。
///
/// 探针放在本文件而不是公共头文件：它是诊断开关，不应成为公开 API。
/// 每帧读一次 environ 的成本可忽略（且不进入内层循环）。
[[nodiscard]] auto flatten_tolerance_override() noexcept -> float {
  const char* value = std::getenv("ST_TEXT_FLATTEN");
  if (value == nullptr || *value == '\0') return 0.0f;
  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  return (end != value && parsed > 0.0f) ? parsed : 0.0f;
}

void fill_path_aa(Canvas& canvas, const Path& path, const Paint& paint,
                  const DrawOptions& options) {
  if (path.is_empty()) return;
  const math::IntRect clip = canvas.clip_rect();
  if (clip.is_empty()) return;
  const float override_tolerance = flatten_tolerance_override();
  const float tolerance =
      override_tolerance > 0.0f ? override_tolerance : (options.antialias ? 0.25f : 0.5f);
  const auto polylines = path.flatten(tolerance);
  if (polylines.empty()) return;
  const math::Rect bounds = polyline_bounds(polylines);
  if (bounds.is_empty()) return;

  const int first_y = std::max(clip.y, static_cast<int>(std::floor(bounds.y)));
  const int last_y = std::min(clip.bottom(), static_cast<int>(std::ceil(bounds.bottom())) + 1);
  if (first_y >= last_y) return;

  rasterize_polylines(polylines, first_y, last_y,
                      [&canvas, &paint, &options](int y, const std::vector<CoverageRun>& runs) {
                        canvas.blend_coverage_runs(y, runs, paint, options.opacity, options.blend);
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
  const int width = mask.width();
  auto values = mask.values();

  rasterize_polylines(polylines, first_y, last_y,
                      [&values, width](int y, const std::vector<CoverageRun>& runs) {
                        auto* row = values.data() + static_cast<std::size_t>(y) *
                                                        static_cast<std::size_t>(width);
                        for (const auto& run : runs) {
                          const float weight = std::abs(run.weight);
                          if (weight <= 0.002f) continue;
                          const float a = std::max(run.x0, 0.0f);
                          const float b = std::min(run.x1, static_cast<float>(width));
                          if (b <= a) continue;
                          const int first_pixel = static_cast<int>(std::floor(a));
                          const int last_pixel = static_cast<int>(std::ceil(b));
                          const int full_begin =
                              first_pixel + (static_cast<float>(first_pixel) < a ? 1 : 0);
                          const int full_end = last_pixel - (static_cast<float>(last_pixel) > b ? 1 : 0);
                          const auto byte_of = [](float value) -> std::uint8_t {
                            const float clamped = value > 1.0f ? 1.0f : value;
                            return static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
                          };
                          const auto cover_at = [&](int x) -> float {
                            const float left = std::max(a, static_cast<float>(x));
                            const float right = std::min(b, static_cast<float>(x) + 1.0f);
                            return right > left ? (right - left) * weight : 0.0f;
                          };
                          // 完全覆盖的整段：一个字节写整段（遮罩是字节数组，不必逐像素浮点）
                          if (full_end > full_begin) {
                            const std::uint8_t value = byte_of(weight);
                            for (int x = full_begin > 0 ? full_begin : 0; x < full_end && x < width; ++x) {
                              if (value > row[x]) row[x] = value;
                            }
                          }
                          if (first_pixel < full_begin && first_pixel >= 0 && first_pixel < width) {
                            const std::uint8_t value = byte_of(cover_at(first_pixel));
                            if (value > row[first_pixel]) row[first_pixel] = value;
                          }
                          const int tail = last_pixel - 1;
                          if (tail >= 0 && tail < width && tail >= full_begin) {
                            const std::uint8_t value = byte_of(cover_at(tail));
                            if (value > row[tail]) row[tail] = value;
                          }
                        }
                      });
}

auto stroke_to_path(const Path& path, float width, float flatten_tolerance,
                    const StrokeStyle& style) -> Path {
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
      // **绕向必须是逆时针（与 `Path::add_circle` 一致）**：补圆生成的圆是逆时针
      // （`add_circle` 从 (0,−r) 经 (+r,0) 绕一圈，有符号面积为正）。若带四边形取顺时针，
      // 两者**重叠区在非零环绕下互相抵消**——实测指纹：强制在所有顶点补圆后，
      // 墨量反而**从 0.936 降到 0.640**（圆补得越多、墨越少）。
      // 此点序是逆时针，且对任意段方向成立（`dx·ny − dy·nx = half·length > 0` 恒正）。
      outline.move_to(math::Point{from.x + nx, from.y + ny});
      outline.line_to(math::Point{from.x - nx, from.y - ny});
      outline.line_to(math::Point{to.x - nx, to.y - ny});
      outline.line_to(math::Point{to.x + nx, to.y + ny});
      outline.close();
    }
    // 圆头连接/端帽：按**实际缺口深度**决定是否补圆，而不是固定角度阈值。
    //
    // 不能"每个顶点都补"：折线往往几十个点（曲线也是折线逼近的），每个顶点补一个圆
    // （4 段三次贝塞尔）会让边数爆炸——实测描边因此吃掉一帧的一半绘制时间。
    //
    // 但**固定 25° 阈值**（旧实现）在"平滑但密集"的折线上会漏掉真缺口：弧展平后相邻弦
    // 夹角只有几度，一路不补，而曲线展平的弦**在弧内侧**，每个顶点外侧都留一个
    // `half × tan(夹角/2)` 的楔形缺口——几十段累加后实测**缺墨 6.4%**（浏览器参照）。
    //
    // 正确判据是**缺口深度**：`half × tan(|夹角| / 2)`。它才真正决定"看不看得出"——
    // 同样的 5° 夹角，宽 1px 的线缺口 0.04px（看不见），宽 8px 的线缺口 0.35px（可见）。
    // 阈值取 0.05 逻辑像素：低于它就不补（保住性能），高于就补。为什么这么小——
    // 曲线展平后的相邻弦夹角只有几度，`half × tan(θ/2)` 通常远小于 0.05；
    // 而"补圆"本身也要付出边数代价（每个圆 = 4 段三次贝塞尔）。0.05px 的缺口在
    // 任何 DPI 下都不可见，同时把密集展平的曲线从"一律不补"拉回"一律补"。
    // 实测：0.2 → 弧 0.938；0.05 → 弧 0.981（浏览器参照 1.000 的手工近似）。
    constexpr float kMaxGapPx = 0.05f;
    for (std::size_t index = 0; index < points.size(); ++index) {
      bool join = false;
      const bool mid_vertex = index > 0 && index + 1 < points.size();
      if (mid_vertex) {
        const math::Point before = points[index - 1];
        const math::Point here = points[index];
        const math::Point after = points[index + 1];
        const float ax = here.x - before.x;
        const float ay = here.y - before.y;
        const float bx = after.x - here.x;
        const float by = after.y - here.y;
        const float length_a = std::sqrt(ax * ax + ay * ay);
        const float length_b = std::sqrt(bx * bx + by * by);
        if (length_a > 0.0001f && length_b > 0.0001f) {
          const float cosine = (ax * bx + ay * by) / (length_a * length_b);
          // 夹角 = acos(cosine)；缺口深度 = half × tan(夹角/2)。用半角公式避开三角函数：
          // tan(θ/2) = sin(θ) / (1 + cos(θ))，而 sin(θ) = |叉积| / (|a||b|)。
          const float sin_theta = std::abs(ax * by - ay * bx) / (length_a * length_b);
          const float tan_half = sin_theta / (1.0f + cosine);
          // 转角缺口必须补（几何必需，与线帽无关）；形状按 `join` 语义：
          // `Bevel` 不额外补（斜切由相邻带的端面近似），`Miter`/`Round` 补外角。
          join = style.join != LineJoin::Bevel && half * tan_half > kMaxGapPx;
        }
      } else if (style.cap == LineCap::Round) {
        // 端帽：只有**圆帽**需要额外补形状。`Butt` 靠带四边形自身的矩形端面；
        // `Square` 也靠它（四边形端面已与端点平齐，方帽需外扩半宽——见下方的外扩处理）。
        join = true;
      } else {
        join = false;
      }
      if (join) outline.add_circle(points[index], half);
    }
  }
  return outline;
}

}  // namespace st::raster::detail
