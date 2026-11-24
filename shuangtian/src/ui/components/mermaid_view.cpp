#include "st/ui/components/mermaid_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/math/path.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/wheel.hpp"

#include "components_internal.hpp"

namespace st::ui {
namespace {

using st::md::mermaid::DiagramKind;
using st::md::mermaid::NodeShape;

constexpr double kPi = std::numbers::pi_v<double>;
/// 线宽（逻辑像素）。0.5 的倍数与主题 `border_width` 同档，1px 发丝在两条腿上都不碎。
constexpr float kEdgeWidth = 1.4f;
constexpr float kFrameWidth = 1.0f;
constexpr float kArrowLength = 8.0f;
constexpr float kArrowHalfWidth = 3.6f;

/// 鼠标按键（**后端口径**：1=左 / 2=右 / 3=中）。
///
/// ⚠ 不要写 0：图形 API（X11 的 `Button1`）常从 0 起，而本框架与真实后端
/// 一律 1 起。写错的表现是“左键拖不动”而代码看着完全正确（`three_view.cpp` 踩过）。
constexpr int kLeftButton{1};

/// 把一段折线按 `on/off` 切成虚线段（光栅层没有 dash，只能在路径层分段）。
void stroke_polyline(raster::Surface& canvas, const std::vector<math::Point>& points,
                     math::Color color, float width, bool dashed) {
  if (points.size() < 2U) return;
  if (!dashed) {
    math::Path path;
    path.move_to(points.front());
    for (std::size_t index = 1; index < points.size(); ++index) path.line_to(points[index]);
    canvas.stroke_path(path, raster::Paint::solid(color), width);
    return;
  }
  constexpr float kOn = 6.0f;
  constexpr float kOff = 4.0f;
  float phase = 0.0f;
  for (std::size_t index = 1; index < points.size(); ++index) {
    const math::Point from = points[index - 1U];
    const math::Point to = points[index];
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0f) continue;
    float travelled = 0.0f;
    while (travelled < length) {
      const float period = kOn + kOff;
      const float position = std::fmod(phase + travelled, period);
      const bool visible = position < kOn;
      const float step = visible ? (kOn - position) : (period - position);
      const float next = std::min(travelled + step, length);
      if (visible) {
        const auto lerp_point = [&](float t) {
          return math::Point{from.x + dx * t, from.y + dy * t};
        };
        math::Path dash;
        dash.move_to(lerp_point(travelled / length));
        dash.line_to(lerp_point(next / length));
        canvas.stroke_path(dash, raster::Paint::solid(color), width);
      }
      travelled = next;
    }
    phase = std::fmod(phase + length, kOn + kOff);
  }
}

/// 箭头（实心三角，尖端在 `tip`，方向由 `from → tip` 决定）。
void fill_arrow(raster::Surface& canvas, math::Point tip, math::Point from, math::Color color) {
  const float dx = tip.x - from.x;
  const float dy = tip.y - from.y;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length <= 0.001f) return;
  const float ux = dx / length;
  const float uy = dy / length;
  const math::Point base{tip.x - ux * kArrowLength, tip.y - uy * kArrowLength};
  const math::Point left{base.x - uy * kArrowHalfWidth, base.y + ux * kArrowHalfWidth};
  const math::Point right{base.x + uy * kArrowHalfWidth, base.y - ux * kArrowHalfWidth};
  math::Path path;
  path.move_to(tip);
  path.line_to(left);
  path.line_to(right);
  path.close();
  canvas.fill_path(path, raster::Paint::solid(color));
}

/// 竖排多行文字（在 `box` 内**水平居中、垂直居中**）。
void draw_lines_centered(const TextPort& port, raster::Surface& canvas,
                         const std::vector<std::string>& lines, math::Rect box, float font_size,
                         math::Color color, float line_gap) {
  if (lines.empty() || box.is_empty()) return;
  const float line_height = port.line_height(font_size) * line_gap;
  const float total = line_height * static_cast<float>(lines.size());
  float y = box.y + (box.height - total) * 0.5f;
  for (const std::string& line : lines) {
    const float width = port.measure_width(line, font_size);
    port.draw(canvas, line, math::Point{box.x + (box.width - width) * 0.5f, y}, font_size, color);
    y += line_height;
  }
}

