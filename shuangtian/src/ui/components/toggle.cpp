#include "st/ui/components/toggle.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  static const NullTextPort fallback;
  return context.text != nullptr ? *context.text : fallback;
}


// —— 几何常量（颜色/间距/字号一律取自 `context.theme` 的 token，此处只留几何与动效常量）——
constexpr float k_box_size = 18.0f;         // 复选框/单选框指示器边长
constexpr float k_box_radius_max = 5.0f;    // 指示器圆角上限（`min(radius_sm, 5)`）
constexpr float k_check_stroke = 2.0f;      // 对勾线宽
constexpr float k_ring_stroke = 2.0f;       // 单选框外圈线宽（选中态加粗）
constexpr float k_radio_dot_radius = 4.5f;  // 单选框内点半径
constexpr float k_track_width = 40.0f;      // 开关轨道宽
constexpr float k_track_height = 22.0f;     // 开关轨道高
constexpr float k_knob_size = 20.0f;        // 开关滑块直径
constexpr float k_knob_inset = 1.0f;        // 滑块外缘与轨道内缘的留白
constexpr float k_glow_alpha = 0.24f;       // 按压时指示器的柔光（primary 派生）

// 动画状态机：`idle` 无动画；`pending` 已切换但尚未读到推进的时间轴（静态帧直接到位）。
constexpr double k_anim_idle = -1.0;
constexpr double k_anim_pending = -2.0;

/// 缓出三次曲线（180ms 的状态切换手感）。
[[nodiscard]] auto ease_out(float t) noexcept -> float {
  const float inverse = 1.0f - t;
  return 1.0f - inverse * inverse * inverse;
}

/// 焦点环：控件矩形外扩 `metrics.focus_width / 2`，圆角跟随控件（与 Input 同一口径）。
/// 圆角夹取到「短边一半 - 2」：半径等于半边的圆角路径描边会退化（无极值直线段时描边塌成发丝线，
/// 圆形指示器/圆形滑块尤其明显），故圆形控件用近似圆的圆角矩形画环。
void paint_focus_ring(const RenderContext& context, raster::Canvas& canvas, math::Rect rect,
                      float radius) {
  const float width = context.theme.metrics().focus_width;
  if (width <= 0.0f || rect.is_empty()) return;
  const float offset = width * 0.5f;
  const math::Rect outer = rect.inflate(offset);
  const float limit = std::max(std::min(outer.width, outer.height) * 0.5f - 2.0f, 0.0f);
  raster::Path ring;
  ring.add_rounded_rect(outer, std::min(radius + offset, limit));
  canvas.stroke_path(ring, raster::Paint::solid(context.theme.colors().focus_ring), width);
}

/// 圆角描边：线宽完全落在矩形内侧（1px 发丝在任何 DPI 下都清晰）。
void paint_outline(raster::Canvas& canvas, math::Rect rect, float radius, math::Color color,
                   float width) {
  if (width <= 0.0f || color.a == 0U || rect.is_empty()) return;
  const float half = width * 0.5f;
  raster::Path outline;
  outline.add_rounded_rect(rect.inset(math::Insets::all(half)), radius > half ? radius - half : 0.0f);
  canvas.stroke_path(outline, raster::Paint::solid(color), width);
}

