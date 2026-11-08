#include "st/ui/components/number_input.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::paint_outline;
using components_internal::text_port_of;

namespace {

/// 浮点比较容差：值「真的变了」才算变化。
///
/// 不能直接 `!=`：`1.0 + 0.1` 与 `1.1` 在二进制里不等，于是每次 `set_value` 都会
/// 派发一次 `on_change`——调用方在回调里再 `set_value` 就会**无限递归**（实测哨兵）。
constexpr double kEpsilon = 1e-9;

auto nearly_equal(double a, double b) -> bool {
  const double scale = std::max({std::abs(a), std::abs(b), 1.0});
  return std::abs(a - b) <= kEpsilon * scale;
}

}  // namespace

NumberInput::NumberInput() = default;

NumberInput::NumberInput(double value) : value_(value) { sync_text_from_value(); }

// ————————————————————————————————————————————————————————————————————————————
// 值与文本
// ————————————————————————————————————————————————————————————————————————————

auto NumberInput::parse(const std::string& text) const -> std::optional<double> {
  // 允许前后空白；**整串**必须被完整消费（`"12abc"` 不算 12——那会让用户
  // 敲错一个字母后值静默变成前缀数字）。
  std::string_view view(text);
  while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) view.remove_prefix(1);
  while (!view.empty() && (view.back() == ' ' || view.back() == '\t')) view.remove_suffix(1);
  // 允许用户敲中文全角减号/数字之外的常见笔误不去猜：只接受 ASCII 数字语法。
  if (view.empty()) return std::nullopt;

  double parsed = 0.0;
  const auto* begin = view.data();
  const auto* end = view.data() + view.size();
  const auto result = std::from_chars(begin, end, parsed, std::chars_format::general);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  if (!std::isfinite(parsed)) return std::nullopt;   // `nan` / `inf` 不接受

  // 夹取
  if (has_min_ && parsed < min_) parsed = min_;
  if (has_max_ && parsed > max_) parsed = max_;
  return parsed;
}

auto NumberInput::format(double value) const -> std::string {
  if (precision_ >= 0) {
    return std::format("{:.{}f}", value, precision_);
  }
  // 自动：整数不带小数点，小数用最短往返表示（`std::format("{}")` 就是最短往返）。
  if (std::abs(value - std::round(value)) < 1e-12 && std::abs(value) < 1e15) {
    // 用 `std::int64_t` 不用 `long long`：后者的宽度随平台变（CONVENTIONS §10 / lint L17）。
    return std::format("{}", static_cast<std::int64_t>(std::llround(value)));
  }
  return std::format("{}", value);
}

void NumberInput::sync_text_from_value() {
  const std::string next = format(value_);
  if (text_ == next) return;
  text_ = next;
  caret_ = text_.size();
  mark_layout_dirty();
}

void NumberInput::set_value(double value) {
  double clamped = value;
  if (has_min_) clamped = std::max(clamped, min_);
  if (has_max_) clamped = std::min(clamped, max_);
  if (precision_ >= 0) {
    const double factor = std::pow(10.0, static_cast<double>(precision_));
    clamped = std::round(clamped * factor) / factor;
  }
  const bool changed = !nearly_equal(clamped, value_);
  value_ = clamped;
  sync_text_from_value();
  if (changed) notify_change();
}

void NumberInput::set_text(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  caret_ = std::min(caret_, text_.size());
  // 能完整解析才同步值；解析不了就**保持旧值**（用户正在敲 `-` 或 `1.`）。
  if (const auto parsed = parse(text_); parsed.has_value()) value_ = *parsed;
  mark_layout_dirty();
}

void NumberInput::set_min(double min) {
  if (has_min_ && nearly_equal(min_, min)) return;
  min_ = min;
  has_min_ = true;
  if (has_max_ && max_ < min_) max_ = min_;   // 区间不能倒置
  set_value(value_);                          // 重新夹取
  mark_layout_dirty();
}

void NumberInput::set_max(double max) {
  if (has_max_ && nearly_equal(max_, max)) return;
  max_ = max;
  has_max_ = true;
  if (has_min_ && min_ > max_) min_ = max_;
  set_value(value_);
  mark_layout_dirty();
}

void NumberInput::clear_range() {
  if (!has_min_ && !has_max_) return;
  has_min_ = false;
  has_max_ = false;
  mark_layout_dirty();
}

void NumberInput::set_step(double step) {
  const double clamped = step > 0.0 ? step : 1.0;
  if (nearly_equal(step_, clamped)) return;
  step_ = clamped;
  mark_dirty();
}

void NumberInput::set_precision(int digits) {
  const int clamped = std::clamp(digits, -1, 10);
  if (precision_ == clamped) return;
  precision_ = clamped;
  sync_text_from_value();
}

void NumberInput::set_suffix(std::string suffix) {
  if (suffix_ == suffix) return;
  suffix_ = std::move(suffix);
  mark_layout_dirty();
}

void NumberInput::step_up() { set_value(value_ + step_); }
void NumberInput::step_down() { set_value(value_ - step_); }

