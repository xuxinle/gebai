#include "st/ui/components/segmented_control.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::paint_focus_ring;
using components_internal::paint_outline;
using components_internal::text_port_of;

namespace {

/// 段内左右内边距（px）。
constexpr float kSegmentPaddingX = 12.0f;
/// 未显式给宽时的自然段宽下限（单段太窄时点击目标不足）。
constexpr float kMinSegmentWidth = 56.0f;

}  // namespace

SegmentedControl::SegmentedControl() {
  style_.background = math::Color{0, 0, 0, 0};   // 外壳自绘（见 paint_content）
  style_.border_width = 0.0f;
  style_.direction = FlexDirection::Row;
  style_.align_items = Align::Center;
  set_focusable(true);
}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void SegmentedControl::set_options(std::vector<std::string> labels) {
  labels_ = std::move(labels);
  widths_.clear();
  rects_.clear();
  indicator_ = math::Rect{};
  if (labels_.empty()) {
    index_ = -1;
  } else if (index_ < 0 || index_ >= static_cast<int>(labels_.size())) {
    index_ = 0;
  }
  mark_layout_dirty();
}

void SegmentedControl::add_option(std::string label) {
  labels_.push_back(std::move(label));
  if (index_ < 0) index_ = 0;
  widths_.clear();
  rects_.clear();
  mark_layout_dirty();
}

void SegmentedControl::clear_options() {
  if (labels_.empty()) return;
  labels_.clear();
  widths_.clear();
  rects_.clear();
  indicator_ = math::Rect{};
  index_ = -1;
  mark_layout_dirty();
}

auto SegmentedControl::option_label(std::size_t index) const -> std::string_view {
  return index < labels_.size() ? std::string_view(labels_[index]) : std::string_view{};
}

void SegmentedControl::set_index(int index, bool notify) {
  if (labels_.empty()) {
    index_ = -1;
    return;
  }
  const int clamped = std::clamp(index, 0, static_cast<int>(labels_.size()) - 1);
  if (clamped == index_) return;
  index_ = clamped;
  // **不在这里动动画状态**：目标矩形要等重排才知道（`rects_` 可能还是旧的）。
  // 绘制侧比对「新目标 vs 上次目标」起跑——重排/主题/缩放引起的重入因此不会重置过渡。
  mark_dirty();
  if (notify && on_change) on_change(index_);
}

auto SegmentedControl::selected_label() const -> std::string_view { return option_label(static_cast<std::size_t>(std::max(index_, 0))); }

auto SegmentedControl::select_label(std::string_view label, bool notify) -> bool {
  for (std::size_t index = 0; index < labels_.size(); ++index) {
    if (labels_[index] == label) {
      set_index(static_cast<int>(index), notify);
      return true;
    }
  }
  return false;
}

void SegmentedControl::set_stretch(bool value) {
  if (stretch_ == value) return;
  stretch_ = value;
  rects_.clear();
  mark_layout_dirty();
}

void SegmentedControl::set_orientation(FlexDirection orientation) {
  if (orientation_ == orientation) return;
  orientation_ = orientation;
  style_.direction = orientation;
  rects_.clear();
  mark_layout_dirty();
}

void SegmentedControl::nudge(int delta, bool notify) {
  if (labels_.empty()) return;
  const int count = static_cast<int>(labels_.size());
  int next = index_ + delta;
  // 循环切换：一排分段的键盘导航里「到头再按一下回到另一端」比卡住更自然。
  next = ((next % count) + count) % count;
  set_index(next, notify);
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void SegmentedControl::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.radius = kRadius;
  style_.font_size = metrics.font_sm;
  style_.color = theme.colors().text;
  apply_text_overrides();
  widths_.clear();   // 字号可能被覆盖 → 段宽缓存作废
  rects_.clear();
}

void SegmentedControl::rebuild_widths(const RenderContext& context) const {
  if (!widths_.empty() || labels_.empty()) return;
  const TextPort& port = text_port_of(context);
  widths_.assign(labels_.size(), kMinSegmentWidth);
  for (std::size_t index = 0; index < labels_.size(); ++index) {
    widths_[index] =
        std::max(kMinSegmentWidth, port.measure_width(labels_[index], style_.font_size) +
                                       kSegmentPaddingX * 2.0f);
  }
}

