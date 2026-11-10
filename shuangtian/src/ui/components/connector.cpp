#include "st/ui/components/connector.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/svg.hpp"   // parse_color：属性面的 `stroke_color` 与图标/主题同一套解析
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

namespace {

constexpr float kEpsilon = 1.0e-4f;

[[nodiscard]] auto length_of(math::Point v) -> float {
  return std::sqrt(v.x * v.x + v.y * v.y);
}

/// 归一化；零向量返回 (0,0)（**不要**返回 NaN——下游会把它当合法方向去算控制点）。
[[nodiscard]] auto normalize(math::Point v) -> math::Point {
  const float len = length_of(v);
  if (len < kEpsilon) return math::Point{0.0f, 0.0f};
  return math::Point{v.x / len, v.y / len};
}

/// 点到线段的距离（命中判定与 `distance_to_line` 共用同一条数学，不各写一份）。
[[nodiscard]] auto distance_point_segment(math::Point point, math::Point a, math::Point b)
    -> float {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float length_squared = dx * dx + dy * dy;
  if (length_squared < kEpsilon) return length_of(math::Point{point.x - a.x, point.y - a.y});
  const float t = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / length_squared, 0.0f,
                             1.0f);
  const math::Point projection{a.x + dx * t, a.y + dy * t};
  return length_of(math::Point{point.x - projection.x, point.y - projection.y});
}

/// 把折线按 `pattern`（实、虚、实、虚…）切开分别描边。
///
/// 为什么要自己切：`StrokeStyle` 只有 cap/join，**没有 dash**（光栅层与 D3D11 两条腿
/// 都没有）。虚线只能在**路径层**表达——切出一段段实线子路径分别 `stroke_path`。
/// 代价是每段各做一次覆盖计算；界面里几十条线的规模（每段几个像素）完全够用。
void append_dashed(raster::Surface& canvas, const std::vector<math::Point>& points,
                   const std::vector<float>& pattern, const raster::Paint& paint, float width,
                   const raster::StrokeStyle& style) {
  float total = 0.0f;
  for (const float value : pattern) total += value;
  if (pattern.empty() || total <= kEpsilon) return;

  std::size_t index = 0;        // 当前处在 pattern 的哪一段
  float remaining = pattern[0];  // 当前段还剩多少长度
  bool pen_down = true;          // 偶数段实、奇数段虚（索引 0 起）

  for (std::size_t i = 1; i < points.size(); ++i) {
    const math::Point from = points[i - 1U];
    const math::Point to = points[i];
    const float segment_length = length_of(math::Point{to.x - from.x, to.y - from.y});
    if (segment_length < kEpsilon) continue;
    float travelled = 0.0f;
    while (segment_length - travelled > 1.0e-3f) {
      const float step = std::min(remaining, segment_length - travelled);
      const float t0 = travelled / segment_length;
      const float t1 = (travelled + step) / segment_length;
      if (pen_down) {
        math::Path piece;
        piece.move_to(math::Point{from.x + (to.x - from.x) * t0, from.y + (to.y - from.y) * t0});
        piece.line_to(math::Point{from.x + (to.x - from.x) * t1, from.y + (to.y - from.y) * t1});
        canvas.stroke_path(piece, paint, width, style);
      }
      travelled += step;
      remaining -= step;
      if (remaining <= 1.0e-3f) {
        index = (index + 1U) % pattern.size();
        remaining = pattern[index];
        pen_down = (index % 2U) == 0U;
      }
    }
  }
}

/// 箭头三角：以 `tip` 为尖，朝 `direction`（未归一化，内部归一）。
[[nodiscard]] auto arrow_path(math::Point tip, math::Point direction, float size) -> math::Path {
  const math::Point dir = normalize(direction);
  if (length_of(dir) < kEpsilon) return {};
  const math::Point side{-dir.y, dir.x};   // 垂直方向（左右各取半宽）
  const float half = size * 0.42f;
  const math::Point back{tip.x - dir.x * size, tip.y - dir.y * size};
  math::Path path;
  path.move_to(tip);
  path.line_to(math::Point{back.x + side.x * half, back.y + side.y * half});
  path.line_to(math::Point{back.x - side.x * half, back.y - side.y * half});
  path.close();
  return path;
}

[[nodiscard]] auto route_name(Connector::Route route) -> std::string_view {
  switch (route) {
    case Connector::Route::Straight: return "straight";
    case Connector::Route::Orthogonal: return "orthogonal";
    case Connector::Route::Curve: return "curve";
  }
  return "straight";
}

[[nodiscard]] auto arrow_name(Connector::Arrow arrow) -> std::string_view {
  switch (arrow) {
    case Connector::Arrow::None: return "none";
    case Connector::Arrow::Start: return "start";
    case Connector::Arrow::End: return "end";
    case Connector::Arrow::Both: return "both";
  }
  return "none";
}

