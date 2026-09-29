#include "st/ui/components/tabs.hpp"

#include <algorithm>
#include <cmath>
#include <format>
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


// —— 几何常量（颜色/间距/字号一律取自 `context.theme` 的 token）——
constexpr float k_indicator_height = 2.0f;  // 选中指示条厚度
constexpr float k_min_tab_width = 36.0f;    // 标签项最小宽度（避免空标签塌陷）

// 指示条动画状态机：`idle` 无动画；`pending` 已切换但尚未读到推进的时间轴。
constexpr double k_anim_idle = -1.0;
constexpr double k_anim_pending = -2.0;

[[nodiscard]] auto ease_out(float t) noexcept -> float {
  const float inverse = 1.0f - t;
  return 1.0f - inverse * inverse * inverse;
}

}  // namespace

Tabs::Tabs() {
  set_focusable(true);
  style_.direction = FlexDirection::Row;
}

void Tabs::set_tabs(std::vector<std::string> labels) {
  labels_ = std::move(labels);
  widths_.clear();
  total_width_ = 0.0f;
  hover_index_ = -1;
  if (active_ >= labels_.size()) active_ = labels_.empty() ? 0 : labels_.size() - 1;
  indicator_start_ = k_anim_pending;
  mark_layout_dirty();
}

void Tabs::add_tab(std::string label) {
  labels_.push_back(std::move(label));
  widths_.clear();
  total_width_ = 0.0f;
  mark_layout_dirty();
}

void Tabs::clear_tabs() {
  labels_.clear();
  widths_.clear();
  total_width_ = 0.0f;
  active_ = 0;
  hover_index_ = -1;
  mark_layout_dirty();
}

auto Tabs::tab_label(std::size_t index) const -> std::string_view {
  return index < labels_.size() ? std::string_view(labels_[index]) : std::string_view{};
}

auto Tabs::index_of_label(std::string_view label) const -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < labels_.size(); ++index) {
    if (labels_[index] == label) return index;
  }
  return std::nullopt;
}

void Tabs::set_active(std::size_t index, bool notify) {
  if (labels_.empty()) return;
  const std::size_t clamped = std::min(index, labels_.size() - 1);
  if (clamped == active_) return;
  active_ = clamped;
  indicator_from_x_ = indicator_x_;
  indicator_from_width_ = indicator_width_;
  indicator_start_ = k_anim_pending;
  mark_dirty();
  if (notify && on_change) on_change(active_);
}

auto Tabs::active_label() const -> std::string_view { return tab_label(active_); }

auto Tabs::tab_rect(std::size_t index) const -> math::Rect {
  if (index >= widths_.size()) return math::Rect{};
  float x = bounds_.x;
  for (std::size_t cursor = 0; cursor < index; ++cursor) {
    x += widths_[cursor] + gap_;
  }
  return math::Rect{x, bounds_.y, widths_[index], bounds_.height};
}

auto Tabs::tab_index_at(math::Point point) const -> std::optional<std::size_t> {
  if (point.y < bounds_.y || point.y >= bounds_.bottom()) return std::nullopt;
  for (std::size_t index = 0; index < widths_.size(); ++index) {
    const math::Rect tab = tab_rect(index);
    if (point.x >= tab.x && point.x < tab.right()) return index;
  }
  return std::nullopt;
}

void Tabs::rebuild_widths(const RenderContext& context) const {
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);
  const float pad = metrics.space_md;
  widths_.clear();
  widths_.reserve(labels_.size());
  float total = 0.0f;
  for (const auto& label : labels_) {
    const float text = port.measure_width(label, style_.font_size);
    const float width = std::max(text, k_min_tab_width) + pad * 2.0f;
    widths_.push_back(width);
    total += width;
  }
  if (!labels_.empty() && gap_ > 0.0f) {
    total += gap_ * static_cast<float>(labels_.size() - 1);
  }
  total_width_ = total;
}

void Tabs::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.font_size = metrics.font_base;
  style_.font_weight = FontWeight::Medium;
  style_.color = enabled_ ? colors.text : colors.text_faint;
  style_.radius = metrics.radius_sm;
  style_.background = math::Color{0, 0, 0, 0};  // 标签条透明，仅 hover/指示条着色
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.text_align = TextAlign::Center;
  style_.direction = FlexDirection::Row;
  style_.align_items = Align::Center;
  gap_ = metrics.space_xs;
}

