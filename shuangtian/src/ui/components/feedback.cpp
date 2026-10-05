#include "st/ui/components/feedback.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;

namespace {

/// 文本端口取用（`RenderContext::text` 可为空 → 退化为 no-op 端口）。
[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  return context.text != nullptr ? *context.text : NullTextPort::instance();
}

// 注：`tone_name` 曾在这里另写一份——现在共用 `st::ui::tone_name`（theme.hpp）。
// 两份实现会让"色调短名"有两个真相源（且匿名命名空间那份还会遮蔽共享版，
// 表现为调用处 `ambiguous` 编译错误），所以只留一份。

[[nodiscard]] auto tone_from_name(std::string_view name) -> std::optional<Tone> {
  if (name == "default") return Tone::Default;
  if (name == "muted") return Tone::Muted;
  if (name == "faint") return Tone::Faint;
  if (name == "primary") return Tone::Primary;
  if (name == "accent") return Tone::Accent;
  if (name == "success") return Tone::Success;
  if (name == "warning") return Tone::Warning;
  if (name == "danger") return Tone::Danger;
  if (name == "on_primary") return Tone::OnPrimary;
  return std::nullopt;
}

/// 进度解析：`"0.5"` 视为比例，`"50%"` 视为百分比。
[[nodiscard]] auto parse_progress(std::string_view value) -> std::optional<float> {
  std::string_view text = st::trim(value);
  bool percent = false;
  if (!text.empty() && text.back() == '%') {
    percent = true;
    text.remove_suffix(1);
  }
  const auto parsed = st::parse_f64(st::trim(text));
  if (!parsed.has_value()) return std::nullopt;
  const auto ratio = static_cast<float>(*parsed);
  return math::clamp01(percent ? ratio / 100.0f : ratio);
}


/// 圆弧采样点（几何用 `raster::Path::add_arc` 求，再取折线点用于环形填充）。
[[nodiscard]] auto arc_points(math::Point center, float radius, float start_degrees,
                             float end_degrees) -> std::vector<math::Point> {
  raster::Path arc;
  arc.add_arc(center, radius, start_degrees, end_degrees);
  std::vector<math::Point> points;
  for (const raster::Polyline& polyline : arc.flatten(0.25f)) {
    points.insert(points.end(), polyline.points.begin(), polyline.points.end());
  }
  return points;
}

}  // namespace

// —— ProgressBar ——

ProgressBar::ProgressBar(float value) : value_(math::clamp01(value)) {
  style_.background = math::Color{0, 0, 0, 0};
}

void ProgressBar::set_value(float value) {
  const float clamped = math::clamp01(value);
  if (clamped == value_) return;
  value_ = clamped;
  mark_dirty();
}

void ProgressBar::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  mark_dirty();
}

void ProgressBar::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.radius = kTrackHeight * 0.5f;
  style_.height = kTrackHeight;
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_xs;
}

void ProgressBar::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float width = constraints.max_width < kUnbounded ? constraints.max_width : 160.0f;
  if (style_.has_explicit_width()) width = style_.width;
  width = std::clamp(width, style_.min_width, style_.max_width);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);

  float height = style_.has_explicit_height() ? style_.height : kTrackHeight;
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void ProgressBar::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Theme& theme = context.theme;
  const Palette& colors = theme.colors();
  const float radius = std::min(bounds_.width, bounds_.height) * 0.5f;

  fill_round_rect(canvas, bounds_, radius, colors.surface_sunken);

  const float filled = bounds_.width * value_;
  if (filled <= 0.0f) return;
  const math::Rect fill{bounds_.x, bounds_.y, filled, bounds_.height};
  const math::Color from = tone_color(theme, tone_);
  const math::Color to = tone_ == Tone::Primary ? colors.primary_hover : from.lighten(0.18f);
  const raster::Gradient gradient = raster::Gradient::linear(
      math::Point{fill.x, fill.y}, math::Point{fill.right(), fill.y},
      std::vector<raster::GradientStop>{raster::GradientStop{0.0f, from},
                                        raster::GradientStop{1.0f, to}});
  fill_round_rect(canvas, fill, radius, raster::Paint::with_gradient(gradient));
}

