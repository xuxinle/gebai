#include "st/ui/components/calendar_panel.hpp"

#include <algorithm>
#include <string>

#include "st/raster/paint.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"
#include "components_internal.hpp"

namespace st::ui {

namespace {

/// 星期表头（0 = 周一，与 `calendar::weekday_of` 同口径）。
constexpr std::string_view kWeekdays[7] = {"一", "二", "三", "四", "五", "六", "日"};

}  // namespace

CalendarPanel::CalendarPanel() : CalendarPanel(calendar::Date{}) {}

CalendarPanel::CalendarPanel(calendar::Date value) : value_(value), view_month_(value) {
  rebuild_grid();
}

void CalendarPanel::rebuild_grid() {
  calendar::fill_month_grid(view_month_.year, view_month_.month, grid_);
}

void CalendarPanel::set_value(calendar::Date value, bool notify) {
  const calendar::Date clamped = calendar::clamp_day(value);
  if (clamped == value_ && view_month_.year == clamped.year && view_month_.month == clamped.month) {
    return;
  }
  value_ = clamped;
  // **选值顺带把视野跟过去**：宿主用 `set_value` 把外部值同步进来时，
  // 面板不该还停在别的月份（否则用户打开看到的是"没选中任何一天"）。
  view_month_.year = clamped.year;
  view_month_.month = clamped.month;
  rebuild_grid();
  mark_dirty();
  if (notify && on_change) on_change(value_);
}

void CalendarPanel::show_month(int year, int month) {
  if (month < 1) {
    month = 12;
    --year;
  } else if (month > 12) {
    month = 1;
    ++year;
  }
  if (view_month_.year == year && view_month_.month == month) return;
  view_month_.year = year;
  view_month_.month = month;
  rebuild_grid();
  mark_dirty();
}

void CalendarPanel::step_month(int delta) {
  const calendar::Date moved = calendar::add_months(calendar::Date{view_month_.year, view_month_.month, 1}, delta);
  show_month(moved.year, moved.month);
}

void CalendarPanel::set_min(std::optional<calendar::Date> min) {
  min_ = min;
  mark_dirty();
}

void CalendarPanel::set_max(std::optional<calendar::Date> max) {
  max_ = max;
  mark_dirty();
}

auto CalendarPanel::is_selectable(calendar::Date date) const noexcept -> bool {
  // 只允许**当前视野月份**里的日子：邻月格子一直是空的（0），从源头排除。
  if (date.year != view_month_.year || date.month != view_month_.month) return false;
  if (date.day < 1 || date.day > calendar::days_in_month(date.year, date.month)) return false;
  if (min_.has_value() && date < *min_) return false;
  if (max_.has_value() && *max_ < date) return false;
  return true;
}

void CalendarPanel::commit(calendar::Date date) {
  if (!is_selectable(date)) return;
  value_ = date;
  mark_dirty();
  if (on_change) on_change(value_);
}

// —— 几何 ——

auto CalendarPanel::prev_button_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  return math::Rect{bounds_.x + kPadding, bounds_.y + kPadding + 6.0f, 20.0f, 20.0f};
}

auto CalendarPanel::next_button_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  return math::Rect{bounds_.right() - kPadding - 20.0f, bounds_.y + kPadding + 6.0f, 20.0f, 20.0f};
}

auto CalendarPanel::cell_rect(int index) const -> math::Rect {
  if (bounds_.is_empty() || index < 0 || index >= calendar::kGridCells) return {};
  const int row = index / calendar::kGridColumns;
  const int column = index % calendar::kGridColumns;
  // 网格**水平居中**：7 格 × 30 可能窄于面板宽（自模态卡片有余量）。
  const float grid_width = kCellSize * static_cast<float>(calendar::kGridColumns);
  const float left = bounds_.x + (bounds_.width - grid_width) * 0.5f;
  const float top = bounds_.y + kPadding + kHeaderHeight + kWeekRowHeight;
  return math::Rect{left + static_cast<float>(column) * kCellSize,
                    top + static_cast<float>(row) * kCellSize, kCellSize, kCellSize};
}

auto CalendarPanel::cell_day(int index) const -> int {
  if (index < 0 || index >= calendar::kGridCells) return 0;
  return static_cast<int>(grid_[index]);
}

auto CalendarPanel::cell_at(math::Point point) const -> int {
  for (int index = 0; index < calendar::kGridCells; ++index) {
    if (cell_rect(index).contains(point)) return index;
  }
  return -1;
}

// —— 生命周期 ——

