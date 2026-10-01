#include "st/text/grid_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace st::text {
namespace {

/// 一个**笔画**（一条近垂直或近水平的窄条）。
struct Stem {
  float edge_lo{0.0f};  ///< 竖画=左边缘 x / 横画=上边缘 y
  float edge_hi{0.0f};  ///< 竖画=右边缘 x / 横画=下边缘 y
  float span_lo{0.0f};  ///< 沿笔画方向的起
  float span_hi{0.0f};  ///< 沿笔画方向的止
  /// 两侧边缘各自的点（`raw_points()` 索引）——**分开存**，因为两侧要独立吸附。
  std::vector<std::size_t> lo_points{};
  std::vector<std::size_t> hi_points{};
};

/// 沿笔画方向是否重叠（同一竖列的多个笔画一起处理）。
[[nodiscard]] auto overlaps(const Stem& a, const Stem& b) -> bool {
  return std::min(a.span_hi, b.span_hi) - std::max(a.span_lo, b.span_lo) > 1.0f;
}

/// 逐轴位移（每个点一份；`active` 表示该点是否被任何一个笔画引用）。
struct Shifts {
  std::vector<float> value{};
  std::vector<bool> active{};
};

/// 记录位移：同一个点被多个笔画引用时取**绝对值较大**者。
///
/// 为什么取大而不是相加：相邻笔画常常共用一条边（「口」的内外沿就是这样）。
/// 相加会把它推出去两次（实测字形会明显变形），取大则"以最想动它的那条为准"，
/// 是保守且可预期的取舍。
void record(Shifts& shifts, std::size_t point, float delta) {
  if (point >= shifts.value.size()) return;
  if (!shifts.active[point] || std::abs(delta) > std::abs(shifts.value[point])) {
    shifts.value[point] = delta;
  }
  shifts.active[point] = true;
}

/// 吸附目标：把 `value` 移到最近的整数（或半整数）。
[[nodiscard]] auto snap_target(float value, GridFitMode mode) -> float {
  // Light/Normal 都按"最近整数"吸附：这正是让边缘落在像素边界上的动作。
  // （FreeType 的 center 策略是给等宽/CJK 用的，本框架的字形不含 hinting 指令，
  //   直接对齐边缘的收益最大且形变可控。）
  (void)mode;
  return std::round(value);
}

/// 一条边（可能是直线，也可能是曲线）：`lo`/`hi` 是它沿**横轴**的范围（由 `axis` 决定
/// 哪一轴是横轴——因此边集必须按轴分别构建）。
struct Edge {
  float lo{0.0f};
  float hi{0.0f};
  float span_lo{0.0f};
  float span_hi{0.0f};
  std::size_t from{0};
  std::size_t to{0};
  /// 该边涉及的全部点（端点 + 控制点）：位移时要一起动，否则曲线会变形。
  std::vector<std::size_t> points{};
};

/// 轴对齐笔画抽取。
///
/// `axis == 0`：竖笔画（边缘取 x、跨度取 y）；`axis == 1`：横笔画（换轴）。
///
/// **只用直线边**（`edge.points.size() == 2`）：曲线边的“横轴范围”是**控制点包围盒**的宽度，
/// 而 `o`/`e`/`D` 这类字形的侧边控制点离得很远（包围盒宽好几像素）——把它们当笔画边配对，
/// 宽度会直接超出 `max_stem_width`，而这些字真正的竖笔画（`b`/`d`/`h` 的主干）反而配不上对。
/// 实测过带曲线边的版本：拉丁收益从 50.9% 掉到 22.1%。所以曲线**不进配对**，
/// 只有直线边参与；曲线控制点仍会**跟随其端点**移动（见 `follow`），保住形状。
[[nodiscard]] auto collect_stems(const std::vector<Edge>& edges, float max_width)
    -> std::vector<Stem> {
  std::vector<Edge> candidates;
  for (const Edge& edge : edges) {
    if (edge.points.size() != 2U) continue;  // 只用**直线边**（见下方注释）
    const float across = edge.hi - edge.lo;
    const float along = edge.span_hi - edge.span_lo;
    // 沿笔画方向要够长；横向范围要够窄（直线是 0，近垂直的曲线也不超过 0.25px）
    if (along < 1.0f || across > 0.25f) continue;
    candidates.push_back(edge);
  }

  // 配对：每条"低边"找最近的、沿笔画方向重叠的"高边"（间距 ≤ max_width）。
  std::vector<Stem> stems;
  std::vector<bool> used(candidates.size(), false);
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (used[index]) continue;
    const Edge& low = candidates[index];
    // 低边的基准位置：取它自身点的最小横坐标（曲线边取极值更稳）
    float low_x = low.lo;
    std::size_t best = candidates.size();
    float best_width = max_width + 1.0f;
    float best_x = 0.0f;
    for (std::size_t other = 0; other < candidates.size(); ++other) {
      if (other == index || used[other]) continue;
      const Edge& high = candidates[other];
      // 共点 = 同一折线的转折（那是角，不是笔画的对边）
      if (high.from == low.from || high.from == low.to || high.to == low.from ||
          high.to == low.to) {
        continue;
      }
      const float width = high.lo - low.lo;
      if (width <= 0.0f || width > max_width || width >= best_width) continue;
      const float span =
          std::min(low.span_hi, high.span_hi) - std::max(low.span_lo, high.span_lo);
      if (span < 1.0f) continue;
      best = other;
      best_width = width;
      best_x = high.lo;
    }
    if (best == candidates.size()) continue;
    const Edge& high = candidates[best];
    used[index] = true;
    used[best] = true;
    Stem stem;
    stem.edge_lo = low_x;
    stem.edge_hi = best_x;
    stem.span_lo = std::max(low.span_lo, high.span_lo);
    stem.span_hi = std::min(low.span_hi, high.span_hi);
    stem.lo_points = low.points;
    stem.hi_points = high.points;
    stems.push_back(std::move(stem));
  }
  return stems;
}

