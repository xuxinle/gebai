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
  /// 是否来自**字体自带 hints**。为什么用标记而不是"前 N 个"：笔画表要按横轴排序，
  /// 排序后 hint 笔画就散落在各处了——按下标判定会随排序悄悄错位。
  bool from_font{false};
};

/// 沿笔画方向是否重叠（同一竖列的多个笔画一起处理）。
/// `grid` = 坐标空间相对物理像素的倍率（阈值按物理像素给，比较时换算）。
[[nodiscard]] auto overlaps(const Stem& a, const Stem& b, float grid) -> bool {
  return std::min(a.span_hi, b.span_hi) - std::max(a.span_lo, b.span_lo) > grid;
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

/// 吸附目标：把 `value` 移到最近的**物理像素网格**（`grid` = 采样单位/物理像素）。
///
/// `grid` 是这条链路上的关键：入参轮廓在**超采样空间**里（1 物理像素 = `supersample` 个单位）
/// 时，吸附目标必须是 `supersample` 的整数倍。传 1 会吸到"半个物理像素"上——
/// 拟合反而把边缘推进像素正中间（0.5 覆盖率糊边），笔画宽度还会随相位在 1px/2px 间跳。
[[nodiscard]] auto snap_target(float value, float grid) -> float {
  return std::round(value / grid) * grid;
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

/// 笔画抽取的**漏斗计数**（诊断口径，2026-10-04）。
///
/// 存在的理由：定位“中文线条粗细不均匀”时，必须回答“那么多笔画为什么没被找到”。
/// 只看“最终找到几条”，分不清是**没被识别为笔画**还是**找到了但配不上对**——
/// 而这两者的修法完全不同。漏斗把两级都报出来。结构定义在头文件里
/// （`GridFitResult::Funnel`），因为调用方（位图诊断字段）要能读到它。
using Funnel = GridFitResult::Funnel;

/// 宽度聚类的容差（物理像素）：类内宽度差 ≤ 本值的笔画，统一取同一个整数宽度。
///
/// 取 0.35 的依据：要同类的两根笔画（如 1.4 / 1.6）落进同一类，
/// 而要设计上真不同宽的（1.5 主竖 vs 1.0 细横）保持分属两类。
/// 实测（汉字 @物理 20.25px）主笔类宽度散布 0.008~0.024 物理像素（约 1~2%），
/// 而粗细两类的中心相距 ≥ 0.4px——0.35 刚好卡在中间。
inline constexpr float kWidthClassTolerancePx = 0.35f;

/// **量化安全上限**：当"取整"会把真实宽度改动超过这么多（物理像素）时，放弃量化。
///
/// 存在的理由（2026-10-06 用户反馈「最小字号里 H/1/B 的竖线比浏览器粗、a/n/l 比浏览器细」）：
/// 逐字形实测（`tools/stroke_width_stats.py` + `tools/text_ab_glyphs_page.html`，11px）——
///
/// | | 大写竖笔 | 小写竖笔 | 比 |
/// |---|---|---|---|
/// | 浏览器（参照） | 1.69 | 1.54 | 1.10 |
/// | 霜天默认（**量化开**） | **2.00** | **1.28** | **1.56** |
/// | 霜天（量化关 / 拟合关） | 1.42~1.58 | 1.30~1.45 | 1.09 |
///
/// 即：字体给大写 ~1.7px、小写 ~1.5px（相差 10%），而 `round()` 把大写推到 **2**、
/// 小写推到 **1**（相差 56%）——**量化把小差异放大成大差异**，正是用户看到的那一对现象。
/// 而量化的初衷（2026-10-04 修「同字里同类笔画宽窄不一」）只在"1.4 与 1.6 各自取整成
/// 1 与 2"那种**同类内**分叉时才成立；跨类（大写 vs 小写本来就是不同宽度）被一起放大
/// 是它的副作用。
///
/// 折中：**量化只在"取整误差够小"时做**——误差超过本值时该笔画不量化（只做边缘吸附）。
/// 这样"薄笔画吸成满黑像素"的收益大多保留（它们本来就接近整数），
/// 而 1.76 这类"取整要挪 0.24px"的笔画不再被硬拉成 2。
inline constexpr float kMaxQuantizeErrorPx = 0.2f;

/// 按宽度聚类取样——每类取一个整数（物理像素）。
///
/// 存在理由（2026-10-04）：宽度量化原来是**每条笔画各自 `round`**，
/// 于是同类的 1.4 与 1.6 会各自变成 1 与 2——**同字里同类笔画宽窄不一**，
/// 正是用户反馈的「线条粗细不均匀」的直接来源。改法是按宽度聚类，
/// 每类只取一个整数，类内全部笔画共用它。
///
/// 聚法是**贪心分段**：按宽度排序后从头扫，与当前类中心相差超过容差就开新类——
/// 一维最简聚类，且**不改变类内顺序**（类中心取类内均值后再 `round`）。
[[nodiscard]] auto quantized_width_for(float width_px, const std::vector<float>& sorted_widths)
    -> float {
  std::size_t index = 0;
  while (index < sorted_widths.size()) {
    // 当前类的起点（与当前笔画最近的类边界），逐类累积成员再定中心。
    const float seed = sorted_widths[index];
    std::size_t end = index;
    double sum = 0.0;
    while (end < sorted_widths.size() && sorted_widths[end] - seed <= kWidthClassTolerancePx) {
      sum += static_cast<double>(sorted_widths[end]);
      ++end;
    }
    const float center = static_cast<float>(sum / static_cast<double>(end - index));
    if (width_px >= seed && width_px <= sorted_widths[end - 1U]) {
      return std::max(1.0f, std::round(center));
    }
    index = end;
  }
  return std::max(1.0f, std::round(width_px));
}

/// 轴对齐笔画抽取。
///
/// `axis == 0`：竖笔画（边缘取 x、跨度取 y）；`axis == 1`：横笔画（换轴）。
///
/// **只用直线边**（`edge.points.size() == 2`）：曲线边的“横轴范围”是**控制点包围盒**的宽度，
/// 而 `o`/`e`/`D` 这类字形的侧边控制点离得很远（包围盒宽好几像素）——把它们当笔画边配对，
/// 宽度会直接超出 `max_stem_width`，而这些字真正的竖笔画（`b`/`d`/`h` 的主干）反而配不上对。
/// 实测过带曲线边的版本：拉丁收益从 50.9% 掉到 22.1%。所以曲线**不进配对**，
/// 只有直线边参与；曲线控制点仍会**跟随其端点**移动（见 `follow`），保住形状。
///
/// 阈值**都按采样单位**传入（调用方已用 `grid` 换算）：本函数在采样空间里工作，
/// 只有吸附格点用 `grid` 回到物理像素。
///
/// `edges_seen`：被抽查的边数（漏斗口诊断，见 `GridFitResult::edges_seen`）。
[[nodiscard]] auto collect_stems(const std::vector<Edge>& edges, float max_width,
                                 float max_slant, float grid, Funnel& funnel) -> std::vector<Stem> {
  std::vector<Edge> candidates;
  funnel.edges_seen += static_cast<int>(edges.size());
  for (const Edge& edge : edges) {
    if (edge.points.size() != 2U) continue;  // 只用**直线边**（见下方注释）
    ++funnel.line;
    const float across = edge.hi - edge.lo;
    const float along = edge.span_hi - edge.span_lo;
    // 沿笔画方向要够长（1 物理像素）；横向跨度限制由 `max_edge_slant` 给。
    //
    // ⚠ 这里原来是**写死的 0.25px**（= 20px 高的笔画只容 0.7° 倾斜）。
    // CJK 字形里大量笔画是微斜直线，于是绝大多数边在这一行被丢掉——
    // 没被找到的笔画就不会被吸附、留着分数相位，渲染出来就是
    // 同一字里有的笔画实、有的笔画灰（用户反馈的“线条粗细不均匀”）。
    // 阈值现已参数化（见 `GridFitOptions::max_edge_slant`），由端效果定值。
    ++funnel.line;
    if (along < grid) continue;
    ++funnel.long_enough;
    if (across > max_slant) continue;
    ++funnel.straight;
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
      if (span < grid) continue;
      best = other;
      best_width = width;
      best_x = high.lo;
    }
    if (best == candidates.size()) {
      ++funnel.pairs_failed;
      continue;
    }
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

/// 由**字形自带的 stem hints** 构造笔画（已换算到拟合坐标空间）。
///
/// 与 `collect_stems`（从轮廓几何反推、近轴直线边两两配对）的关系：
/// **hints 优先，几何只补缺**。理由：几何法的召回实测只有 ~19%，漏掉的那些笔画
/// 不被吸附、留着分数相位，直接造成"同一字里粗细不一"；而 hints 是字体设计者写下的
/// 权威位置与宽度，召回接近 100%。几何法保留用于没有 hints 的字体（`glyf` 轮廓）。
///
/// `path` 是**字体单位且 y 向上**，而本空间是物理像素、y 向下——调用方（`TextRenderer`）
/// 传入的 hints 应先按同一变换换算过（见 `text.cpp` 的 `stems_from_hints` 调用点）。
auto stems_from_hints(const std::vector<StemHint>& hints, int axis, float grid,
                                    float max_stem_width, float x_min, float x_max, float y_min,
                                    float y_max, std::vector<Stem>& out) -> void {
  out.reserve(out.size() + hints.size());
  for (const StemHint& hint : hints) {
    if (hint.vertical != (axis == 0)) continue;
    const float a = hint.lo * grid;   // 已换算：该空间下 1 物理像素 = `grid` 单位
    const float b = hint.hi * grid;
    if (!(b > a)) continue;
    if (b - a > max_stem_width) continue;   // 过宽部件不是笔画，不吸
    Stem stem{};
    if (axis == 0) {
      stem.edge_lo = std::max(a, x_min);
      stem.edge_hi = std::min(b, x_max);
      stem.span_lo = y_min;
      stem.span_hi = y_max;
    } else {
      stem.edge_lo = std::max(a, y_min);
      stem.edge_hi = std::min(b, y_max);
      stem.span_lo = x_min;
      stem.span_hi = x_max;
    }
    if (!(stem.edge_hi > stem.edge_lo)) continue;
    out.push_back(std::move(stem));
  }
}

/// 笔画加墨量（物理像素）：按 FreeType 的 `darkening-parameters` 四点折线插值。
///
/// 控制点语义：x = 笔画宽度、y = 加墨量（都是物理像素）；超出最后一个点 → 0（不加墨）。
/// 之所以照搬这条曲线而不是自己定：它是 FreeType CFF 引擎的出厂默认，
/// 而我们的参照（浏览器）走的就是那条路——**照搬才有可比性**。
[[nodiscard]] auto stem_darkening_amount(float width_px, const GridFitOptions& options) -> float {
  const auto& x = options.darkening_width;
  const auto& y = options.darkening_amount;
  if (!(width_px > 0.0f)) return 0.0f;
  if (width_px <= x[0]) return y[0];
  for (std::size_t index = 1; index < x.size(); ++index) {
    if (width_px <= x[index]) {
      const float span = x[index] - x[index - 1];
      const float t = span > 0.0f ? (width_px - x[index - 1]) / span : 0.0f;
      return y[index - 1] + t * (y[index] - y[index - 1]);
    }
  }
  return 0.0f;
}

/// 把一条 hint 笔画的某个边缘接到轮廓点上：取该边缘**容差内**的全部点。
///
/// 为什么不能要求"坐标完全相等"：stem hints 是设计者对笔画的**近似描述**，
/// 与轮廓实际坐标并不逐位吻合（典型差 0.5~3 字体单位）——按等号匹配会几乎找不到点，
/// 拟合看上去"什么都没做"。
auto bind_hint_points(const std::vector<math::Point>& points, int axis, float edge,
                                    float tolerance, std::vector<std::size_t>& out) -> void {
  for (std::size_t index = 0; index < points.size(); ++index) {
    const float value = axis == 0 ? points[index].x : points[index].y;
    if (std::abs(value - edge) <= tolerance) out.push_back(index);
  }
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

  // 轮廓的横轴范围（两个方向各一份）：用于把 hint 裁到字身内。
  const math::Rect raw_bounds = path.bounds();
  const float axis_lo[2] = {raw_bounds.x, raw_bounds.y};
  const float axis_hi[2] = {raw_bounds.right(), raw_bounds.bottom()};

  // 两轴各自的位移表（x 位移给竖笔画、y 位移给横笔画）——**必须分开**：
  // 同一个端点可以既是竖笔画的边、又是横笔画的边，合成一个标量会把它推错方向。
  Shifts shift_x;
  Shifts shift_y;
  shift_x.value.assign(points.size(), 0.0f);
  shift_x.active.assign(points.size(), false);
  shift_y.value.assign(points.size(), 0.0f);
  shift_y.active.assign(points.size(), false);

  // 坐标空间 → 物理像素的换算：`grid` = 该空间下 1 物理像素占几个单位。
  //
  // 入参 `path` 由调用方按需要缩放（`TextRenderer` 在**超采样空间**里调用），
  // 而拟合的**语义必须锚在物理像素上**：吸附格点、笔画宽度阈值、位移护栏
  // 三件事全部按物理像素定义，再乘 `grid` 换算到当前空间。
  // 曾经这些量直接用原始单位比较，于是超采样倍率一变，实际生效的阈值就随之减半
  // （2 倍采样下只剩 1.3 物理像素宽的“笔画”能配对），拟合几乎全程袖手——
  // 且吸附格点落在**半个物理像素**上，反而把边缘推入像素正中间。
  const float grid = options.grid > 0.0f ? options.grid : 1.0f;
  const float max_stem_width = options.max_stem_width * grid;  // 采样单位
  const float max_shift = options.max_shift * grid;            // 采样单位

  const float max_slant = options.max_edge_slant * grid;       // 采样单位（按物理像素定义）
  // **每一轴的笔画来源分别决定**：
  //   · 竖笔画（axis 0）：Light/Normal 都做（几何反推 + 自带 hints 都可用）；
  //   · 横笔画（axis 1）：只有 Normal 做**几何反推**，但**自带 hints 任何档都采用**。
  //
  // 为什么横笔画要给 hints 开口子：hints 是字体设计者写下的权威笔画位置与宽度，
  // 用它吸附**不会**引入"猜错笔画把字形吸歪"的风险——而那正是 Light 档只做竖笔画的
  // 原因（横笔画靠几何反推容易把撇捺的横跨误判成一横，形变风险高）。
  // 而 CJK 的横提示远比竖提示多（实测「霜」19 条提示里 19 条都是横），
  // 关掉它等于把最有价值的信息挡在门外。
  const int axes = options.mode == GridFitMode::Normal ? 2 : 1;
  const bool hints_on_horizontal = true;
  for (int axis = 0; axis < (hints_on_horizontal ? 2 : axes); ++axis) {
    if (axis == 1 && axes < 2 && options.stem_hints.empty() && options.mode != GridFitMode::Normal) {
      continue;   // 没有 hints 可用的 Light 档不做横笔画（与旧行为一致）
    }
    Shifts& shifts = axis == 0 ? shift_x : shift_y;
    // **笔画来源：字体自带 hints 优先，几何反推补缺**（2026-10-06 方案 B）。
    //
    // 为什么优先用 hints：几何法（近轴直线边两两配对）的召回实测只有 ~19%，
    // 漏掉的笔画不被吸附、留着分数相位——那正是“同一字里有的笔画实、有的发灰”。
    // hints 是字体设计者给的权威位置与宽度，召回接近 100%。
    // 几何法保留：`glyf` 字体没有 CFF hints，且 hints 偶尔不含某些画笔。
    std::vector<Stem> stems;
    stems_from_hints(options.stem_hints, axis, grid, max_stem_width, axis_lo[axis],
                     axis_hi[axis], axis_lo[axis == 0 ? 1 : 0], axis_hi[axis == 0 ? 1 : 0], stems);
    for (Stem& stem : stems) stem.from_font = true;
    if (axis == 0 || options.mode == GridFitMode::Normal) {
      // 几何法只补 hints **没盖住**的笔画：与已有 hint 横轴重叠过半就视为同一根。
      std::vector<Stem> geometric =
          collect_stems(collect_edges(path, offsets, axis), max_stem_width, max_slant, grid,
                        result.funnel);
      for (Stem& candidate : geometric) {
        bool covered = false;
        for (const Stem& taken : stems) {
          if (!taken.from_font) continue;
          const float overlap = std::min(candidate.edge_hi, taken.edge_hi) -
                                std::max(candidate.edge_lo, taken.edge_lo);
          if (overlap > (candidate.edge_hi - candidate.edge_lo) * 0.5f) covered = true;
        }
        if (!covered) stems.push_back(std::move(candidate));
      }
    }
    if (stems.empty()) continue;
    std::ranges::sort(stems, {}, &Stem::edge_lo);
    std::vector<std::vector<Stem>> groups;
    // **按整个字形（而不是按组）建宽度类表**——这是结构修复的关键：
    // 组 = 沿笔画方向重叠的同一竖列，通常只 1~2 条笔画（配对率低）——在组内聚类
    // 等于什么都没做（类就是自己，`round` 原样）。而“同类笔画”是**跨组**的概念
    //（同一个字里的两根竖画），所以类表必须跨字形统计。
    //
    // 宽度量化只对**几何反推**的笔画做：字体自带 hints 已经给出设计者定义的宽度，
    // 再去 `round` 反而把"设计上就不同宽"的笔画拉平（与 hints 的本意相反）。
    std::vector<float> axis_widths;
    if (options.quantize_width) {
      axis_widths.reserve(stems.size());
      for (const Stem& stem : stems) {
        if (!stem.from_font) axis_widths.push_back((stem.edge_hi - stem.edge_lo) / grid);
      }
      std::ranges::sort(axis_widths);
    }
    // **把 hint 笔画接到轮廓点上**：hints 与轮廓坐标**并不逐位吻合**
    // （提示本就是设计者对笔画的近似描述），所以按"距离边缘在容差内的点"认定归属。
    // 容差取笔画宽度的 1/3（足够吸收提示与轮廓的差距，又不会抳到相邻笔画）。
    for (Stem& stem : stems) {
      if (!stem.from_font) continue;
      result.hint_seen += 1;
      const float tol = std::max((stem.edge_hi - stem.edge_lo) / 3.0f, grid * 0.34f);
      bind_hint_points(points, axis, stem.edge_lo, tol, stem.lo_points);
      bind_hint_points(points, axis, stem.edge_hi, tol, stem.hi_points);
    }
    // 一侧都没接上点的 hint 无法驱动任何点，如实丢弃（否则会计入生效数、骗过验收）。
    const std::size_t before_bind = stems.size();
    std::erase_if(stems, [](const Stem& stem) {
      return stem.lo_points.empty() && stem.hi_points.empty();
    });
    result.hint_bound += static_cast<int>(before_bind - stems.size());
    if (stems.empty()) continue;
    std::ranges::sort(stems, {}, &Stem::edge_lo);
    groups.clear();
    for (const Stem& stem : stems) {
      bool merged = false;
      for (auto& group : groups) {
        if (overlaps(group.front(), stem, grid)) {
          group.push_back(stem);
          merged = true;
          break;
        }
      }
      if (!merged) groups.push_back({stem});
    }
    // **hint 笔画数在这里才定**：上面那两处统计把"被宽度/护栏滤掉的 hint"也算进去了。
    result.hint_stems = static_cast<int>(
        std::count_if(stems.begin(), stems.end(), [](const Stem& s) { return s.from_font; }));
    for (const auto& group : groups) {
      // **宽度量化 + 单边锚定**（而不是整条笔画平移，也不是两侧各自吸整数）：
      // 只平移的话另一侧仍在分数相位上（只解决一半）；两侧各自独立吸整数又会
      // 让宽度随相位跳——量化宽度才同时满足"边缘在网格"与"宽度一致"。
      //
      // ⚠ **量化值必须按「宽度类」而非按「每条笔画」决定**（2026-10-04 结构修复）：
      // 同类的两根笔画若宽度是 1.4 与 1.6，各自的 `round` 会得到 1 与 2——
      // 同字里同类笔画宽窄不一，正是用户反馈的「线条粗细不均匀」的直接来源
      // （实测：字内离散变成 2.2 倍 / 24.6% 长笔画内部“实心与发灰共存”）。
      // 改法是**先按宽度聚类，每类统一取一个整数**（类内宽度差 ≤ `kWidthClassPx`，
      // 远小于 1px，所以设计上真不同宽的主笔与细横仍分属不同类、不会被拉平）。
      for (const Stem& stem : group) {
        // hint 笔画不量化宽度：hints 本身就是设计者定义的宽度（见上）。
        // **量化的适用闸**：取整量超过 `kMaxQuantizeErrorPx` 就不量化这一条。
        // `quantized_width_for` 给的是"类中心取整"，先算出来再判误差。
        float quantized_px = 0.0f;
        if (options.quantize_width && !stem.from_font && !axis_widths.empty()) {
          const float width_px = (stem.edge_hi - stem.edge_lo) / grid;
          quantized_px = quantized_width_for(width_px, axis_widths);
          if (std::abs(quantized_px - width_px) > kMaxQuantizeErrorPx) quantized_px = 0.0f;
        }
        const bool quantize = quantized_px > 0.0f;
        const float quantized = quantize ? quantized_px * grid : 0.0f;
        // 锚点取**移动更小**的一侧：吸住它，另一侧由量化后的宽度推出
        // （整数宽度 ⇒ 两边都在网格上）。选更小的一侧是为了少动字形。
        const float lo_anchor = snap_target(stem.edge_lo, grid);
        const float hi_anchor = snap_target(stem.edge_hi, grid);
        const float lo_anchor_delta = lo_anchor - stem.edge_lo;
        const float hi_anchor_delta = hi_anchor - stem.edge_hi;
        float lo_delta = lo_anchor_delta;
        float hi_delta = hi_anchor_delta;
        if (quantize) {
          if (std::abs(lo_anchor_delta) <= std::abs(hi_anchor_delta)) {
            hi_delta = (stem.edge_lo + lo_delta + quantized) - stem.edge_hi;
          } else {
            lo_delta = (stem.edge_hi + hi_delta - quantized) - stem.edge_lo;
          }
        }
        // 护栏的**预算**：量化把宽度取到整数像素时，远边要额外叠上取整量，
        // 几何上最坏就是半个像素（`round` 的最大偏差）——所以量化时预算必须
        // 至少是 `max_shift + grid/2`，否则会出现「近边通过、远边被拒」：
        // 笔画被平移了却没被改宽，既拿不到网格对齐又把字形推歪
        //（实测 ss=2：CJK 13.5px 半覆盖像素 310 / 中间调占比 1.04；
        //  预算放到 1.0 后是 171 / 0.549）。
        //
        // 未量化时不加：那时两条边各自吸到最近网格（位移 ≤ grid/2，
        // 已在 `max_shift` 范围内），加宽预算只会白改墨量。
        // **笔画加墨（stem darkening）**：两条边各自向**外**挪 `amount/2`。
        //
        // 为什么按"每条笔画"而不是"整个字形"：FreeType 的加墨量是**按该笔画的宽度**
        // 查表得到的（细笔画加得多、宽笔画不加），加墨也**只加在笔画上**、不动字腔。
        // 外扩方向由边缘在笔画哪一侧决定——`lo` 往外是负方向、`hi` 往外是正方向。
        // 加墨量与吸附位移**分开记账**：护栏只该管吸附（几何对齐），加墨是刻意的形变。
        float darken_lo = 0.0f;
        float darken_hi = 0.0f;
        if (options.stem_darkening) {
          const float amount = stem_darkening_amount((stem.edge_hi - stem.edge_lo) / grid, options);
          if (amount > 0.0f) {
            darken_lo = -amount * 0.5f * grid;
            darken_hi = amount * 0.5f * grid;
            result.darkened_stems += 1;
          }
        }
        const float budget = quantize ? max_shift + grid * 0.5f : max_shift;
        // 两侧要么**一起动**、要么都不动。
        //
        // 为什么不能“各自独立判定”：宽度量化必然让两侧的位移不等。若近边通过、
        // 远边被拒，这条笔画就只被**平移**而没被改宽——既拿不到网格对齐，
        // 又把字形推了一点，净效果是更糊。
        if (std::abs(lo_delta) <= budget && std::abs(hi_delta) <= budget) {
          for (const std::size_t point : stem.lo_points) record(shifts, point, lo_delta + darken_lo);
          for (const std::size_t point : stem.hi_points) record(shifts, point, hi_delta + darken_hi);
        } else {
          // **预算不足时如实上报**（2026-10-04 新增，为定位“线条粗细不均匀”）。
          //
          // 不要小看这个分支：被拒的笔画在画面上就是「没被网格对齐的那一根」——
          // 同一字里其余笔画落在网格上（满黑），它却摊成两个灰边，于是**看着粗细不一**。
          // 在它之前只数“生效数”，分不清「找不到笔画」与「找到了但推不动」——
          // 而两者要调的参数完全不同（前者调 `max_stem_width`，后者调 `max_shift`）。
          ++result.rejected_stems;
          const float overshoot =
              std::max(std::abs(lo_delta), std::abs(hi_delta)) / grid;
          result.worst_rejected_shift = std::max(result.worst_rejected_shift, overshoot);
        }
      }
      if (axis == 0) result.vertical_stems += static_cast<int>(group.size());
      else result.horizontal_stems += static_cast<int>(group.size());
    }
  }

  // 护栏①：**整体平移**超限 → 放弃。
  //
  // 度量 = 各轴位移的**有符号均值**（= 字形整体被平移了多少像素）。
  //
  // 为什么不是“每点 |位移| 的均值”：那个量**随笔画密度单调上升**——笔画越多的字
  // （国/回/目/霜 这类最需要锐化的 CJK）必然算出更大的值，于是**恰恰把收益最大的字形
  // 全部拒之门外**（实测 ss=2：口 0.27 / 国 0.27 / 回 0.32 / 目 0.33 / 霜 0.26，全部超阈）。
  // 而护栏的初衷是“不许把字挪出去”（见头文件）——那就该量“整体平移”本身：
  // 各边向两侧吸附时正负抵消，符号均值接近零；真平移（全往一边跑）才会顶到阈值。
  // 局部形变另有护栏：单边位移 ≤ `max_shift`。
  float sum_x = 0.0f;
  float sum_y = 0.0f;
  float total = 0.0f;
  std::size_t moved = 0;
  for (std::size_t point = 0; point < points.size(); ++point) {
    if (shift_x.active[point]) {
      sum_x += shift_x.value[point];
      total += std::abs(shift_x.value[point]);
      if (std::abs(shift_x.value[point]) > 1.0e-4f) ++moved;
    }
    if (shift_y.active[point]) {
      sum_y += shift_y.value[point];
      total += std::abs(shift_y.value[point]);
      if (std::abs(shift_y.value[point]) > 1.0e-4f) ++moved;
    }
  }
  // 护栏②：一个点都没真的动（比如 `max_shift` 把所有位移都拒了）→
  // 如实报 `applied=false`。曾经这里只看“有没有被标记为 active”，
  // 于是零位移也被当成“已拟合”（调用方无从区分）。
  if (moved == 0) return result;

  // 护栏③：**覆盖率门槛——要么大多数字形笔画都被吸附，要么整个字形放弃**。
  //
  // 存在理由：“看着粗细不均”的直接形态是**双峰**——同一字里一部分笔画被吸成满黑、
  // 另一部分留在原相位（摊成灰边）。根因是**抽取召回有限**。
  //
  // ⚠ **当前默认 0（禁用），因为实测覆盖率只有 ~19%**：每字形约 43 条候选边只成 8 对笔画，
  // 所以任何 ≥0.5 的门槛都会放弃**全部**中文字形（`tools/grid_fit_coverage_scan.cpp`：
  // 应用率 100% → 0%）。即目前只能二选一：全部拟合（更锐但 24.6% 笔画带双峰）
  // 或全部不拟合（更均匀但更糊）。本旋钮等**召回改善后**才能起到区分作用。
  // 代码保留（不是死代码）——它是“同一判据的可用形态”，且量尺已在 (grid_fit_coverage_scan)。
  if (options.min_stem_coverage > 0.0f) {
    const int found = result.vertical_stems + result.horizontal_stems;
    const int tried = result.funnel.straight;
    if (tried > 0 &&
        static_cast<float>(found) < options.min_stem_coverage * static_cast<float>(tried)) {
      result.path = path;
      result.applied = false;
      result.coverage_abandoned = true;
      return result;
    }
  }
  // 两个口径都报**物理像素**（与阈值同口径，跨采样倍率可比较）：
  // - `mean_shift`：形变量（逐点 |位移| 均值），只作诊断；
  // - `drift`：整体平移量（有符号均值），护栏用的是它。
  const float point_count = static_cast<float>(points.size());
  result.mean_shift = total / point_count / grid;
  result.drift = std::max(std::abs(sum_x), std::abs(sum_y)) / point_count / grid;
  if (result.drift > options.max_drift) return result;

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