void CalendarPanel::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  surface_ = colors.surface;
  surface_raised_ = colors.surface_raised;
  border_ = colors.border;
  text_color_ = colors.text;
  text_muted_ = colors.text_muted;
  text_faint_ = colors.text_faint;
  primary_ = colors.primary;
  primary_soft_ = colors.primary_soft;
  font_size_ = theme.metrics().font_sm;
  font_small_ = theme.metrics().font_xs;
}

void CalendarPanel::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  // 尺寸由**内容**决定（42 格固定）：不给显式宽高时按网格算，避免调用方抄公式。
  const float grid_width = kCellSize * static_cast<float>(calendar::kGridColumns);
  const float grid_height = kCellSize * static_cast<float>(calendar::kGridRows);
  float width = grid_width + kPadding * 2.0f;
  float height = grid_height + kHeaderHeight + kWeekRowHeight + kPadding * 2.0f;
  if (style_.has_explicit_width()) width = style_.width;
  if (style_.has_explicit_height()) height = style_.height;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void CalendarPanel::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  // **不接受“被拉宽”**：网格是固定 7 列的方块阵列，拉宽只会让格子之间裂开
  // （实测：列容器默认 `align_items = Stretch` 把它拉到整窗宽，7 列散在 1400px 上，
  //  看着像“日历丢了右半边”）。宽度超出自身需要时，收到自己需要的宽度并**居中**。
  const float own_width = kCellSize * static_cast<float>(calendar::kGridColumns) + kPadding * 2.0f;
  if (rect.width > own_width) {
    const float left = rect.x + (rect.width - own_width) * 0.5f;
    rect = math::Rect{left, rect.y, own_width, rect.height};
  }
  bounds_ = rect;
  layout_dirty_ = false;
}

void CalendarPanel::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const TextPort& port = components_internal::text_port_of(context);
  const float radius = context.theme.metrics().radius_md;

  // 底：卡片面（自模态里它就是那张卡）
  canvas.fill_rect(bounds_, raster::Paint::solid(surface_raised_), radius);

  // —— 标题行：‹ 2026年11月 › ——
  const std::string title = std::to_string(view_month_.year) + "年" +
                            std::to_string(view_month_.month) + "月";
  const float title_size = font_size_ + 1.0f;
  const float title_line = port.line_height(title_size);
  const float title_width = port.measure_width(title, title_size);
  port.draw(canvas, title,
            math::Point{bounds_.x + (bounds_.width - title_width) * 0.5f,
                        bounds_.y + kPadding + (kHeaderHeight - title_line) * 0.5f},
            title_size, text_color_);

  // 翻月按钮：图标画在矩形中心，悬停/按下状态由基类给的 hover 走不到这里
  // （本组件不自带子元素），所以直接用 `primary` 表达可交互。
  const math::Rect prev = prev_button_rect();
  const math::Rect next = next_button_rect();
  Icon::draw(canvas, "chevron-left",
             math::Rect{prev.x + 4.0f, prev.y + 4.0f, 12.0f, 12.0f}, text_muted_, 0.0f);
  Icon::draw(canvas, "chevron-right",
             math::Rect{next.x + 4.0f, next.y + 4.0f, 12.0f, 12.0f}, text_muted_, 0.0f);

  // —— 星期表头 ——
  const float week_top = bounds_.y + kPadding + kHeaderHeight;
  const float grid_left =
      bounds_.x + (bounds_.width - kCellSize * static_cast<float>(calendar::kGridColumns)) * 0.5f;
  for (int column = 0; column < calendar::kGridColumns; ++column) {
    const float line = port.line_height(font_small_);
    const float width = port.measure_width(kWeekdays[column], font_small_);
    port.draw(canvas, std::string(kWeekdays[column]),
              math::Point{grid_left + static_cast<float>(column) * kCellSize +
                              (kCellSize - width) * 0.5f,
                          week_top + (kWeekRowHeight - line) * 0.5f},
              font_small_, text_faint_);
  }

  // —— 日期格 ——
  const calendar::Date today = today_.value_or(calendar::Date{});
  const bool has_today = today_.has_value();
  for (int index = 0; index < calendar::kGridCells; ++index) {
    const int day = static_cast<int>(grid_[index]);
    if (day == 0) continue;   // 邻月空位：不画任何东西（连底框都不给，避免看着可点）
    const math::Rect cell = cell_rect(index);
    // 内缩 1px 当格间呼吸（30 格紧贴会连成一片）
    const math::Rect chip = cell.inset(math::Insets{1.0f, 1.0f, 1.0f, 1.0f});
    const calendar::Date date{view_month_.year, view_month_.month, day};
    const bool selected = date == value_;
    const bool selectable = is_selectable(date);
    const bool is_today = has_today && date == today;

    if (selected) {
      canvas.fill_rect(chip, raster::Paint::solid(primary_), context.theme.metrics().radius_sm);
    } else if (index == hover_cell_ && selectable) {
      canvas.fill_rect(chip, raster::Paint::solid(surface_), context.theme.metrics().radius_sm);
    }
    // 「今天」用**小圆点/短下划线式的强调**而不是填色或描边：
    //
    // 实测两次都不够：① `primary_soft` 1px 描边在暗底上几乎与底色同亮度（看不见）；
    // ② 改成 `primary` 描边后又是“一圈很细的框”，在 30px 格子里读不出含义，
    // 而且与选中态（填充）的区分只有一—圈线，反而更容易误认。
    //
    // 现在的形态：日期下方一个 3px 圆点。它与“选中 = 整格填充”在形状上正交，
    // 两者同时出现也不会混。
    if (is_today && !selected) {
      const float dot_radius = 1.5f;
      const math::Point center{chip.x + chip.width * 0.5f, chip.bottom() - 4.0f};
      canvas.fill_rect(
          math::Rect{center.x - dot_radius, center.y - dot_radius, dot_radius * 2.0f,
                     dot_radius * 2.0f},
          raster::Paint::solid(primary_), dot_radius);
    }

    const math::Color color = selected      ? context.theme.colors().on_primary
                              : !selectable ? text_faint_
                                            : text_color_;
    const std::string label = std::to_string(day);
    const float line = port.line_height(font_size_);
    const float width = port.measure_width(label, font_size_);
    port.draw(canvas, label,
              math::Point{chip.x + (chip.width - width) * 0.5f,
                          chip.y + (chip.height - line) * 0.5f},
              font_size_, color);
  }
}

