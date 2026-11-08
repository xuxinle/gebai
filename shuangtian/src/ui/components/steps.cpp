#include "st/ui/components/steps.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/math/path.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::draw_line;
using components_internal::fill_round_rect;
using components_internal::text_port_of;

Steps::Steps() {
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.direction = FlexDirection::Row;
  style_.align_items = Align::Start;
  set_focusable(true);
}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void Steps::set_steps(std::vector<Step> steps) {
  steps_ = std::move(steps);
  placed_valid_ = false;
  hover_index_ = -1;
  mark_layout_dirty();
}

void Steps::set_titles(std::vector<std::string> titles) {
  std::vector<Step> next;
  next.reserve(titles.size());
  for (std::string& title : titles) {
    Step step{};
    step.title = std::move(title);
    next.push_back(std::move(step));
  }
  set_steps(std::move(next));
}

void Steps::set_current(std::size_t index, bool notify) {
  if (steps_.empty()) return;
  const std::size_t target = std::min(index, steps_.size() - 1U);
  for (std::size_t at = 0; at < steps_.size(); ++at) {
    steps_[at].state = at < target ? State::Done : (at == target ? State::Active : State::Todo);
  }
  hover_index_ = -1;
  mark_dirty();
  if (notify && on_change) on_change(target);
}

auto Steps::current() const -> std::size_t {
  for (std::size_t index = 0; index < steps_.size(); ++index) {
    if (steps_[index].state == State::Active) return index;
  }
  return steps_.empty() ? 0U : steps_.size() - 1U;
}

auto Steps::all_done() const noexcept -> bool {
  if (steps_.empty()) return false;
  for (const Step& step : steps_) {
    if (step.state != State::Done) return false;
  }
  return true;
}

void Steps::set_orientation(FlexDirection orientation) {
  if (orientation_ == orientation) return;
  orientation_ = orientation;
  placed_valid_ = false;
  mark_layout_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void Steps::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = metrics.font_sm;
  title_size_ = metrics.font_sm;
  style_.color = theme.colors().text;
  apply_text_overrides();
  // 字号覆盖会改标题宽 → 几何作废。
  title_size_ = style_.font_size;
  placed_valid_ = false;
}