void Tabs::measure(const RenderContext& context, const Constraints& constraints) {
  apply_theme(context.theme);
  rebuild_widths(context);
  float width = style_.has_explicit_width() ? style_.width : total_width_;
  float height = style_.has_explicit_height() ? style_.height : context.theme.metrics().control_height;
  width = std::clamp(width, style_.min_width, style_.max_width);
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

auto Tabs::resolve_indicator(const RenderContext& context, math::Rect target) const -> math::Rect {
  const double now = context.time_seconds;
  const bool advancing = now > last_time_;
  const auto snapped = [&target, this]() -> math::Rect {
    indicator_x_ = target.x;
    indicator_width_ = target.width;
    return target;
  };

  if (indicator_start_ == k_anim_idle || !enabled_) {
    last_time_ = now;
    return snapped();
  }
  if (indicator_start_ == k_anim_pending) {
    last_time_ = now;
    if (!advancing) {  // 静态帧/离屏首帧：直接到位，避免画面停在半程
      indicator_start_ = k_anim_idle;
      return snapped();
    }
    indicator_start_ = now;
    indicator_from_x_ = indicator_x_;
    indicator_from_width_ = indicator_width_;
    return math::Rect{indicator_from_x_, target.y, indicator_from_width_, target.height};  // 起步帧停在旧位置
  }

  const double duration = static_cast<double>(context.theme.metrics().motion_normal) / 1000.0;
  const double elapsed = now - indicator_start_;
  last_time_ = now;
  if (duration <= 0.0 || elapsed >= duration || !advancing) {
    indicator_start_ = k_anim_idle;
    return snapped();
  }
  const float t = ease_out(static_cast<float>(elapsed / duration));
  indicator_x_ = indicator_from_x_ + (target.x - indicator_from_x_) * t;
  indicator_width_ = indicator_from_width_ + (target.width - indicator_from_width_) * t;
  return math::Rect{indicator_x_, target.y, indicator_width_, target.height};
}

void Tabs::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || labels_.empty()) return;
  if (widths_.size() != labels_.size()) rebuild_widths(context);

  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);
  const float radius = style_.radius > 0.0f ? style_.radius : metrics.radius_sm;
  const float line = port.line_height(style_.font_size);
  const bool clipped = total_width_ > bounds_.width;
  if (clipped) canvas.push_clip_rect(bounds_);

  for (std::size_t index = 0; index < labels_.size(); ++index) {
    const math::Rect tab = tab_rect(index);
    const bool is_active = index == active_;
    const bool is_hovered = static_cast<int>(index) == hover_index_;

    if (is_hovered && !is_active && enabled_) {
      // hover 背景：上下留出呼吸，底部让开指示条。
      const math::Rect backdrop = tab.inset(
          math::Insets{0.0f, metrics.space_xs, 0.0f, metrics.space_xs + k_indicator_height});
      if (!backdrop.is_empty()) {
        canvas.fill_rect(backdrop, raster::Paint::solid(colors.surface_alt), radius);
      }
    }

    const std::string_view label = labels_[index];
    if (!label.empty()) {
      const float text_width = port.measure_width(label, style_.font_size);
      const std::string shown = port.ellipsize(label, style_.font_size, std::max(tab.width, 0.0f));
      const float draw_x = tab.x + (tab.width - std::min(text_width, tab.width)) * 0.5f;
      const float draw_y = tab.y + (tab.height - line) * 0.5f;
      math::Color color = colors.text_muted;
      if (!enabled_) {
        color = colors.text_faint;
      } else if (is_active) {
        color = colors.text;
      }
      if (!shown.empty()) port.draw(canvas, shown, math::Point{draw_x, draw_y}, style_.font_size, color);
    }
  }

  // 选中指示条（2px primary）。
  const math::Rect active_tab = tab_rect(active_);
  const math::Rect indicator =
      resolve_indicator(context, math::Rect{active_tab.x, bounds_.bottom() - k_indicator_height,
                                            active_tab.width, k_indicator_height});
  const math::Color bar = enabled_ ? colors.primary : colors.border_strong;
  if (!indicator.is_empty()) {
    canvas.fill_rect(indicator, raster::Paint::solid(bar), k_indicator_height * 0.5f);
  }

  if (focused_) {
    // 圆角夹取到「短边一半 - 2」：半径等于半边的圆角路径描边会退化成发丝线。
    const float width = metrics.focus_width;
    const math::Rect ring_box = bounds_.inflate(width * 0.5f);
    const float limit =
        std::max(std::min(ring_box.width, ring_box.height) * 0.5f - 2.0f, 0.0f);
    if (!ring_box.is_empty()) {
      raster::Path ring;
      ring.add_rounded_rect(ring_box, std::min(radius + width * 0.5f, limit));
      canvas.stroke_path(ring, raster::Paint::solid(colors.focus_ring), width);
    }
  }
  if (clipped) canvas.pop_clip();
}

