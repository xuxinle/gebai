#pragma once

/// **月历面板**（`CalendarPanel`）：单月 6×7 网格 + 翻月 + 选日。
///
/// ## 为什么在框架层（而不是待办页自己画）
///
/// 「挑一个日期」是**任何带截止时间/排期的东西**都要的能力：待办、日程、报表区间、
/// 文件筛选。它又恰好是**很容易画错且难自查**的一类——少一天、错一格、闰年 2 月
/// 只 28 天，界面上看着都像"这个月就是这样"。所以日历算法拆进了
/// `st/ui/calendar.hpp`（纯函数、可穷举断言），本组件只负责把它画出来并派发点击。
///
/// ## 固定 6 行，不随月份变高
///
/// 网格恒为 42 格（不足补空）。否则切到"1 日是周日且 31 天"的月份时面板会长一行，
/// 弹层上下跳——而这类跳动最容易被当成"闪了一下"而查不到原因。
///
/// ## 邻月格子画空、不可点
///
/// 不显示上月/下月的真实日期：点了它会跳到别的月，与"我在挑这个月的某天"
/// 的心智冲突。空位在视觉上也表达"这里不属于本月"。
///
/// ## 禁用日（`min` / `max`）
///
/// 早于 min / 晚于 max 的日子画成弱化且**不可点**。给的是"可选范围"而不是
/// "校验提示"：选不中比选完再报错好——后者让用户先白点一次。
///
/// 边界语义：`min` / `max` **包含**（`min=2026-11-01` 时 1 号可选）。

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

class CalendarPanel : public Element {
 public:
  static constexpr float kCellSize{30.0f};      ///< 单格见方
  static constexpr float kHeaderHeight{32.0f};  ///< 月份标题 + 翻月按钮行
  static constexpr float kWeekRowHeight{20.0f}; ///< 星期表头行
  static constexpr float kPadding{8.0f};

  CalendarPanel();
  explicit CalendarPanel(calendar::Date value);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CalendarPanel"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::List; }

  // —— 值 ——
  [[nodiscard]] auto value() const noexcept -> calendar::Date { return value_; }
  /// 设选中日（`notify = false` 时不触发 `on_change`）。
  ///
  /// 为何要这个开关（同 `ColorPicker::set_color`）：宿主"把当前值同步进控件"时
  /// 不该收到回调，否则会与用户操作混在一起（设一次触发两次业务逻辑）。
  void set_value(calendar::Date value, bool notify = false);
  std::function<void(calendar::Date)> on_change{};

  // —— 视野月份（与 `value` 解耦：翻到别的月看，不改变已选值）——
  [[nodiscard]] auto visible_month() const noexcept -> calendar::Date { return view_month_; }
  void show_month(int year, int month);

  // —— 可选范围（含边界；空 = 不限）——
  void set_min(std::optional<calendar::Date> min);
  void set_max(std::optional<calendar::Date> max);
  [[nodiscard]] auto min() const noexcept -> std::optional<calendar::Date> { return min_; }
  [[nodiscard]] auto max() const noexcept -> std::optional<calendar::Date> { return max_; }
  /// 某日是否可选（范围内、且属于当月——邻月空格恒不可选）。
  [[nodiscard]] auto is_selectable(calendar::Date date) const noexcept -> bool;

  // —— 「今天」（由宿主注入，组件不读系统时钟）——
  //
  // 为何不自己取 `st::time`：那让组件**不可测**（"今天"每天变，断言无法固定），
  // 且无头/CI 里时区不一致会让"今天高亮"落在别处。宿主注入即可。
  void set_today(std::optional<calendar::Date> today) {
    today_ = today;
    mark_dirty();
  }
  [[nodiscard]] auto today() const noexcept -> std::optional<calendar::Date> { return today_; }

  // —— 几何（命中与绘制共用同一份；避免"测试复刻布局公式"）——
  [[nodiscard]] auto prev_button_rect() const -> math::Rect;
  [[nodiscard]] auto next_button_rect() const -> math::Rect;
  /// 第 `index` 格（0..41）的矩形。
  [[nodiscard]] auto cell_rect(int index) const -> math::Rect;
  /// 某格对应的日号（0 = 不属于当月）。
  [[nodiscard]] auto cell_day(int index) const -> int;
  /// 点落在哪一格（-1 = 不在网格里）。
  [[nodiscard]] auto cell_at(math::Point point) const -> int;

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

 private:
  void rebuild_grid();
  void step_month(int delta);
  void commit(calendar::Date date);

  calendar::Date value_{};
  calendar::Date view_month_{};   ///< 当前显示的月份（只用到 year/month）
  std::int8_t grid_[calendar::kGridCells]{};
  std::optional<calendar::Date> min_{};
  std::optional<calendar::Date> max_{};
  std::optional<calendar::Date> today_{};
  int hover_cell_{-1};

  math::Color surface_{};
  math::Color surface_raised_{};
  math::Color border_{};
  math::Color text_color_{};
  math::Color text_muted_{};
  math::Color text_faint_{};
  math::Color primary_{};
  math::Color primary_soft_{};
  float font_size_{13.0f};
  float font_small_{11.0f};
};

}  // namespace st::ui

#include "st/ui/component_registry.hpp"
ST_UI_COMPONENT(::st::ui::CalendarPanel, "CalendarPanel")