/// 形状轮廓路径（内容坐标 → 屏幕坐标由调用方在外层做变换，这里只按屏幕矩形画）。
auto node_path(NodeShape shape, math::Rect rect) -> math::Path {
  math::Path path;
  switch (shape) {
    case NodeShape::Rounded:
      path.add_rounded_rect(rect, std::min(rect.width, rect.height) * 0.22f);
      break;
    case NodeShape::Stadium:
      path.add_rounded_rect(rect, rect.height * 0.5f);
      break;
    case NodeShape::Diamond:
      path.move_to(math::Point{rect.center().x, rect.y});
      path.line_to(math::Point{rect.right(), rect.center().y});
      path.line_to(math::Point{rect.center().x, rect.bottom()});
      path.line_to(math::Point{rect.x, rect.center().y});
      path.close();
      break;
    case NodeShape::Circle: {
      const float radius = std::min(rect.width, rect.height) * 0.5f;
      path.add_circle(rect.center(), radius);
      break;
    }
    case NodeShape::Rect:
      path.add_rounded_rect(rect, 4.0f);
      break;
  }
  return path;
}

/// 饼图扇形的路径：从圆心沿外弧扫过一段（0 弧度 = 12 点、顺时针为正）。
///
/// ## 外弧必须是贝塞尔，不能是折线
///
/// 框架的 `math::Path` 只有 move/line/quad/cubic——想要“真圆”就得自己用
/// **标准 kappa 三次曲线**逼近（与 `math::Path::add_circle` 同一套数值），
/// 而不是把弧切成很多小直线。折线逼近在圆轮廓上永远能看出折角：
/// 实测 16 段（22.5°）时轮廓就是明显的多边形，栅格化后还带串动。
///
/// 本函数把弧切到每段 ≤ 90°——四分之一圆弧用 `kappa = 4/3·tan(θ/4)` 的单段
/// 三次曲线逼近，最大径向误差约 0.027% 半径（在 200px 半径上不到 0.06px）。
///
/// ## 扇形的两条直边不能省
///
/// `move_to(center)` 之后**必须**先 `line_to(弧起点)`：直接接 `cubic_to` 会把
/// 圆心当成贝塞尔起点，得到的是一条从圆心弯向弧的曲线，闭合后形状不再是扇形
/// （两条半径边变成一道弧）——实测就是这个错法。
auto slice_path(math::Point center, float radius, double start, double sweep) -> math::Path {
  math::Path path;
  path.move_to(center);
  const auto at = [&](double angle) {
    return math::Point{center.x + radius * static_cast<float>(std::sin(angle)),
                       center.y - radius * static_cast<float>(std::cos(angle))};
  };
  // 切线方向（沿弧前进方向，屏幕坐标系顺时针）。
  const auto tangent = [](double angle) {
    return math::Point{static_cast<float>(std::cos(angle)),
                       static_cast<float>(std::sin(angle))};
  };
  const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (kPi * 0.5))));
  const double step = sweep / static_cast<double>(segments);
  const double kappa = 4.0 / 3.0 * std::tan(step * 0.25);
  // 半径线1：圆心 → 弧起点（扇形的起始直边）。
  path.line_to(at(start));
  double angle = start;
  for (int index = 0; index < segments; ++index) {
    const math::Point from = at(angle);
    const math::Point to = at(angle + step);
    const math::Point out = tangent(angle);
    const math::Point in = tangent(angle + step);
    const float handle = static_cast<float>(kappa) * radius;
    path.cubic_to(math::Point{from.x + out.x * handle, from.y + out.y * handle},
                  math::Point{to.x - in.x * handle, to.y - in.y * handle}, to);
    angle += step;
  }
  // `close()` 补上半径线2（弧终点 → 圆心）。
  path.close();
  return path;
}

}  // namespace

MermaidView::MermaidView() { set_text_selectable(false); }

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void MermaidView::set_source(std::string source) {
  if (source_ == source) return;
  source_ = std::move(source);
  // 换了内容就回到初始视角：新图的坐标系与旧图无对应关系，
  // 保留平移只会把人带到空白处。
  pan_x_ = 0.0f;
  pan_y_ = 0.0f;
  relayout();
}

