#include "st/ui/components/time_picker.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include "st/raster/paint.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/time_picker_panel.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"
#include "components_internal.hpp"

namespace st::ui {

TimePicker::TimePicker() : TimePicker(calendar::Clock{}) {}

TimePicker::TimePicker(calendar::Clock value) : value_(value) {
  set_focusable(true);
  auto field = std::make_unique<Input>();
  field->set_id("field");
  field->on_submit = [this](std::string_view text) { commit_text(text); };
  field_ = field.get();
  add_child(std::move(field));
  push_text_to_field();
}

void TimePicker::push_text_to_field() {
  if (field_ == nullptr) return;
  const std::string text = calendar::format_clock(value_);
  // 同值不写：每帧回写会把用户正在输入的内容冲掉（`set_text` 还会重置光标）。
  if (field_->value() == text) return;
  field_->set_text(text);
}

void TimePicker::apply_clock(calendar::Clock value, bool notify) {
  const bool changed = !(value == value_);
  value_ = value;
  invalid_ = false;
  push_text_to_field();
  if (panel_ != nullptr) panel_->set_value(value_);
  mark_dirty();
  if (notify && changed && on_change) on_change(value_);
}

void TimePicker::set_value(calendar::Clock value, bool notify) { apply_clock(value, notify); }

void TimePicker::set_minute_step(int step) { minute_step_ = step < 1 ? 1 : step; }

void TimePicker::commit_text(std::string_view text) {
  const auto parsed = calendar::parse_clock(text);
  if (!parsed.ok) {
    // 如实拒绝：保留原值，只标非法。静默兜底会让"我设了 25:00"变成某个合法时刻。
    invalid_ = true;
    mark_dirty();
    return;
  }
  apply_clock(parsed.value, true);
}

auto TimePicker::open() -> bool {
  if (open_) return true;
  if (host() == nullptr) return false;
  auto panel = std::make_unique<TimePickerPanel>(value_, minute_step_);
  panel->set_id(id().empty() ? "time-picker-panel" : std::string(id()) + "-panel");
  panel->on_pick = [this](calendar::Clock picked) {
    apply_clock(picked, true);
    close();   // 选定即收（同 DatePicker 的口径）
  };
  panel->on_dismiss = [this] { close(); };
  TimePickerPanel* raw = panel.get();
  if (!host()->host_add_overlay(std::move(panel), OverlayLayout::FillViewport)) return false;
  panel_ = raw;
  open_ = true;
  mark_dirty();
  return true;
}

void TimePicker::close() {
  if (!open_) return;
  open_ = false;
  if (panel_ != nullptr) {
    panel_->set_visible(false);
    if (host() != nullptr) (void)host()->host_remove_overlay(panel_);
    panel_ = nullptr;
  }
  mark_dirty();
}

void TimePicker::toggle() {
  if (open_) close();
  else (void)open();
}

auto TimePicker::field_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  return math::Rect{bounds_.x, bounds_.y, std::max(0.0f, bounds_.width - kButtonWidth),
                    bounds_.height};
}

auto TimePicker::button_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  return math::Rect{bounds_.right() - kButtonWidth, bounds_.y, kButtonWidth, bounds_.height};
}

void TimePicker::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  surface_ = colors.surface;
  border_ = colors.border;
  text_color_ = colors.text;
  font_size_ = theme.metrics().font_sm;
  if (field_ != nullptr) field_->apply_theme(theme);
}

void TimePicker::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float width = style_.has_explicit_width() ? style_.width : kDefaultWidth;
  float height = style_.has_explicit_height() ? style_.height : kDefaultHeight;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void TimePicker::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  layout_dirty_ = false;
  if (field_ == nullptr) return;
  const math::Rect field = field_rect();
  Constraints own;
  own.max_width = field.width;
  own.max_height = field.height;
  own.available_width = field.width;
  own.available_height = field.height;
  field_->measure(context, own);
  field_->arrange(context, field);
}

auto TimePicker::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (event.kind == EventKind::Click && button_rect().contains(event.position)) {
    toggle();
    return true;
  }
  return false;
}

void TimePicker::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  // 右侧时钟钮（理由同 DatePicker：不画就只是一块空白，用户找不到入口）。
  const math::Rect button = button_rect();
  const math::Color icon_color = open_ ? context.theme.colors().primary : text_color_;
  Icon::draw(canvas, "clock",
             math::Rect{button.x + (button.width - 14.0f) * 0.5f,
                        button.y + (button.height - 14.0f) * 0.5f, 14.0f, 14.0f},
             icon_color, 0.0f);
}

auto TimePicker::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return calendar::format_clock(value_);
  if (name == "minute_step") return std::to_string(minute_step_);
  if (name == "open") return std::string(open_ ? "true" : "false");
  if (name == "invalid") return std::string(invalid_ ? "true" : "false");
  return Element::get_property(name);
}

auto TimePicker::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value") {
    const auto parsed = calendar::parse_clock(value);
    if (!parsed.ok) return false;
    apply_clock(parsed.value, false);
    return true;
  }
  if (name == "minute_step") {
    const auto step = st::parse_i64(value);
    if (!step.has_value() || *step < 1 || *step > 60) return false;
    set_minute_step(static_cast<int>(*step));
    return true;
  }
  if (name == "open") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    if (*parsed) return open();
    close();
    return true;
  }
  return Element::set_property(name, value);
}

auto TimePicker::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  for (const std::string_view extra : {"value", "minute_step", "open", "invalid"}) {
    names.push_back(extra);
  }
  return names;
}

auto TimePicker::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "open") return open();
  if (action == "close") {
    close();
    return true;
  }
  if (action == "toggle" || action == "activate") {
    toggle();
    return true;
  }
  if (action == "submit") {
    if (field_ != nullptr) commit_text(field_->value());
    return true;
  }
  if (action == "select") {
    const auto parsed = calendar::parse_clock(argument);
    if (!parsed.ok) return false;
    apply_clock(parsed.value, true);
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

ST_UI_COMPONENT_IMPL(::st::ui::TimePicker);
