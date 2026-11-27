#include "st/ui/components/time_picker_panel.hpp"

#include <algorithm>
#include <string>

#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"
#include "components_internal.hpp"

namespace st::ui {

TimePickerPanel::TimePickerPanel(calendar::Clock value, int minute_step)
    : value_(value), minute_step_(minute_step < 1 ? 1 : minute_step) {
  rebuild_minutes();
  // 两列的滚动起点对齐到"当前值附近"：否则打开时看不到已选项（要滚很久）。
  hour_scroll_ = std::clamp(value_.hour - static_cast<int>(kMaxVisibleRows) / 2, 0,
                            24 - static_cast<int>(kMaxVisibleRows));
  const auto current_index =
      std::find(minutes_.begin(), minutes_.end(), value_.minute);
  const int index = current_index == minutes_.end()
                        ? 0
                        : static_cast<int>(current_index - minutes_.begin());
  minute_scroll_ = std::clamp(index - static_cast<int>(kMaxVisibleRows) / 2, 0,
                              std::max(0, minute_count_ - static_cast<int>(kMaxVisibleRows)));
}

void TimePickerPanel::rebuild_minutes() {
  minutes_.clear();
  for (int minute = 0; minute < 60; minute += minute_step_) minutes_.push_back(minute);
  // 步进不整除 60 时（如 7 分钟）末尾补一个 59，避免"最高可选值"意外变小。
  if (!minutes_.empty() && minutes_.back() != 59 && 60 % minute_step_ != 0) {
    minutes_.push_back(59);
  }
  if (minutes_.empty()) minutes_.push_back(0);
  minute_count_ = static_cast<int>(minutes_.size());
}

void TimePickerPanel::set_value(calendar::Clock value) {
  if (value == value_) return;
  value_ = value;
  mark_dirty();
}

auto TimePickerPanel::card_rect() const -> math::Rect {
  return card_;
}

auto TimePickerPanel::hour_rect(int index) const -> math::Rect {
  if (index < 0 || index >= 24) return {};
  const float left = card_.x + kPadding;
  const float top = card_.y + kPadding + kHeaderHeight +
                    (static_cast<float>(index) - static_cast<float>(hour_scroll_)) * kRowHeight;
  return math::Rect{left, top, kColumnWidth, kRowHeight};
}

auto TimePickerPanel::minute_rect(int index) const -> math::Rect {
  if (index < 0 || index >= minute_count_) return {};
  const float left = card_.x + kPadding * 2.0f + kColumnWidth;
  const float top = card_.y + kPadding + kHeaderHeight +
                    (static_cast<float>(index) - static_cast<float>(minute_scroll_)) * kRowHeight;
  return math::Rect{left, top, kColumnWidth, kRowHeight};
}

void TimePickerPanel::scroll_row(int column, int delta) {
  const int visible = static_cast<int>(kMaxVisibleRows);
  if (column == 0) {
    hour_scroll_ = std::clamp(hour_scroll_ + delta, 0, std::max(0, 24 - visible));
  } else {
    minute_scroll_ =
        std::clamp(minute_scroll_ + delta, 0, std::max(0, minute_count_ - visible));
  }
  mark_dirty();
}

void TimePickerPanel::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  overlay_ = colors.overlay;
  surface_ = colors.surface_raised;
  surface_hover_ = colors.surface_hover;
  text_color_ = colors.text;
  text_muted_ = colors.text_muted;
  primary_ = colors.primary;
  primary_soft_ = colors.primary_soft;
  radius_ = theme.metrics().radius_lg;
  font_size_ = theme.metrics().font_sm;
}

void TimePickerPanel::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  measured_ = math::Size{constraints.max_width < kUnbounded ? constraints.max_width : 0.0f,
                         constraints.max_height < kUnbounded ? constraints.max_height : 0.0f};
}

void TimePickerPanel::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  bounds_ = rect;
  layout_dirty_ = false;
  const float width = kPadding * 2.0f + kColumnWidth * 2.0f + kPadding;
  const float height = kPadding * 2.0f + kHeaderHeight + kRowHeight * kMaxVisibleRows;
  card_ = math::Rect{rect.x + (rect.width - width) * 0.5f, rect.y + (rect.height - height) * 0.5f,
                     width, height};
}