void SegmentedControl::measure(const RenderContext& context, const Constraints& constraints) {
  rebuild_widths(context);
  const bool horizontal = orientation_ == FlexDirection::Row;

  float natural = 0.0f;
  for (const float width : widths_) natural += width;
  if (horizontal) natural += kInset * 2.0f;

  float width = style_.has_explicit_width() ? style_.width : natural;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float natural_height = horizontal ? kHeight
                                    : kHeight * static_cast<float>(labels_.size()) + kInset * 2.0f;
  float height = style_.has_explicit_height() ? style_.height : natural_height;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);

  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void SegmentedControl::relayout() const {
  rects_.assign(labels_.size(), math::Rect{});
  if (labels_.empty() || bounds_.is_empty() || widths_.size() != labels_.size()) {
    indicator_ = math::Rect{};
    return;
  }
  const bool horizontal = orientation_ == FlexDirection::Row;

  if (horizontal) {
    const float inner_x = bounds_.x + kInset;
    const float inner_w = std::max(bounds_.width - kInset * 2.0f, 0.0f);
    const float inner_h = std::max(bounds_.height - kInset * 2.0f, 0.0f);
    if (stretch_) {
      const float each = inner_w / static_cast<float>(labels_.size());
      for (std::size_t index = 0; index < labels_.size(); ++index) {
        rects_[index] =
            math::Rect{inner_x + each * static_cast<float>(index), bounds_.y + kInset, each, inner_h};
      }
    } else {
      // 按文本宽排；总宽不足时整体居中（显式给了更宽的容器时不要把段落贴左）。
      float total = 0.0f;
      for (const float width : widths_) total += width;
      float x = inner_x + std::max((inner_w - total) * 0.5f, 0.0f);
      for (std::size_t index = 0; index < labels_.size(); ++index) {
        rects_[index] = math::Rect{x, bounds_.y + kInset, widths_[index], inner_h};
        x += widths_[index];
      }
    }
  } else {
    const float inner_x = bounds_.x + kInset;
    const float inner_w = std::max(bounds_.width - kInset * 2.0f, 0.0f);
    const float inner_h = std::max(bounds_.height - kInset * 2.0f, 0.0f);
    const float each = inner_h / static_cast<float>(labels_.size());
    for (std::size_t index = 0; index < labels_.size(); ++index) {
      rects_[index] =
          math::Rect{inner_x, bounds_.y + kInset + each * static_cast<float>(index), inner_w, each};
    }
  }

  // 目标位置：当前段的矩形（无选中时为空）。
  //
  // `indicator_` 存的是**上一帧的目标**，用来检测“目标变了 → 起跑动画”；
  // 它不存动画中间位置（那个只活在 `paint_content` 的局部变量里）。
  const math::Rect target =
      (index_ >= 0 && static_cast<std::size_t>(index_) < rects_.size())
          ? rects_[static_cast<std::size_t>(index_)]
          : math::Rect{};
  if (!(target.x == indicator_.x && target.y == indicator_.y &&
        target.width == indicator_.width && target.height == indicator_.height)) {
    indicator_from_ = indicator_.is_empty() ? target : indicator_;
    indicator_ = target;
    indicator_start_ = kAnimPending;
  } else if (target.is_empty()) {
    indicator_from_ = math::Rect{};
    indicator_start_ = kAnimIdle;
  }
}

void SegmentedControl::arrange(const RenderContext& context, math::Rect rect) {
  rebuild_widths(context);
  Element::arrange(context, rect);
  relayout();
}

auto SegmentedControl::segment_rect(std::size_t index) const -> math::Rect {
  return index < rects_.size() ? rects_[index] : math::Rect{};
}

