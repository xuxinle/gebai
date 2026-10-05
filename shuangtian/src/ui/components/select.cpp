#include "st/ui/components/select.hpp"

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
#include "st/ui/icon.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::paint_focus_ring;
using components_internal::paint_outline;

namespace {

[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  static const NullTextPort fallback;
  return context.text != nullptr ? *context.text : fallback;
}


// —— 几何常量（颜色/间距/圆角/字号一律取自 `context.theme` 的 token）——
constexpr float k_row_height = 32.0f;      // 选项行高
constexpr float k_chevron_size = 16.0f;    // 右侧下三角盒尺寸
constexpr float k_check_size = 14.0f;      // 选中行对勾盒尺寸
constexpr float k_min_control_width = 160.0f;

/// 选项显示文本（`label` 空则回落到 `value`）。
[[nodiscard]] auto shown_label(const SelectOption& option) -> std::string_view {
  return option.label.empty() ? std::string_view(option.value) : std::string_view(option.label);
}

/// 左对齐、垂直居中的单行文本。
void paint_line(const RenderContext& context, raster::Surface& canvas, std::string_view text,
                math::Rect box, float size, math::Color color) {
  if (text.empty() || box.is_empty() || box.width <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float line = port.line_height(size);
  port.draw(canvas, clipped, math::Point{box.x, box.y + (box.height - line) * 0.5f}, size, color);
}

}  // namespace

// ————————————————— SelectPanel —————————————————

SelectPanel::SelectPanel(std::vector<SelectOption> options, std::optional<std::size_t> selected)
    : options_(std::move(options)), selected_(selected) {}

void SelectPanel::set_anchor(math::Point anchor) {
  anchor_ = anchor;
  mark_layout_dirty();
}

void SelectPanel::set_panel_width(float width) {
  if (panel_width_ == width) return;
  panel_width_ = width;
  mark_layout_dirty();
}

void SelectPanel::set_selected(std::optional<std::size_t> index) {
  selected_ = index;
  mark_dirty();
}

auto SelectPanel::option_rect(std::size_t index) const -> math::Rect {
  if (index >= options_.size()) return math::Rect{};
  const math::Rect content = bounds_.inset(style_.padding);
  const float y = content.y + static_cast<float>(index) * k_row_height;
  return math::Rect{content.x, y, content.width, k_row_height};
}

auto SelectPanel::index_at(math::Point point) const -> int {
  if (point.x < bounds_.x || point.x >= bounds_.right()) return -1;
  for (std::size_t index = 0; index < options_.size(); ++index) {
    const math::Rect row = option_rect(index);
    if (point.y >= row.y && point.y < row.bottom()) return static_cast<int>(index);
  }
  return -1;
}

void SelectPanel::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.background = colors.surface;
  style_.border_color = colors.border;
  style_.border_width = metrics.border_width;
  style_.radius = metrics.radius_md;
  style_.shadow = shadow_md(theme);
  style_.padding = math::Insets::all(metrics.space_xs);
  style_.font_size = metrics.font_base;
  style_.font_weight = FontWeight::Regular;
  style_.color = colors.text;
  style_.direction = FlexDirection::Column;
}

void SelectPanel::measure(const RenderContext& context, const Constraints& constraints) {
  apply_theme(context.theme);
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);

  float width = panel_width_ > 0.0f ? panel_width_ : style_.width;
  if (width <= 0.0f) {
    float widest = 0.0f;
    for (const auto& option : options_) {
      widest = std::max(widest, port.measure_width(shown_label(option), style_.font_size));
    }
    width = widest + metrics.space_md * 2.0f + k_chevron_size;
  }
  const float rows = static_cast<float>(options_.size());
  float height = rows * k_row_height + style_.padding.vertical();
  width = std::clamp(width, style_.min_width, style_.max_width);
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void SelectPanel::arrange(const RenderContext& context, math::Rect rect) {
  const float width = measured_.width > 0.0f ? measured_.width : rect.width;
  const float height = measured_.height > 0.0f ? measured_.height : rect.height;
  // 叠加层容器只负责尺寸与生命周期，面板按锚点自行定位（贴控件下沿）。
  Element::arrange(context, math::Rect{anchor_.x, anchor_.y, width, height});
}