/// 左对齐、垂直居中的单行标签（超出以省略号收尾）。
void paint_label(const RenderContext& context, raster::Canvas& canvas, std::string_view text,
                 math::Rect box, float size, math::Color color) {
  if (text.empty() || box.is_empty() || box.width <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float line = port.line_height(size);
  port.draw(canvas, clipped, math::Point{box.x, box.y + (box.height - line) * 0.5f}, size, color);
}

/// 指示器旁的标签盒（与指示器同行，左侧让出 gap）。
[[nodiscard]] auto label_box_after(math::Rect indicator, math::Rect bounds, float gap) -> math::Rect {
  const float left = indicator.right() + gap;
  return math::Rect{left, bounds.y, std::max(0.0f, bounds.right() - left), bounds.height};
}

/// 控件自身尺寸（指示器 + 间距 + 标签 + 上下留白，留白容纳焦点环）。
[[nodiscard]] auto control_size(const RenderContext& context, const TextPort& port, float marker_width,
                                const std::string& label) -> math::Size {
  const Metrics& metrics = context.theme.metrics();
  const float pad = metrics.space_xs;
  const float line = port.line_height(metrics.font_base);
  float width = pad * 2.0f + marker_width;
  if (!label.empty()) width += metrics.space_sm + port.measure_width(label, metrics.font_base);
  return math::Size{width, std::max(k_box_size, line) + pad * 2.0f};
}

/// 夹取到布局约束（组件自算尺寸后自行收敛，与容器约束保持一致）。
[[nodiscard]] auto fit_size(math::Size size, const Style& style, const Constraints& constraints)
    -> math::Size {
  float width = std::clamp(size.width, style.min_width, style.max_width);
  float height = std::clamp(size.height, style.min_height, style.max_height);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  return math::Size{width, height};
}

/// 把主题 token 落到 `style_`（外观真相由 `paint_content` 按四态解析；此处固定字体/圆角/透明底色）。
void sync_style(Element& element, const RenderContext& context, float radius) {
  const Palette& colors = context.theme.colors();
  Style& style = element.style();
  style.font_size = context.theme.metrics().font_base;
  style.font_weight = FontWeight::Medium;
  style.color = element.enabled() ? colors.text : colors.text_faint;
  style.radius = radius;
  style.background = math::Color{0, 0, 0, 0};  // 指示器自绘，控件自身透明
  style.border_width = 0.0f;
  style.border_color = math::Color{0, 0, 0, 0};
  style.align_items = Align::Center;
}

}  // namespace

// ————————————————— Checkbox —————————————————

Checkbox::Checkbox(std::string label) : label_(std::move(label)) { set_focusable(true); }

void Checkbox::set_label(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  mark_layout_dirty();
}

void Checkbox::set_checked(bool value) noexcept {
  if (checked_ == value) return;
  checked_ = value;
  mark_dirty();
}

void Checkbox::toggle() {
  checked_ = !checked_;
  mark_dirty();
  if (on_change) on_change(checked_);
}

auto Checkbox::indicator_rect(const RenderContext& context) const -> math::Rect {
  const float pad = context.theme.metrics().space_xs;
  const float side = std::min(k_box_size, std::max(bounds_.height, 0.0f));
  return math::Rect{bounds_.x + pad, bounds_.y + (bounds_.height - side) * 0.5f, side, side};
}

void Checkbox::measure(const RenderContext& context, const Constraints& constraints) {
  const float radius = std::min(context.theme.metrics().radius_sm, k_box_radius_max);
  sync_style(*this, context, radius);
  const TextPort& port = text_port_of(context);
  measured_ = fit_size(control_size(context, port, k_box_size, label_), style_, constraints);
}

void Checkbox::paint_content(const RenderContext& context, raster::Canvas& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const float radius =
      style_.radius > 0.0f ? style_.radius : std::min(metrics.radius_sm, k_box_radius_max);
  const math::Rect box = indicator_rect(context);
  if (box.is_empty()) return;

  math::Color fill = colors.surface;
  math::Color border = colors.border;
  math::Color mark = colors.on_primary;
  if (!enabled_) {
    fill = checked_ ? colors.primary_soft : colors.surface_alt;
    border = colors.border;
    mark = colors.primary;
  } else if (checked_) {
    fill = pressed_ ? colors.primary_active : (hovered_ ? colors.primary_hover : colors.primary);
    border = fill;
  } else {
    fill = pressed_ ? colors.surface_sunken : (hovered_ ? colors.surface_alt : colors.surface);
    border = hovered_ ? colors.border_strong : colors.border;
  }

  canvas.fill_rect(box, raster::Paint::solid(fill), radius);
  paint_outline(canvas, box, radius, border, metrics.border_width);

  if (checked_) {
    // 两段线对勾（描边式，圆角连接/端帽由光栅器保证）。
    raster::Path tick;
    tick.move_to(math::Point{box.x + box.width * 0.27f, box.y + box.height * 0.53f});
    tick.line_to(math::Point{box.x + box.width * 0.44f, box.y + box.height * 0.71f});
    tick.line_to(math::Point{box.x + box.width * 0.74f, box.y + box.height * 0.31f});
    canvas.stroke_path(tick, raster::Paint::solid(mark), k_check_stroke);
  }

  const float gap = label_.empty() ? 0.0f : metrics.space_sm;
  paint_label(context, canvas, label_, label_box_after(box, bounds_, gap), style_.font_size,
              enabled_ ? colors.text : colors.text_faint);
  if (focused_) paint_focus_ring(context, canvas, box, radius);
}

