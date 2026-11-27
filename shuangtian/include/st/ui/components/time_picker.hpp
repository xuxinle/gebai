#pragma once

/// **时间选择器**（`TimePicker`）：文本输入 `HH:MM` + 弹出时/分两列选择。
///
/// ## 为什么是两列而不是一个滚轮
///
/// 待办/日程的典型输入是「14:30」这种**两个离散维度**：时（0~23）与分（按步进，默认 5）。
/// 两列并列的点击次数远少于滚轮（滚轮要滚过中间所有值，而"从 9 点到 17 点"要滚 8 格）。
/// 列内可滚动，但**直接点**才是主路径。
///
/// ## 份钟步进（`minute_step`）
///
/// 默认 5 分钟。用户仍可**手输任意分钟**（`14:07` 合法）：步进只约束"列里给哪些候选"，
/// 不是值的合法性边界。把手输也吸附到步进会让"我就想设 14:07"变成不可能——
/// 而通知类场景恰恰常要具体到分。
///
/// ## 与 `DatePicker` 的一致性
///
/// 同一套约定：手输回车即生效、非法**如实拒绝并保留原值**、`invalid_text()` 供宿主标红、
/// 弹出走宿主契约（`host_add_overlay` + `FillViewport`），无宿主时 `open()` 返回 false。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/calendar.hpp"
#include "st/ui/element.hpp"
#include "st/ui/host_focus.hpp"

namespace st::ui {

class Input;
class TimePickerPanel;

class TimePicker : public Element {
 public:
  static constexpr float kDefaultHeight{32.0f};
  static constexpr float kDefaultWidth{120.0f};
  static constexpr float kButtonWidth{28.0f};

  TimePicker();
  explicit TimePicker(calendar::Clock value);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "TimePicker"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::TextInput; }

  [[nodiscard]] auto value() const noexcept -> calendar::Clock { return value_; }
  void set_value(calendar::Clock value, bool notify = false);
  std::function<void(calendar::Clock)> on_change{};

  /// 列里给出的分钟候选间隔（默认 5）。**不影响手输的合法性**。
  void set_minute_step(int step);
  [[nodiscard]] auto minute_step() const noexcept -> int { return minute_step_; }

  [[nodiscard]] auto invalid_text() const noexcept -> bool { return invalid_; }

  [[nodiscard]] auto is_open() const noexcept -> bool { return open_; }
  auto open() -> bool;
  void close();
  void toggle();

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
    return calendar::format_clock(value_);
  }

  [[nodiscard]] auto text_field() -> Input* { return field_; }
  [[nodiscard]] auto panel() -> TimePickerPanel* { return panel_; }

 private:
  void push_text_to_field();
  void commit_text(std::string_view text);
  void apply_clock(calendar::Clock value, bool notify);

  calendar::Clock value_{};
  int minute_step_{5};
  bool invalid_{false};
  bool open_{false};

  Input* field_{nullptr};
  TimePickerPanel* panel_{nullptr};

  math::Color surface_{};
  math::Color border_{};
  math::Color text_color_{};
  float font_size_{13.0f};
};

}  // namespace st::ui

#include "st/ui/component_registry.hpp"
ST_UI_COMPONENT(::st::ui::TimePicker, "TimePicker")