auto Steps::measure(const RenderContext& context, const Constraints& constraints)
    -> void {
  const TextPort& port = text_port_of(context);
  const float size = title_size_;

  // 文字块宽：标题与说明取较宽者。
  const auto text_block_width = [&](const Step& step) {
    float width = port.measure_width(step.title, size);
    if (!step.description.empty()) {
      width = std::max(width, port.measure_width(step.description, size * 0.92f));
    }
    return width;
  };

  if (orientation_ == FlexDirection::Row) {
    float natural = 0.0f;
    for (std::size_t index = 0; index < steps_.size(); ++index) {
      natural += kCircleSize + kCircleGap + text_block_width(steps_[index]);
      if (index + 1 < steps_.size()) natural += kLineGap * 2.0f + 24.0f;   // 连线最短 24px
    }
    float width = style_.has_explicit_width() ? style_.width : natural;
    if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
    width = std::clamp(width, style_.min_width, style_.max_width);
    float height = style_.has_explicit_height() ? style_.height : kCircleSize;
    if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
    measured_ = math::Size{width, std::clamp(height, style_.min_height, style_.max_height)};
  } else {
    float width = 0.0f;
    float height = 0.0f;
    for (std::size_t index = 0; index < steps_.size(); ++index) {
      width = std::max(width, kCircleSize + kCircleGap + text_block_width(steps_[index]));
      height += kCircleSize;
      if (index + 1 < steps_.size()) height += kLineGap * 2.0f + 12.0f;   // 竖向连线最短 12px
    }
    float final_width = style_.has_explicit_width() ? style_.width : width;
    if (constraints.max_width < kUnbounded) final_width = std::min(final_width, constraints.max_width);
    float final_height = style_.has_explicit_height() ? style_.height : height;
    if (constraints.max_height < kUnbounded) final_height = std::min(final_height, constraints.max_height);
    measured_ = math::Size{std::clamp(final_width, style_.min_width, style_.max_width),
                           std::clamp(final_height, style_.min_height, style_.max_height)};
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void Steps::relayout(const RenderContext& context) const {
  placed_.assign(steps_.size(), Placed{});
  if (steps_.empty() || bounds_.is_empty()) return;
  const TextPort& port = text_port_of(context);
  const float size = title_size_;
  const auto text_block_width = [&](const Step& step) {
    float width = port.measure_width(step.title, size);
    if (!step.description.empty()) {
      width = std::max(width, port.measure_width(step.description, size * 0.92f));
    }
    return width;
  };

  if (orientation_ == FlexDirection::Row) {
    // 横向：先算文字块宽度，剩余宽在连线间均分（连线必须**等长**，
    // 否则步骤条看起来像随手摆的一排）。
    std::vector<float> text_widths(steps_.size(), 0.0f);
    float fixed = 0.0f;
    for (std::size_t index = 0; index < steps_.size(); ++index) {
      text_widths[index] = text_block_width(steps_[index]);
      fixed += kCircleSize + kCircleGap + text_widths[index];
    }
    // ⚠ 类型必须写生 `float`：`std::max(size_t - 1U, 0U)` 推成 `unsigned long long`，
    // 再与浮点相乘就推不出模板参数（实测报 mismatched types initializer_list）。
    const float connectors = steps_.empty() ? 0.0f : static_cast<float>(steps_.size() - 1U);
    const float remaining = std::max(bounds_.width - fixed - kLineGap * 2.0f * connectors,
                                     24.0f * connectors);
    const float segment = connectors > 0.0f ? remaining / connectors : 0.0f;

    float x = bounds_.x;
    const float center_y = bounds_.y + kCircleSize * 0.5f;
    for (std::size_t index = 0; index < steps_.size(); ++index) {
      Placed placed{};
      placed.circle = math::Rect{x, center_y - kCircleSize * 0.5f, kCircleSize, kCircleSize};
      placed.step = math::Rect{x, bounds_.y, kCircleSize + kCircleGap + text_widths[index],
                               kCircleSize};
      x += kCircleSize + kCircleGap + text_widths[index];
      if (index + 1 < steps_.size()) {
        const float line_x = x + kLineGap;
        const float line_w = std::max(segment - kLineGap * 2.0f, 1.0f);
        placed.connector = math::Rect{line_x, center_y - kLineThickness * 0.5f, line_w,
                                      kLineThickness};
        x += segment;
      }
      placed_[index] = placed;
    }
  } else {
    // 纵向：圆在左、文字在右；连线竖直穿过圆下方，长度均分剩余高。
    const float fixed = kCircleSize * static_cast<float>(steps_.size());
    const float connectors = steps_.empty() ? 0.0f : static_cast<float>(steps_.size() - 1U);
    const float remaining = std::max(bounds_.height - fixed - kLineGap * 2.0f * connectors,
                                     12.0f * connectors);
    const float segment = connectors > 0.0f ? remaining / connectors : 0.0f;
    float y = bounds_.y;
    const float center_x = bounds_.x + kCircleSize * 0.5f;
    for (std::size_t index = 0; index < steps_.size(); ++index) {
      Placed placed{};
      placed.circle = math::Rect{center_x - kCircleSize * 0.5f, y, kCircleSize, kCircleSize};
      placed.step = math::Rect{bounds_.x, y, bounds_.width, kCircleSize};
      y += kCircleSize;
      if (index + 1 < steps_.size()) {
        const float line_y = y + kLineGap;
        const float line_h = std::max(segment - kLineGap * 2.0f, 1.0f);
        placed.connector = math::Rect{center_x - kLineThickness * 0.5f, line_y, kLineThickness,
                                      line_h};
        y += segment;
      }
      placed_[index] = placed;
    }
  }
  placed_valid_ = true;
}

void Steps::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context);
}

auto Steps::circle_rect(std::size_t index) const -> math::Rect {
  return index < placed_.size() ? placed_[index].circle : math::Rect{};
}

auto Steps::connector_rect(std::size_t index) const -> math::Rect {
  return index < placed_.size() ? placed_[index].connector : math::Rect{};
}

