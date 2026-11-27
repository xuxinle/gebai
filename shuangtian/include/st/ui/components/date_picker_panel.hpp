#pragma once

/// **日期选择弹层**（`DatePickerPanel`）：半透明遮罩 + 居中的月历卡片。
///
/// ## 为什么需要一个"壳"而不是直接把 CalendarPanel 挂成浮层
///
/// 浮层是 `FillViewport`（铺满视口）。`CalendarPanel` 自己只是一个固定尺寸的方块，
/// 挂上去会是"贴在左上角的一小块"，而且**点外面收不起来**。壳负责三件事：
///
/// 1. 铺满的遮罩（`colors.overlay`）——同时把"点外部 = 关闭"变成一次普通点击；
/// 2. 把月历卡片**居中**；
/// 3. Esc / 点遮罩 → `on_dismiss`。
///
/// 与 `ThemePanel` 同构（那是本仓已有的自模态先例），刻意保持一致的形状，
/// 免得同一个框架里出现两种模态手感。
///
/// ## 为什么卡片尺寸固定
///
/// 月历是 6×7 的方块阵列，尺寸由 `CalendarPanel` 自己决定。壳不再"按内容自适应"——
/// 那会在切换月份（行数变化）时改尺寸，弹层跟着抖，而这类抖动最容易被当成"闪了一下"。

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "st/math/geometry.hpp"
#include "st/ui/calendar.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class CalendarPanel;

class DatePickerPanel : public Element {
 public:
  /// 接管一个已配好的月历面板（所有权归本弹层）。
  explicit DatePickerPanel(std::unique_ptr<CalendarPanel> calendar);

  [[nodiscard]] auto type() const noexcept -> std::string_view override {
    return "DatePickerPanel";
  }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Dialog; }
  /// 模态：在场即拦截输入（遮罩吃掉对下层的点击）。
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return visible(); }

  [[nodiscard]] auto calendar() -> CalendarPanel* { return calendar_; }
  /// 把值/范围/今天转给内嵌月历（宿主改值时用；面板可能尚未创建）。
  void set_value(calendar::Date value);
  void set_min(std::optional<calendar::Date> min);
  void set_max(std::optional<calendar::Date> max);
  void set_today(std::optional<calendar::Date> today);
  /// 居中卡片矩形（命中/测试用；arrange 后有效）。
  [[nodiscard]] auto card_rect() const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  /// 请求收起（Esc / 点遮罩）。
  std::function<void()> on_dismiss{};

 private:
  CalendarPanel* calendar_{nullptr};   ///< 所有权在 `children_`，本指针仅观察
  math::Rect card_{};
  math::Color overlay_{};
  math::Color surface_{};
  float radius_{12.0f};
};

}  // namespace st::ui
