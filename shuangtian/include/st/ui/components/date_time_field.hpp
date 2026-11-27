#pragma once

/// **日期时间字段**（`DateTimeField`）：日期 + 时间 + **可清空**。
///
/// ## 为什么需要"可空"
///
/// 待办的「到期时间」是**可选**的：空 = 不提醒。用一个"总得有个值"的控件表达
/// "没有值"，只能靠哨兵值（0 / 1970-01-01 / 空串），而那类哨兵最终一定会
/// 在某个分支里被当成真值用（"1970 年到期"这种通知真的会发出来）。
/// 所以空态是一等状态：`has_value()` 为假、两个子控件的文本是占位符、清空钮可见。
///
/// ## 组合方式
///
/// 内部是 `[DatePicker][TimePicker][清空钮]` 三个子元素：
///
/// - 有值时清空钮可见且可点；空态时它就**不可点**（而不是隐藏——隐藏会让左右
///   元素在"有没有值"之间跳位，看着像布局在抖）。
/// - 空态点日期或时间**直接给一个默认值**（今天 09:00）：用户点它就是为了填值，
///   再让他"先点清空再点设置"是多余的一步。
///
/// 默认时刻取 **09:00** 而不是 00:00：待办的到期时间几乎都在白天，
/// 00:00 会被理解成"前一晚午夜"。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/calendar.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class DatePicker;
class TimePicker;

/// 日期 + 时刻。
struct DateTimeValue {
  calendar::Date date{};
  calendar::Clock clock{};

  [[nodiscard]] friend constexpr auto operator==(const DateTimeValue&, const DateTimeValue&)
      -> bool = default;
};

class DateTimeField : public Element {
 public:
  static constexpr float kDefaultHeight{32.0f};
  static constexpr float kDefaultWidth{300.0f};
  static constexpr float kGap{6.0f};
  static constexpr float kClearWidth{26.0f};
  static constexpr float kDateWidth{150.0f};

  DateTimeField();

  [[nodiscard]] auto type() const noexcept -> std::string_view override {
    return "DateTimeField";
  }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::TextInput; }

  // —— 值 ——
  [[nodiscard]] auto has_value() const noexcept -> bool { return value_.has_value(); }
  [[nodiscard]] auto value() const noexcept -> std::optional<DateTimeValue> { return value_; }
  /// 设值（`notify = false` 不触发 `on_change`）。
  void set_value(std::optional<DateTimeValue> value, bool notify = false);
  void clear(bool notify = false);
  std::function<void(std::optional<DateTimeValue>)> on_change{};

  // —— 透传给子控件 ——
  void set_min(std::optional<calendar::Date> min);
  void set_max(std::optional<calendar::Date> max);
  void set_today(std::optional<calendar::Date> today);
  void set_minute_step(int step);

  // —— 几何 ——
  [[nodiscard]] auto date_rect() const -> math::Rect;
  [[nodiscard]] auto time_rect() const -> math::Rect;
  [[nodiscard]] auto clear_rect() const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action,
                                   std::string_view argument) -> bool override;
  [[nodiscard]] auto semantics_value() const -> std::string override;

  [[nodiscard]] auto date_picker() -> DatePicker* { return date_; }
  [[nodiscard]] auto time_picker() -> TimePicker* { return time_; }

 private:
  void sync_children(bool notify);

  std::optional<DateTimeValue> value_{};
  std::optional<calendar::Date> min_{};
  std::optional<calendar::Date> max_{};
  std::optional<calendar::Date> today_{};
  int minute_step_{5};

  DatePicker* date_{nullptr};
  TimePicker* time_{nullptr};

  math::Color border_{};
  float font_size_{13.0f};
};

}  // namespace st::ui

#include "st/ui/component_registry.hpp"
ST_UI_COMPONENT(::st::ui::DateTimeField, "DateTimeField")
