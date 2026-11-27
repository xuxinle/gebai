#pragma once

/// **日期选择器**（`DatePicker`）：文本输入 `YYYY-MM-DD` + 点击弹出月历选日。
///
/// ## 两条输入路径，同一个值
///
/// | 路径 | 行为 |
/// |---|---|
/// | 手输 `2026-11-15` 回车 | 解析成功即生效；失败**如实拒绝**并保留原值 |
/// | 点输入框右侧按钮弹月历 | 点某天即选中并收起 |
///
/// 为什么两条都要：手输是熟手的快路径（也是剪贴板粘贴的唯一入口），月历是
/// "我想看看那个日期是周几"的路径——只留一条都会让另一半用户绕路。
///
/// ## 非法输入不静默兜底
///
/// `2026-02-30` / `abc` 一律**拒绝并保持原值**。静默兜底成今天（或空）是这类控件
/// 最糟的失败方式：用户以为设上了，实际是另一个日期，而中间没有任何提示。
/// 失败时 `invalid_text()` 为真，宿主可据此标红。
///
/// ## 与 `CalendarPanel` 的分工
///
/// 面板只管"画一个月并报告点了哪天"；本组件管"值的真值、文本同步、弹出与收起"。
/// 这样面板能独立复用（日程表内嵌月历）而不带弹出逻辑。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/calendar.hpp"
#include "st/ui/element.hpp"
#include "st/ui/host_focus.hpp"   // OverlayLayout（顶层枚举，不是 UiRoot 的嵌套）

namespace st::ui {

class CalendarPanel;
class DatePickerPanel;
class Input;

class DatePicker : public Element {
 public:
  static constexpr float kDefaultHeight{32.0f};
  static constexpr float kDefaultWidth{170.0f};
  static constexpr float kButtonWidth{28.0f};   ///< 右侧日历钮

  DatePicker();
  explicit DatePicker(calendar::Date value);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "DatePicker"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::TextInput; }

  // —— 值 ——
  [[nodiscard]] auto value() const noexcept -> calendar::Date { return value_; }
  /// 设值（`notify = false` 不触发 `on_change`；宿主同步用）。
  void set_value(calendar::Date value, bool notify = false);
  std::function<void(calendar::Date)> on_change{};

  /// 文本是否处于"用户输入了非法内容"状态（宿主可据此标红）。
  [[nodiscard]] auto invalid_text() const noexcept -> bool { return invalid_; }

  // —— 可选范围与「今天」（透传给面板）——
  void set_min(std::optional<calendar::Date> min);
  void set_max(std::optional<calendar::Date> max);
  void set_today(std::optional<calendar::Date> today);
  [[nodiscard]] auto min() const noexcept -> std::optional<calendar::Date> { return min_; }
  [[nodiscard]] auto max() const noexcept -> std::optional<calendar::Date> { return max_; }

  // —— 弹出 ——
  [[nodiscard]] auto is_open() const noexcept -> bool { return open_; }
  /// 打开月历弹层。无宿主（未上树）/不可用时**如实返回 false**。
  auto open() -> bool;
  void close();
  void toggle();

  // —— 几何 ——
  [[nodiscard]] auto field_rect() const -> math::Rect;
  [[nodiscard]] auto button_rect() const -> math::Rect;

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
  [[nodiscard]] auto semantics_value() const -> std::string override {
    return calendar::format_date(value_);
  }

  // —— 测试/自动化用的内部访问（不作为公共 API 承诺）——
  [[nodiscard]] auto text_field() -> Input* { return field_; }
  [[nodiscard]] auto panel() -> DatePickerPanel* { return panel_; }

 private:
  void push_text_to_field();
  void commit_text(std::string_view text);

  calendar::Date value_{};
  /// 是否有**真实设置过的值**（`value_` 的零值 `Date{}` = 1970-01-01，无法当哨兵）。
  ///
  /// 为何必须单独一位而不是“`value_ == Date{}` 就算空”：1970-01-01 是合法日期，
  /// 用户真的可以选它——用零值当哨兵会让"选了 1970-01-01"被当成"没选"。
  bool has_explicit_value_{false};
  std::optional<calendar::Date> min_{};
  std::optional<calendar::Date> max_{};
  std::optional<calendar::Date> today_{};
  bool invalid_{false};
  bool open_{false};

  Input* field_{nullptr};              ///< 观察指针（所有权在子树）
  DatePickerPanel* panel_{nullptr};    ///< 弹层（观察指针）

  math::Color surface_{};
  math::Color border_{};
  math::Color text_color_{};
  math::Color text_faint_{};
  float font_size_{13.0f};
};

}  // namespace st::ui

#include "st/ui/component_registry.hpp"
ST_UI_COMPONENT(::st::ui::DatePicker, "DatePicker")