auto Steps::step_rect(std::size_t index) const -> math::Rect {
  return index < placed_.size() ? placed_[index].step : math::Rect{};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Steps::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || steps_.empty()) return;
  relayout(context);
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);
  const float size = title_size_;

  const auto state_ink = [&](State state) {
    switch (state) {
      case State::Done: return colors.text;
      case State::Active: return colors.text;
      case State::Todo: return colors.text_muted;
    }
    return colors.text;
  };

  for (std::size_t index = 0; index < steps_.size(); ++index) {
    const Step& step = steps_[index];
    const Placed& placed = placed_[index];
    const math::Rect circle = placed.circle;
    const float radius = circle.width * 0.5f;

    // 连线颜色取**左端这一步**的状态：已完成步右侧的线是通行色（正确读法）。
    if (!placed.connector.is_empty()) {
      const math::Color line = step.state == State::Done ? colors.primary : colors.border;
      fill_round_rect(canvas, placed.connector, kLineThickness * 0.5f, line);
    }

    // 进行中：外圈光环（先画，压在圆下面）。
    if (step.state == State::Active) {
      canvas.fill_circle(circle.center(), radius + kActiveHalo,
                         raster::Paint::solid(colors.primary_soft));
    }

    switch (step.state) {
      case State::Done:
        canvas.fill_circle(circle.center(), radius, raster::Paint::solid(colors.primary));
        break;
      case State::Active:
        canvas.fill_circle(circle.center(), radius, raster::Paint::solid(colors.primary));
        break;
      case State::Todo:
        canvas.fill_circle(circle.center(), radius, raster::Paint::solid(colors.surface_sunken));
        // 发丝描边：浅底上 `surface_sunken` 与背景对比不足，没有描边就"看不见圆"。
        canvas.stroke_path(
            math::make_circle(circle.center(), std::max(radius - metrics.border_width * 0.5f, 0.0f)),
            raster::Paint::solid(colors.border), metrics.border_width);
        break;
    }

    // 圆内内容：图标优先；否则「已完成画对勾、其余画编号」。
    const float glyph = radius * 1.05f;
    const math::Rect glyph_box{circle.center().x - glyph * 0.5f, circle.center().y - glyph * 0.5f,
                               glyph, glyph};
    if (!step.icon.empty()) {
      const math::Color ink = step.state == State::Todo ? colors.text_muted : colors.on_primary;
      Icon::draw(canvas, step.icon, glyph_box, ink, Icon::effective_stroke(step.icon, glyph));
    } else if (step.state == State::Done) {
      Icon::draw(canvas, "check", glyph_box, colors.on_primary,
                 Icon::effective_stroke("check", glyph));
    } else {
      const std::string number = std::to_string(index + 1);
      const float width = port.measure_width(number, size * 0.85f);
      const float top = centered_line_top(port, number, size * 0.85f, circle.y, circle.height);
      port.draw(canvas, number, math::Point{circle.center().x - width * 0.5f, top}, size * 0.85f,
                step.state == State::Active ? colors.on_primary : colors.text_muted);
    }

    // 文字块：标题 + 可选说明。
    const bool horizontal = orientation_ == FlexDirection::Row;
    const float text_x = horizontal ? circle.right() + kCircleGap : circle.right() + kCircleGap;
    const float text_width = horizontal
                                 ? std::max(placed.step.right() - text_x, 0.0f)
                                 : std::max(bounds_.right() - text_x, 0.0f);
    if (horizontal) {
      const math::Rect title_box{text_x, circle.y, text_width, circle.height};
      draw_line(context, canvas, step.title, title_box, size, state_ink(step.state));
    } else {
      const float title_h = port.line_height(size);
      const math::Rect title_box{text_x, circle.y, text_width, title_h};
      draw_line(context, canvas, step.title, title_box, size, state_ink(step.state));
      if (!step.description.empty()) {
        const math::Rect desc_box{text_x, circle.y + title_h, text_width,
                                  port.line_height(size * 0.92f)};
        draw_line(context, canvas, step.description, desc_box, size * 0.92f, colors.text_muted);
      }
    }

    // 横向时说明放标题下面（圆只有 24px 高，说明会溢出到圆下方——
    // 这是纵向外唯一合理的排法，且不会与连线打架，因为连线在圆的心线上）。
    if (horizontal && !step.description.empty()) {
      const float title_h = port.line_height(size);
      const math::Rect desc_box{text_x, circle.y + title_h, text_width,
                                port.line_height(size * 0.92f)};
      draw_line(context, canvas, step.description, desc_box, size * 0.92f, colors.text_muted);
    }

    if (static_cast<int>(index) == hover_index_) {
      fill_round_rect(canvas, placed.step, style_.radius, colors.surface_hover.with_alpha(28));
    }
  }

  if (focused_) components_internal::paint_focus_ring(context, canvas, bounds_, style_.radius);
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