/// 收集全部**边**（直线段与曲线段，`Close` 也构成一条），并标出每边涉及的点。
/// 索引均为 `raw_points()` 空间。
[[nodiscard]] auto collect_edges(const raster::Path& path, const std::vector<std::size_t>& offsets,
                                 int axis) -> std::vector<Edge> {
  std::vector<Edge> edges;
  const auto commands = path.commands();
  const auto raw = path.raw_points();
  std::size_t subpath_start = 0;
  bool has_subpath = false;
  std::size_t last = 0;
  const auto push = [&](std::size_t from, std::size_t to,
                        std::initializer_list<std::size_t> controls) {
    Edge edge;
    edge.from = from;
    edge.to = to;
    std::vector<std::size_t> all{from, to};
    for (const std::size_t control : controls) all.push_back(control);
    float lo = 1.0e9f;
    float hi = -1.0e9f;
    float span_lo = 1.0e9f;
    float span_hi = -1.0e9f;
    for (const std::size_t index : all) {
      if (index >= raw.size()) return;
      const math::Point& point = raw[index];
      const float across = axis == 0 ? point.x : point.y;
      const float along = axis == 0 ? point.y : point.x;
      lo = std::min(lo, across);
      hi = std::max(hi, across);
      span_lo = std::min(span_lo, along);
      span_hi = std::max(span_hi, along);
    }
    edge.lo = lo;
    edge.hi = hi;
    edge.span_lo = span_lo;
    edge.span_hi = span_hi;
    // 控制点也要跟着端点走（否则曲线形状会变）
    edge.points = std::move(all);
    edges.push_back(std::move(edge));
  };
  for (std::size_t index = 0; index < commands.size(); ++index) {
    const std::size_t offset = offsets[index];
    switch (commands[index].kind) {
      case raster::PathCommand::Kind::MoveTo:
        subpath_start = offset;
        has_subpath = true;
        last = offset;
        break;
      case raster::PathCommand::Kind::LineTo:
        if (has_subpath) push(last, offset, {});
        last = offset;
        break;
      case raster::PathCommand::Kind::QuadTo:
        if (has_subpath) push(last, offset + 1U, {offset});
        last = offset + 1U;
        break;
      case raster::PathCommand::Kind::CubicTo:
        if (has_subpath) push(last, offset + 2U, {offset, offset + 1U});
        last = offset + 2U;
        break;
      case raster::PathCommand::Kind::Close:
        if (has_subpath && last != subpath_start) push(last, subpath_start, {});
        last = subpath_start;
        break;
    }
  }
  return edges;
}

}  // namespace