void SelectPanel::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || options_.empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const float radius = metrics.radius_sm;

  for (std::size_t index = 0; index < options_.size(); ++index) {
    const math::Rect row = option_rect(index);
    const bool is_selected = selected_.has_value() && *selected_ == index;
    const bool is_hovered = static_cast<int>(index) == hover_index_;
    if (is_selected) {
      canvas.fill_rect(row, raster::Paint::solid(colors.primary_soft), radius);
    } else if (is_hovered) {
      canvas.fill_rect(row, raster::Paint::solid(colors.surface_alt), radius);
    }

    const std::string_view label = shown_label(options_[index]);
    const float trailing = k_check_size + metrics.space_sm;
    const math::Rect text_box{row.x + metrics.space_md, row.y,
                              std::max(row.width - metrics.space_md * 2.0f - trailing, 0.0f),
                              row.height};
    paint_line(context, canvas, label, text_box, style_.font_size,
               is_selected ? colors.primary : colors.text);

    if (is_selected) {
      const math::Rect box{row.right() - metrics.space_md - k_check_size,
                           row.y + (row.height - k_check_size) * 0.5f, k_check_size, k_check_size};
      Icon::draw(canvas, "check", box, colors.primary);
    }
  }
}

auto SelectPanel::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const int index = index_at(event.position);
      if (index != hover_index_) {
        hover_index_ = index;
        mark_dirty();
      }
      return true;
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
      const int index = index_at(event.position);
      if (index < 0) return true;
      // 先自我隐藏再回调：回调可能触发摘除（甚至析构本对象）。
      set_visible(false);
      const auto pick = on_pick;
      const auto target = static_cast<std::size_t>(index);
      if (pick) pick(target);
      return true;
    }
    case EventKind::KeyDown:
      if (event.key == "Escape") {
        set_visible(false);
        const auto dismiss = on_dismiss;
        if (dismiss) dismiss();
        return true;
      }
      return false;
    default: return false;
  }
}

auto SelectPanel::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = selected_.has_value();
  return flags;
}

// ————————————————— Select —————————————————

Select::Select() {
  set_focusable(true);
  style_.direction = FlexDirection::Row;
  // 选择器触发器：悬浮时描边 + 背景提亮（明显"可点"），不上浮（它常在一行里对齐）
  // 悬浮特效：背景提亮 + 上浮（"可点"的手感）；参数由主题令牌统一给，
  // 组件只声明要哪几项——这样按钮/列表/表格的悬浮反馈不会各走一套。
  set_hover_effect(HoverEffect{.enabled = true, .background = true, .border = true, .lift = false, .glow = false, .cursor = true});
}

Select::~Select() {
  if (panel_ != nullptr && overlay_remove) overlay_remove(panel_);
  panel_ = nullptr;
}

void Select::add_option(std::string value, std::string label) {
  SelectOption option;
  option.value = std::move(value);
  option.label = std::move(label);
  options_.push_back(std::move(option));
  mark_layout_dirty();
}

void Select::set_options(std::vector<SelectOption> options) {
  if (open_) set_open(false);
  options_ = std::move(options);
  if (selected_.has_value() && *selected_ >= options_.size()) selected_.reset();
  mark_layout_dirty();
}

void Select::clear_options() {
  if (open_) set_open(false);
  options_.clear();
  selected_.reset();
  mark_layout_dirty();
}

void Select::set_placeholder(std::string text) {
  if (placeholder_ == text) return;
  placeholder_ = std::move(text);
  mark_layout_dirty();
}

void Select::set_selected_index(std::optional<std::size_t> index, bool notify) {
  std::optional<std::size_t> normalized = index;
  if (normalized.has_value() && *normalized >= options_.size()) normalized.reset();
  if (normalized == selected_) return;
  selected_ = normalized;
  if (panel_ != nullptr) panel_->set_selected(selected_);
  mark_dirty();
  if (notify && on_change) on_change(selected_value());
}

auto Select::display_label(std::optional<std::size_t> index) const -> std::string_view {
  if (!index.has_value() || *index >= options_.size()) return {};
  return shown_label(options_[*index]);
}

auto Select::index_of(std::string_view text) const -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < options_.size(); ++index) {
    if (options_[index].value == text || shown_label(options_[index]) == text) return index;
  }
  return std::nullopt;
}

auto Select::selected_value() const -> std::string_view {
  if (!selected_.has_value() || *selected_ >= options_.size()) return {};
  return options_[*selected_].value;
}