auto ProgressBar::semantics_text() const -> std::string { return "progress"; }

auto ProgressBar::semantics_value() const -> std::string {
  const auto percent = static_cast<int>(std::lround(value_ * 100.0f));
  return std::format("{}%", percent);
}

auto ProgressBar::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return std::format("{}", value_);
  if (name == "tone") return std::string(tone_name(tone_));
  if (name == "text") return semantics_value();
  return std::nullopt;
}

auto ProgressBar::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value") {
    const auto parsed = parse_progress(value);
    if (!parsed.has_value()) return false;
    set_value(*parsed);
    return true;
  }
  if (name == "tone") {
    const auto parsed = tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  return false;
}

auto ProgressBar::property_names() const -> std::vector<std::string_view> {
  return {"value", "tone", "text"};
}

auto ProgressBar::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "set_value") {
    const auto parsed = parse_progress(argument);
    if (!parsed.has_value()) return false;
    set_value(*parsed);
    return true;
  }
  return Element::invoke_action(action, argument);
}

// —— Spinner ——

Spinner::Spinner(float diameter) : diameter_(diameter > 0.0f ? diameter : kDiameter) {}

void Spinner::set_diameter(float diameter) {
  const float clamped = diameter > 0.0f ? diameter : kDiameter;
  if (diameter_ == clamped) return;
  diameter_ = clamped;
  mark_layout_dirty();
}

void Spinner::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  mark_dirty();
}

auto Spinner::phase_degrees(double time_seconds) const noexcept -> float {
  const double turns = time_seconds / static_cast<double>(kPeriodSeconds);
  const double fraction = turns - std::floor(turns);
  return static_cast<float>(fraction * 360.0);
}

void Spinner::apply_theme(const Theme& theme) {
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.radius = 0.0f;
  style_.width = diameter_;
  style_.height = diameter_;
  style_.color = tone_color(theme, tone_);
  style_.font_size = theme.metrics().font_xs;
}

void Spinner::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float side = diameter_;
  if (constraints.max_width < kUnbounded) side = std::min(side, constraints.max_width);
  if (constraints.max_height < kUnbounded) side = std::min(side, constraints.max_height);
  measured_ = math::Size{side, side};
}

void Spinner::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const float side = std::min(bounds_.width, bounds_.height);
  if (side <= 0.0f) return;
  const float thickness = std::min(kStroke, side * 0.25f);
  const float radius = (side - thickness) * 0.5f;
  if (radius <= 0.0f) return;

  // 3/4 圆弧：内外两条 `add_arc` 折线闭合成环形（raster 层 stroke 在曲线顶点处
  // 的圆头子路径现况下会被负绕向抵消，故直接闭合成可填充的环形，端点更干净）。
  const float start = phase_degrees(context.time_seconds);
  const float end = start + kSweepDegrees;
  const math::Point center = bounds_.center();
  const auto outer_points = arc_points(center, radius + thickness * 0.5f, start, end);
  const auto inner_points =
      arc_points(center, std::max(0.5f, radius - thickness * 0.5f), end, start);
  if (outer_points.size() < 2U || inner_points.size() < 2U) return;

  raster::Path ring;
  ring.move_to(outer_points.front());
  for (std::size_t index = 1; index < outer_points.size(); ++index) {
    ring.line_to(outer_points[index]);
  }
  for (const math::Point& point : inner_points) ring.line_to(point);
  ring.close();
  canvas.fill_path(ring, raster::Paint::solid(tone_color(context.theme, tone_)));
}

auto Spinner::semantics_text() const -> std::string { return "loading"; }
auto Spinner::semantics_value() const -> std::string { return "loading"; }

auto Spinner::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "size" || name == "diameter") return std::format("{}", diameter_);
  if (name == "tone") return std::string(tone_name(tone_));
  return std::nullopt;
}

auto Spinner::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "size" || name == "diameter") {
    const auto parsed = st::parse_f64(value);
    if (!parsed.has_value()) return false;
    set_diameter(static_cast<float>(*parsed));
    return true;
  }
  if (name == "tone") {
    const auto parsed = tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  return false;
}