void TimePickerPanel::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || card_.is_empty()) return;
  const TextPort& port = components_internal::text_port_of(context);
  // 遮罩
  canvas.fill_rect(bounds_, raster::Paint::solid(overlay_));
  // 卡片
  canvas.fill_rect(card_, raster::Paint::solid(surface_), radius_);

  // 表头：时 / 分
  const auto draw_header = [&](const char* label, float x) {
    const float line = port.line_height(font_size_ - 1.0f);
    const float width = port.measure_width(label, font_size_ - 1.0f);
    port.draw(canvas, label,
              math::Point{x + (kColumnWidth - width) * 0.5f,
                          card_.y + kPadding + (kHeaderHeight - line) * 0.5f},
              font_size_ - 1.0f, text_muted_);
  };
  draw_header("时", card_.x + kPadding);
  draw_header("分", card_.x + kPadding * 2.0f + kColumnWidth);

  // 列分隔线（两列之间一道极淡的竖线，帮助"这是两列"的读法）
  const float divider_x = card_.x + kPadding * 1.5f + kColumnWidth;
  canvas.fill_rect(math::Rect{divider_x, card_.y + kPadding + kHeaderHeight, 1.0f,
                              kRowHeight * kMaxVisibleRows},
                   raster::Paint::solid(context.theme.colors().border_subtle));

  // 时列
  const float rows_top = card_.y + kPadding + kHeaderHeight;
  const float rows_bottom = rows_top + kRowHeight * kMaxVisibleRows;
  for (int hour = 0; hour < 24; ++hour) {
    const math::Rect cell = hour_rect(hour);
    if (cell.bottom() <= rows_top || cell.y >= rows_bottom) continue;
    const bool selected = hour == value_.hour;
    if (selected) {
      canvas.fill_rect(cell.inset(math::Insets{1.0f, 1.0f, 1.0f, 1.0f}),
                       raster::Paint::solid(primary_soft_), 6.0f);
    }
    const std::string label =
        (hour < 10 ? std::string("0") : std::string()) + std::to_string(hour);
    const float line = port.line_height(font_size_);
    const float width = port.measure_width(label, font_size_);
    port.draw(canvas, label,
              math::Point{cell.x + (cell.width - width) * 0.5f,
                          cell.y + (cell.height - line) * 0.5f},
              font_size_, selected ? primary_ : text_color_);
  }

  // 分列
  for (int index = 0; index < minute_count_; ++index) {
    const math::Rect cell = minute_rect(index);
    if (cell.bottom() <= rows_top || cell.y >= rows_bottom) continue;
    const int minute = minutes_[static_cast<std::size_t>(index)];
    const bool selected = minute == value_.minute;
    if (selected) {
      canvas.fill_rect(cell.inset(math::Insets{1.0f, 1.0f, 1.0f, 1.0f}),
                       raster::Paint::solid(primary_soft_), 6.0f);
    }
    const std::string label =
        (minute < 10 ? std::string("0") : std::string()) + std::to_string(minute);
    const float line = port.line_height(font_size_);
    const float width = port.measure_width(label, font_size_);
    port.draw(canvas, label,
              math::Point{cell.x + (cell.width - width) * 0.5f,
                          cell.y + (cell.height - line) * 0.5f},
              font_size_, selected ? primary_ : text_color_);
  }
  // 列外（滚动出界）不画：`hour_rect` 已经按 scroll 偏移算，越界格自然落在 rows 范围外。
}

auto TimePickerPanel::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!visible()) return false;

  if (event.kind == EventKind::Wheel) {
    // 滚轮按列：x 决定改哪一列（这是两列并排时唯一自然的映射）。
    const bool over_hour = event.position.x < card_.x + kPadding * 1.5f + kColumnWidth;
    const int direction = event.wheel_delta > 0.0f ? 1 : -1;
    scroll_row(over_hour ? 0 : 1, direction);
    return true;
  }

  if (event.kind == EventKind::Click) {
    if (!card_.contains(event.position)) {
      if (on_dismiss) on_dismiss();
      return true;
    }
    // 点"时"列
    for (int hour = 0; hour < 24; ++hour) {
      const math::Rect cell = hour_rect(hour);
      if (cell.is_empty() || !cell.contains(event.position)) continue;
      const float top = card_.y + kPadding + kHeaderHeight;
      if (cell.y < top || cell.bottom() > top + kRowHeight * kMaxVisibleRows) break;   // 滚动出界
      calendar::Clock picked = value_;
      picked.hour = hour;
      value_ = picked;
      mark_dirty();
      if (on_pick) on_pick(value_);
      return true;
    }
    // 点"分"列
    for (int index = 0; index < minute_count_; ++index) {
      const math::Rect cell = minute_rect(index);
      if (cell.is_empty() || !cell.contains(event.position)) continue;
      const float top = card_.y + kPadding + kHeaderHeight;
      if (cell.y < top || cell.bottom() > top + kRowHeight * kMaxVisibleRows) break;
      calendar::Clock picked = value_;
      picked.minute = minutes_[static_cast<std::size_t>(index)];
      value_ = picked;
      mark_dirty();
      if (on_pick) on_pick(value_);
      return true;
    }
    return true;   // 卡片内的空白区域：吃掉点击，不穿透
  }

  if (event.kind == EventKind::KeyDown && event.key == "Escape") {
    if (on_dismiss) on_dismiss();
    return true;
  }
  return false;
}

}  // namespace st::ui
