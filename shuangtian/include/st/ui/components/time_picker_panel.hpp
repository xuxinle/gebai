#pragma once

/// **时间选择弹层**（`TimePickerPanel`）：半透明遮罩 + 居中的「时 / 分」两列卡片。
///
/// 与 `DatePickerPanel` 同构（遮罩 + 居中卡片 + Esc/点外部收起），差别只在卡片里
/// 装的是两列而不是月历。
///
/// ## 两列的交互
///
/// 单击某格即选定该维度并**收起**（时或分单独改完就结束）——这是待办/日程的
/// 典型节奏（"改成 17 点"只需要一次点击）。两列同时改就先改时、再开一次改分；
/// 做"确认按钮"反而多一次点击。

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

class TimePickerPanel : public Element {
 public:
  static constexpr float kColumnWidth{64.0f};
  static constexpr float kRowHeight{28.0f};
  static constexpr float kMaxVisibleRows{8};
  static constexpr float kHeaderHeight{28.0f};
  static constexpr float kPadding{8.0f};

  TimePickerPanel(calendar::Clock value, int minute_step);

  [[nodiscard]] auto type() const noexcept -> std::string_view override {
    return "TimePickerPanel";
  }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Dialog; }
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return visible(); }

  [[nodiscard]] auto value() const noexcept -> calendar::Clock { return value_; }
  void set_value(calendar::Clock value);

  [[nodiscard]] auto card_rect() const -> math::Rect;
  /// 第 `index` 个"时"格 / "分"格矩形（测试与命中共用）。
  [[nodiscard]] auto hour_rect(int index) const -> math::Rect;
  [[nodiscard]] auto minute_rect(int index) const -> math::Rect;
  [[nodiscard]] auto minute_count() const noexcept -> int { return minute_count_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  /// 选定（时或分）。参数是**选完之后的完整时间**。
  std::function<void(calendar::Clock)> on_pick{};
  std::function<void()> on_dismiss{};

 private:
  void rebuild_minutes();
  void scroll_row(int column, int delta);

  calendar::Clock value_{};
  int minute_step_{5};
  std::vector<int> minutes_{};
  int minute_count_{0};
  /// 两列各自的滚动偏移（以"行"为单位，0 = 从第一个候选开始）
  int hour_scroll_{0};
  int minute_scroll_{0};

  math::Rect card_{};
  math::Color overlay_{};
  math::Color surface_{};
  math::Color surface_hover_{};
  math::Color text_color_{};
  math::Color text_muted_{};
  math::Color primary_{};
  math::Color primary_soft_{};
  float radius_{12.0f};
  float font_size_{13.0f};
};

}  // namespace st::ui
