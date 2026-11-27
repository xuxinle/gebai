#include "st/ui/components/date_time_field.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include "st/raster/paint.hpp"
#include "st/ui/components/date_picker.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/time_picker.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"
#include "components_internal.hpp"

namespace st::ui {

namespace {

/// 空态下的占位文本。与"有值"的格式**同宽**（`YYYY-MM-DD` / `HH:MM`），
/// 这样点进空字段时不会有文字宽度突变。
constexpr std::string_view kDatePlaceholder{"年-月-日"};
constexpr std::string_view kTimePlaceholder{"时:分"};

}  // namespace

DateTimeField::DateTimeField() {
  set_focusable(true);

  auto date = std::make_unique<DatePicker>();
  date->set_id("date");
  date->on_change = [this](calendar::Date picked) {
    // 空态点日期 = 用户想填值：时刻用默认 09:00。
    DateTimeValue next = value_.value_or(DateTimeValue{picked, calendar::Clock{9, 0}});
    next.date = picked;
    value_ = next;
    sync_children(true);
  };
  date_ = date.get();
  add_child(std::move(date));

  auto time = std::make_unique<TimePicker>();
  time->set_id("time");
  time->on_change = [this](calendar::Clock picked) {
    DateTimeValue next = value_.value_or(DateTimeValue{today_.value_or(calendar::Date{}),
                                                       calendar::Clock{9, 0}});
    next.clock = picked;
    value_ = next;
    sync_children(true);
  };
  time_ = time.get();
  add_child(std::move(time));

  sync_children(false);
}

void DateTimeField::sync_children(bool notify) {
  if (date_ != nullptr) {
    if (value_.has_value()) {
      date_->set_value(value_->date, false);
    } else if (auto* field = date_->text_field(); field != nullptr) {
      // 空态：输入框显示占位符而不是某个具体日期——显示具体日期等于
      // 悄悄给了一个值（用户会以为已经设上了）。
      field->set_text(std::string(kDatePlaceholder));
    }
  }
  if (time_ != nullptr) {
    if (value_.has_value()) {
      time_->set_value(value_->clock, false);
    } else if (auto* field = time_->text_field(); field != nullptr) {
      field->set_text(std::string(kTimePlaceholder));
    }
  }
  mark_dirty();
  if (notify && on_change) on_change(value_);
}

void DateTimeField::set_value(std::optional<DateTimeValue> value, bool notify) {
  if (value.has_value()) {
    value->date = calendar::clamp_day(value->date);
    if (min_.has_value() && value->date < *min_) value->date = *min_;
    if (max_.has_value() && *max_ < value->date) value->date = *max_;
  }
  value_ = value;
  sync_children(notify);
}

void DateTimeField::clear(bool notify) {
  value_.reset();
  sync_children(notify);
}

void DateTimeField::set_min(std::optional<calendar::Date> min) {
  min_ = min;
  if (date_ != nullptr) date_->set_min(min);
}

void DateTimeField::set_max(std::optional<calendar::Date> max) {
  max_ = max;
  if (date_ != nullptr) date_->set_max(max);
}

void DateTimeField::set_today(std::optional<calendar::Date> today) {
  today_ = today;
  if (date_ != nullptr) date_->set_today(today);
}

void DateTimeField::set_minute_step(int step) {
  minute_step_ = step < 1 ? 1 : step;
  if (time_ != nullptr) time_->set_minute_step(minute_step_);
}

// —— 几何 ——

auto DateTimeField::date_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  const float available = std::max(0.0f, bounds_.width - kClearWidth - kGap * 2.0f);
  const float date_width = std::min(kDateWidth, available * 0.58f);
  return math::Rect{bounds_.x, bounds_.y, date_width, bounds_.height};
}

auto DateTimeField::time_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  const math::Rect date = date_rect();
  const float available = std::max(0.0f, bounds_.width - kClearWidth - kGap * 2.0f);
  const float time_width = std::max(0.0f, available - date.width);
  return math::Rect{date.right() + kGap, bounds_.y, time_width, bounds_.height};
}

auto DateTimeField::clear_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  return math::Rect{bounds_.right() - kClearWidth, bounds_.y, kClearWidth, bounds_.height};
}

void DateTimeField::apply_theme(const Theme& theme) {
  border_ = theme.colors().border;
  font_size_ = theme.metrics().font_sm;
  if (date_ != nullptr) date_->apply_theme(theme);
  if (time_ != nullptr) time_->apply_theme(theme);
}

