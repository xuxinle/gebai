#include "st/ui/components/date_picker.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include "st/raster/paint.hpp"
#include "st/ui/components/calendar_panel.hpp"
#include "st/ui/components/date_picker_panel.hpp"
#include "st/ui/components/input_box.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"
#include "components_internal.hpp"

namespace st::ui {

DatePicker::DatePicker() : value_(calendar::Date{}) {
  // **默认构造 = 没有值**：`has_explicit_value_` 保持 false，于是空态下打开日历
  // 会定位到 today（而不是 1970），也不会画一个“看着已选中”的高亮。
  //
  // 这里把带参构造的初始化体抽不开（C++ 委派构造无法回退标志位），
  // 所以两个构造各写一份初始化：重复的两行比“委派后再改状态”好读。
  set_focusable(true);
  auto field = std::make_unique<Input>();
  field->set_id("field");
  field->set_placeholder(calendar::format_date(value_));
  field->on_change = [this](std::string_view text) {
    // 手输不即时生效：等提交（回车/失焦），否则用户打到一半就被判非法
    invalid_ = false;
    (void)text;
  };
  field->on_submit = [this](std::string_view text) { commit_text(text); };
  field_ = field.get();
  add_child(std::move(field));
}

DatePicker::DatePicker(calendar::Date value)
    : value_(calendar::clamp_day(value)), has_explicit_value_(true) {
  set_focusable(true);
  // 子元素：文本框 + 右侧日历钮（钮不单独建模，本组件自己画并命中——
  // 它是"输入框的附属件"，做成独立 Button 会多一层焦点/语义噪音）。
  auto field = std::make_unique<Input>();
  field->set_id("field");
  field->on_submit = [this](std::string_view text) { commit_text(text); };
  field_ = field.get();
  add_child(std::move(field));
  push_text_to_field();
}

void DatePicker::push_text_to_field() {
  if (field_ == nullptr) return;
  const std::string text = calendar::format_date(value_);
  // **同值不写**：每帧回写会把用户正在输入的内容冲掉（`set_text` 还会重置光标）。
  if (field_->value() == text) return;
  field_->set_text(text);
}

void DatePicker::set_value(calendar::Date value, bool notify) {
  const calendar::Date clamped = calendar::clamp_day(value);
  // **标志位要先于早退置位**：`clamped == value_` 在空态下可能成立
  // （零值就是 1970-01-01），而那次调用恰恰是“用户真的选了 1970-01-01”——
  // 早退掉就会让“选了它”与“没选”混为一谈（实测：这条让回归用例变红）。
  has_explicit_value_ = true;
  if (clamped == value_) {
    mark_dirty();
    if (notify && on_change) on_change(value_);
    return;
  }
  value_ = clamped;
  invalid_ = false;
  push_text_to_field();
  if (panel_ != nullptr) panel_->set_value(value_);
  mark_dirty();
  if (notify && on_change) on_change(value_);
}

void DatePicker::set_min(std::optional<calendar::Date> min) {
  min_ = min;
  if (panel_ != nullptr) panel_->set_min(min);
}

void DatePicker::set_max(std::optional<calendar::Date> max) {
  max_ = max;
  if (panel_ != nullptr) panel_->set_max(max);
}

void DatePicker::set_today(std::optional<calendar::Date> today) {
  today_ = today;
  if (panel_ != nullptr) panel_->set_today(today);
}

void DatePicker::commit_text(std::string_view text) {
  const auto parsed = calendar::parse_date(text);
  if (!parsed.ok) {
    // **如实拒绝**：保留原值，只把文本标成非法（宿主可标红）。
    // 静默兜底是这类控件最糟的失败方式——用户以为设上了，实际是另一个日期。
    invalid_ = true;
    mark_dirty();
    return;
  }
  invalid_ = false;
  // 范围外的输入也拒绝：与月历里点不动是同一套规则，避免"输入能绕过 min/max"。
  if (min_.has_value() && parsed.value < *min_) {
    invalid_ = true;
    mark_dirty();
    return;
  }
  if (max_.has_value() && *max_ < parsed.value) {
    invalid_ = true;
    mark_dirty();
    return;
  }
  const bool changed = parsed.value != value_;
  value_ = parsed.value;
  has_explicit_value_ = true;
  push_text_to_field();
  mark_dirty();
  if (changed && on_change) on_change(value_);
}

// —— 弹出 ——

auto DatePicker::open() -> bool {
  if (open_) return true;
  if (host() == nullptr) return false;   // 未上树：如实拒绝（不假装成功）
  // **初始视图**：有值就定位到那个月；没值就定位到“今天”。
  //
  // 为何不是直接 `value_`：那在空态下是 `Date{}`（= 1970-01-01），于是用户点开
  // 日历看到的是 56 年前的 1 月，得逐月翻到今年——实测截图撞到。
  // 而“今天”是宿主注入的（组件不读系统时钟，见 `CalendarPanel::set_today` 的理由），
  // 宿主没注入时退回 `value_`（服务端/无头场景仍需一个确定值）。
  const calendar::Date initial = has_explicit_value_ ? value_ : today_.value_or(value_);
  auto calendar_panel = std::make_unique<CalendarPanel>(initial);
  calendar_panel->set_id(id().empty() ? "date-picker-calendar" : std::string(id()) + "-calendar");
  // 初始视野由上行的 `initial` 一手决定（构造时就定下来了），**不再补一次
  // `show_month`**：两道机制做同一件事会让回归测试失去判别力——实测过：
  // 单独回退任意一道，用例依旧全绿（另一道把它补上了）。
  //
  // 至于“今天≠已选中”：靠 `CalendarPanel` 自己“只有 `value` 属当前视野月份才画
  // 高亮”的口径——空态下 `has_explicit_value_` 为 false，值仍是零值，
  // 而视野在 today 的月份，两者不同月，所以不画选中。
  calendar_panel->set_min(min_);
  calendar_panel->set_max(max_);
  calendar_panel->set_today(today_);
  // 选日 → 回写值并收起（选定即收，同 ThemePicker 的口径）。
  calendar_panel->on_change = [this](calendar::Date date) {
    const bool changed = date != value_;
    value_ = date;
    has_explicit_value_ = true;
    invalid_ = false;
    push_text_to_field();
    mark_dirty();
    if (changed && on_change) on_change(value_);
    close();
  };

  auto panel = std::make_unique<DatePickerPanel>(std::move(calendar_panel));
  panel->set_id(id().empty() ? "date-picker-panel" : std::string(id()) + "-panel");
  panel->on_dismiss = [this] { close(); };
  DatePickerPanel* raw = panel.get();
  if (!host()->host_add_overlay(std::move(panel), OverlayLayout::FillViewport)) {
    return false;
  }
  panel_ = raw;
  open_ = true;
  mark_dirty();
  return true;
}

void DatePicker::close() {
  if (!open_) return;
  open_ = false;
  if (panel_ != nullptr) {
    panel_->set_visible(false);
    if (host() != nullptr) (void)host()->host_remove_overlay(panel_);
    panel_ = nullptr;
  }
  mark_dirty();
}

void DatePicker::toggle() {
  if (open_) close();
  else (void)open();
}

// —— 几何 ——

auto DatePicker::field_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  const float width = std::max(0.0f, bounds_.width - kButtonWidth);
  return math::Rect{bounds_.x, bounds_.y, width, bounds_.height};
}