void NumberInput::notify_change() {
  if (on_change) on_change(value_);
}

void NumberInput::commit() {
  const auto parsed = parse(text_);
  if (!parsed.has_value()) {
    // 非法输入：回滚显示（否则界面上留着一个"值不知道是什么"的文本）。
    sync_text_from_value();
    return;
  }
  set_value(*parsed);
  sync_text_from_value();   // 夹取/舍入后回写（用户能看到 `300` 变成 `100`）
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void NumberInput::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.background = colors.surface;
  style_.border_color = colors.border;
  style_.border_width = 1.0f;
  style_.radius = kRadius;
  style_.font_size = metrics.font_sm;
  style_.color = colors.text;
  apply_text_overrides();
}

void NumberInput::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const std::string sample = text_ + suffix_;
  float width = port.measure_width(sample, style_.font_size) + kPaddingX * 2.0f + kStepperWidth;
  width = std::max(width, 72.0f);   // 至少容得下 `-12` + 步进列
  if (style_.has_explicit_width()) width = style_.width;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

void NumberInput::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  const float stepper = std::min(kStepperWidth, rect.width * 0.35f);
  field_ = math::Rect{rect.x, rect.y, std::max(rect.width - stepper, 0.0f), rect.height};
  const float half = rect.height * 0.5f;
  up_ = math::Rect{rect.right() - stepper, rect.y, stepper, half};
  down_ = math::Rect{rect.right() - stepper, rect.y + half, stepper, rect.height - half};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void NumberInput::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);

  // 输入区的底 + 描边由 `style_` 画（`Element::paint_box`）；步进列补一块底色与
  // 一道竖分隔，让它看起来是"输入框的一部分"而不是两个叠在一起的控件。
  canvas.fill_rect(math::Rect{up_.x, bounds_.y, up_.width, bounds_.height},
                   raster::Paint::solid(colors.surface_alt));
  canvas.fill_rect(math::Rect{up_.x, bounds_.y, 1.0f, bounds_.height},
                   raster::Paint::solid(colors.border));

  // 文本（右对齐到步进列左侧；数值习惯右对齐，位数变化时视觉稳定）。
  // 文本（右对齐到步进列左侧；数值习惯右对齐，位数变化时视觉稳定）。
  //
  // ⚠ 右侧留白取 `kPaddingX * 2`：步进列左边紧贴一道竖分隔线，若只留 `kPaddingX`，
  // 后缀（如 `%`）会**贴上那条线**（实测截图里「5%」贴着竖线，看起来像被夹住）。
  const float text_right = field_.right() - kPaddingX * 2.0f;
  const std::string shown = text_ + suffix_;
  const float text_width = port.measure_width(shown, style_.font_size);
  const float text_x = std::max(field_.x + kPaddingX, text_right - text_width);
  const float top = centered_line_top(port, shown, style_.font_size, bounds_.y, bounds_.height);
  port.draw(canvas, shown, math::Point{text_x, top}, style_.font_size,
            enabled_ ? colors.text : colors.text_faint);

  // 光标（聚焦时按 1s 周期闪烁；相位由 `time_seconds` 决定 → 可截图回归）。
  if (focused_ && enabled_) {
    const bool visible = std::fmod(context.time_seconds, 1.0) < 0.5;
    if (visible) {
      const float caret = caret_x(context);
      canvas.fill_rect(math::Rect{caret, bounds_.y + 6.0f, 1.0f, bounds_.height - 12.0f},
                       raster::Paint::solid(colors.text));
    }
    request_animation();
  }

  // 步进三角（上/下）。悬停时加深，禁用时变淡。
  const auto triangle = [&](math::Rect area, bool up, bool hover) {
    const float cx = area.center().x;
    const float cy = area.center().y;
    const float half_w = 4.0f;
    const float half_h = 2.5f;
    const math::Color color = !enabled_ ? colors.text_faint
                               : hover   ? colors.text
                                         : colors.text_muted;
    // 用两行像素画实心三角（比走 path 光栅化便宜，且和小尺寸图标的观感一致）。
    for (float row = 0.0f; row < half_h; row += 1.0f) {
      const float inset = half_w * (row / half_h);
      const float y = up ? cy - half_h + row : cy + half_h - row - 1.0f;
      canvas.fill_rect(math::Rect{cx - half_w + inset, y, (half_w - inset) * 2.0f, 1.0f},
                       raster::Paint::solid(color));
    }
  };
  triangle(up_, /*up=*/true, hover_up_);
  triangle(down_, /*up=*/false, hover_down_);
}