void MermaidView::relayout() {
  document_ = st::md::mermaid::parse(source_);
  // 还没拿到 `RenderContext` 时先按字号估宽（只为让第一帧就有非零尺寸，
  // 避免把图缩成一个点）；`measure` 会用真实端口重排一次覆盖它。
  apply_layout([](std::string_view text, float font_size) {
    // 0.58 × 字号 的 ASCII 宽、汉字按 1.0 × 字号（估的，只要量级对）。
    float width = 0.0f;
    std::size_t index = 0;
    while (index < text.size()) {
      const auto byte = static_cast<unsigned char>(text[index]);
      width += byte >= 0x80U ? font_size : font_size * 0.58f;
      // 跳过 UTF-8 续字节（否则一个汉字会被数成 3 个字宽）
      while (index + 1U < text.size() &&
             (static_cast<unsigned char>(text[index + 1U]) & 0xC0U) == 0x80U) {
        ++index;
      }
      ++index;
    }
    return width;
  });
}

void MermaidView::apply_layout(const st::md::mermaid::MeasureText& measure) {
  flow_ = {};
  sequence_ = {};
  pie_ = {};
  content_size_ = math::Size{};
  switch (document_.kind) {
    case DiagramKind::Flowchart:
      flow_ = st::md::mermaid::layout_flowchart(document_.flowchart, options_, measure);
      content_size_ = flow_.size;
      break;
    case DiagramKind::Sequence:
      sequence_ = st::md::mermaid::layout_sequence(document_.sequence, options_, measure);
      content_size_ = sequence_.size;
      break;
    case DiagramKind::Pie:
      pie_ = st::md::mermaid::layout_pie(document_.pie, options_, measure);
      content_size_ = pie_.size;
      break;
    case DiagramKind::Unknown:
      break;
  }
  update_view();
  mark_layout_dirty();
}

auto MermaidView::first_diagnostic() const -> std::string {
  return document_.diagnostics.empty() ? std::string{} : document_.diagnostics.front();
}

void MermaidView::set_zoom(float zoom) {
  const float clamped = std::clamp(zoom, kMinZoom, kMaxZoom);
  if (std::abs(clamped - zoom_) < 1e-6f) return;
  zoom_ = clamped;
  update_view();
  mark_dirty();
}

void MermaidView::set_fit(bool fit) {
  if (fit_ == fit) return;
  fit_ = fit;
  update_view();
  mark_dirty();
}

void MermaidView::set_layout_font_size(float size) {
  const float clamped = std::clamp(size, 6.0f, 64.0f);
  if (std::abs(options_.font_size - clamped) < 1e-6f) return;
  options_.font_size = clamped;
  relayout();
}

void MermaidView::pan_by(float dx, float dy) {
  pan_x_ += dx;
  pan_y_ += dy;
  update_view();
  mark_dirty();
}

void MermaidView::zoom_at(math::Point anchor, float factor) {
  const float clamped = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
  if (std::abs(clamped - zoom_) < 1e-6f) return;
  // ① 先把锚点换成**内容坐标**（用缩放前的倍率）
  const math::Point origin = content_origin();
  const float content_x = (anchor.x - origin.x) / scale_;
  const float content_y = (anchor.y - origin.y) / scale_;
  // ② 改倍率 ③ 反解 `pan`，让同一个内容点重新落在锚点下。
  //
  // 为何要反解而不能只改 `zoom`：`content_origin()` 里的居中偏移包含
  // `bounds - content × scale` 这一项，倍率一变它就变——只改 `zoom`
  // 等于以**视口中心**为轴缩放，指针下的东西会跑走。
  zoom_ = clamped;
  update_view();
  const math::Point moved_origin = content_origin();
  pan_x_ += (anchor.x - moved_origin.x) - content_x * scale_;
  pan_y_ += (anchor.y - moved_origin.y) - content_y * scale_;
  update_view();
  mark_dirty();
}

void MermaidView::set_pan(float x, float y) {
  pan_x_ = x;
  pan_y_ = y;
  update_view();
  mark_dirty();
}