auto Select::selected_label() const -> std::string_view { return display_label(selected_); }

void Select::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.font_size = metrics.font_base;
  style_.font_weight = FontWeight::Regular;
  style_.color = enabled_ ? colors.text : colors.text_faint;
  style_.radius = metrics.radius_sm;
  style_.background = colors.surface;
  style_.border_color = colors.border;
  style_.border_width = metrics.border_width;
  style_.text_align = TextAlign::Start;
  style_.direction = FlexDirection::Row;
  style_.align_items = Align::Center;
}

void Select::measure(const RenderContext& context, const Constraints& constraints) {
  apply_theme(context.theme);
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);

  float width = style_.has_explicit_width() ? style_.width : 0.0f;
  if (width <= 0.0f) {
    const std::string_view shown =
        selected_.has_value() ? display_label(selected_) : std::string_view(placeholder_);
    width = port.measure_width(shown, style_.font_size) + metrics.space_md * 2.0f +
            metrics.space_sm + k_chevron_size;
    width = std::max(width, k_min_control_width);
  }
  float height = style_.has_explicit_height() ? style_.height : metrics.control_height;
  width = std::clamp(width, style_.min_width, style_.max_width);
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Select::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  control_width_ = bounds_.width;
  anchor_hint_ = math::Point{bounds_.x, bounds_.bottom() + context.theme.metrics().space_xs};
  if (open_ && panel_ != nullptr) panel_->set_anchor(anchor_hint_);
  flush_dismiss();
}

void Select::flush_dismiss() const {
  if (!dismiss_pending_) return;
  dismiss_pending_ = false;
  if (panel_ == nullptr) return;
  Element* panel = panel_;
  panel_ = nullptr;
  panel->set_visible(false);
  if (overlay_remove) overlay_remove(panel);
}

void Select::open_panel() {
  if (dismiss_pending_) flush_dismiss();  // 上一次收起尚未摘除：先摘掉再开新的
  if (panel_ != nullptr || options_.empty() || !overlay_host) return;
  auto panel = std::make_unique<SelectPanel>(options_, selected_);
  panel->set_panel_width(control_width_);
  panel->set_anchor(anchor_hint_);
  panel->on_pick = [this](std::size_t index) { handle_pick(index); };
  panel->on_dismiss = [this]() {
    open_ = false;
    dismiss_pending_ = true;
    mark_dirty();
  };
  panel_ = panel.get();
  open_ = true;
  overlay_host(std::move(panel));
}

void Select::set_open(bool value) {
  if (value == open_) return;
  if (value) {
    if (!enabled_) return;
    open_panel();
    if (!open_) return;  // 缺少宿主回调/无选项：无法展开
  } else {
    open_ = false;
    dismiss_pending_ = true;
    if (panel_ != nullptr) panel_->set_visible(false);
  }
  mark_dirty();
}

void Select::handle_pick(std::size_t index) {
  if (index >= options_.size()) return;
  const bool changed = !selected_.has_value() || *selected_ != index;
  selected_ = index;
  open_ = false;
  dismiss_pending_ = true;
  mark_dirty();
  if (changed && on_change) on_change(selected_value());
}

void Select::move_selection(int delta) {
  if (options_.empty()) return;
  const auto count = static_cast<int>(options_.size());
  int current = selected_.has_value() ? static_cast<int>(*selected_) : (delta > 0 ? -1 : 0);
  current = (current + delta) % count;
  if (current < 0) current += count;
  set_selected_index(static_cast<std::size_t>(current), true);
}