auto NumberInput::caret_x(const RenderContext& context) const -> float {
  const TextPort& port = text_port_of(context);
  const std::string head = text_.substr(0, std::min(caret_, text_.size()));
  const std::string shown = text_ + suffix_;
  const float text_width = port.measure_width(shown, style_.font_size);
  const float text_x =
      std::max(field_.x + kPaddingX, field_.right() - kPaddingX * 2.0f - text_width);
  return text_x + port.measure_width(head, style_.font_size);
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

void NumberInput::activate() {
  if (!enabled_) return;
  commit();
  if (on_commit) on_commit(text_);
}

auto NumberInput::handle_key(const Event& event) -> bool {
  if (event.key == "ArrowUp") {
    step_up();
    return true;
  }
  if (event.key == "ArrowDown") {
    step_down();
    return true;
  }
  if (event.key == "Enter") {
    commit();
    return true;
  }
  if (event.key == "Backspace") {
    if (caret_ == 0U) return true;
    const std::size_t begin = st::utf8_prev(text_, caret_);
    text_.erase(begin, caret_ - begin);
    caret_ = begin;
    set_text(text_);
    return true;
  }
  if (event.key == "Delete") {
    if (caret_ >= text_.size()) return true;
    const std::size_t end = st::utf8_next(text_, caret_);
    text_.erase(caret_, end - caret_);
    set_text(text_);
    return true;
  }
  if (event.key == "ArrowLeft") {
    caret_ = st::utf8_prev(text_, caret_);
    mark_dirty();
    return true;
  }
  if (event.key == "ArrowRight") {
    caret_ = st::utf8_next(text_, caret_);
    mark_dirty();
    return true;
  }
  if (event.key == "Home") {
    caret_ = 0;
    mark_dirty();
    return true;
  }
  if (event.key == "End") {
    caret_ = text_.size();
    mark_dirty();
    return true;
  }
  return false;
}

void NumberInput::insert_text(std::string_view inserted) {
  // 只接受数值语法字符：拒绝在输入阶段就拦掉（而不是等 commit 时判非法）。
  std::string filtered;
  for (const char ch : inserted) {
    if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+') filtered.push_back(ch);
  }
  if (filtered.empty()) return;
  caret_ = std::min(caret_, text_.size());
  text_.insert(caret_, filtered);
  caret_ += filtered.size();
  set_text(text_);
}

auto NumberInput::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_up_ = false;
      hover_down_ = false;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const bool on_up = up_.contains(event.position);
      const bool on_down = down_.contains(event.position);
      if (on_up != hover_up_ || on_down != hover_down_) {
        hover_up_ = on_up;
        hover_down_ = on_down;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      if (up_.contains(event.position)) {
        step_up();
        event.handled = true;
        return true;
      }
      if (down_.contains(event.position)) {
        step_down();
        event.handled = true;
        return true;
      }
      // 点输入区：聚焦（实际光标定位由 UiRoot 的 focusable 路径处理）。
      return false;
    }
    case EventKind::Wheel: {
      // 滚轮步进：只在聚焦时吃（否则页面滚动会被每个数值框劫持）。
      if (!focused_) return false;
      if (event.wheel_delta > 0.0f) step_down();
      else if (event.wheel_delta < 0.0f) step_up();
      else return false;
      event.handled = true;
      return true;
    }
    case EventKind::TextInput: {
      insert_text(event.text);
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: return handle_key(event);
    case EventKind::FocusOut:
      commit();
      return false;
    default: return false;
  }
}

auto NumberInput::hit_test(math::Point point) const noexcept -> bool {
  return bounds_.contains(point);
}

auto NumberInput::cursor_at(math::Point point) const noexcept -> MouseCursor {
  if (up_.contains(point) || down_.contains(point)) return MouseCursor::Hand;
  return MouseCursor::Text;
}

auto NumberInput::semantics_value() const -> std::string { return format(value_) + suffix_; }

auto NumberInput::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.editable = true;
  return flags;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto NumberInput::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return format(value_);
  if (name == "text" || name == "content") return text_;
  if (name == "min") return has_min_ ? std::optional<std::string>(format(min_)) : std::nullopt;
  if (name == "max") return has_max_ ? std::optional<std::string>(format(max_)) : std::nullopt;
  if (name == "step") return format(step_);
  if (name == "precision") return std::to_string(precision_);
  if (name == "suffix") return suffix_;
  return std::nullopt;
}

auto NumberInput::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_value(*parsed);
    return true;
  }
  if (name == "text" || name == "content") {
    set_text(std::string(value));
    return true;
  }
  if (name == "min") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_min(*parsed);
    return true;
  }
  if (name == "max") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_max(*parsed);
    return true;
  }
  if (name == "step") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_step(*parsed);
    return true;
  }
  if (name == "precision") {
    const auto parsed = st::parse_i64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_precision(static_cast<int>(*parsed));
    return true;
  }
  if (name == "suffix") {
    set_suffix(std::string(value));
    return true;
  }
  return false;
}

auto NumberInput::property_names() const -> std::vector<std::string_view> {
  return {"value", "text", "min", "max", "step", "precision", "suffix"};
}

auto NumberInput::readonly_property_names() const -> std::vector<std::string_view> {
  return {};
}

auto NumberInput::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "step_up" || action == "increment" || action == "up") {
    step_up();
    return true;
  }
  if (action == "step_down" || action == "decrement" || action == "down") {
    step_down();
    return true;
  }
  if (action == "commit" || action == "submit") {
    commit();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::NumberInput);
