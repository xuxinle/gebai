#include "st/raster/path.hpp"

#include <cmath>

namespace st::raster {
namespace {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kCircleKappa = 0.5522847498307936f;  // 圆的三次贝塞尔拟合常数

[[nodiscard]] auto degrees_to_radians(float degrees) noexcept -> float {
  return degrees * kPi / 180.0f;
}

/// 按弦高误差估算曲线细分段数（控制多边形长度近似）。
[[nodiscard]] auto segment_count(float polygon_length, float tolerance) noexcept -> std::size_t {
  if (tolerance <= 0.01f) tolerance = 0.01f;
  const float estimate = std::sqrt(polygon_length / tolerance);
  const auto count = static_cast<int>(estimate);
  if (count < 2) return 2;
  if (count > 128) return 128;
  return static_cast<std::size_t>(count);
}

[[nodiscard]] auto quad_point(math::Point p0, math::Point p1, math::Point p2, float t) noexcept
    -> math::Point {
  const float inverse = 1.0f - t;
  return math::Point{inverse * inverse * p0.x + 2.0f * inverse * t * p1.x + t * t * p2.x,
                     inverse * inverse * p0.y + 2.0f * inverse * t * p1.y + t * t * p2.y};
}

[[nodiscard]] auto cubic_point(math::Point p0, math::Point p1, math::Point p2, math::Point p3,
                               float t) noexcept -> math::Point {
  const float inverse = 1.0f - t;
  const float w0 = inverse * inverse * inverse;
  const float w1 = 3.0f * inverse * inverse * t;
  const float w2 = 3.0f * inverse * t * t;
  const float w3 = t * t * t;
  return math::Point{w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                     w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y};
}

[[nodiscard]] auto distance(math::Point a, math::Point b) noexcept -> float {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

auto Path::move_to(math::Point point) -> Path& {
  commands_.push_back(PathCommand{PathCommand::Kind::MoveTo, point, {}, {}});
  return *this;
}

auto Path::line_to(math::Point point) -> Path& {
  commands_.push_back(PathCommand{PathCommand::Kind::LineTo, point, {}, {}});
  return *this;
}

auto Path::quad_to(math::Point control, math::Point end) -> Path& {
  commands_.push_back(PathCommand{PathCommand::Kind::QuadTo, control, end, {}});
  return *this;
}

auto Path::cubic_to(math::Point control1, math::Point control2, math::Point end) -> Path& {
  commands_.push_back(PathCommand{PathCommand::Kind::CubicTo, control1, control2, end});
  return *this;
}

auto Path::close() -> Path& {
  commands_.push_back(PathCommand{PathCommand::Kind::Close, {}, {}, {}});
  return *this;
}

auto Path::add_rect(math::Rect rect) -> Path& {
  move_to(math::Point{rect.x, rect.y});
  line_to(math::Point{rect.right(), rect.y});
  line_to(math::Point{rect.right(), rect.bottom()});
  line_to(math::Point{rect.x, rect.bottom()});
  close();
  return *this;
}

auto Path::add_rounded_rect(math::Rect rect, float radius) -> Path& {
  return add_rounded_rect(rect, radius, radius, radius, radius);
}

auto Path::add_rounded_rect(math::Rect rect, float top_left, float top_right, float bottom_right,
                            float bottom_left) -> Path& {
  const float limit = rect.width < rect.height ? rect.width * 0.5f : rect.height * 0.5f;
  const auto clamp_radius = [limit](float value) noexcept -> float {
    if (value < 0.0f) return 0.0f;
    return value > limit ? limit : value;
  };
  const float tl = clamp_radius(top_left);
  const float tr = clamp_radius(top_right);
  const float br = clamp_radius(bottom_right);
  const float bl = clamp_radius(bottom_left);
  const float ktl = tl * kCircleKappa;
  const float ktr = tr * kCircleKappa;
  const float kbr = br * kCircleKappa;
  const float kbl = bl * kCircleKappa;

  move_to(math::Point{rect.x + tl, rect.y});
  line_to(math::Point{rect.right() - tr, rect.y});
  if (tr > 0.0f) {
    cubic_to(math::Point{rect.right() - tr + ktr, rect.y}, math::Point{rect.right(), rect.y + tr - ktr},
             math::Point{rect.right(), rect.y + tr});
  }
  line_to(math::Point{rect.right(), rect.bottom() - br});
  if (br > 0.0f) {
    cubic_to(math::Point{rect.right(), rect.bottom() - br + kbr},
             math::Point{rect.right() - br + kbr, rect.bottom()},
             math::Point{rect.right() - br, rect.bottom()});
  }
  line_to(math::Point{rect.x + bl, rect.bottom()});
  if (bl > 0.0f) {
    cubic_to(math::Point{rect.x + bl - kbl, rect.bottom()}, math::Point{rect.x, rect.bottom() - bl + kbl},
             math::Point{rect.x, rect.bottom() - bl});
  }
  line_to(math::Point{rect.x, rect.y + tl});
  if (tl > 0.0f) {
    cubic_to(math::Point{rect.x, rect.y + tl - ktl}, math::Point{rect.x + tl - ktl, rect.y},
             math::Point{rect.x + tl, rect.y});
  }
  close();
  return *this;
}

auto Path::add_circle(math::Point center, float radius) -> Path& {
  const float kappa = radius * kCircleKappa;
  move_to(math::Point{center.x, center.y - radius});
  cubic_to(math::Point{center.x + kappa, center.y - radius}, math::Point{center.x + radius, center.y - kappa},
           math::Point{center.x + radius, center.y});
  cubic_to(math::Point{center.x + radius, center.y + kappa}, math::Point{center.x + kappa, center.y + radius},
           math::Point{center.x, center.y + radius});
  cubic_to(math::Point{center.x - kappa, center.y + radius}, math::Point{center.x - radius, center.y + kappa},
           math::Point{center.x - radius, center.y});
  cubic_to(math::Point{center.x - radius, center.y - kappa}, math::Point{center.x - kappa, center.y - radius},
           math::Point{center.x, center.y - radius});
  close();
  return *this;
}

auto Path::add_ellipse(math::Rect bounds) -> Path& {
  const math::Point center = bounds.center();
  const float radius_x = bounds.width * 0.5f;
  const float radius_y = bounds.height * 0.5f;
  const float kx = radius_x * kCircleKappa;
  const float ky = radius_y * kCircleKappa;
  move_to(math::Point{center.x, center.y - radius_y});
  cubic_to(math::Point{center.x + kx, center.y - radius_y}, math::Point{center.x + radius_x, center.y - ky},
           math::Point{center.x + radius_x, center.y});
  cubic_to(math::Point{center.x + radius_x, center.y + ky}, math::Point{center.x + kx, center.y + radius_y},
           math::Point{center.x, center.y + radius_y});
  cubic_to(math::Point{center.x - kx, center.y + radius_y}, math::Point{center.x - radius_x, center.y + ky},
           math::Point{center.x - radius_x, center.y});
  cubic_to(math::Point{center.x - radius_x, center.y - ky}, math::Point{center.x - kx, center.y - radius_y},
           math::Point{center.x, center.y - radius_y});
  close();
  return *this;
}

auto Path::add_arc(math::Point center, float radius, float start_degrees, float end_degrees) -> Path& {
  const float start = degrees_to_radians(start_degrees);
  const float end = degrees_to_radians(end_degrees);
  const float sweep = end - start;
  const auto steps = static_cast<int>(std::ceil(std::abs(sweep) / (kPi * 0.25f)));
  const int total = steps < 1 ? 1 : steps;
  for (int index = 0; index <= total; ++index) {
    const float ratio = static_cast<float>(index) / static_cast<float>(total);
    const float angle = start + sweep * ratio;
    const math::Point point{center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius};
    if (index == 0) {
      move_to(point);
    } else {
      line_to(point);
    }
  }
  return *this;
}

auto Path::add_path(const Path& other, float dx, float dy) -> Path& {
  for (const auto& command : other.commands_) {
    PathCommand shifted = command;
    shifted.p1 = math::Point{command.p1.x + dx, command.p1.y + dy};
    shifted.p2 = math::Point{command.p2.x + dx, command.p2.y + dy};
    shifted.p3 = math::Point{command.p3.x + dx, command.p3.y + dy};
    commands_.push_back(shifted);
  }
  return *this;
}

void Path::clear() { commands_.clear(); }

auto Path::bounds() const noexcept -> math::Rect {
  bool has_point = false;
  float min_x = 0.0f;
  float min_y = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
  const auto include = [&](math::Point point) noexcept {
    if (!has_point) {
      min_x = max_x = point.x;
      min_y = max_y = point.y;
      has_point = true;
      return;
    }
    if (point.x < min_x) min_x = point.x;
    if (point.y < min_y) min_y = point.y;
    if (point.x > max_x) max_x = point.x;
    if (point.y > max_y) max_y = point.y;
  };
  for (const auto& command : commands_) {
    switch (command.kind) {
      case PathCommand::Kind::MoveTo:
      case PathCommand::Kind::LineTo:
        include(command.p1);
        break;
      case PathCommand::Kind::QuadTo:
        include(command.p1);
        include(command.p2);
        break;
      case PathCommand::Kind::CubicTo:
        include(command.p1);
        include(command.p2);
        include(command.p3);
        break;
      case PathCommand::Kind::Close:
        break;
    }
  }
  if (!has_point) return math::Rect{};
  return math::Rect::from_ltrb(min_x, min_y, max_x, max_y);
}

void Path::reverse() {
  std::vector<PathCommand> reversed;
  reversed.reserve(commands_.size());
  for (auto iterator = commands_.rbegin(); iterator != commands_.rend(); ++iterator) {
    const PathCommand& command = *iterator;
    switch (command.kind) {
      case PathCommand::Kind::MoveTo:
        reversed.push_back(PathCommand{PathCommand::Kind::LineTo, command.p1, {}, {}});
        break;
      case PathCommand::Kind::LineTo:
        reversed.push_back(PathCommand{PathCommand::Kind::MoveTo, command.p1, {}, {}});
        break;
      case PathCommand::Kind::QuadTo:
        reversed.push_back(PathCommand{PathCommand::Kind::MoveTo, command.p2, {}, {}});
        break;
      case PathCommand::Kind::CubicTo:
        reversed.push_back(PathCommand{PathCommand::Kind::MoveTo, command.p3, {}, {}});
        break;
      case PathCommand::Kind::Close:
        break;
    }
  }
  commands_ = std::move(reversed);
}

auto Path::scaled(float factor) const -> Path {
  Path out;
  out.commands_.reserve(commands_.size());
  for (const auto& command : commands_) {
    PathCommand item = command;
    item.p1 = math::Point{command.p1.x * factor, command.p1.y * factor};
    item.p2 = math::Point{command.p2.x * factor, command.p2.y * factor};
    item.p3 = math::Point{command.p3.x * factor, command.p3.y * factor};
    out.commands_.push_back(item);
  }
  return out;
}

auto Path::translated(float dx, float dy) const -> Path {
  Path out;
  out.commands_.reserve(commands_.size());
  for (const auto& command : commands_) {
    PathCommand item = command;
    item.p1 = math::Point{command.p1.x + dx, command.p1.y + dy};
    item.p2 = math::Point{command.p2.x + dx, command.p2.y + dy};
    item.p3 = math::Point{command.p3.x + dx, command.p3.y + dy};
    out.commands_.push_back(item);
  }
  return out;
}

/// 单条命令携带的控制点数（与 `Path::raw_points()` 的展开顺序一致）。
[[nodiscard]] constexpr auto point_count(PathCommand::Kind kind) noexcept -> int {
  switch (kind) {
    case PathCommand::Kind::MoveTo:
    case PathCommand::Kind::LineTo: return 1;
    case PathCommand::Kind::QuadTo: return 2;
    case PathCommand::Kind::CubicTo: return 3;
    case PathCommand::Kind::Close: return 0;
  }
  return 0;
}

auto Path::raw_points() const -> std::vector<math::Point> {
  std::vector<math::Point> points;
  points.reserve(commands_.size() * 2U);
  for (const auto& command : commands_) {
    switch (command.kind) {
      case PathCommand::Kind::MoveTo:
      case PathCommand::Kind::LineTo:
        points.push_back(command.p1);
        break;
      case PathCommand::Kind::QuadTo:
        points.push_back(command.p1);
        points.push_back(command.p2);
        break;
      case PathCommand::Kind::CubicTo:
        points.push_back(command.p1);
        points.push_back(command.p2);
        points.push_back(command.p3);
        break;
      case PathCommand::Kind::Close:
        break;
    }
  }
  return points;
}

auto Path::set_raw_points(std::span<const math::Point> points) -> bool {
  // 先**数一遍**再写：命令流与我们手里的点数不一致时绝不能半途写一半
  // （那会得到一个几何自相矛盾的路径，而且报错时已经晚了）。
  std::size_t needed = 0;
  for (const auto& command : commands_) needed += static_cast<std::size_t>(point_count(command.kind));
  if (points.size() != needed) return false;
  std::size_t cursor = 0;
  for (auto& command : commands_) {
    switch (command.kind) {
      case PathCommand::Kind::MoveTo:
      case PathCommand::Kind::LineTo:
        command.p1 = points[cursor++];
        break;
      case PathCommand::Kind::QuadTo:
        command.p1 = points[cursor++];
        command.p2 = points[cursor++];
        break;
      case PathCommand::Kind::CubicTo:
        command.p1 = points[cursor++];
        command.p2 = points[cursor++];
        command.p3 = points[cursor++];
        break;
      case PathCommand::Kind::Close:
        break;
    }
  }
  return true;
}

auto Path::flatten(float tolerance) const -> std::vector<Polyline> {
  std::vector<Polyline> polylines;
  Polyline current;
  math::Point cursor{};
  math::Point subpath_start{};
  bool has_cursor = false;

  const auto flush = [&polylines, &current]() {
    if (current.points.size() >= 2) polylines.push_back(current);
    current = Polyline{};
  };

  for (const auto& command : commands_) {
    switch (command.kind) {
      case PathCommand::Kind::MoveTo: {
        flush();
        current.points.push_back(command.p1);
        cursor = command.p1;
        subpath_start = command.p1;
        has_cursor = true;
        break;
      }
      case PathCommand::Kind::LineTo: {
        if (!has_cursor) {
          cursor = command.p1;
          subpath_start = command.p1;
          has_cursor = true;
        }
        current.points.push_back(command.p1);
        cursor = command.p1;
        break;
      }
      case PathCommand::Kind::QuadTo: {
        if (!has_cursor) {
          cursor = command.p1;
          subpath_start = command.p1;
          has_cursor = true;
        }
        const float polygon = distance(cursor, command.p1) + distance(command.p1, command.p2);
        const std::size_t steps = segment_count(polygon, tolerance);
        for (std::size_t step = 1; step <= steps; ++step) {
          const float t = static_cast<float>(step) / static_cast<float>(steps);
          current.points.push_back(quad_point(cursor, command.p1, command.p2, t));
        }
        cursor = command.p2;
        break;
      }
      case PathCommand::Kind::CubicTo: {
        if (!has_cursor) {
          cursor = command.p1;
          subpath_start = command.p1;
          has_cursor = true;
        }
        const float polygon = distance(cursor, command.p1) + distance(command.p1, command.p2) +
                              distance(command.p2, command.p3);
        const std::size_t steps = segment_count(polygon, tolerance);
        for (std::size_t step = 1; step <= steps; ++step) {
          const float t = static_cast<float>(step) / static_cast<float>(steps);
          current.points.push_back(cubic_point(cursor, command.p1, command.p2, command.p3, t));
        }
        cursor = command.p3;
        break;
      }
      case PathCommand::Kind::Close: {
        if (has_cursor && !current.points.empty()) {
          current.closed = true;
          if (distance(current.points.front(), current.points.back()) > 0.0001f) {
            current.points.push_back(current.points.front());
          }
          flush();
          cursor = subpath_start;
        }
        break;
      }
    }
  }
  flush();
  return polylines;
}

auto Path::flattened_bounds(float tolerance) const -> math::Rect {
  const auto polylines = flatten(tolerance);
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
      if (point.x < min_x) min_x = point.x;
      if (point.y < min_y) min_y = point.y;
      if (point.x > max_x) max_x = point.x;
      if (point.y > max_y) max_y = point.y;
    }
  }
  if (!has_point) return math::Rect{};
  return math::Rect::from_ltrb(min_x, min_y, max_x, max_y);
}

auto make_rounded_rect(math::Rect rect, float radius) -> Path {
  Path path;
  path.add_rounded_rect(rect, radius);
  return path;
}

auto make_circle(math::Point center, float radius) -> Path {
  Path path;
  path.add_circle(center, radius);
  return path;
}

auto make_line(math::Point from, math::Point to) -> Path {
  Path path;
  path.move_to(from);
  path.line_to(to);
  return path;
}

namespace {

/// 反转一条**闭合子路径**的命令流（贝塞尔控制点保持完整）。
///
/// 为什么不能用 `Path::reverse()`：它逐段发独立 `MoveTo`、曲线控制点直接丢掉，
/// 内圈会散架（`src/ui/svg.cpp` 已记着这条坑）。
///
/// 为什么不能「边倒序边发命令」：一段曲线的反转形式是「终点变起点、两个控制点交换」，
/// 而那个**起点**是**前一条正向命令的终点**——它在倒序遍历时还没读到。
/// 必须先扫一遍把各段起点算出来（这就是本节的关键）。
///
/// 字段语义按 `PathCommand`：`LineTo.p1` = 端点；`QuadTo.p1` = 控制点、`p2` = 端点；
/// `CubicTo.p1` = 控制点1、`p2` = 控制点2、`p3` = 端点。
[[nodiscard]] auto reversed_closed_subpath(const Path& forward) -> Path {
  const auto commands = forward.commands();
  std::vector<math::Point> starts;
  math::Point cursor{};
  math::Point last{};
  bool has_last = false;
  for (const PathCommand& command : commands) {
    switch (command.kind) {
      case PathCommand::Kind::MoveTo:
        cursor = command.p1;
        last = command.p1;
        has_last = true;
        break;
      case PathCommand::Kind::LineTo:
        starts.push_back(cursor);
        cursor = command.p1;
        last = command.p1;
        has_last = true;
        break;
      case PathCommand::Kind::QuadTo:
        starts.push_back(cursor);
        cursor = command.p2;
        last = command.p2;
        has_last = true;
        break;
      case PathCommand::Kind::CubicTo:
        starts.push_back(cursor);
        cursor = command.p3;
        last = command.p3;
        has_last = true;
        break;
      case PathCommand::Kind::Close:
        break;
    }
  }
  Path reversed;
  if (starts.empty() || !has_last) return reversed;
  // 反转后的游标从原路径的**终点**起步。
  reversed.move_to(last);
  std::size_t segment = starts.size();
  for (std::size_t index = commands.size(); index-- > 0;) {
    const PathCommand& command = commands[index];
    switch (command.kind) {
      case PathCommand::Kind::MoveTo:
      case PathCommand::Kind::Close: break;
      case PathCommand::Kind::LineTo:
        --segment;
        reversed.line_to(starts[segment]);
        break;
      case PathCommand::Kind::QuadTo:
        --segment;
        reversed.quad_to(command.p1, starts[segment]);
        break;
      case PathCommand::Kind::CubicTo:
        --segment;
        reversed.cubic_to(command.p2, command.p1, starts[segment]);
        break;
    }
  }
  reversed.close();
  return reversed;
}

}  // namespace

auto make_rounded_border_ring(math::Rect rect, float radius, float width) -> Path {
  Path path;
  if (rect.is_empty()) return path;
  const float limit = std::min(rect.width, rect.height) * 0.5f;
  const float outer_radius = radius < 0.0f ? 0.0f : (radius > limit ? limit : radius);
  path.add_rounded_rect(rect, outer_radius);
  if (!(width > 0.0f)) return path;   // 非正厚度 → 实心圆角矩形

  const math::Rect inner_box = rect.inset(math::Insets::all(width));
  // 内圈放不下（矩形被线宽吃穿）→ 视觉上就是"全涂"，实心是正确退化。
  if (inner_box.is_empty()) return path;
  const float inner_limit = std::min(inner_box.width, inner_box.height) * 0.5f;
  const float inner_radius_raw = outer_radius - width;
  const float inner_radius = inner_radius_raw < 0.0f
                                 ? 0.0f
                                 : (inner_radius_raw > inner_limit ? inner_limit
                                                                   : inner_radius_raw);

  // 内圈**反向**：按逆序重发，并把每条曲线段的两个控制点交换——
  // 控制点因此保持完整，内圈仍是精确的圆角矩形（不做折线逼近）。
  // 反转的正确形式连同「起点取自前一条正向命令的终点」一起写在
  // `reversed_closed_subpath()` 里（那里记着错误形态的后果）。
  Path inner_forward;
  inner_forward.add_rounded_rect(inner_box, inner_radius);
  path.add_path(reversed_closed_subpath(inner_forward));
  return path;
}

}  // namespace st::raster