auto Checkbox::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseDown:
      cycle_consumed_ = false;  // 新的按压周期
      mark_dirty();
      return true;
    case EventKind::MouseUp:
    case EventKind::Click:
    case EventKind::DoubleClick:
      mark_dirty();
      if (cycle_consumed_) return true;  // 同一按压周期内只切换一次
      cycle_consumed_ = true;
      toggle();
      return true;
    case EventKind::KeyDown:
      if (event.key == "Enter" || event.key == " " || event.key == "Space") {
        toggle();
        return true;
      }
      return false;
    default: return false;
  }
}

void Checkbox::activate() {
  if (!enabled_) return;
  toggle();
}

auto Checkbox::semantics_text() const -> std::string { return label_; }

auto Checkbox::semantics_value() const -> std::string { return checked_ ? "true" : "false"; }

auto Checkbox::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.checked = checked_;
  return flags;
}

auto Checkbox::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "checked" || name == "value") return std::string(checked_ ? "true" : "false");
  if (name == "label" || name == "text") return label_;
  return std::nullopt;
}

auto Checkbox::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "checked" || name == "value") {
    const auto flag = parse_bool(value);
    if (!flag.has_value()) return false;
    set_checked(*flag);
    return true;
  }
  if (name == "label" || name == "text") {
    set_label(std::string(value));
    return true;
  }
  return false;
}

auto Checkbox::property_names() const -> std::vector<std::string_view> {
  return {"checked", "value", "label"};
}

// ————————————————— Radio —————————————————

Radio::Radio(std::string label) : label_(std::move(label)) { set_focusable(true); }

void Radio::set_label(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  mark_layout_dirty();
}

void Radio::set_checked(bool value) noexcept {
  if (checked_ == value) return;
  checked_ = value;
  mark_dirty();
}

/// 单选语义：点击/激活即「选中」，已选中时为幂等空操作（不会取消选中）。
void Radio::toggle() {
  if (checked_ || !enabled_) return;
  checked_ = true;
  mark_dirty();
  if (on_change) on_change(true);
}

auto Radio::indicator_rect(const RenderContext& context) const -> math::Rect {
  const float pad = context.theme.metrics().space_xs;
  const float side = std::min(k_box_size, std::max(bounds_.height, 0.0f));
  return math::Rect{bounds_.x + pad, bounds_.y + (bounds_.height - side) * 0.5f, side, side};
}

void Radio::measure(const RenderContext& context, const Constraints& constraints) {
  sync_style(*this, context, k_box_size * 0.5f);  // 圆形：圆角 = 半径
  const TextPort& port = text_port_of(context);
  measured_ = fit_size(control_size(context, port, k_box_size, label_), style_, constraints);
}

void Radio::paint_content(const RenderContext& context, raster::Canvas& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const math::Rect box = indicator_rect(context);
  if (box.is_empty()) return;

  const float radius = std::min(box.width, box.height) * 0.5f;
  math::Color face = colors.surface;
  math::Color border = colors.border;
  math::Color dot = colors.primary;
  if (!enabled_) {
    face = checked_ ? colors.primary_soft : colors.surface_alt;
    border = colors.border;
    dot = colors.text_faint;
  } else if (checked_) {
    face = colors.surface;
    border = pressed_ ? colors.primary_active
                      : (hovered_ ? colors.primary_hover : colors.primary);
    dot = border;
  } else {
    face = pressed_ ? colors.surface_sunken : (hovered_ ? colors.surface_alt : colors.surface);
    border = hovered_ ? colors.border_strong : colors.border;
  }

  canvas.fill_circle(box.center(), radius, raster::Paint::solid(face));
  raster::Path circle;
  circle.add_circle(box.center(), std::max(radius - k_ring_stroke * 0.5f, 0.0f));
  canvas.stroke_path(circle, raster::Paint::solid(border), k_ring_stroke);
  if (checked_) {
    const float inner = std::max(radius - k_ring_stroke, 0.0f);
    canvas.fill_circle(box.center(), std::min(k_radio_dot_radius, inner), raster::Paint::solid(dot));
  }

  const float gap = label_.empty() ? 0.0f : metrics.space_sm;
  paint_label(context, canvas, label_, label_box_after(box, bounds_, gap), style_.font_size,
              enabled_ ? colors.text : colors.text_faint);
  if (focused_ && box.width > 0.0f) {
    // 圆弧描边在交点处靠圆头连接补形，故环画在指示器外沿（半径夹取见 `paint_focus_ring`）。
    paint_focus_ring(context, canvas, box, radius);
  }
}