auto grid_fit(const raster::Path& path, const GridFitOptions& options) -> GridFitResult {
  GridFitResult result;
  result.path = path;
  if (options.mode == GridFitMode::Off || path.is_empty() || options.max_stem_width <= 0.0f) {
    return result;
  }

  std::vector<math::Point> points = path.raw_points();
  // 少于 4 个点连一个四边形都凑不出（两条对边 = 笔画的最小形态）。
  // 门槛不能定高：真实字形动辄几十点，但**简单形状（图标化的矩形、直线构成的字形）
  // 也是合法的字体轮廓**，不该被静默跳过。
  if (points.size() < 4) return result;

  const auto commands = path.commands();
  std::vector<std::size_t> offsets(commands.size(), 0);
  std::vector<std::size_t> counts(commands.size(), 0);
  {
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < commands.size(); ++index) {
      offsets[index] = cursor;
      std::size_t count = 0;
      switch (commands[index].kind) {
        case raster::PathCommand::Kind::MoveTo:
        case raster::PathCommand::Kind::LineTo: count = 1; break;
        case raster::PathCommand::Kind::QuadTo: count = 2; break;
        case raster::PathCommand::Kind::CubicTo: count = 3; break;
        case raster::PathCommand::Kind::Close: count = 0; break;
      }
      counts[index] = count;
      cursor += count;
    }
  }
  const auto edges = collect_edges(path, offsets, 0);  // 两轴共用一套点，边集按轴重建
  if (edges.empty()) return result;

  // 两轴各自的位移表（x 位移给竖笔画、y 位移给横笔画）——**必须分开**：
  // 同一个端点可以既是竖笔画的边、又是横笔画的边，合成一个标量会把它推错方向。
  Shifts shift_x;
  Shifts shift_y;
  shift_x.value.assign(points.size(), 0.0f);
  shift_x.active.assign(points.size(), false);
  shift_y.value.assign(points.size(), 0.0f);
  shift_y.active.assign(points.size(), false);

  const int axes = options.mode == GridFitMode::Normal ? 2 : 1;  // Light 只拟合竖笔画
  for (int axis = 0; axis < axes; ++axis) {
    Shifts& shifts = axis == 0 ? shift_x : shift_y;
    std::vector<Stem> stems = collect_stems(collect_edges(path, offsets, axis),
                                            options.max_stem_width);
    if (stems.empty()) continue;
    std::ranges::sort(stems, {}, &Stem::edge_lo);
    // 归组：沿笔画方向重叠者同组（同一竖列的多个笔画要一起动，否则间距会乱）
    std::vector<std::vector<Stem>> groups;
    for (const Stem& stem : stems) {
      bool merged = false;
      for (auto& group : groups) {
        if (overlaps(group.front(), stem)) {
          group.push_back(stem);
          merged = true;
          break;
        }
      }
      if (!merged) groups.push_back({stem});
    }
    for (const auto& group : groups) {
      // **两侧边缘各自吸附**（而不是整条笔画一起平移）：
      // 只平移的话右边缘仍在分数相位上，等于只解决一半——实测那样做
      // "每边各一个过渡像素"的缓坡只消失一半。
      for (const Stem& stem : group) {
        const float lo_target = snap_target(stem.edge_lo, options.mode);
        const float hi_target = snap_target(stem.edge_hi, options.mode);
        // 吸附后不能塌成零宽（亚像素笔画确实存在）：不足 1px 就撑到 1px
        const float lo_delta = lo_target - stem.edge_lo;
        float hi_delta = hi_target - stem.edge_hi;
        if (hi_target - lo_target < 1.0f) hi_delta = lo_target + 1.0f - stem.edge_hi;
        if (std::abs(lo_delta) <= options.max_shift) {
          for (const std::size_t point : stem.lo_points) record(shifts, point, lo_delta);
        }
        if (std::abs(hi_delta) <= options.max_shift) {
          for (const std::size_t point : stem.hi_points) record(shifts, point, hi_delta);
        }
      }
      if (axis == 0) result.vertical_stems += static_cast<int>(group.size());
      else result.horizontal_stems += static_cast<int>(group.size());
    }
  }

  // 护栏①：平均位移超限 → 放弃（宁可保持原样，也不让字形走样）。
  //
  // **必须按全部点摊销**，而不是只算“被移动的点”：笔画本来就该动 0..0.5px，
  // 只按移动点平均的话，任何有笔画的字都会算出 0.25~0.31 而被护栏误拒
  // （实测：H/N/m/霜 全部被拒，拉丁收益因此只剩一半）。
  // 按全部点摊销得到的才是“字形整体被挪了多少”，这才是形变的正确度量。
  float total = 0.0f;
  std::size_t moved = 0;
  for (std::size_t point = 0; point < points.size(); ++point) {
    if (shift_x.active[point]) {
      total += std::abs(shift_x.value[point]);
      if (std::abs(shift_x.value[point]) > 1.0e-4f) ++moved;
    }
    if (shift_y.active[point]) {
      total += std::abs(shift_y.value[point]);
      if (std::abs(shift_y.value[point]) > 1.0e-4f) ++moved;
    }
  }
  // 护栏②：一个点都没真的动（比如 `max_shift` 把所有位移都拒了）→
  // 如实报 `applied=false`。曾经这里只看“有没有被标记为 active”，
  // 于是零位移也被当成“已拟合”（调用方无从区分）。
  if (moved == 0) return result;
  result.mean_shift = total / static_cast<float>(points.size());
  if (result.mean_shift > options.max_mean_shift) return result;

  // 曲线控制点**按参数权重跟随两端点**：只挪端点不动控制点会让曲线形状变样，
  // 那比不拟合更糟。二次贝塞尔控制点权重 1:2，三次贝塞尔 1:3（标准 Bernstein 系数），
  // 于是“整个曲线随两端一起平移”这个特例刚好精确成立（权重和为 1）。
  const auto follow = [&](Shifts& shifts) {
    const auto shift_of = [&](std::size_t point) -> float {
      return shifts.active[point] ? shifts.value[point] : 0.0f;
    };
    for (std::size_t index = 0; index < commands.size(); ++index) {
      const auto kind = commands[index].kind;
      if (kind != raster::PathCommand::Kind::QuadTo && kind != raster::PathCommand::Kind::CubicTo) {
        continue;
      }
      const std::size_t offset = offsets[index];
      const std::size_t count = counts[index];
      const std::size_t end = offset + count - 1U;
      // 起点 = 上一条命令的终点（命令流顺序决定了它就在 offset 之前一个点）。
      // 用**累积点索引**而不是“上一条命令”，这样 Close 也不会把起点算错。
      const std::size_t start = offset == 0 ? 0 : offset - 1U;
      const float start_shift = shift_of(start);
      const float end_shift = shift_of(end);
      if (std::abs(start_shift) <= 1.0e-4f && std::abs(end_shift) <= 1.0e-4f) continue;
      for (std::size_t step = 0; step + 1U < count; ++step) {
        const std::size_t control = offset + step;
        if (shifts.active[control]) continue;  // 已被别的笔画单独指定：不覆盖
        const float weight = count == 2U ? 1.0f / 3.0f : 1.0f / 4.0f;  // 1:2 / 1:3
        record(shifts, control, (1.0f - weight) * start_shift + weight * end_shift);
      }
    }
  };
  follow(shift_x);
  follow(shift_y);

  for (std::size_t point = 0; point < points.size(); ++point) {
    points[point].x += shift_x.active[point] ? shift_x.value[point] : 0.0f;
    points[point].y += shift_y.active[point] ? shift_y.value[point] : 0.0f;
  }
  if (!result.path.set_raw_points(points)) return result;
  result.applied = true;
  return result;
}

}  // namespace st::text