auto Spinner::property_names() const -> std::vector<std::string_view> {
  return {"size", "diameter", "tone"};
}

// —— Badge ——

Badge::Badge(std::string text, Tone tone) : text_(std::move(text)), tone_(tone) {}

void Badge::set_text(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  mark_layout_dirty();
}

void Badge::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  mark_dirty();
}

void Badge::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};  // 胶囊底自绘（见 fill_round_rect 说明）
  style_.color = tone_color(theme, tone_);
  style_.radius = metrics.radius_pill;
  style_.font_size = metrics.font_xs;
  style_.font_weight = FontWeight::Medium;
  style_.text_align = TextAlign::Center;
  style_.height = kHeight;
  style_.padding = math::Insets::symmetric(kPaddingX, 0.0f);
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
}

void Badge::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float width = port.measure_width(text_, style_.font_size) + style_.padding.horizontal();
  width = std::max(width, kHeight);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Badge::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  fill_round_rect(canvas, bounds_, style_.radius, tone_soft_color(context.theme, tone_));
  paint_text(context, canvas, text_, content_box());
}

auto Badge::semantics_value() const -> std::string { return std::string(tone_name(tone_)); }

auto Badge::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "text" || name == "value") return text_;
  if (name == "tone") return std::string(tone_name(tone_));
  return std::nullopt;
}

auto Badge::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "text" || name == "value") {
    set_text(std::string(value));
    return true;
  }
  if (name == "tone") {
    const auto parsed = tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  return false;
}

auto Badge::property_names() const -> std::vector<std::string_view> {
  return {"text", "value", "tone"};
}

// —— Chip ——

Chip::Chip(std::string text, bool closable) : text_(std::move(text)), closable_(closable) {
  style_.background = math::Color{0, 0, 0, 0};
}

void Chip::set_text(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  mark_layout_dirty();
}

void Chip::set_dot(bool dot) {
  if (dot_ == dot) return;
  dot_ = dot;
  mark_layout_dirty();
}

void Chip::set_closable(bool closable) {
  if (closable_ == closable) return;
  closable_ = closable;
  mark_layout_dirty();
}

void Chip::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  mark_dirty();
}

auto Chip::close_area() const noexcept -> math::Rect {
  const float x = bounds_.right() - kPaddingX - kCloseArea;
  return math::Rect{x, bounds_.y + (bounds_.height - kCloseArea) * 0.5f, kCloseArea, kCloseArea};
}

auto Chip::text_area() const noexcept -> math::Rect {
  float x = bounds_.x + kPaddingX;
  if (dot_) x += kDotSize + kGap;
  float right = bounds_.right() - kPaddingX;
  if (closable_) right = close_area().x - kGap;
  const float width = right - x;
  return math::Rect{x, bounds_.y, width > 0.0f ? width : 0.0f, bounds_.height};
}

void Chip::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};  // 胶囊底自绘（见 fill_round_rect 说明）
  style_.color = tone_color(theme, tone_);
  style_.radius = metrics.radius_pill;
  style_.font_size = metrics.font_sm;
  style_.font_weight = FontWeight::Medium;
  style_.text_align = TextAlign::Start;
  style_.height = kHeight;
  style_.padding = math::Insets{};
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
}

void Chip::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float width = kPaddingX + port.measure_width(text_, style_.font_size);
  if (dot_) width += kDotSize + kGap;
  if (closable_) width += kGap + kCloseArea;
  width += kPaddingX;
  width = std::max(width, kHeight * 2.0f);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Chip::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  fill_round_rect(canvas, bounds_, style_.radius, tone_soft_color(context.theme, tone_));
  if (dot_) {
    const math::Point center{bounds_.x + kPaddingX + kDotSize * 0.5f, bounds_.center().y};
    const math::Rect dot{center.x - kDotSize * 0.5f, center.y - kDotSize * 0.5f, kDotSize, kDotSize};
    fill_round_rect(canvas, dot, kDotSize * 0.5f, tone_color(context.theme, tone_));
  }
  paint_text(context, canvas, text_, text_area());
  if (closable_) {
    const math::Rect area = close_area();
    const math::Rect glyph{area.center().x - kCloseGlyph * 0.5f,
                           area.center().y - kCloseGlyph * 0.5f, kCloseGlyph, kCloseGlyph};
    Icon::draw(canvas, "close", glyph, hovered_ ? colors.text : colors.text_faint, 1.5f);
  }
}