void DateTimeField::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float width = style_.has_explicit_width() ? style_.width : kDefaultWidth;
  float height = style_.has_explicit_height() ? style_.height : kDefaultHeight;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void DateTimeField::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  layout_dirty_ = false;
  const auto place = [&](Element* element, math::Rect target) {
    if (element == nullptr) return;
    Constraints own;
    own.max_width = target.width;
    own.max_height = target.height;
    own.available_width = target.width;
    own.available_height = target.height;
    element->measure(context, own);
    element->arrange(context, target);
  };
  place(date_, date_rect());
  place(time_, time_rect());
}

void DateTimeField::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  (void)context;
  if (bounds_.is_empty()) return;
  const math::Rect clear = clear_rect();
  // 清空钮：**空态时不可点**（画成极淡），而不是隐藏——隐藏会让左右子控件在
  // "有没有值"之间跳位，看着像布局在抖。
  //
  // 淡色用 `border_` 而不是 `text_faint_`：后者是给**正文次级信息**的
  // （深色主题下 #696976），拿它当"禁用图形"看着仍像可用按钮——实测截图里
  // 空态的 × 与有值态的分不出差别。`border_` 的对比度刚好是"看得见但不想点"。
  const bool enabled = value_.has_value();
  Icon::draw(canvas, "close",
             math::Rect{clear.x + (clear.width - 12.0f) * 0.5f,
                        clear.y + (clear.height - 12.0f) * 0.5f, 12.0f, 12.0f},
             enabled ? context.theme.colors().text_muted : border_, 0.0f);
}

auto DateTimeField::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (event.kind == EventKind::Click && clear_rect().contains(event.position)) {
    if (!value_.has_value()) return true;   // 空态点了没反应（不假装清空成功）
    clear(true);
    return true;
  }
  return false;
}

auto DateTimeField::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") {
    if (!value_.has_value()) return std::string();   // 空串 = 没有值
    return calendar::format_date(value_->date) + " " + calendar::format_clock(value_->clock);
  }
  if (name == "has_value") return std::string(value_.has_value() ? "true" : "false");
  if (name == "min") {
    return min_.has_value() ? std::optional(calendar::format_date(*min_))
                            : std::optional<std::string>();
  }
  if (name == "max") {
    return max_.has_value() ? std::optional(calendar::format_date(*max_))
                            : std::optional<std::string>();
  }
  if (name == "minute_step") return std::to_string(minute_step_);
  return Element::get_property(name);
}

auto DateTimeField::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value") {
    if (value.empty()) {
      clear(false);
      return true;
    }
    // `YYYY-MM-DD HH:MM`（时间可省，缺省 09:00——与空态点日期的默认一致）
    std::string text(value);
    const std::size_t space = text.find(' ');
    const auto date = calendar::parse_date(space == std::string::npos ? text
                                                                     : text.substr(0, space));
    if (!date.ok) return false;
    calendar::Clock clock{9, 0};
    if (space != std::string::npos) {
      const auto parsed_clock = calendar::parse_clock(text.substr(space + 1));
      if (!parsed_clock.ok) return false;
      clock = parsed_clock.value;
    }
    set_value(DateTimeValue{date.value, clock}, false);
    return true;
  }
  if (name == "min" || name == "max") {
    if (value.empty()) {
      if (name == "min") set_min(std::nullopt);
      else set_max(std::nullopt);
      return true;
    }
    const auto parsed = calendar::parse_date(value);
    if (!parsed.ok) return false;
    if (name == "min") set_min(parsed.value);
    else set_max(parsed.value);
    return true;
  }
  if (name == "minute_step") {
    const auto step = st::parse_i64(value);
    if (!step.has_value() || *step < 1 || *step > 60) return false;
    set_minute_step(static_cast<int>(*step));
    return true;
  }
  return Element::set_property(name, value);
}

auto DateTimeField::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  for (const std::string_view extra : {"value", "has_value", "min", "max", "minute_step"}) {
    names.push_back(extra);
  }
  return names;
}

auto DateTimeField::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "clear") {
    clear(true);
    return true;
  }
  if (action == "select") {
    return set_property("value", argument);
  }
  return Element::invoke_action(action, argument);
}

auto DateTimeField::semantics_value() const -> std::string {
  if (!value_.has_value()) return std::string();
  return calendar::format_date(value_->date) + " " + calendar::format_clock(value_->clock);
}

}  // namespace st::ui

ST_UI_COMPONENT_IMPL(::st::ui::DateTimeField);