void MermaidView::reset_view() {
  zoom_ = 1.0f;
  pan_x_ = 0.0f;
  pan_y_ = 0.0f;
  update_view();
  mark_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 视图变换
// ————————————————————————————————————————————————————————————————————————————

auto MermaidView::content_origin() const -> math::Point {
  const float scaled_width = content_size_.width * scale_;
  const float scaled_height = content_size_.height * scale_;
  return math::Point{bounds_.x + (bounds_.width - scaled_width) * 0.5f + pan_x_,
                     bounds_.y + (bounds_.height - scaled_height) * 0.5f + pan_y_};
}

void MermaidView::update_view() {
  float fit_scale = 1.0f;
  if (fit_ && !bounds_.is_empty() && content_size_.width > 0.0f && content_size_.height > 0.0f) {
    // **只缩不放**：小图放大到撑满会把 1px 线画成 3px 粗，看着像低清预览。
    fit_scale = std::min(bounds_.width / content_size_.width,
                         bounds_.height / content_size_.height);
    fit_scale = std::min(fit_scale, 1.0f);
    fit_scale = std::max(fit_scale, 0.05f);
  }
  scale_ = fit_scale * zoom_;
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void MermaidView::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  surface_ = colors.surface;
  border_ = colors.border;
  text_color_ = colors.text;
  text_muted_ = colors.text_muted;
  edge_color_ = colors.text_muted;
  accent_ = colors.primary;
  label_background_ = colors.surface;
  // 饼图配色：主题里的语义色 + 主色系，足够八片不重样（超出后循环取用）。
  slice_colors_ = {colors.primary,   colors.success, colors.warning, colors.danger,
                   colors.accent,    colors.text_muted, colors.border_strong,
                   colors.primary_hover};
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.color = text_color_;
  apply_text_overrides();
}

void MermaidView::measure(const RenderContext& context, const Constraints& constraints) {
  // 关键一步：**用真实端口重新布局**。布局结果依赖字宽，而字宽只有这里拿得到。
  // 没这一步会得到“用估算宽度排好版、用真实宽度画字”的错位（标签溢出节点框）。
  const TextPort& port = components_internal::text_port_of(context);
  apply_layout([&port](std::string_view text, float font_size) {
    return port.measure_width(text, font_size);
  });
  const float natural_width = content_size_.width > 0.0f ? content_size_.width : 240.0f;
  const float natural_height = content_size_.height > 0.0f ? content_size_.height : 140.0f;
  float width = fill_width(constraints, style_.width, natural_width);
  float height = fill_height(constraints, style_.height, natural_height);
  if (width <= 0.0f) width = natural_width;
  if (height <= 0.0f) height = natural_height;
  measured_ = math::Size{std::max(width, 0.0f), std::max(height, 0.0f)};
}

void MermaidView::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  update_view();
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void MermaidView::paint_placeholder(const RenderContext& context, raster::Surface& canvas) const {
  const TextPort& port = components_internal::text_port_of(context);
  const float size = options_.font_size;
  std::vector<std::string> lines;
  if (document_.kind == DiagramKind::Unknown) {
    lines.push_back(source_.empty() ? "（空内容）" : "暂不支持的图类型");
    for (const std::string& diagnostic : document_.diagnostics) lines.push_back(diagnostic);
  } else {
    lines.push_back("（没有可绘制的内容）");
    for (const std::string& diagnostic : document_.diagnostics) lines.push_back(diagnostic);
  }
  draw_lines_centered(port, canvas, lines, bounds_.inset(math::Insets::all(8.0f)), size,
                      text_muted_, options_.line_gap);
}

void MermaidView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const TextPort& port = components_internal::text_port_of(context);
  const float font = options_.font_size * scale_;

  // 底板：与 `GraphView` 同口径——图有独立底色，避免与周围文字混在一起。
  components_internal::fill_round_rect(canvas, bounds_, 6.0f,
                                       surface_.with_alpha_f(1.0f));
  components_internal::paint_outline(canvas, bounds_, 6.0f, border_, kFrameWidth);

  canvas.push_clip_rect(bounds_.inset(math::Insets::all(2.0f)));
  const math::Point origin = content_origin();
  const auto to_screen = [&](math::Point point) {
    return math::Point{origin.x + point.x * scale_, origin.y + point.y * scale_};
  };

  if (content_size_.width <= 0.0f || content_size_.height <= 0.0f) {
    paint_placeholder(context, canvas);
    canvas.pop_clip();
    return;
  }

  switch (document_.kind) {
    case DiagramKind::Flowchart: {
      // ① 连线（先画，压在节点下面：箭头尖落进节点边缘时不该盖住节点的字）
      for (const auto& edge : flow_.edges) {
        std::vector<math::Point> points;
        points.reserve(edge.points.size());
        for (const math::Point point : edge.points) points.push_back(to_screen(point));
        if (points.size() < 2U) continue;
        stroke_polyline(canvas, points, edge_color_, std::max(kEdgeWidth * scale_, 1.0f),
                        edge.style == st::md::mermaid::EdgeStyle::Dashed);
        if (edge.arrow && !edge.self) {
          fill_arrow(canvas, points.back(), points[points.size() - 2U], edge_color_);
        }
        if (edge.has_label) {
          const math::Rect label = math::Rect::from_ltrb(
              origin.x + edge.label.x * scale_, origin.y + edge.label.y * scale_,
              origin.x + edge.label.right() * scale_, origin.y + edge.label.bottom() * scale_);
          components_internal::fill_round_rect(canvas, label, 3.0f, label_background_);
          draw_lines_centered(port, canvas, edge.label_lines, label, font, text_muted_,
                              options_.line_gap);
        }
      }
      // ② 节点
      for (std::size_t index = 0; index < flow_.nodes.size(); ++index) {
        const auto& node = flow_.nodes[index];
        const math::Rect rect = math::Rect::from_ltrb(
            origin.x + node.rect.x * scale_, origin.y + node.rect.y * scale_,
            origin.x + node.rect.right() * scale_, origin.y + node.rect.bottom() * scale_);
        const NodeShape shape = document_.flowchart.nodes[index].shape;
        const math::Path path = node_path(shape, rect);
        canvas.fill_path(path, raster::Paint::solid(surface_.lighten(0.06f)));
        canvas.stroke_path(path, raster::Paint::solid(border_), std::max(kEdgeWidth * scale_, 1.0f));
        // 文字区用布局层算好的 `text_rect`（已按形状缩到内接范围，与盒子尺寸同源）。
        const math::Rect text_box =
            math::Rect::from_ltrb(origin.x + node.text_rect.x * scale_,
                                  origin.y + node.text_rect.y * scale_,
                                  origin.x + node.text_rect.right() * scale_,
                                  origin.y + node.text_rect.bottom() * scale_);
        draw_lines_centered(port, canvas, node.lines, text_box, font, text_color_,
                            options_.line_gap);
      }
      break;
    }
    case DiagramKind::Sequence: {
      // ① 生命线
      const float top = origin.y + sequence_.lifeline_top * scale_;
      const float bottom = origin.y + sequence_.lifeline_bottom * scale_;
      for (const float lifeline : sequence_.lifelines) {
        const float x = origin.x + lifeline * scale_;
        stroke_polyline(canvas, {math::Point{x, top}, math::Point{x, bottom}},
                        border_, std::max(kEdgeWidth * scale_, 1.0f), true);
      }
      // ② 参与者头
      for (std::size_t index = 0; index < sequence_.participants.size(); ++index) {
        const auto& participant = sequence_.participants[index];
        const math::Rect rect = math::Rect::from_ltrb(
            origin.x + participant.rect.x * scale_, origin.y + participant.rect.y * scale_,
            origin.x + participant.rect.right() * scale_,
            origin.y + participant.rect.bottom() * scale_);
        components_internal::fill_round_rect(canvas, rect, 5.0f, surface_.lighten(0.08f));
        components_internal::paint_outline(canvas, rect, 5.0f, border_,
                                           std::max(kEdgeWidth * scale_, 1.0f));
        draw_lines_centered(port, canvas, participant.lines, rect, font, text_color_,
                            options_.line_gap);
      }
      // ③ 消息
      for (const auto& message : sequence_.messages) {
        const float y = origin.y + message.y * scale_;
        const float x_from = origin.x + message.x_from * scale_;
        const float x_to = origin.x + message.x_to * scale_;
        if (message.self) {
          // 自发消息：一条小折线回到原处（箭头朝左）。
          const float arm = 16.0f * scale_;
          const std::vector<math::Point> points{{x_from, y}, {x_from + arm, y},
                                               {x_from + arm, y + 12.0f * scale_},
                                               {x_from, y + 12.0f * scale_}};
          stroke_polyline(canvas, points, edge_color_, std::max(kEdgeWidth * scale_, 1.0f),
                          message.dashed);
          if (message.arrow) {
            fill_arrow(canvas, points.back(), points[points.size() - 2U], edge_color_);
          }
        } else {
          stroke_polyline(canvas, {math::Point{x_from, y}, math::Point{x_to, y}}, edge_color_,
                          std::max(kEdgeWidth * scale_, 1.0f), message.dashed);
          if (message.arrow) fill_arrow(canvas, math::Point{x_to, y}, math::Point{x_from, y},
                                        edge_color_);
        }
        if (!message.lines.empty()) {
          const math::Rect label = math::Rect::from_ltrb(
              origin.x + message.label.x * scale_, origin.y + message.label.y * scale_,
              origin.x + message.label.right() * scale_, origin.y + message.label.bottom() * scale_);
          components_internal::fill_round_rect(canvas, label, 3.0f, label_background_);
          draw_lines_centered(port, canvas, message.lines, label, font, text_color_,
                              options_.line_gap);
        }
      }
      break;
    }
    case DiagramKind::Pie: {
      const math::Point center{origin.x + pie_.center.x * scale_,
                               origin.y + pie_.center.y * scale_};
      const float radius = pie_.radius * scale_;
      for (std::size_t index = 0; index < pie_.slices.size(); ++index) {
        const auto& slice = pie_.slices[index];
        if (slice.sweep <= 0.0) continue;
        const math::Color color = slice_colors_.empty()
                                      ? accent_
                                      : slice_colors_[index % slice_colors_.size()];
        // 相邻扇形**各自抗锯齿**时，分界处会留下白缝（各自盖一层半透明，
        // 合成后不等于全不透明）。修法：**每一片**都把起点往回扩一点、终点也补回
        // 同样的角度（边界重叠），并把半径多画 0.5px 盖住圆外缘。
        //
        // ⚠ 不能特判第一片：圆周上第一个与最后一个是**相邻**的，漏了特判
        // 就等于在 12 点方向留一道白缝（实测就是这个位置最明显）。
        constexpr double kSliceOverlap = 0.004;   // ≈0.23°
        canvas.fill_path(slice_path(center, radius + 0.5f, slice.start_angle - kSliceOverlap,
                                    slice.sweep + kSliceOverlap * 2.0),
                         raster::Paint::solid(color));
      }
      // 外圈不再描边：每一片已自行多画 0.5px 并靠边界重叠消除白缝。
      //
      // ⚠ 曾经在这里画过一道圆描边，代价是**盖住了扇形的真实轮廓**：
      // 描边本身是真圆，于是扇形退化成折线也看不出来（像素回归测试恒绿——
      // 实测踩到）。想“把边缘抿干净”应当从前面的填充入手，不能在这里补。
      // 图例
      for (std::size_t index = 0; index < pie_.slices.size(); ++index) {
        const auto& slice = pie_.slices[index];
        const math::Color color = slice_colors_.empty()
                                      ? accent_
                                      : slice_colors_[index % slice_colors_.size()];
        const math::Rect legend = math::Rect::from_ltrb(
            origin.x + slice.legend.x * scale_, origin.y + slice.legend.y * scale_,
            origin.x + slice.legend.right() * scale_, origin.y + slice.legend.bottom() * scale_);
        const float swatch = std::min(legend.height * 0.6f, 12.0f * scale_);
        components_internal::fill_round_rect(
            canvas, math::Rect{legend.x, legend.y + (legend.height - swatch) * 0.5f, swatch, swatch},
            2.0f, color);
        const math::Rect text_area{legend.x + swatch + 6.0f * scale_, legend.y,
                                   legend.width - swatch - 6.0f * scale_, legend.height};
        std::vector<std::string> lines = slice.lines;
        draw_lines_centered(port, canvas, lines, text_area, font, text_color_,
                            options_.line_gap);
      }
      // 标题（在饼图上方、居左）
      if (!pie_.title_lines.empty()) {
        const math::Rect title = math::Rect::from_ltrb(
            origin.x + pie_.title_box.x * scale_, origin.y + pie_.title_box.y * scale_,
            origin.x + pie_.title_box.right() * scale_, origin.y + pie_.title_box.bottom() * scale_);
        float y = title.y + (title.height - port.line_height(font) *
                                                static_cast<float>(pie_.title_lines.size())) *
                                   0.5f;
        for (const std::string& line : pie_.title_lines) {
          port.draw(canvas, line, math::Point{title.x, y}, font, text_color_);
          y += port.line_height(font);
        }
      }
      break;
    }
    case DiagramKind::Unknown:
      paint_placeholder(context, canvas);
      break;
  }

  canvas.pop_clip();
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

auto MermaidView::on_event(const RenderContext& context, Event& event) -> bool {
  const bool inside = bounds_.contains(event.position);
  switch (event.kind) {
    case EventKind::Wheel: {
      if (!inside || event.wheel_delta == 0.0f) return false;
      // **滚轮 = 缩放**（以指针为锚），不要求 Ctrl。
      //
      // 为何改掉“Ctrl+滚轮才缩放”（实测）：图的阅读动作就是“看细节/看全局”，
      // 滚轮是最直接的手势；藏在 Ctrl 后面等于没实现——不告知就没人会发现。
      // 嵌套在滚动容器里时，图区内的滚轮被消费、区外照常滚页面（边界清楚）。
      if (wheel_zoom_) {
        zoom_at(event.position,
                event.wheel_delta > 0.0f ? kWheelZoomStep : 1.0f / kWheelZoomStep);
        return true;
      }
      // 关掉缩放后：退回纵向平移；内容没超出视口则让给外层（嵌套滚动的正解）。
      const float scaled_height = content_size_.height * scale_;
      if (scaled_height <= bounds_.height) return false;
      pan_by(0.0f, event.wheel_delta * wheel_step(context.theme));
      return true;
    }
    case EventKind::DoubleClick: {
      // 双击复位：拖到别处/放到很大之后，这是“回去”的通行手势
      //（与 `ThreeView` 同一约定）。
      if (!inside) return false;
      reset_view();
      return true;
    }
    case EventKind::MouseDown: {
      // 只吃**左键**：右键/中键在宿主里通常另有语义（上下文菜单），不该被图吞掉。
      if (!inside || !pan_enabled_ || event.button != kLeftButton) return false;
      dragging_ = true;
      drag_origin_ = event.position;
      drag_pan_x_ = pan_x_;
      drag_pan_y_ = pan_y_;
      drag_distance_ = 0.0f;
      return true;
    }
    case EventKind::MouseMove: {
      if (!dragging_) return false;
      const float dx = event.position.x - drag_origin_.x;
      const float dy = event.position.y - drag_origin_.y;
      drag_distance_ = std::max(drag_distance_, std::sqrt(dx * dx + dy * dy));
      pan_x_ = drag_pan_x_ + dx;
      pan_y_ = drag_pan_y_ + dy;
      mark_dirty();
      return true;
    }
    case EventKind::MouseUp: {
      if (!dragging_) return false;
      dragging_ = false;
      return true;
    }
    default:
      return false;
  }
}

auto MermaidView::cursor_at(math::Point point) const noexcept -> MouseCursor {
  if (!bounds_.contains(point) || !pan_enabled_) return MouseCursor::Arrow;
  // 能拖动的地方一律给 `Move`：光标是**系统级效果，截图里不存在**，
  // 不给它就没有任何“这里能拖”的视觉线索（用户只能靠试）。
  return MouseCursor::Move;
}

// ————————————————————————————————————————————————————————————————————————————
// 语义与属性面
// ————————————————————————————————————————————————————————————————————————————

auto MermaidView::semantics_text() const -> std::string {
  switch (document_.kind) {
    case DiagramKind::Flowchart:
      return std::format("流程图（{} 个节点 / {} 条连线）", document_.flowchart.nodes.size(),
                         document_.flowchart.edges.size());
    case DiagramKind::Sequence:
      return std::format("时序图（{} 个参与者 / {} 条消息）",
                         document_.sequence.participants.size(),
                         document_.sequence.messages.size());
    case DiagramKind::Pie:
      return std::format("饼图（{} 片）{}", document_.pie.slices.size(),
                         document_.pie.title.empty() ? std::string{} : "：" + document_.pie.title);
    case DiagramKind::Unknown:
      return "未识别的图";
  }
  return {};
}

auto MermaidView::semantics_value() const -> std::string {
  return std::string(st::md::mermaid::to_string(document_.kind));
}

auto MermaidView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "source") return source_;
  if (name == "kind") return std::string(st::md::mermaid::to_string(document_.kind));
  if (name == "zoom") return std::format("{:.3f}", zoom_);
  if (name == "fit") return std::string(fit_ ? kFlagTrue : "false");
  if (name == "font_size") return std::format("{:.1f}", options_.font_size);
  if (name == "pan_x") return std::format("{:.1f}", pan_x_);
  if (name == "pan_y") return std::format("{:.1f}", pan_y_);
  if (name == "scale") return std::format("{:.3f}", scale_);
  if (name == "dragging") return std::string(dragging_ ? kFlagTrue : "false");
  if (name == "pan_enabled") return std::string(pan_enabled_ ? kFlagTrue : "false");
  if (name == "wheel_zoom") return std::string(wheel_zoom_ ? kFlagTrue : "false");
  if (name == "error") return first_diagnostic();
  if (name == "diagnostics") return std::to_string(document_.diagnostics.size());
  if (name == "node_count") {
    return std::to_string(document_.kind == DiagramKind::Flowchart ? document_.flowchart.nodes.size()
                                                                  : document_.sequence.participants.size());
  }
  if (name == "edge_count") {
    return std::to_string(document_.kind == DiagramKind::Flowchart ? document_.flowchart.edges.size()
                                                                  : document_.sequence.messages.size());
  }
  if (name == "content_width") return std::format("{:.1f}", content_size_.width);
  if (name == "content_height") return std::format("{:.1f}", content_size_.height);
  return Element::get_property(name);
}