/// 选中滑块的**目标矩形**（= 当前段的矩形）。
///
/// 它给的是“该在哪”，**不是**动画中间位置（那个只用于绘制）。
/// 早先把两者混在一个字段里，导致「刚点完、还没重绘时读到的还是上一段的位置」——
/// 属性面与画面不一致，且与 `index()` 矛盾。
auto SegmentedControl::indicator_rect() const -> math::Rect {
  if (index_ < 0 || static_cast<std::size_t>(index_) >= rects_.size()) return math::Rect{};
  return rects_[static_cast<std::size_t>(index_)];
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void SegmentedControl::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || labels_.empty()) return;
  // ⚠ 先重建段宽缓存：`apply_theme` 每帧都会清空它（字号可能被覆盖），
  // 而 `relayout()` 是 const、拿不到 `RenderContext`。不先重建的话 relayout 会
  // 因为「缓存长度 ≠ 选项数」直接返回，结果是**外壳画了、文字与滑块全无**——
  // 属性面全对、只有截图能发现（实测踩到）。
  rebuild_widths(context);
  relayout();
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);
  const float size = style_.font_size;

  // 外壳：凹槽底 + 发丝描边（分段控件的「容器」语义靠这一层表达）。
  fill_round_rect(canvas, bounds_, kRadius, colors.surface_sunken);
  paint_outline(canvas, bounds_, kRadius, colors.border, metrics.border_width);

  // 滑块：按 `motion_normal` 从旧位置滑到新位置；静态帧（时间没走）直接到位。
  const double now = context.time_seconds;
  const bool advancing = now > last_time_ + 1e-9;
  math::Rect slider = indicator_;
  if (indicator_start_ == kAnimIdle || !enabled_) {
    slider = indicator_;
  } else if (indicator_start_ == kAnimPending) {
    if (!advancing) {
      // 静态帧/离屏首帧：直接到位，否则画面会停在半程。
      indicator_start_ = kAnimIdle;
      slider = indicator_;
    } else {
      indicator_start_ = now;
      slider = indicator_from_;   // 起跑帧停在旧位置
    }
  }
  if (indicator_start_ > 0.0) {
    const double duration = std::max(static_cast<double>(metrics.motion_normal) / 1000.0, 1e-6);
    const double elapsed = now - indicator_start_;
    if (elapsed >= duration || !advancing) {
      indicator_start_ = kAnimIdle;
      slider = indicator_;
    } else {
      const float t = static_cast<float>(std::clamp(elapsed / duration, 0.0, 1.0));
      // ease-out（三次）：与框架其它动效同一手感。
      const double p = 1.0 - static_cast<double>(t);
      const float eased = static_cast<float>(1.0 - p * p * p);
      slider = math::Rect{
          indicator_from_.x + (indicator_.x - indicator_from_.x) * eased,
          indicator_from_.y + (indicator_.y - indicator_from_.y) * eased,
          indicator_from_.width + (indicator_.width - indicator_from_.width) * eased,
          indicator_from_.height + (indicator_.height - indicator_from_.height) * eased};
      request_animation();   // 还在路上 → 要下一帧（paint 是 const 路径，只能用这个）
    }
  }
  last_time_ = now;

  if (!slider.is_empty()) {
    const Shadow shadow = shadow_sm(context.theme);
    canvas.draw_shadow(slider, kRadius - kInset, shadow.blur, shadow.color,
                       math::Point{shadow.offset_x, shadow.offset_y});
    fill_round_rect(canvas, slider, std::max(kRadius - kInset, 0.0f), colors.surface);
  }

  for (std::size_t index = 0; index < labels_.size(); ++index) {
    const math::Rect rect = rects_[index];
    if (rect.is_empty()) continue;
    const bool active = static_cast<int>(index) == index_;
    const bool hovered = static_cast<int>(index) == hover_index_;
    math::Color ink = active ? colors.text : (enabled_ ? colors.text_muted : colors.text_faint);
    if (hovered && !active && enabled_) ink = colors.text;
    const std::string& label = labels_[index];
    const std::string clipped = port.ellipsize(label, size, rect.width - kSegmentPaddingX * 2.0f);
    if (clipped.empty()) continue;
    const float width = port.measure_width(clipped, size);
    const float top = centered_line_top(port, clipped, size, rect.y, rect.height);
    port.draw(canvas, clipped, math::Point{rect.center().x - width * 0.5f, top}, size, ink);
  }

  if (focused_) paint_focus_ring(context, canvas, bounds_, kRadius);
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

auto SegmentedControl::segment_at(math::Point point) const -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < rects_.size(); ++index) {
    if (rects_[index].contains(point)) return index;
  }
  return std::nullopt;
}