auto Radio::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseDown:
      cycle_consumed_ = false;
      mark_dirty();
      return true;
    case EventKind::MouseUp:
    case EventKind::Click:
    case EventKind::DoubleClick:
      mark_dirty();
      if (cycle_consumed_) return true;
      cycle_consumed_ = true;
      toggle();
      return true;
    case EventKind::KeyDown:
      if (event.key == "Enter" || event.key == " " || event.key == "Space") {
        toggle();
        return true;
      }
      return false;
    default: return false;
  }
}

void Radio::activate() { toggle(); }

auto Radio::semantics_text() const -> std::string { return label_; }

auto Radio::semantics_value() const -> std::string { return checked_ ? "true" : "false"; }

auto Radio::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.checked = checked_;
  return flags;
}

auto Radio::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "checked" || name == "value") return std::string(checked_ ? "true" : "false");
  if (name == "label" || name == "text") return label_;
  return std::nullopt;
}

auto Radio::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "checked" || name == "value") {
    const auto flag = parse_bool(value);
    if (!flag.has_value()) return false;
    set_checked(*flag);
    return true;
  }
  if (name == "label" || name == "text") {
    set_label(std::string(value));
    return true;
  }
  return false;
}

auto Radio::property_names() const -> std::vector<std::string_view> {
  return {"checked", "value", "label"};
}

// ————————————————— Switch —————————————————

Switch::Switch(std::string label) : label_(std::move(label)) { set_focusable(true); }

void Switch::set_label(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  mark_layout_dirty();
}

void Switch::set_checked(bool value) noexcept {
  if (checked_ == value) return;
  animation_from_ = checked_;
  toggle_time_ = k_anim_pending;
  checked_ = value;
  mark_dirty();
}

void Switch::toggle() {
  animation_from_ = checked_;
  toggle_time_ = k_anim_pending;
  checked_ = !checked_;
  mark_dirty();
  if (on_change) on_change(checked_);
}

auto Switch::track_rect(const RenderContext& context) const -> math::Rect {
  (void)context;
  const float height = std::min(k_track_height, std::max(bounds_.height, 0.0f));
  const float width = std::min(k_track_width, std::max(bounds_.width, 0.0f));
  return math::Rect{bounds_.x, bounds_.y + (bounds_.height - height) * 0.5f, width, height};
}

auto Switch::knob_progress(const RenderContext& context) const -> float {
  const float target = checked_ ? 1.0f : 0.0f;
  const double now = context.time_seconds;
  const bool advancing = now > last_time_;

  if (toggle_time_ == k_anim_idle) {
    last_time_ = now;
    return target;
  }
  if (toggle_time_ == k_anim_pending) {
    last_time_ = now;
    if (!advancing) {  // 静态帧/离屏首帧：直接到位，避免画面停在半程
      toggle_time_ = k_anim_idle;
      return target;
    }
    toggle_time_ = now;
    return animation_from_ ? 1.0f : 0.0f;
  }

  const double duration = static_cast<double>(context.theme.metrics().motion_normal) / 1000.0;
  const double elapsed = now - toggle_time_;
  last_time_ = now;
  if (duration <= 0.0 || elapsed >= duration || !advancing) {
    toggle_time_ = k_anim_idle;
    return target;
  }
  const float t = ease_out(static_cast<float>(elapsed / duration));
  const float from = animation_from_ ? 1.0f : 0.0f;
  return from + (target - from) * t;
}