void Select::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  flush_dismiss();
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const float radius = style_.radius > 0.0f ? style_.radius : metrics.radius_sm;

  math::Color background = colors.surface;
  math::Color border = colors.border;
  if (!enabled_) {
    background = colors.surface_alt;
  } else if (open_ || focused_) {
    border = colors.primary;
  } else if (hovered_ || pressed_) {
    border = colors.border_strong;
  }
  canvas.fill_rect(bounds_, raster::Paint::solid(background), radius);
  paint_outline(canvas, bounds_, radius, border, metrics.border_width);

  const bool has_value = selected_.has_value() && *selected_ < options_.size();
  const std::string_view shown =
      has_value ? display_label(selected_) : std::string_view(placeholder_);
  const math::Rect text_box{bounds_.x + metrics.space_md, bounds_.y,
                            std::max(bounds_.width - metrics.space_md * 2.0f - metrics.space_sm -
                                         k_chevron_size,
                                     0.0f),
                            bounds_.height};
  const math::Color text_color =
      !enabled_ ? colors.text_faint : (has_value ? colors.text : colors.text_faint);
  paint_line(context, canvas, shown, text_box, style_.font_size, text_color);

  const math::Rect chevron{bounds_.right() - metrics.space_md - k_chevron_size,
                           bounds_.y + (bounds_.height - k_chevron_size) * 0.5f, k_chevron_size,
                           k_chevron_size};
  Icon::draw(canvas, open_ ? "chevron-up" : "chevron-down", chevron,
             enabled_ ? colors.text_muted : colors.text_faint);
  if (enabled_ && focused_) paint_focus_ring(context, canvas, bounds_, radius);
}

auto Select::on_event(const RenderContext& context, Event& event) -> bool {
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
      if (cycle_consumed_) return true;  // down+up+click 三帧只切换一次
      cycle_consumed_ = true;
      set_open(!open_);
      return true;
    case EventKind::KeyDown: {
      const std::string& key = event.key;
      if (key == "Enter" || key == " " || key == "Space") {
        set_open(!open_);
        return true;
      }
      if (key == "Escape") {
        if (!open_) return false;
        set_open(false);
        return true;
      }
      if (key == "ArrowDown") {
        move_selection(1);
        return true;
      }
      if (key == "ArrowUp") {
        move_selection(-1);
        return true;
      }
      if (key == "Home") {
        if (options_.empty()) return false;
        set_selected_index(0U, true);
        return true;
      }
      if (key == "End") {
        if (options_.empty()) return false;
        set_selected_index(options_.size() - 1U, true);
        return true;
      }
      return false;
    }
    default: return false;
  }
}

void Select::activate() { set_open(!open_); }

auto Select::semantics_text() const -> std::string { return placeholder_; }

auto Select::semantics_value() const -> std::string {
  return selected_.has_value() ? std::string(selected_value()) : std::string{};
}

auto Select::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = selected_.has_value();
  flags.editable = open_;
  return flags;
}

auto Select::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return std::string(selected_value());
  if (name == "active") {
    return selected_.has_value() ? std::format("{}", *selected_) : std::string("none");
  }
  if (name == "label" || name == "text") return std::string(selected_label());
  if (name == "options") {
    std::string joined;
    for (const auto& option : options_) {
      if (!joined.empty()) joined.push_back('|');
      joined.append(shown_label(option));
    }
    return joined;
  }
  if (name == "open") return std::string(open_ ? "true" : "false");
  return std::nullopt;
}

auto Select::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value" || name == "label" || name == "text") {
    const auto index = index_of(value);
    if (!index.has_value()) return false;
    set_selected_index(index, false);
    return true;
  }
  if (name == "active") {
    if (ascii_iequals(value, "none") || value.empty()) {
      set_selected_index(std::nullopt, false);
      return true;
    }
    if (const auto index = parse_u64(value); index.has_value()) {
      if (*index >= options_.size()) return false;
      set_selected_index(static_cast<std::size_t>(*index), false);
      return true;
    }
    const auto index = index_of(value);
    if (!index.has_value()) return false;
    set_selected_index(index, false);
    return true;
  }
  if (name == "options") {
    std::vector<SelectOption> options;
    for (const auto part : split(value, '|')) {
      if (part.empty()) continue;
      SelectOption option;
      option.value = std::string(part);
      option.label = std::string(part);
      options.push_back(std::move(option));
    }
    set_options(std::move(options));
    return true;
  }
  if (name == "open") {
    const auto flag = parse_bool(value);
    if (!flag.has_value()) return false;
    set_open(*flag);
    return true;
  }
  return false;
}

auto Select::property_names() const -> std::vector<std::string_view> {
  return {"value", "active", "label", "options", "open"};
}

auto Select::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "open") {
    set_open(true);
    return true;
  }
  if (action == "close") {
    set_open(false);
    return true;
  }
  if (action == "toggle") {
    set_open(!open_);
    return true;
  }
  if (action == "next") {
    move_selection(1);
    return true;
  }
  if (action == "previous" || action == "prev") {
    move_selection(-1);
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