auto Chip::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (event.kind == EventKind::MouseDown || event.kind == EventKind::Click) {
    if (closable_ && close_area().contains(event.position)) {
      if (on_close) on_close();
      event.handled = true;
      return true;
    }
    if (event.kind == EventKind::Click && bounds_.contains(event.position)) {
      if (on_click) on_click();
      event.handled = true;
      return true;
    }
    return bounds_.contains(event.position);
  }
  return false;
}

auto Chip::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto Chip::semantics_value() const -> std::string { return std::string(tone_name(tone_)); }

auto Chip::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "text" || name == "value") return text_;
  if (name == "tone") return std::string(tone_name(tone_));
  if (name == "dot") return std::string(dot_ ? kFlagTrue : "false");
  if (name == "closable") return std::string(closable_ ? kFlagTrue : "false");
  return std::nullopt;
}

auto Chip::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "text" || name == "value") {
    set_text(std::string(value));
    return true;
  }
  if (name == "tone") {
    const auto parsed = tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  if (name == "dot" || name == "closable") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    if (name == "dot") {
      set_dot(*parsed);
    } else {
      set_closable(*parsed);
    }
    return true;
  }
  return false;
}

auto Chip::property_names() const -> std::vector<std::string_view> {
  return {"text", "value", "tone", "dot", "closable"};
}

// —— Avatar ——

Avatar::Avatar(std::string name, float size)
    : name_(std::move(name)), size_(size > 0.0f ? size : kMedium) {
  style_.background = math::Color{0, 0, 0, 0};
  style_.width = size_;
  style_.height = size_;
  style_.radius = size_ * 0.5f;
  style_.text_align = TextAlign::Center;
}

void Avatar::set_name(std::string name) {
  if (name_ == name) return;
  name_ = std::move(name);
  mark_dirty();
}

void Avatar::set_size(float size) {
  const float clamped = size > 0.0f ? size : kMedium;
  if (size_ == clamped) return;
  size_ = clamped;
  style_.width = size_;
  style_.height = size_;
  style_.radius = size_ * 0.5f;
  style_.font_size = std::max(10.0f, size_ * 0.42f);
  mark_layout_dirty();
}

auto Avatar::initials() const -> std::string {
  std::size_t index = 0;
  char32_t value = 0;
  bool found = false;
  while (index < name_.size()) {
    const Codepoint codepoint = st::decode_utf8(name_, index);
    if (codepoint.bytes == 0U) break;
    if (!st::is_space_codepoint(codepoint.value)) {
      value = codepoint.value;
      found = true;
      break;
    }
  }
  if (!found) return {};
  if (value >= U'a' && value <= U'z') value = value - U'a' + U'A';
  std::string out;
  st::encode_utf8(value, out);
  return out;
}

void Avatar::apply_theme(const Theme& theme) {
  style_.background = math::Color{0, 0, 0, 0};  // 圆形底自绘（见 fill_round_rect 说明）
  style_.color = theme.colors().primary;
  style_.width = size_;
  style_.height = size_;
  style_.radius = size_ * 0.5f;
  style_.font_size = std::max(10.0f, size_ * 0.42f);
  style_.font_weight = FontWeight::SemiBold;
  style_.text_align = TextAlign::Center;
  style_.padding = math::Insets{};
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
}

void Avatar::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float side = size_;
  if (constraints.max_width < kUnbounded) side = std::min(side, constraints.max_width);
  if (constraints.max_height < kUnbounded) side = std::min(side, constraints.max_height);
  measured_ = math::Size{side, side};
}

void Avatar::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  fill_round_rect(canvas, bounds_, style_.radius, context.theme.colors().primary_soft);
  paint_text(context, canvas, initials(), bounds_);
}