void Switch::measure(const RenderContext& context, const Constraints& constraints) {
  sync_style(*this, context, k_track_height * 0.5f);
  const TextPort& port = text_port_of(context);
  measured_ = fit_size(control_size(context, port, k_track_width, label_), style_, constraints);
}

void Switch::paint_content(const RenderContext& context, raster::Canvas& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const math::Rect track = track_rect(context);
  if (track.is_empty()) return;
  const float progress = knob_progress(context);
  const float radius = track.height * 0.5f;

  math::Color fill = colors.surface_sunken;
  if (!enabled_) {
    fill = checked_ ? colors.primary_soft : colors.surface_alt;
  } else if (checked_) {
    fill = pressed_ ? colors.primary_active : (hovered_ ? colors.primary_hover : colors.primary);
  } else if (pressed_) {
    fill = colors.surface_sunken;
  } else if (hovered_) {
    fill = colors.border;
  }
  canvas.fill_rect(track, raster::Paint::solid(fill), radius);
  if (!checked_ || !enabled_) {
    // 关态轨道与水底接近，需描边勾出边界（开态由 primary 自身承担）。
    paint_outline(canvas, track, radius, colors.border, metrics.border_width);
  }

  const float knob = std::min(k_knob_size, std::max(track.height - k_knob_inset * 2.0f, 0.0f));
  const float travel = std::max(track.width - knob - k_knob_inset * 2.0f, 0.0f);
  const math::Rect slider{track.x + k_knob_inset + travel * progress,
                          track.y + (track.height - knob) * 0.5f, knob, knob};
  const float knob_radius = knob * 0.5f;
  if (pressed_ && enabled_) {
    // 按压光圈（primary 派生，先画再压滑块，避免给滑块染色）。
    canvas.fill_circle(slider.center(), knob_radius + metrics.space_xs,
                       raster::Paint::solid(colors.primary.with_alpha_f(k_glow_alpha)));
  }
  const Shadow shadow = shadow_sm(context.theme);
  canvas.draw_shadow(slider, knob_radius, shadow.blur, shadow.color,
                     math::Point{shadow.offset_x, shadow.offset_y});
  canvas.fill_circle(slider.center(), knob_radius, raster::Paint::solid(colors.on_primary));
  // 发丝描边：深色主题下滑块与轨道同调时仍可辨识轮廓。
  raster::Path knob_edge;
  knob_edge.add_circle(slider.center(),
                       std::max(knob_radius - metrics.border_width * 0.5f, 0.0f));
  canvas.stroke_path(knob_edge, raster::Paint::solid(colors.border_strong), metrics.border_width);

  const float gap = label_.empty() ? 0.0f : metrics.space_sm;
  paint_label(context, canvas, label_, label_box_after(track, bounds_, gap), style_.font_size,
              enabled_ ? colors.text : colors.text_faint);
  if (focused_) paint_focus_ring(context, canvas, track, radius);
}

auto Switch::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseDown:
      cycle_consumed_ = false;
      mark_dirty();
      return true;
    case EventKind::MouseUp:
    case EventKind::Click:
    case EventKind::DoubleClick:
      mark_dirty();
      if (cycle_consumed_) return true;
      cycle_consumed_ = true;
      toggle();
      return true;
    case EventKind::KeyDown:
      if (event.key == "Enter" || event.key == " " || event.key == "Space") {
        toggle();
        return true;
      }
      return false;
    default: return false;
  }
}

void Switch::activate() {
  if (!enabled_) return;
  toggle();
}

auto Switch::semantics_text() const -> std::string { return label_; }

auto Switch::semantics_value() const -> std::string { return checked_ ? "true" : "false"; }

auto Switch::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.checked = checked_;
  return flags;
}

auto Switch::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "checked" || name == "value") return std::string(checked_ ? "true" : "false");
  if (name == "label" || name == "text") return label_;
  return std::nullopt;
}

auto Switch::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "checked" || name == "value") {
    const auto flag = parse_bool(value);
    if (!flag.has_value()) return false;
    set_checked(*flag);
    return true;
  }
  if (name == "label" || name == "text") {
    set_label(std::string(value));
    return true;
  }
  return false;
}

auto Switch::property_names() const -> std::vector<std::string_view> {
  return {"checked", "value", "label"};
}

}  // namespace st::ui