auto DatePicker::button_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  return math::Rect{bounds_.right() - kButtonWidth, bounds_.y, kButtonWidth, bounds_.height};
}

void DatePicker::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  surface_ = colors.surface;
  border_ = colors.border;
  text_color_ = colors.text;
  text_faint_ = colors.text_faint;
  font_size_ = theme.metrics().font_sm;
  if (field_ != nullptr) field_->apply_theme(theme);
}

void DatePicker::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float width = style_.has_explicit_width() ? style_.width : kDefaultWidth;
  float height = style_.has_explicit_height() ? style_.height : kDefaultHeight;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void DatePicker::arrange(const RenderContext& context, math::Rect rect) {
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

auto DatePicker::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (event.kind == EventKind::Click) {
    if (button_rect().contains(event.position)) {
      toggle();
      return true;
    }
  }
  // 其余事件（输入、光标、键盘）交给文本框：事件先派给子元素（命中链正常会先到它），
  // 这里只兜"点在文本框上"的那部分。
  return false;
}

void DatePicker::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  // 右侧日历钮：不画就只是一个 28px 的空白区，而用户找不到“怎么弹出来”。
  // （实测：首版漏了这一步，截图上去就是「输入框右边空一块」。）
  const math::Rect button = button_rect();
  const math::Color icon_color = open_ ? context.theme.colors().primary : text_faint_;
  Icon::draw(canvas, "calendar",
             math::Rect{button.x + (button.width - 14.0f) * 0.5f,
                        button.y + (button.height - 14.0f) * 0.5f, 14.0f, 14.0f},
             icon_color, 0.0f);
}

// —— 属性面 / 动作面 ——

auto DatePicker::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return calendar::format_date(value_);
  if (name == "min") {
    return min_.has_value() ? std::optional(calendar::format_date(*min_))
                            : std::optional<std::string>();
  }
  if (name == "max") {
    return max_.has_value() ? std::optional(calendar::format_date(*max_))
                            : std::optional<std::string>();
  }
  if (name == "today") {
    return today_.has_value() ? std::optional(calendar::format_date(*today_))
                              : std::optional<std::string>();
  }
  if (name == "open") return std::string(open_ ? "true" : "false");
  if (name == "invalid") return std::string(invalid_ ? "true" : "false");
  // “是否真的设过值”：`value_` 的零值 1970-01-01 是**合法日期**，
  // 所以“选了 1970-01-01”与“没选”必须能区分（用零值当哨兵就分不出来）。
  if (name == "has_explicit_value") {
    return std::string(has_explicit_value_ ? "true" : "false");
  }
  return Element::get_property(name);
}

auto DatePicker::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value") {
    const auto parsed = calendar::parse_date(value);
    if (!parsed.ok) return false;
    set_value(parsed.value, false);
    return true;
  }
  if (name == "min" || name == "max" || name == "today") {
    if (value.empty()) {
      if (name == "min") set_min(std::nullopt);
      else if (name == "max") set_max(std::nullopt);
      else set_today(std::nullopt);
      return true;
    }
    const auto parsed = calendar::parse_date(value);
    if (!parsed.ok) return false;
    if (name == "min") set_min(parsed.value);
    else if (name == "max") set_max(parsed.value);
    else set_today(parsed.value);
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

auto DatePicker::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  for (const std::string_view extra :
       {"value", "min", "max", "today", "open", "invalid", "has_explicit_value"}) {
    names.push_back(extra);
  }
  return names;
}

auto DatePicker::invoke_action(std::string_view action, std::string_view argument) -> bool {
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
    const auto parsed = calendar::parse_date(argument);
    if (!parsed.ok) return false;
    set_value(parsed.value, true);
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

ST_UI_COMPONENT_IMPL(::st::ui::DatePicker);