auto SegmentedControl::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const auto hit = segment_at(event.position);
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
      const auto hit = segment_at(event.position);
      if (!hit.has_value()) return false;
      if (static_cast<int>(*hit) == index_) {
        // 已选中的段再点一下：不算变化，但算「处理了」（否则事件会冒泡到外层容器，
        // 那个容器可能正好是可点的卡片——用户看到的是“点分段却打开了详情”）。
        event.handled = true;
        return true;
      }
      set_index(static_cast<int>(*hit), true);
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: {
      const std::string& key = event.key;
      const bool horizontal = orientation_ == FlexDirection::Row;
      if (key == (horizontal ? "ArrowLeft" : "ArrowUp")) {
        nudge(-1, true);
        return true;
      }
      if (key == (horizontal ? "ArrowRight" : "ArrowDown")) {
        nudge(1, true);
        return true;
      }
      if (key == "Home" && !labels_.empty()) {
        set_index(0, true);
        return true;
      }
      if (key == "End" && !labels_.empty()) {
        set_index(static_cast<int>(labels_.size()) - 1, true);
        return true;
      }
      return false;
    }
    default: return false;
  }
}

auto SegmentedControl::hit_test(math::Point point) const noexcept -> bool {
  return bounds_.contains(point);
}

auto SegmentedControl::cursor_at(math::Point point) const noexcept -> MouseCursor {
  return segment_at(point).has_value() ? MouseCursor::Hand : MouseCursor::Arrow;
}

auto SegmentedControl::semantics_text() const -> std::string {
  std::string out;
  for (std::size_t index = 0; index < labels_.size(); ++index) {
    if (index > 0) out += " | ";
    out += labels_[index];
  }
  return out;
}

auto SegmentedControl::semantics_value() const -> std::string {
  return std::string(selected_label());
}

auto SegmentedControl::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = index_ >= 0;
  flags.focused = focused_;
  return flags;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto SegmentedControl::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "options" || name == "items" || name == "segments") {
    std::string out;
    for (std::size_t index = 0; index < labels_.size(); ++index) {
      if (index > 0) out += '|';
      out += labels_[index];
    }
    return out;
  }
  if (name == "index" || name == "active" || name == "value") return std::to_string(index_);
  if (name == "text" || name == "label" || name == "current") return std::string(selected_label());
  if (name == "count") return std::to_string(labels_.size());
  if (name == "stretch") return std::string(stretch_ ? kFlagTrue : "false");
  if (name == "orientation") {
    return std::string(orientation_ == FlexDirection::Row ? "horizontal" : "vertical");
  }
  return std::nullopt;
}

auto SegmentedControl::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "options" || name == "items" || name == "segments") {
    std::vector<std::string> next;
    for (const std::string_view part : st::split(value, '|')) {
      next.emplace_back(st::trim(part));
    }
    set_options(std::move(next));
    return true;
  }
  if (name == "index" || name == "active" || name == "value") {
    const auto parsed = st::parse_i64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_index(static_cast<int>(*parsed));
    return true;
  }
  if (name == "text" || name == "label" || name == "current") {
    return select_label(st::trim(value));
  }
  if (name == "stretch") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_stretch(*parsed);
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

auto SegmentedControl::property_names() const -> std::vector<std::string_view> {
  return {"options", "index", "stretch", "orientation"};
}

auto SegmentedControl::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "select" || action == "set") {
    // 参数可以是序号，也可以是标签名。
    if (const auto parsed = st::parse_i64(st::trim(argument)); parsed.has_value()) {
      const int target = static_cast<int>(*parsed);
      if (target < 0 || target >= static_cast<int>(labels_.size())) return false;
      set_index(target, true);
      return true;
    }
    return select_label(st::trim(argument), true);
  }
  if (action == "step_forward" || action == "next") {
    if (labels_.empty()) return false;
    nudge(1, true);
    return true;
  }
  if (action == "step_backward" || action == "prev" || action == "previous") {
    if (labels_.empty()) return false;
    nudge(-1, true);
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
ST_UI_COMPONENT_IMPL(::st::ui::SegmentedControl);