[[nodiscard]] auto side_name(Connector::Side side) -> std::string_view {
  switch (side) {
    case Connector::Side::Center: return "center";
    case Connector::Side::Top: return "top";
    case Connector::Side::Right: return "right";
    case Connector::Side::Bottom: return "bottom";
    case Connector::Side::Left: return "left";
    case Connector::Side::Auto: return "auto";
  }
  return "auto";
}

/// "x,y" → 点。失败返回 nullopt（属性面据此**如实拒绝**非法输入）。
[[nodiscard]] auto parse_point(std::string_view text) -> std::optional<math::Point> {
  const auto parts = st::split(st::trim(text), ',');
  if (parts.size() != 2U) return std::nullopt;
  try {
    return math::Point{std::stof(std::string(st::trim(parts[0]))),
                       std::stof(std::string(st::trim(parts[1])))};
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 构造
// ————————————————————————————————————————————————————————————————————————————

Connector::Connector() {
  // 线本身**不画底**（`paint_box` 在 background 透明时是空操作），但把描边/圆角显式
  // 清零——否则主题或宿主的默认样式会给它糊上一层看不见却会吃掉命中的框。
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
}

Connector::Connector(math::Point from, math::Point to) : Connector() { set_points(from, to); }

// ————————————————————————————————————————————————————————————————————————————
// 端点
// ————————————————————————————————————————————————————————————————————————————

void Connector::set_points(math::Point from, math::Point to) {
  fixed_points_ = true;
  fixed_from_ = from;
  fixed_to_ = to;
  source_.has_fixed = true;
  source_.fixed = from;
  source_.rect_fn = nullptr;
  source_.point_fn = nullptr;
  source_.node = nullptr;
  target_.has_fixed = true;
  target_.fixed = to;
  target_.rect_fn = nullptr;
  target_.point_fn = nullptr;
  target_.node = nullptr;
  mark_dirty();
}

void Connector::set_source_point(math::Point from) {
  fixed_points_ = true;
  fixed_from_ = from;
  source_.has_fixed = true;
  source_.fixed = from;
  source_.rect_fn = nullptr;
  source_.point_fn = nullptr;
  source_.node = nullptr;
  mark_dirty();
}

void Connector::set_target_point(math::Point to) {
  fixed_points_ = true;
  fixed_to_ = to;
  target_.has_fixed = true;
  target_.fixed = to;
  target_.rect_fn = nullptr;
  target_.point_fn = nullptr;
  target_.node = nullptr;
  mark_dirty();
}

void Connector::set_source(Element* element, Side side) {
  source_.node = element;
  source_.side = side;
  source_.rect_fn = nullptr;
  source_.point_fn = nullptr;
  source_.has_fixed = false;
  mark_dirty();
}

void Connector::set_target(Element* element, Side side) {
  target_.node = element;
  target_.side = side;
  target_.rect_fn = nullptr;
  target_.point_fn = nullptr;
  target_.has_fixed = false;
  mark_dirty();
}

void Connector::set_source_side(Side side) {
  source_.side = side;
  mark_dirty();
}

void Connector::set_target_side(Side side) {
  target_.side = side;
  mark_dirty();
}

void Connector::set_source_fn(std::function<std::optional<math::Rect>()> fn) {
  source_.rect_fn = std::move(fn);
  source_.point_fn = nullptr;
  source_.node = nullptr;
  source_.has_fixed = false;
  mark_dirty();
}

void Connector::set_target_fn(std::function<std::optional<math::Rect>()> fn) {
  target_.rect_fn = std::move(fn);
  target_.point_fn = nullptr;
  target_.node = nullptr;
  target_.has_fixed = false;
  mark_dirty();
}

void Connector::set_source_point_fn(std::function<std::optional<math::Point>()> fn) {
  source_.point_fn = std::move(fn);
  source_.rect_fn = nullptr;
  source_.node = nullptr;
  source_.has_fixed = false;
  mark_dirty();
}

void Connector::set_target_point_fn(std::function<std::optional<math::Point>()> fn) {
  target_.point_fn = std::move(fn);
  target_.rect_fn = nullptr;
  target_.node = nullptr;
  target_.has_fixed = false;
  mark_dirty();
}

auto Connector::resolve_endpoint(const Endpoint& endpoint, math::Point fallback,
                                 math::Point toward) const
    -> std::optional<std::pair<math::Point, math::Point>> {
  // 优先级：函数 > 元素 > 固定点 > 兜底（自身 bounds 的左下到右上）。
  if (endpoint.rect_fn) {
    const auto rect = endpoint.rect_fn();
    if (!rect.has_value()) return std::nullopt;
    const math::Point anchor = anchor_of(*rect, endpoint.side, toward);
    return std::make_pair(anchor, outward_of(*rect, anchor));
  }
  if (endpoint.point_fn) {
    const auto point = endpoint.point_fn();
    if (!point.has_value()) return std::nullopt;
    return std::make_pair(*point, math::Point{0.0f, 0.0f});
  }
  if (endpoint.node != nullptr) {
    // 元素不在树上/被隐藏 → 这一刻没有端点，**整条线不画**。
    // 不用 `bounds()` 兜底的理由：未上树的元素 bounds 是 0×0，画出来是一条从 (0,0)
    // 出发的线——比"不画"更难查（用户看到的是"线跑到左上角去了"）。
    if (!endpoint.node->visible() || endpoint.node->bounds().is_empty()) return std::nullopt;
    // ⚠ 元素的 `bounds()` 是**画布绝对坐标**，而本组件的端点约定是**本地坐标**
    //（`paint_content` 会统一加回 `bounds_` 原点）。这里必须换到同一坐标系：
    // 少了这一步，端点会多偏移一个自身原点，且偏移量**随组件位置变**——
    // 表现为“部分卡片的连线对得上、部分差一截”（最难查的那类）。
    const math::Rect absolute = endpoint.node->bounds();
    const math::Rect rect{absolute.x - bounds_.x, absolute.y - bounds_.y, absolute.width,
                          absolute.height};
    const math::Point anchor = anchor_of(rect, endpoint.side, toward);
    return std::make_pair(anchor, outward_of(rect, anchor));
  }
  if (endpoint.has_fixed) return std::make_pair(endpoint.fixed, math::Point{0.0f, 0.0f});
  return std::make_pair(fallback, math::Point{0.0f, 0.0f});
}

auto Connector::anchor_of(math::Rect rect, Side side, math::Point toward) -> math::Point {
  Side effective = side;
  if (side == Side::Auto) {
    // 朝对方那一侧：按"从矩形中心指向对方"的**主分量**决定（对角线情形下 |dx| 与 |dy|
    // 接近时也只会选一条边，不会出现"两边都选成上下"的歧义）。
    const math::Point center = rect.center();
    const float dx = toward.x - center.x;
    const float dy = toward.y - center.y;
    if (std::abs(dx) >= std::abs(dy)) {
      effective = dx >= 0.0f ? Side::Right : Side::Left;
    } else {
      effective = dy >= 0.0f ? Side::Bottom : Side::Top;
    }
  }
  switch (effective) {
    case Side::Top: return math::Point{rect.center().x, rect.top()};
    case Side::Right: return math::Point{rect.right(), rect.center().y};
    case Side::Bottom: return math::Point{rect.center().x, rect.bottom()};
    case Side::Left: return math::Point{rect.left(), rect.center().y};
    case Side::Center:
    case Side::Auto: break;
  }
  return rect.center();
}

auto Connector::outward_of(math::Rect rect, math::Point anchor) -> math::Point {
  // 锚点只可能落在四条边的中点、或中心（`anchor_of` 的全部产出），所以判序无歧义。
  if (std::abs(anchor.y - rect.top()) < 0.5f) return math::Point{0.0f, -1.0f};
  if (std::abs(anchor.y - rect.bottom()) < 0.5f) return math::Point{0.0f, 1.0f};
  if (std::abs(anchor.x - rect.left()) < 0.5f) return math::Point{-1.0f, 0.0f};
  if (std::abs(anchor.x - rect.right()) < 0.5f) return math::Point{1.0f, 0.0f};
  return math::Point{0.0f, 0.0f};
}

// ————————————————————————————————————————————————————————————————————————————
// 形态与样式
// ————————————————————————————————————————————————————————————————————————————

void Connector::set_route(Route route) {
  if (route_ == route) return;
  route_ = route;
  mark_dirty();
}

void Connector::set_smooth_corners(bool smooth) {
  if (smooth_corners_ == smooth) return;
  smooth_corners_ = smooth;
  mark_dirty();
}

void Connector::set_min_segment(float length) {
  const float clamped = std::clamp(length, 0.0f, 400.0f);
  if (min_segment_ == clamped) return;
  min_segment_ = clamped;
  mark_dirty();
}

void Connector::set_curvature(float curvature) {
  const float clamped = std::clamp(curvature, 0.0f, 4.0f);
  if (curvature_ == clamped) return;
  curvature_ = clamped;
  mark_dirty();
}

void Connector::set_arrow(Arrow arrow) {
  if (arrow_ == arrow) return;
  arrow_ = arrow;
  mark_dirty();
}

void Connector::set_arrow_size(float size) {
  const float clamped = std::clamp(size, 2.0f, 40.0f);
  if (arrow_size_ == clamped) return;
  arrow_size_ = clamped;
  mark_dirty();
}

void Connector::set_width(float width) {
  const float clamped = std::clamp(width, 0.5f, 24.0f);
  if (width_ == clamped) return;
  width_ = clamped;
  mark_dirty();
}

void Connector::set_dash(std::vector<float> pattern) {
  // 非法输入**忽略**（而不是截成空）：`"6 4 0"` 的意图显然是虚线，静默变实线会让人
  // 以为"虚线没生效"。所以全零/负值/过长一律当没设置，保留上一次的有效样式。
  if (pattern.empty() || pattern.size() > 16U) return;
  for (const float value : pattern) {
    if (value <= 0.0f || value > 400.0f) return;
  }
  dash_ = std::move(pattern);
  mark_dirty();
}

void Connector::set_dash_csv(std::string_view csv) {
  const std::string_view trimmed = st::trim(csv);
  if (trimmed.empty()) {
    if (dash_.empty()) return;
    dash_.clear();
    mark_dirty();
    return;
  }
  std::vector<float> pattern;
  for (const std::string_view part : st::split_whitespace(trimmed)) {
    try {
      pattern.push_back(std::stof(std::string(part)));
    } catch (...) {
      return;   // 非法串：整条忽略，保持上一次的有效样式
    }
  }
  set_dash(std::move(pattern));
}

auto Connector::dash_csv() const -> std::string {
  std::string out;
  for (std::size_t i = 0; i < dash_.size(); ++i) {
    if (i > 0U) out.push_back(' ');
    out += std::format("{:g}", static_cast<double>(dash_[i]));   // 去尾零：6.0 写成 6
  }
  return out;
}

void Connector::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  explicit_color_.reset();
  mark_dirty();
}

auto Connector::set_color_hex(std::string_view hex) -> bool {
  const std::string_view trimmed = st::trim(hex);
  if (trimmed.empty()) {
    explicit_color_.reset();
    mark_dirty();
    return true;
  }
  const auto parsed = svg::parse_color(trimmed);
  if (!parsed.has_value()) return false;   // 非法色值如实拒绝（不假装改了）
  explicit_color_ = *parsed;
  mark_dirty();
  return true;
}

auto Connector::color_hex() const -> std::string {
  if (!explicit_color_.has_value()) return {};
  const math::Color c = *explicit_color_;
  return std::format("#{:02x}{:02x}{:02x}{:02x}", c.r, c.g, c.b, c.a);
}

void Connector::set_hit_tolerance(float tolerance) {
  const float clamped = std::clamp(tolerance, 0.0f, 32.0f);
  if (hit_tolerance_ == clamped) return;
  hit_tolerance_ = clamped;
  mark_dirty();   // 命中带变了要重绘（hover 高亮等视觉依赖它）
}

void Connector::set_label(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  mark_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 解析与路径
// ————————————————————————————————————————————————————————————————————————————

auto Connector::resolve() const -> Resolved {
  Resolved out;
  // 兜底：自身 bounds 的左上 → 右下（"什么都不设"时也能看到东西，便于 gallery 展示）。
  const math::Point from_fallback{bounds_.left(), bounds_.top()};
  const math::Point to_fallback{bounds_.right(), bounds_.bottom()};
  const math::Point seed_source = source_.has_fixed ? source_.fixed : from_fallback;
  const math::Point seed_target = target_.has_fixed ? target_.fixed : to_fallback;

  auto source_pair = resolve_endpoint(source_, from_fallback, seed_target);
  auto target_pair = resolve_endpoint(target_, to_fallback, seed_source);
  if (!source_pair.has_value() || !target_pair.has_value()) return out;

  // `Auto` 的判据是"朝对方"，而对方的锚点又依赖本端位置——**两趟**才能收敛：
  // 第一趟用对方的大致位置定自己这条边，第二趟用已定的锚点精算对方。
  // 单趟的失败形态：两端都是 Auto 且一左一右时，各自朝"兜底点"选边，右侧节点
  // 会从左边缘出线（线穿过自己的盒子）。
  if (source_.side == Side::Auto || target_.side == Side::Auto) {
    source_pair = resolve_endpoint(source_, from_fallback, target_pair->first);
    if (!source_pair.has_value()) return out;
    target_pair = resolve_endpoint(target_, to_fallback, source_pair->first);
    if (!target_pair.has_value()) return out;
  }

  out.ok = true;
  out.from = source_pair->first;
  out.to = target_pair->first;
  out.from_dir = normalize(source_pair->second);
  out.to_dir = normalize(target_pair->second);
  return out;
}

auto Connector::route_points(math::Point from, math::Point to, math::Point from_dir,
                             math::Point to_dir) const -> math::Path {
  // **端点没有法线时给一个默认出线方向**（固定坐标/点表达式都没有“边”可吸附）。
  //
  // 为什么必须补（实测）：`Curve` 路由的控制点沿出线方向外推，零向量会让控制点正好
  // 落在端点上——三次贝塞尔退化成直线，用户选“曲线”却得到一根斜线，而且**不会报错**。
  // 默认取水平出线（左→右 / 右→左，看两点相对位置）：这是流程图/思维导图的通行形态，
  // 也是“想画一根弧线”时最不容易画错的默认。
  if (length_of(from_dir) < kEpsilon) {
    from_dir = math::Point{to.x >= from.x ? 1.0f : -1.0f, 0.0f};
    // 但**别默认水平**当两端已经明确上下时：这里只补“没有方向”的那一端，
    // 另一端要是给了竖直法线（元素吸附到 top/bottom），水平出线会与它打个直角。
    if (length_of(to_dir) >= kEpsilon && std::abs(to_dir.y) > std::abs(to_dir.x)) {
      from_dir = math::Point{0.0f, to.y >= from.y ? 1.0f : -1.0f};
    }
  }
  if (length_of(to_dir) < kEpsilon) {
    to_dir = math::Point{from.x <= to.x ? 1.0f : -1.0f, 0.0f};
    if (length_of(from_dir) >= kEpsilon && std::abs(from_dir.y) > std::abs(from_dir.x)) {
      to_dir = math::Point{0.0f, from.y <= to.y ? 1.0f : -1.0f};
    }
  }

  math::Path path;
  switch (route_) {
    case Route::Straight: {
      path.move_to(from);
      path.line_to(to);
      return path;
    }
    case Route::Curve: {
      // 控制点沿**出线方向**外推：水平出线则曲线在端点处切线水平（这是"看着自然"的
      // 关键——直接连两点得到的曲线在端点处是斜的，像一根斜插进去的绳子）。
      // 外推距离取两端距离的 40%（下限 24px）：太短会贴成直线，太长会鼓出大包。
      const float span = length_of(math::Point{to.x - from.x, to.y - from.y});
      const float reach = std::max(span * 0.4f, 24.0f) * curvature_;
      const math::Point c1{from.x + from_dir.x * reach, from.y + from_dir.y * reach};
      const math::Point c2{to.x - to_dir.x * reach, to.y - to_dir.y * reach};
      path.move_to(from);
      path.cubic_to(c1, c2, to);
      return path;
    }
    case Route::Orthogonal: {
      const bool horizontal_exit = std::abs(from_dir.x) >= std::abs(from_dir.y) ||
                                   length_of(from_dir) < kEpsilon;
      const bool vertical_enter = std::abs(to_dir.y) > std::abs(to_dir.x);
      std::vector<math::Point> points;
      points.push_back(from);
      if (horizontal_exit && !vertical_enter) {
        // 水平出、水平入：中间竖着走一段（两个折点）。
        const float mid_x = (from.x + to.x) * 0.5f;
        if (std::abs(mid_x - from.x) >= min_segment_ * 0.5f) {
          points.push_back(math::Point{mid_x, from.y});
          points.push_back(math::Point{mid_x, to.y});
        }
      } else if (!horizontal_exit && vertical_enter) {
        // 竖直出、竖直入：中间横着走一段。
        const float mid_y = (from.y + to.y) * 0.5f;
        if (std::abs(mid_y - from.y) >= min_segment_ * 0.5f) {
          points.push_back(math::Point{from.x, mid_y});
          points.push_back(math::Point{to.x, mid_y});
        }
      } else if (horizontal_exit) {
        points.push_back(math::Point{to.x, from.y});   // 一个转角
      } else {
        points.push_back(math::Point{from.x, to.y});
      }
      points.push_back(to);

      // 去掉重合点：重合点会让描边生成零长段，并在转角处补出一个圆点（看起来像线上有节）。
      std::vector<math::Point> cleaned;
      cleaned.reserve(points.size());
      for (const math::Point& point : points) {
        if (!cleaned.empty() &&
            length_of(math::Point{point.x - cleaned.back().x, point.y - cleaned.back().y}) <
                kEpsilon) {
          continue;
        }
        cleaned.push_back(point);
      }
      if (cleaned.size() < 2U) {
        path.move_to(from);
        path.line_to(to);
        return path;
      }
      if (!smooth_corners_) {
        path.move_to(cleaned.front());
        for (std::size_t i = 1; i < cleaned.size(); ++i) path.line_to(cleaned[i]);
        return path;
      }
      // 圆角折线：每个转角按"相邻两段的较小一半"取半径（上下限夹取），用二次贝塞尔
      // 把角切掉——90° 硬角看起来像表格线，圆弧转角才是流程图的读法。
      path.move_to(cleaned.front());
      for (std::size_t i = 1; i + 1 < cleaned.size(); ++i) {
        const math::Point prev = cleaned[i - 1U];
        const math::Point corner = cleaned[i];
        const math::Point next = cleaned[i + 1U];
        const float len_prev = length_of(math::Point{corner.x - prev.x, corner.y - prev.y});
        const float len_next = length_of(math::Point{next.x - corner.x, next.y - corner.y});
        const float radius = std::clamp(std::min(len_prev, len_next) * 0.5f, kMinCornerRadius,
                                        kMaxCornerRadius);
        const math::Point dir_prev = normalize(math::Point{corner.x - prev.x, corner.y - prev.y});
        const math::Point dir_next = normalize(math::Point{next.x - corner.x, next.y - corner.y});
        path.line_to(
            math::Point{corner.x - dir_prev.x * radius, corner.y - dir_prev.y * radius});
        path.quad_to(corner,
                     math::Point{corner.x + dir_next.x * radius, corner.y + dir_next.y * radius});
      }
      path.line_to(cleaned.back());
      return path;
    }
  }
  path.move_to(from);
  path.line_to(to);
  return path;
}

auto Connector::build_path() const -> math::Path {
  const Resolved resolved = resolve();
  if (!resolved.ok) return {};
  // 端点坐标 → **画布绝对坐标**：本组件 `bounds_` 的原点。框架约定自绘几何一律用
  // 绝对坐标（`element.hpp` 的 `paint_content` 注释：按局部坐标画会整块位移）。
  const math::Point from{resolved.from.x + bounds_.x, resolved.from.y + bounds_.y};
  const math::Point to{resolved.to.x + bounds_.x, resolved.to.y + bounds_.y};
  return route_points(from, to, resolved.from_dir, resolved.to_dir);
}

auto Connector::path_length() const -> float {
  const math::Path path = build_path();
  if (path.is_empty()) return 0.0f;
  float total = 0.0f;
  for (const math::Polyline& polyline : path.flatten(0.25f)) {
    for (std::size_t i = 1; i < polyline.points.size(); ++i) {
      total += length_of(math::Point{polyline.points[i].x - polyline.points[i - 1U].x,
                                     polyline.points[i].y - polyline.points[i - 1U].y});
    }
  }
  return total;
}

auto Connector::path_point_count() const -> std::size_t { return build_path().raw_points().size(); }

auto Connector::distance_to_line(math::Point point) const -> float {
  const math::Path path = build_path();
  if (path.is_empty()) return 1.0e9f;
  float best = 1.0e9f;
  for (const math::Polyline& polyline : path.flatten(0.25f)) {
    if (polyline.points.size() == 1U) {
      best = std::min(best, length_of(math::Point{point.x - polyline.points[0].x,
                                                  point.y - polyline.points[0].y}));
      continue;
    }
    for (std::size_t i = 1; i < polyline.points.size(); ++i) {
      best = std::min(best,
                      distance_point_segment(point, polyline.points[i - 1U], polyline.points[i]));
    }
  }
  return best;
}

auto Connector::source_ok() const -> bool { return resolve().ok; }

auto Connector::target_ok() const -> bool { return resolve().ok; }

auto Connector::resolved_from() const -> math::Point { return resolve().from; }

auto Connector::resolved_to() const -> math::Point { return resolve().to; }

void Connector::refresh() {
  mark_dirty();
  // 端点移动时"旧线"可能落在本组件 bounds 之外（上一帧端点更远）——那块区域的旧像素
  // 没人清，表现为"拖动后留下一条残影"。所以把**上一帧画过的路径包围盒**（外扩到线宽 +
  // 箭头尺寸）与当前 bounds 一起上报。
  if (last_path_valid_) {
    const float pad = std::max(width_, arrow_size_) + 4.0f;
    record_damage_rect(last_path_bounds_.inflate(pad));
  }
  record_damage();
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void Connector::apply_theme(const Theme& theme) {
  stroke_color_ = explicit_color_.has_value() ? *explicit_color_ : tone_color(theme, tone_);
  label_color_ = theme.colors().text;
  const math::Color surface = theme.colors().surface;
  // 标注底衬：面板色 + 高 alpha。压在线上时靠它把线遮住，字才读得清（否则一条
  // 细线正好穿过文字中缝，读起来像"字被划掉了"）。
  label_background_ = math::Color{surface.r, surface.g, surface.b, 0xE6};
  style_.font_size = theme.metrics().font_xs;
  style_.color = label_color_;
  apply_text_overrides();
}

void Connector::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  // 线通常 `style().absolute = true`（不进 flex 流）。绝对定位下元素的尺寸取自己的
  // `width`/`height`，`kAuto` 时沿用这里的测量值——所以**必须铺满父级可用空间**：
  // 量成 0×0 的话，`Element::paint` 的视口剔除会把它当"不可见"跳过（端点判定又
  // 依赖 bounds 原点），表现为"组件加了但画面上什么都没有"。
  measured_ = math::Size{fill_width(constraints, style_.width, 0.0f),
                         fill_height(constraints, style_.height, 0.0f)};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Connector::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const Resolved resolved = resolve();
  if (!resolved.ok) return;   // 端点不在场：整条不画（见 `resolve_endpoint` 的理由）

  const math::Point from{resolved.from.x + bounds_.x, resolved.from.y + bounds_.y};
  const math::Point to{resolved.to.x + bounds_.x, resolved.to.y + bounds_.y};
  const math::Path path = route_points(from, to, resolved.from_dir, resolved.to_dir);
  if (path.is_empty()) return;
  // 记下本帧几何：`refresh()` 靠它把旧线的残影也报进损坏区（见那里的说明）。
  last_path_bounds_ = path.flattened_bounds(0.25f);
  last_path_valid_ = true;

  const raster::Paint paint = raster::Paint::solid(stroke_color_);
  const raster::StrokeStyle stroke_style{raster::LineCap::Round, raster::LineJoin::Round};

  if (dash_.empty()) {
    canvas.stroke_path(path, paint, width_, stroke_style);
  } else {
    for (const math::Polyline& polyline : path.flatten(0.25f)) {
      append_dashed(canvas, polyline.points, dash_, paint, width_, stroke_style);
    }
  }

  // 箭头方向取**端点处的切向**（不是两端直线方向）：正交/曲线路由下两者不同，
  // 用后者会让箭头斜着扎进端点。
  if (arrow_ != Arrow::None) {
    const std::vector<math::Polyline> polylines = path.flatten(0.25f);
    if (!polylines.empty() && polylines.front().points.size() >= 2U) {
      const auto& points = polylines.front().points;
      if (arrow_ == Arrow::Start || arrow_ == Arrow::Both) {
        const math::Point tip = points.front();
        const math::Point back = points[1U];
        canvas.fill_path(
            arrow_path(tip, math::Point{tip.x - back.x, tip.y - back.y}, arrow_size_), paint);
      }
      if (arrow_ == Arrow::End || arrow_ == Arrow::Both) {
        const math::Point tip = points.back();
        const math::Point back = points[points.size() - 2U];
        canvas.fill_path(
            arrow_path(tip, math::Point{tip.x - back.x, tip.y - back.y}, arrow_size_), paint);
      }
    }
  }

  // 中点标注：先铺底衬再画字。
  if (!label_.empty()) {
    const TextPort& port = text_port_of(context);
    const float size = style_.font_size;
    const float text_width = port.measure_width(label_, size);
    const float line_height = port.line_height(size);
    const math::Point mid{(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f};
    const math::Rect box{mid.x - text_width * 0.5f - 4.0f, mid.y - line_height * 0.5f - 1.0f,
                         text_width + 8.0f, line_height + 2.0f};
    canvas.fill_rect(box, raster::Paint::solid(label_background_), 3.0f);
    components_internal::draw_line(context, canvas, label_, box, size, label_color_);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 命中与事件
// ————————————————————————————————————————————————————————————————————————————

auto Connector::hit_test(math::Point point) const noexcept -> bool {
  if (!visible()) return false;
  // 命中区 = 线附近 `hit_tolerance` 像素的**窄带**，不是整块 bounds。
  // 为什么必须这样：线常以 `absolute` 叠在画布层、且 bounds 铺满父级——按矩形命中
  // 会把整片区域的点击吞掉（"压在下面的按钮点不动"）。窄带命中与"看得见的线才可点"
  // 一致，也正是画布类工具里连线的手感。
  try {
    return distance_to_line(point) <= std::max(hit_tolerance_, width_ * 0.5f + 1.0f);
  } catch (...) {
    // `hit_test` 是 noexcept：端点表达式（用户 lambda）抛异常时**不崩**，当未命中。
    return false;
  }
}

auto Connector::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!on_line_event) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::MouseMove:
    case EventKind::MouseDown:
    case EventKind::MouseUp:
    case EventKind::Click:
    case EventKind::DoubleClick:
      // 与 `hit_test` 同源：线附近才转交（框架只会把命中到本元素的事件派发过来，
      // 但 `absolute` 铺满画布时命中已按窄带算——这里再确认一次，保证"画出的线"
      // 与"能点的线"是同一个几何）。
      if (!hit_test(event.position)) return false;
      return on_line_event(event);
    case EventKind::HoverOut:
      return on_line_event(event);   // 离开通知照常转交（不再判几何）
    default: return false;
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 语义与属性面
// ————————————————————————————————————————————————————————————————————————————

auto Connector::semantics_text() const -> std::string {
  const Resolved resolved = resolve();
  if (!resolved.ok) return "连线（端点不可用）";
  return std::format("{} 连线 · {:.0f}px{}", route_name(route_),
                     static_cast<double>(path_length()),
                     label_.empty() ? std::string{} : " · " + label_);
}

auto Connector::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "route") return std::string(route_name(route_));
  if (name == "from" || name == "to") {
    const Resolved resolved = resolve();
    const math::Point point = name == "from" ? resolved.from : resolved.to;
    return std::format("{:.1f},{:.1f}", static_cast<double>(point.x),
                       static_cast<double>(point.y));
  }
  if (name == "arrow") return std::string(arrow_name(arrow_));
  if (name == "arrow_size") return std::format("{:g}", static_cast<double>(arrow_size_));
  if (name == "dash") return dash_csv();
  if (name == "width") return std::format("{:g}", static_cast<double>(width_));
  if (name == "smooth") return smooth_corners_ ? "true" : "false";
  if (name == "curvature") return std::format("{:g}", static_cast<double>(curvature_));
  if (name == "min_segment") return std::format("{:g}", static_cast<double>(min_segment_));
  if (name == "tone") return std::string(tone_name(tone_));
  if (name == "stroke_color") return color_hex();
  if (name == "hit_tolerance") return std::format("{:g}", static_cast<double>(hit_tolerance_));
  if (name == "label") return label_;
  if (name == "source_side") return std::string(side_name(source_.side));
  if (name == "target_side") return std::string(side_name(target_.side));
  // —— 只读 ——
  if (name == "source_ok") return source_ok() ? "true" : "false";
  if (name == "target_ok") return target_ok() ? "true" : "false";
  if (name == "path_length") return std::format("{:.1f}", static_cast<double>(path_length()));
  return Element::get_property(name);
}

auto Connector::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "route") {
    if (value == "straight") set_route(Route::Straight);
    else if (value == "orthogonal") set_route(Route::Orthogonal);
    else if (value == "curve") set_route(Route::Curve);
    else return false;
    return true;
  }
  if (name == "from" || name == "to") {
    const auto point = parse_point(value);
    if (!point.has_value()) return false;
    if (name == "from") set_source_point(*point);
    else set_target_point(*point);
    return true;
  }
  if (name == "arrow") {
    if (value == "none") set_arrow(Arrow::None);
    else if (value == "start") set_arrow(Arrow::Start);
    else if (value == "end") set_arrow(Arrow::End);
    else if (value == "both") set_arrow(Arrow::Both);
    else return false;
    return true;
  }
  if (name == "arrow_size" || name == "width" || name == "curvature" || name == "min_segment" ||
      name == "hit_tolerance") {
    float parsed = 0.0f;
    try {
      parsed = std::stof(std::string(st::trim(value)));
    } catch (...) {
      return false;
    }
    if (name == "arrow_size") set_arrow_size(parsed);
    else if (name == "width") set_width(parsed);
    else if (name == "curvature") set_curvature(parsed);
    else if (name == "min_segment") set_min_segment(parsed);
    else set_hit_tolerance(parsed);
    return true;
  }
  if (name == "dash") {
    set_dash_csv(value);
    return true;
  }
  if (name == "smooth") {
    if (value == "true" || value == "1") set_smooth_corners(true);
    else if (value == "false" || value == "0") set_smooth_corners(false);
    else return false;
    return true;
  }
  if (name == "tone") {
    const auto parsed = components_internal::tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  if (name == "stroke_color") return set_color_hex(value);
  if (name == "label") {
    set_label(std::string(value));
    return true;
  }
  if (name == "source_side" || name == "target_side") {
    Side parsed = Side::Auto;
    if (value == "center") parsed = Side::Center;
    else if (value == "top") parsed = Side::Top;
    else if (value == "right") parsed = Side::Right;
    else if (value == "bottom") parsed = Side::Bottom;
    else if (value == "left") parsed = Side::Left;
    else if (value == "auto") parsed = Side::Auto;
    else return false;
    if (name == "source_side") set_source_side(parsed);
    else set_target_side(parsed);
    return true;
  }
  return Element::set_property(name, value);
}

auto Connector::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"route", "from", "to", "arrow", "arrow_size", "dash", "width",
                                  "smooth", "curvature", "min_segment", "tone", "stroke_color",
                                  "hit_tolerance", "label", "source_side", "target_side"});
}

auto Connector::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"source_ok", "target_ok", "path_length"});
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::Connector);