auto Steps::step_at(math::Point point) const -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < placed_.size(); ++index) {
    // 横向时只认「圆 + 文字」这一块；纵向时整行可点（行高才是可扫区域）。
    if (placed_[index].step.contains(point)) return index;
  }
  return std::nullopt;
}

auto Steps::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const auto hit = step_at(event.position);
      const int next = hit.has_value() ? static_cast<int>(*hit) : -1;
      if (next != hover_index_) {
        hover_index_ = next;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      const auto hit = step_at(event.position);
      if (!hit.has_value()) return false;
      if (on_click) on_click(*hit);
      event.handled = true;
      return true;
    }
    default: return false;
  }
}

auto Steps::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto Steps::cursor_at(math::Point point) const noexcept -> MouseCursor {
  return step_at(point).has_value() ? MouseCursor::Hand : MouseCursor::Arrow;
}

auto Steps::semantics_text() const -> std::string {
  std::string out;
  for (std::size_t index = 0; index < steps_.size(); ++index) {
    if (index > 0) out += " > ";
    out += steps_[index].title;
  }
  return out;
}

auto Steps::semantics_value() const -> std::string {
  return std::format("{}/{}", current() + 1, steps_.size());
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto Steps::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "current" || name == "index") return std::to_string(current());
  if (name == "steps" || name == "options" || name == "items") {
    std::string out;
    for (std::size_t index = 0; index < steps_.size(); ++index) {
      if (index > 0) out += '|';
      out += steps_[index].title;
    }
    return out;
  }
  if (name == "count") return std::to_string(steps_.size());
  if (name == "orientation") {
    return std::string(orientation_ == FlexDirection::Row ? "horizontal" : "vertical");
  }
  if (name == "all_done") return std::string(all_done() ? kFlagTrue : "false");
  if (name == "states") {
    // 逐步状态（诊断「跳步」这类流程用；`set` 不接受，只读）。
    std::string out;
    for (std::size_t index = 0; index < steps_.size(); ++index) {
      if (index > 0) out += '|';
      switch (steps_[index].state) {
        case State::Todo: out += "todo"; break;
        case State::Active: out += "active"; break;
        case State::Done: out += "done"; break;
      }
    }
    return out;
  }
  return std::nullopt;
}

auto Steps::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "current" || name == "index") {
    const auto parsed = st::parse_u64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_current(static_cast<std::size_t>(*parsed));
    return true;
  }
  if (name == "steps" || name == "options" || name == "items") {
    std::vector<std::string> titles;
    for (const std::string_view part : st::split(value, '|')) {
      titles.emplace_back(st::trim(part));
    }
    set_titles(std::move(titles));
    return true;
  }
  if (name == "orientation") {
    const std::string text(st::trim(value));
    if (text == "horizontal" || text == "row" || text == "h") {
      set_orientation(FlexDirection::Row);
    } else if (text == "vertical" || text == "column" || text == "v") {
      set_orientation(FlexDirection::Column);
    } else {
      return false;
    }
    return true;
  }
  return false;
}

auto Steps::property_names() const -> std::vector<std::string_view> {
  return {"current", "steps", "orientation"};
}

auto Steps::readonly_property_names() const -> std::vector<std::string_view> {
  return {"count", "all_done", "states"};
}

auto Steps::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "next" || action == "step_forward") {
    if (steps_.empty()) return false;
    const std::size_t at = current();
    if (at + 1 >= steps_.size()) return false;
    set_current(at + 1, true);
    return true;
  }
  if (action == "prev" || action == "previous" || action == "step_backward") {
    const std::size_t at = current();
    if (at == 0U) return false;
    set_current(at - 1U, true);
    return true;
  }
  if (action == "goto" || action == "set") {
    const auto parsed = st::parse_u64(st::trim(argument));
    if (!parsed.has_value()) return false;
    if (*parsed >= steps_.size()) return false;
    set_current(static_cast<std::size_t>(*parsed), true);
    return true;
  }
  if (action == "done") {
    // 全部标记完成（流程收尾）。
    if (steps_.empty()) return false;
    for (Step& step : steps_) step.state = State::Done;
    mark_dirty();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
//
// 一行一个组件，展开后包含**静态登记**（本 .o 进产物即在 main 前完成注册）
// 与**导入锚点**（`import_components<T>()` 取它的地址，把本 .o 拉进产物）。
// 所以：真的用到组件 → 自动注册；要按名字建 → 另调 import。
ST_UI_COMPONENT_IMPL(::st::ui::Steps);