auto Avatar::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "name" || name == "text") return name_;
  if (name == "initials") return initials();
  if (name == "size") return std::format("{}", size_);
  return std::nullopt;
}

auto Avatar::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "name" || name == "text") {
    set_name(std::string(value));
    return true;
  }
  if (name == "size") {
    const auto parsed = st::parse_f64(value);
    if (!parsed.has_value()) return false;
    set_size(static_cast<float>(*parsed));
    return true;
  }
  return false;
}

auto Avatar::property_names() const -> std::vector<std::string_view> {
  return {"name", "text", "initials", "size"};
}

// —— Tooltip ——

Tooltip::Tooltip(std::string text) : text_(std::move(text)) {
  style_.background = math::Color{0, 0, 0, 0};
}

void Tooltip::set_text(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  mark_layout_dirty();
}

void Tooltip::set_target_rect(math::Rect rect) {
  target_ = rect;
  mark_layout_dirty();
}

void Tooltip::set_active(bool active) {
  if (active_ == active) return;
  active_ = active;
  mark_dirty();
}

void Tooltip::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};  // 气泡底自绘（见 fill_round_rect 说明）
  style_.color = theme.colors().text;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.radius = kRadius;
  style_.shadow = shadow_md(theme);
  style_.font_size = metrics.font_xs;
  style_.font_weight = FontWeight::Medium;
  style_.text_align = TextAlign::Center;
  style_.padding = math::Insets::symmetric(kPaddingX, kPaddingY);
}

void Tooltip::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  fill_round_rect(canvas, bounds_, style_.radius, context.theme.colors().surface);
  paint_text(context, canvas, text_, content_box());
}

void Tooltip::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float width = port.measure_width(text_, style_.font_size) + kPaddingX * 2.0f;
  float height = port.line_height(style_.font_size) + kPaddingY * 2.0f;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Tooltip::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  bounds_ = rect;
  if (!target_.is_empty()) {
    const float width = measured_.width > 0.0f ? measured_.width : rect.width;
    const float height = measured_.height > 0.0f ? measured_.height : rect.height;
    float x = target_.center().x - width * 0.5f;
    float y = target_.y - height - kGap;
    if (y < 0.0f) y = target_.bottom() + kGap;  // 上方空间不足 → 放到目标下方
    if (x < 0.0f) x = 0.0f;
    bounds_ = math::Rect{x, y, width, height};
  }
  layout_dirty_ = false;
}

void Tooltip::paint(const RenderContext& context, raster::Surface& canvas) const {
  if (!active_ && !hovered_) return;
  Element::paint(context, canvas);
}

auto Tooltip::hit_test(math::Point point) const noexcept -> bool {
  if (!active_ && !hovered_) return false;
  return bounds_.contains(point);
}

auto Tooltip::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "text" || name == "value") return text_;
  if (name == "active") return std::string(active_ ? kFlagTrue : "false");
  if (name == "target") {
    return std::format("{},{},{},{}", target_.x, target_.y, target_.width, target_.height);
  }
  return std::nullopt;
}

auto Tooltip::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "text" || name == "value") {
    set_text(std::string(value));
    return true;
  }
  if (name == "active") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_active(*parsed);
    return true;
  }
  if (name == "target") {
    const std::vector<std::string_view> parts = st::split(value, ',');
    if (parts.size() != 4U) return false;
    math::Rect rect;
    const auto parse = [&parts](std::size_t index) -> std::optional<float> {
      const auto parsed = st::parse_f64(st::trim(parts[index]));
      if (!parsed.has_value()) return std::nullopt;
      return static_cast<float>(*parsed);
    };
    const auto x = parse(0);
    const auto y = parse(1);
    const auto width = parse(2);
    const auto height = parse(3);
    if (!x.has_value() || !y.has_value() || !width.has_value() || !height.has_value()) return false;
    rect.x = *x;
    rect.y = *y;
    rect.width = *width;
    rect.height = *height;
    set_target_rect(rect);
    return true;
  }
  return false;
}

auto Tooltip::property_names() const -> std::vector<std::string_view> {
  return {"text", "value", "active", "target"};
}

}  // namespace st::ui