auto MermaidView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "source" || name == "text") {
    set_source(std::string(value));
    return true;
  }
  if (name == "zoom") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_zoom(static_cast<float>(*parsed));
    return true;
  }
  if (name == "fit") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_fit(*parsed);
    return true;
  }
  if (name == "font_size") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_layout_font_size(static_cast<float>(*parsed));
    return true;
  }
  if (name == "pan_x" || name == "pan_y") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    if (name == "pan_x") set_pan(static_cast<float>(*parsed), pan_y_);
    else set_pan(pan_x_, static_cast<float>(*parsed));
    return true;
  }
  if (name == "pan_enabled" || name == "wheel_zoom") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    if (name == "pan_enabled") set_pan_enabled(*parsed);
    else set_wheel_zoom(*parsed);
    return true;
  }
  return Element::set_property(name, value);
}

auto MermaidView::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal(
      {"source", "zoom", "fit", "font_size", "pan_x", "pan_y", "pan_enabled", "wheel_zoom"});
}

auto MermaidView::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"kind", "error", "diagnostics", "node_count",
                                           "edge_count", "content_width", "content_height",
                                           "scale", "dragging"});
}

auto MermaidView::invoke_action(std::string_view action, std::string_view /*argument*/) -> bool {
  if (action == "zoom_in") {
    set_zoom(zoom_ * kWheelZoomStep);
    return true;
  }
  if (action == "zoom_out") {
    set_zoom(zoom_ / kWheelZoomStep);
    return true;
  }
  if (action == "reset_view") {
    reset_view();
    return true;
  }
  if (action == "fit") {
    set_fit(true);
    reset_view();
    return true;
  }
  return false;
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::MermaidView);