auto Tabs::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const auto hit = tab_index_at(event.position);
      const int index = hit.has_value() ? static_cast<int>(*hit) : -1;
      if (index != hover_index_) {
        hover_index_ = index;
        mark_dirty();
      }
      return false;
    }
    case EventKind::HoverOut:
      if (hover_index_ != -1) {
        hover_index_ = -1;
        mark_dirty();
      }
      return false;
    case EventKind::HoverIn:
      mark_dirty();
      return false;
    case EventKind::MouseDown:
    case EventKind::MouseUp:
    case EventKind::Click:
    case EventKind::DoubleClick: {
      if (!enabled_) return false;
      const auto hit = tab_index_at(event.position);
      if (!hit.has_value()) return true;  // 落在标签条空白处：吞掉，不冒泡
      set_active(*hit, true);
      return true;
    }
    case EventKind::KeyDown: {
      if (!enabled_ || labels_.empty()) return false;
      const std::string& key = event.key;
      if (key == "ArrowRight" || key == "ArrowDown") {
        set_active((active_ + 1) % labels_.size(), true);
        return true;
      }
      if (key == "ArrowLeft" || key == "ArrowUp") {
        set_active((active_ + labels_.size() - 1) % labels_.size(), true);
        return true;
      }
      if (key == "Home") {
        set_active(0, true);
        return true;
      }
      if (key == "End") {
        set_active(labels_.size() - 1, true);
        return true;
      }
      if (key == "Enter" || key == " " || key == "Space") {
        activate();
        return true;
      }
      return false;
    }
    default: return false;
  }
}

void Tabs::activate() {
  if (!enabled_ || labels_.empty()) return;
  if (on_change) on_change(active_);
}

auto Tabs::semantics_text() const -> std::string { return std::string(active_label()); }

auto Tabs::semantics_value() const -> std::string { return std::format("{}", active_); }

auto Tabs::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = active_ < labels_.size();
  return flags;
}

auto Tabs::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "active") return std::format("{}", active_);
  if (name == "label" || name == "text") return std::string(active_label());
  if (name == "options") {
    std::string joined;
    for (const auto& label : labels_) {
      if (!joined.empty()) joined.push_back('|');
      joined.append(label);
    }
    return joined;
  }
  return std::nullopt;
}

auto Tabs::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "active") {
    if (const auto index = parse_u64(value); index.has_value()) {
      set_active(static_cast<std::size_t>(*index), false);
      return true;
    }
    if (const auto index = index_of_label(value); index.has_value()) {
      set_active(*index, false);
      return true;
    }
    return false;
  }
  if (name == "label" || name == "text") {
    const auto index = index_of_label(value);
    if (!index.has_value()) return false;
    set_active(*index, false);
    return true;
  }
  if (name == "options") {
    std::vector<std::string> labels;
    for (const auto part : split(value, '|')) {
      labels.emplace_back(part);
    }
    set_tabs(std::move(labels));
    return true;
  }
  return false;
}

auto Tabs::property_names() const -> std::vector<std::string_view> {
  return {"active", "label", "options"};
}

auto Tabs::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (labels_.empty()) return false;
  if (action == "next") {
    set_active((active_ + 1) % labels_.size(), true);
    return true;
  }
  if (action == "previous" || action == "prev") {
    set_active((active_ + labels_.size() - 1) % labels_.size(), true);
    return true;
  }
  if (action == "first") {
    set_active(0, true);
    return true;
  }
  if (action == "last") {
    set_active(labels_.size() - 1, true);
    return true;
  }
  if (action == "select") {
    if (const auto index = parse_u64(argument); index.has_value()) {
      set_active(static_cast<std::size_t>(*index), true);
      return true;
    }
    if (const auto index = index_of_label(argument); index.has_value()) {
      set_active(*index, true);
      return true;
    }
    return false;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
