#include "st/ui/components/date_picker_panel.hpp"

#include <algorithm>

#include "st/raster/paint.hpp"
#include "st/ui/components/calendar_panel.hpp"
#include "st/ui/theme.hpp"
#include "components_internal.hpp"

namespace st::ui {

DatePickerPanel::DatePickerPanel(std::unique_ptr<CalendarPanel> calendar) {
  if (calendar != nullptr) {
    calendar_ = calendar.get();
    add_child(std::move(calendar));
  }
}

auto DatePickerPanel::card_rect() const -> math::Rect {
  if (calendar_ == nullptr) return {};
  return calendar_->bounds();
}

// 转发：宿主在弹层开着时改值/范围，要能穿透到内嵌月历。
void DatePickerPanel::set_value(calendar::Date value) {
  if (calendar_ != nullptr) calendar_->set_value(value, false);
}

void DatePickerPanel::set_min(std::optional<calendar::Date> min) {
  if (calendar_ != nullptr) calendar_->set_min(min);
}

void DatePickerPanel::set_max(std::optional<calendar::Date> max) {
  if (calendar_ != nullptr) calendar_->set_max(max);
}

void DatePickerPanel::set_today(std::optional<calendar::Date> today) {
  if (calendar_ != nullptr) calendar_->set_today(today);
}

void DatePickerPanel::apply_theme(const Theme& theme) {
  overlay_ = theme.colors().overlay;
  surface_ = theme.colors().surface_raised;
  radius_ = theme.metrics().radius_lg;
}

void DatePickerPanel::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  // 壳**铺满视口**（遮罩要覆盖整块），尺寸不看约束。
  measured_ = math::Size{constraints.max_width < kUnbounded ? constraints.max_width : 0.0f,
                         constraints.max_height < kUnbounded ? constraints.max_height : 0.0f};
}

void DatePickerPanel::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  layout_dirty_ = false;
  if (calendar_ == nullptr) return;
  // 卡片尺寸 = 面板自己量出来的尺寸（它按 7×6 网格算），这里只负责居中。
  calendar_->measure(context, Constraints{.max_width = rect.width,
                                           .max_height = rect.height,
                                           .available_width = rect.width,
                                           .available_height = rect.height});
  const math::Size size = calendar_->measured_size();
  const float x = rect.x + (rect.width - size.width) * 0.5f;
  const float y = rect.y + (rect.height - size.height) * 0.5f;
  calendar_->arrange(context, math::Rect{x, y, size.width, size.height});
  card_ = math::Rect{x, y, size.width, size.height};
}

void DatePickerPanel::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  (void)context;
  if (bounds_.is_empty()) return;
  // 遮罩铺满（点击遮罩 = 关闭，见 on_event）
  canvas.fill_rect(bounds_, raster::Paint::solid(overlay_));
}

auto DatePickerPanel::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!visible()) return false;
  // 卡片内的点击交给月历（它是子元素，正常冒泡到这里时已经被消费过；
  // 这里只处理"点在卡片外"）。
  if (event.kind == EventKind::Click) {
    if (!card_.is_empty() && card_.contains(event.position)) return false;
    if (on_dismiss) on_dismiss();
    return true;
  }
  if (event.kind == EventKind::KeyDown) {
    // Esc = 取消（不选）。用 keycode 比较：本仓 Event 的 key 字段是字符串形态。
    if (event.key == "Escape") {
      if (on_dismiss) on_dismiss();
      return true;
    }
  }
  return false;
}

}  // namespace st::ui