auto CalendarPanel::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const int hit = cell_at(event.position);
      if (hit != hover_cell_) {
        hover_cell_ = hit;
        mark_dirty();
      }
      return hit >= 0;
    }
    case EventKind::HoverOut: {
      if (hover_cell_ >= 0) {
        hover_cell_ = -1;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click: {
      if (prev_button_rect().contains(event.position)) {
        step_month(-1);
        return true;
      }
      if (next_button_rect().contains(event.position)) {
        step_month(1);
        return true;
      }
      const int hit = cell_at(event.position);
      if (hit < 0) return false;
      const int day = static_cast<int>(grid_[hit]);
      if (day == 0) return false;
      const calendar::Date date{view_month_.year, view_month_.month, day};
      if (!is_selectable(date)) return false;   // 范围内才响应（禁用格点了没反应是对的）
      commit(date);
      return true;
    }
    default: break;
  }
  return false;
}

// —— 属性面 / 动作面 ——

auto CalendarPanel::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return calendar::format_date(value_);
  if (name == "month") {
    return std::to_string(view_month_.year) + "-" + (view_month_.month < 10 ? "0" : "") +
           std::to_string(view_month_.month);
  }
  if (name == "min") return min_.has_value() ? std::optional(calendar::format_date(*min_)) : std::optional<std::string>();
  if (name == "max") return max_.has_value() ? std::optional(calendar::format_date(*max_)) : std::optional<std::string>();
  if (name == "today") {
    return today_.has_value() ? std::optional(calendar::format_date(*today_))
                              : std::optional<std::string>();
  }
  return Element::get_property(name);
}

auto CalendarPanel::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value") {
    const auto parsed = calendar::parse_date(value);
    if (!parsed.ok) return false;   // 如实拒绝：静默兜底成今天是最糟的失败方式
    set_value(parsed.value, false);
    return true;
  }
  if (name == "month") {
    std::string text(value);
    text += "-01";
    const auto parsed = calendar::parse_date(text);
    if (!parsed.ok) return false;
    show_month(parsed.value.year, parsed.value.month);
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
  return Element::set_property(name, value);
}

auto CalendarPanel::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  for (const std::string_view extra : {"value", "month", "min", "max", "today"}) {
    names.push_back(extra);
  }
  return names;
}

auto CalendarPanel::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "prev-month") {
    step_month(-1);
    return true;
  }
  if (action == "next-month") {
    step_month(1);
    return true;
  }
  if (action == "select") {
    const auto parsed = calendar::parse_date(argument);
    if (!parsed.ok) return false;
    commit(parsed.value);
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

ST_UI_COMPONENT_IMPL(::st::ui::CalendarPanel);
