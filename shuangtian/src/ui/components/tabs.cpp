#include "st/ui/components/tabs.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  static const NullTextPort fallback;
  return context.text != nullptr ? *context.text : fallback;
}

// —— 几何常量（颜色/间距/字号一律取自 `context.theme` 的 token）——
constexpr float k_indicator_height = 2.0f;  // 选中指示条厚度
constexpr float k_min_tab_width = 36.0f;    // 标签项最小宽度（避免空标签塌陷）
constexpr float k_close_zone = 20.0f;       // × 命中区边长（大于视觉 × 本体，好点）
constexpr float k_close_glyph = 8.0f;       // × 视觉尺寸
constexpr float k_modified_dot = 6.0f;      // 修改点直径
constexpr float k_arrow_zone = 18.0f;       // 溢出箭头命中区宽
constexpr float k_arrow_glyph = 6.0f;       // 箭头视觉尺寸
constexpr float k_wheel_step = 48.0f;       // 滚轮一格的滚动步长（与编辑器行滚一致）

// 指示条动画状态机：`idle` 无动画；`pending` 已切换但尚未读到推进的时间轴。
constexpr double k_anim_idle = -1.0;
constexpr double k_anim_pending = -2.0;

[[nodiscard]] auto ease_out(float t) noexcept -> float {
  const float inverse = 1.0f - t;
  return 1.0f - inverse * inverse * inverse;
}

/// 画 ×（两条对角线，stroke_path 走线条；颜色由调用方给）。
void draw_cross(raster::Surface& canvas, math::Point center, float size, math::Color color) {
  const float half = size * 0.5f;
  raster::Path cross;
  cross.move_to(math::Point{center.x - half, center.y - half});
  cross.line_to(math::Point{center.x + half, center.y + half});
  cross.move_to(math::Point{center.x + half, center.y - half});
  cross.line_to(math::Point{center.x - half, center.y + half});
  canvas.stroke_path(cross, raster::Paint::solid(color), 1.5f);
}

/// 画实心小三角（箭头；`degrees` 为指向）。
void draw_triangle(raster::Surface& canvas, math::Point tip, float size, float degrees,
                   math::Color color) {
  const double radians = static_cast<double>(degrees) * 3.14159265358979323846 / 180.0;
  const float dx = static_cast<float>(std::cos(radians));
  const float dy = static_cast<float>(std::sin(radians));
  const float px = -dy;
  const float py = dx;
  raster::Path triangle;
  triangle.move_to(tip);
  triangle.line_to(math::Point{tip.x - dx * size + px * size * 0.5f,
                               tip.y - dy * size + py * size * 0.5f});
  triangle.line_to(math::Point{tip.x - dx * size - px * size * 0.5f,
                               tip.y - dy * size - py * size * 0.5f});
  triangle.close();
  canvas.fill_path(triangle, raster::Paint::solid(color));
}

}  // namespace

Tabs::Tabs() {
  set_focusable(true);
  style_.direction = FlexDirection::Row;
  // 标签页本体不悬浮（悬浮的是每个 tab 子项），背景透明
  set_hover_effect(HoverEffect{.enabled = false});
}

void Tabs::set_tabs(std::vector<std::string> labels) {
  labels_ = std::move(labels);
  keys_.assign(labels_.size(), std::string{});
  modified_.assign(labels_.size(), char{0});
  closable_.assign(labels_.size(), char{0});
  widths_.clear();
  total_width_ = 0.0f;
  hover_index_ = -1;
  scroll_offset_ = 0.0f;
  if (active_ >= labels_.size()) active_ = labels_.empty() ? 0 : labels_.size() - 1;
  indicator_start_ = k_anim_pending;
  mark_layout_dirty();
}

void Tabs::add_tab(std::string label) {
  labels_.push_back(std::move(label));
  keys_.emplace_back();
  modified_.push_back(char{0});
  closable_.push_back(char{0});
  widths_.clear();
  total_width_ = 0.0f;
  mark_layout_dirty();
}

void Tabs::clear_tabs() {
  labels_.clear();
  keys_.clear();
  modified_.clear();
  closable_.clear();
  widths_.clear();
  total_width_ = 0.0f;
  active_ = 0;
  hover_index_ = -1;
  scroll_offset_ = 0.0f;
  mark_layout_dirty();
}

void Tabs::sync_tabs(const std::vector<Tab>& tabs) {
  // 按 key（空则 label 值）对齐，**次序以新数据为准**（语义同 `List::sync_items`：
  // 同 key 仍是同一标签，但位置跟随新数据重排）。Tabs 的标签是纯数据自绘，
  // 「复用」的意义在活动态：**活动 key 保留则指向新位置，消失则夹取**（不派发回调，
  // 数据刷新不是用户选择）。
  const std::string active_before = std::string(active_key());
  std::vector<std::string> next_labels;
  std::vector<std::string> next_keys;
  std::vector<char> next_modified;
  std::vector<char> next_closable;
  next_labels.reserve(tabs.size());
  next_keys.reserve(tabs.size());
  next_modified.reserve(tabs.size());
  next_closable.reserve(tabs.size());
  for (const Tab& tab : tabs) {
    next_labels.push_back(tab.label);
    next_keys.push_back(tab.key);
    next_modified.push_back(tab.modified ? char{1} : char{0});
    next_closable.push_back(tab.closable ? char{1} : char{0});
  }
  labels_ = std::move(next_labels);
  keys_ = std::move(next_keys);
  modified_ = std::move(next_modified);
  closable_ = std::move(next_closable);
  std::size_t next_active = 0;
  if (!labels_.empty()) {
    // 初次同步（旧表为空）：首项；活动 key 保留：指向新位置；
    // 活动 key 消失：夹取旧索引（数据刷新不是用户选择，不派发回调）。
    next_active = labels_.empty() ? std::size_t{0}
                                  : std::min(active_, labels_.size() - 1);
    if (!active_before.empty()) {
      for (std::size_t index = 0; index < labels_.size(); ++index) {
        const std::string_view candidate = keys_[index].empty()
                                               ? std::string_view(labels_[index])
                                               : std::string_view(keys_[index]);
        if (candidate == active_before) {
          next_active = index;
          break;
        }
      }
    }
  }
  active_ = next_active;
  widths_.clear();
  total_width_ = 0.0f;
  hover_index_ = -1;
  indicator_start_ = k_anim_pending;
  set_scroll_offset(scroll_offset_);
  mark_layout_dirty();
}

void Tabs::set_tab_meta(std::size_t index, bool modified, bool closable) {
  if (index >= labels_.size()) return;
  modified_[index] = modified ? char{1} : char{0};
  closable_[index] = closable ? char{1} : char{0};
  mark_dirty();
}

auto Tabs::tab_modified(std::size_t index) const -> bool {
  return index < modified_.size() && modified_[index] != char{0};
}

auto Tabs::tab_closable(std::size_t index) const -> bool {
  return index < closable_.size() && closable_[index] != char{0};
}

auto Tabs::active_key() const -> std::string_view {
  if (active_ >= labels_.size()) return std::string_view{};
  return keys_[active_].empty() ? std::string_view(labels_[active_]) : std::string_view(keys_[active_]);
}

auto Tabs::index_of_key(std::string_view key) const -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < labels_.size(); ++index) {
    const std::string_view candidate =
        keys_[index].empty() ? std::string_view(labels_[index]) : std::string_view(keys_[index]);
    if (candidate == key) return index;
  }
  return std::nullopt;
}

auto Tabs::tab_label(std::size_t index) const -> std::string_view {
  return index < labels_.size() ? std::string_view(labels_[index]) : std::string_view{};
}

auto Tabs::index_of_label(std::string_view label) const -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < labels_.size(); ++index) {
    if (labels_[index] == label) return index;
  }
  return std::nullopt;
}

void Tabs::set_active(std::size_t index, bool notify) {
  if (labels_.empty()) return;
  const std::size_t clamped = std::min(index, labels_.size() - 1);
  if (clamped == active_) {
    // 同项重复激活：不重置指示条动画，但仍要保证它可见（溢出时可能正滚在别处）。
    scroll_to_visible(active_);
    return;
  }
  active_ = clamped;
  indicator_from_x_ = indicator_x_;
  indicator_from_width_ = indicator_width_;
  indicator_start_ = k_anim_pending;
  scroll_to_visible(active_);
  mark_dirty();
  if (notify && on_change) on_change(active_);
}

auto Tabs::active_label() const -> std::string_view { return tab_label(active_); }

auto Tabs::tab_rect(std::size_t index) const -> math::Rect {
  if (index >= widths_.size()) return math::Rect{};
  float x = bounds_.x - scroll_offset_;
  for (std::size_t cursor = 0; cursor < index; ++cursor) {
    x += widths_[cursor] + gap_;
  }
  return math::Rect{x, bounds_.y, widths_[index], bounds_.height};
}

auto Tabs::max_scroll() const noexcept -> float {
  const float overflow = total_width_ - bounds_.width;
  return overflow > 0.0f ? overflow : 0.0f;
}

void Tabs::set_scroll_offset(float offset) {
  const float clamped = std::clamp(offset, 0.0f, max_scroll());
  if (clamped == scroll_offset_) return;
  scroll_offset_ = clamped;
  mark_dirty();
}

auto Tabs::close_rect(std::size_t index) const -> math::Rect {
  if (!tab_closable(index) || index >= widths_.size()) return math::Rect{};
  const math::Rect tab = tab_rect(index);
  if (tab.is_empty()) return math::Rect{};
  // × 固定在标签右侧（留 padding 内缩），命中区稍大。
  const float y = tab.y + (tab.height - k_close_zone) * 0.5f;
  return math::Rect{tab.right() - k_close_zone - style_.padding.horizontal() * 0.5f, y,
                    k_close_zone, k_close_zone};
}

auto Tabs::arrow_left_rect() const -> math::Rect {
  if (max_scroll() <= 0.0f) return math::Rect{};
  return math::Rect{bounds_.x, bounds_.y, k_arrow_zone, bounds_.height};
}

auto Tabs::arrow_right_rect() const -> math::Rect {
  if (max_scroll() <= 0.0f) return math::Rect{};
  return math::Rect{bounds_.right() - k_arrow_zone, bounds_.y, k_arrow_zone, bounds_.height};
}

auto Tabs::overflow_arrow_rect(bool right) const -> math::Rect {
  return right ? arrow_right_rect() : arrow_left_rect();
}

void Tabs::scroll_to_visible(std::size_t index) {
  if (index >= widths_.size() || widths_.size() != labels_.size()) return;
  const math::Rect tab = tab_rect(index);
  const float left_leak = bounds_.x - tab.x;                       // 左侧溢出量
  const float right_leak = tab.right() - bounds_.right();          // 右侧溢出量
  if (left_leak > 0.0f) {
    set_scroll_offset(scroll_offset_ - left_leak);
  } else if (right_leak > 0.0f) {
    set_scroll_offset(scroll_offset_ + right_leak);
  }
}

auto Tabs::tab_index_at(math::Point point) const -> std::optional<std::size_t> {
  if (point.y < bounds_.y || point.y >= bounds_.bottom()) return std::nullopt;
  for (std::size_t index = 0; index < widths_.size(); ++index) {
    const math::Rect tab = tab_rect(index);
    if (point.x >= tab.x && point.x < tab.right()) return index;
  }
  return std::nullopt;
}

void Tabs::rebuild_widths(const RenderContext& context) const {
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);
  const float pad = metrics.space_md;
  widths_.clear();
  widths_.reserve(labels_.size());
  float total = 0.0f;
  for (std::size_t index = 0; index < labels_.size(); ++index) {
    const float text = port.measure_width(labels_[index], style_.font_size);
    // 可关闭项与修改点占用的额外横向空间（× 与圆点都在标签内部右侧/文本旁）。
    float extra = 0.0f;
    if (index < closable_.size() && closable_[index] != char{0}) extra += k_close_zone;
    if (index < modified_.size() && modified_[index] != char{0}) extra += k_modified_dot + 4.0f;
    const float width = std::max(text, k_min_tab_width) + pad * 2.0f + extra;
    widths_.push_back(width);
    total += width;
  }
  if (!labels_.empty() && gap_ > 0.0f) {
    total += gap_ * static_cast<float>(labels_.size() - 1);
  }
  total_width_ = total;
}

void Tabs::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.font_size = metrics.font_base;
  style_.font_weight = FontWeight::Medium;
  style_.color = enabled_ ? colors.text : colors.text_faint;
  style_.radius = metrics.radius_sm;
  style_.background = math::Color{0, 0, 0, 0};  // 标签条透明，仅 hover/指示条着色
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.text_align = TextAlign::Center;
  style_.direction = FlexDirection::Row;
  style_.align_items = Align::Center;
  gap_ = metrics.space_xs;
}

void Tabs::measure(const RenderContext& context, const Constraints& constraints) {
  apply_theme(context.theme);
  rebuild_widths(context);
  float width = style_.has_explicit_width() ? style_.width : total_width_;
  float height = style_.has_explicit_height() ? style_.height : context.theme.metrics().control_height;
  width = std::clamp(width, style_.min_width, style_.max_width);
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Tabs::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  // 容器变窄/标签减少后旧偏移可能越界：夹回合法范围。
  const float clamped = std::clamp(scroll_offset_, 0.0f, max_scroll());
  if (clamped != scroll_offset_) scroll_offset_ = clamped;
}

auto Tabs::resolve_indicator(const RenderContext& context, math::Rect target) const -> math::Rect {
  const double now = context.time_seconds;
  const bool advancing = now > last_time_;
  const auto snapped = [&target, this]() -> math::Rect {
    indicator_x_ = target.x;
    indicator_width_ = target.width;
    return target;
  };

  if (indicator_start_ == k_anim_idle || !enabled_) {
    last_time_ = now;
    return snapped();
  }
  if (indicator_start_ == k_anim_pending) {
    last_time_ = now;
    if (!advancing) {  // 静态帧/离屏首帧：直接到位，避免画面停在半程
      indicator_start_ = k_anim_idle;
      return snapped();
    }
    indicator_start_ = now;
    indicator_from_x_ = indicator_x_;
    indicator_from_width_ = indicator_width_;
    return math::Rect{indicator_from_x_, target.y, indicator_from_width_, target.height};  // 起步帧停在旧位置
  }

  const double duration = static_cast<double>(context.theme.metrics().motion_normal) / 1000.0;
  const double elapsed = now - indicator_start_;
  last_time_ = now;
  if (duration <= 0.0 || elapsed >= duration || !advancing) {
    indicator_start_ = k_anim_idle;
    return snapped();
  }
  const float t = ease_out(static_cast<float>(elapsed / duration));
  indicator_x_ = indicator_from_x_ + (target.x - indicator_from_x_) * t;
  indicator_width_ = indicator_from_width_ + (target.width - indicator_from_width_) * t;
  return math::Rect{indicator_x_, target.y, indicator_width_, target.height};
}

void Tabs::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || labels_.empty()) return;
  if (widths_.size() != labels_.size()) rebuild_widths(context);

  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);
  const float radius = style_.radius > 0.0f ? style_.radius : metrics.radius_sm;
  const float line = port.line_height(style_.font_size);
  const bool clipped = max_scroll() > 0.0f;
  if (clipped) canvas.push_clip_rect(bounds_);

  for (std::size_t index = 0; index < labels_.size(); ++index) {
    const math::Rect tab = tab_rect(index);
    const bool is_active = index == active_;
    const bool is_hovered = static_cast<int>(index) == hover_index_;

    if (is_hovered && !is_active && enabled_) {
      // hover 背景：上下留出呼吸，底部让开指示条。
      const math::Rect backdrop = tab.inset(
          math::Insets{0.0f, metrics.space_xs, 0.0f, metrics.space_xs + k_indicator_height});
      if (!backdrop.is_empty()) {
        canvas.fill_rect(backdrop, raster::Paint::solid(colors.surface_alt), radius);
      }
    }

    const std::string_view label = labels_[index];
    // 文本区起点：让出修改点（文本左侧）与 ×（右侧）。
    const bool show_dot = tab_modified(index);
    float text_left = tab.x;
    float text_right = tab.right();
    if (show_dot) text_left += k_modified_dot + 4.0f;
    if (tab_closable(index)) text_right -= k_close_zone;
    const float text_room = std::max(0.0f, text_right - text_left);
    if (!label.empty() && text_room > 0.0f) {
      const std::string shown = port.ellipsize(label, style_.font_size, text_room);
      const float text_width = port.measure_width(shown, style_.font_size);
      const float draw_x = text_left + (text_room - std::min(text_width, text_room)) * 0.5f;
      const float draw_y = tab.y + (tab.height - line) * 0.5f;
      math::Color color = colors.text_muted;
      if (!enabled_) {
        color = colors.text_faint;
      } else if (is_active) {
        color = colors.text;
      }
      if (!shown.empty()) {
        port.draw(canvas, shown, math::Point{draw_x, draw_y}, style_.font_size, color);
      }
    }

    // 修改点：文本左侧小圆点（直径 6，text_muted）。
    if (show_dot) {
      const math::Point dot_center{tab.x + 2.0f + k_modified_dot * 0.5f,
                                   tab.center().y};
      canvas.fill_circle(dot_center, k_modified_dot * 0.5f,
                         raster::Paint::solid(colors.text_muted));
    }
    // ×：closable 且 hover（或活动项常显，编辑器习惯：活动标签的关闭键最常用）。
    if (tab_closable(index)) {
      const bool visible = is_hovered || is_active;
      if (visible && enabled_) {
        const math::Rect zone = close_rect(index);
        if (!zone.is_empty()) {
          draw_cross(canvas, zone.center(), k_close_glyph, colors.text_muted);
        }
      }
    }
  }

  // 选中指示条（2px primary）。
  const math::Rect active_tab = tab_rect(active_);
  const math::Rect indicator =
      resolve_indicator(context, math::Rect{active_tab.x, bounds_.bottom() - k_indicator_height,
                                            active_tab.width, k_indicator_height});
  const math::Color bar = enabled_ ? colors.primary : colors.border_strong;
  if (!indicator.is_empty()) {
    canvas.fill_rect(indicator, raster::Paint::solid(bar), k_indicator_height * 0.5f);
  }
  if (clipped) canvas.pop_clip();

  // 溢出箭头（画在裁剪区外，常驻可见）。
  if (clipped) {
    const float mid_y = bounds_.center().y;
    if (scroll_offset_ > 0.0f) {
      const math::Rect zone = arrow_left_rect();
      canvas.fill_rect(zone, raster::Paint::solid(colors.surface), 4.0f);
      draw_triangle(canvas, math::Point{zone.x + 5.0f, mid_y}, k_arrow_glyph, 180.0f,
                    colors.text_muted);
    }
    if (scroll_offset_ < max_scroll()) {
      const math::Rect zone = arrow_right_rect();
      canvas.fill_rect(zone, raster::Paint::solid(colors.surface), 4.0f);
      draw_triangle(canvas, math::Point{zone.right() - 5.0f, mid_y}, k_arrow_glyph, 0.0f,
                    colors.text_muted);
    }
  }

  if (focused_) {
    // 圆角夹取到「短边一半 - 2」：半径等于半边的圆角路径描边会退化成发丝线。
    const float width = metrics.focus_width;
    const math::Rect ring_box = bounds_.inflate(width * 0.5f);
    const float limit =
        std::max(std::min(ring_box.width, ring_box.height) * 0.5f - 2.0f, 0.0f);
    if (!ring_box.is_empty()) {
      raster::Path ring;
      ring.add_rounded_rect(ring_box, std::min(radius + width * 0.5f, limit));
      canvas.stroke_path(ring, raster::Paint::solid(colors.focus_ring), width);
    }
  }
}

auto Tabs::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const auto hit = tab_index_at(event.position);
      const int index = hit.has_value() ? static_cast<int>(*hit) : -1;
      if (index != hover_index_) {
        hover_index_ = index;
        mark_dirty();
      }
      return false;
    }
    case EventKind::HoverOut:
      if (hover_index_ != -1) {
        hover_index_ = -1;
        mark_dirty();
      }
      return false;
    case EventKind::HoverIn:
      mark_dirty();
      return false;
    case EventKind::Wheel: {
      if (max_scroll() <= 0.0f) return false;
      // 滚轮向上（delta>0）= 向左看（内容后退），与垂直滚动习惯同构。
      set_scroll_offset(scroll_offset_ - event.wheel_delta * k_wheel_step);
      return true;
    }
    case EventKind::MouseDown:
    case EventKind::MouseUp:
    case EventKind::Click:
    case EventKind::DoubleClick: {
      if (!enabled_) return false;
      // 溢出箭头优先于标签。
      if (max_scroll() > 0.0f) {
        if (arrow_left_rect().contains(event.position)) {
          set_scroll_offset(scroll_offset_ - k_arrow_zone * 2.0f);
          return true;
        }
        if (arrow_right_rect().contains(event.position)) {
          set_scroll_offset(scroll_offset_ + k_arrow_zone * 2.0f);
          return true;
        }
      }
      // × 命中优先于标签切换（只在 Click 触发一次：MouseDown/Up 只吞不派，
      // 否则一次点击 MouseUp+Click 各进一次分支，回调翻倍）。
      for (std::size_t index = 0; index < labels_.size(); ++index) {
        const math::Rect zone = close_rect(index);
        if (!zone.is_empty() && zone.contains(event.position)) {
          if (event.kind == EventKind::Click && on_close) on_close(index);
          return true;  // 按下/悬停都吞掉，不触发切换
        }
      }
      const auto hit = tab_index_at(event.position);
      if (!hit.has_value()) return true;  // 落在标签条空白处：吞掉，不冒泡
      set_active(*hit, true);
      return true;
    }
    case EventKind::KeyDown: {
      if (!enabled_ || labels_.empty()) return false;
      const std::string& key = event.key;
      if (key == "ArrowRight" || key == "ArrowDown") {
        set_active((active_ + 1) % labels_.size(), true);
        return true;
      }
      if (key == "ArrowLeft" || key == "ArrowUp") {
        set_active((active_ + labels_.size() - 1) % labels_.size(), true);
        return true;
      }
      if (key == "Home") {
        set_active(0, true);
        return true;
      }
      if (key == "End") {
        set_active(labels_.size() - 1, true);
        return true;
      }
      if (key == "Enter" || key == " " || key == "Space") {
        activate();
        return true;
      }
      return false;
    }
    default: return false;
  }
}

void Tabs::activate() {
  if (!enabled_ || labels_.empty()) return;
  if (on_change) on_change(active_);
}

auto Tabs::semantics_text() const -> std::string { return std::string(active_label()); }

auto Tabs::semantics_value() const -> std::string { return std::format("{}", active_); }

auto Tabs::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = active_ < labels_.size();
  return flags;
}

auto Tabs::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "active") return std::format("{}", active_);
  if (name == "label" || name == "text") return std::string(active_label());
  if (name == "scroll") return std::format("{}", scroll_offset_);
  if (name == "options") {
    std::string joined;
    for (const auto& label : labels_) {
      if (!joined.empty()) joined.push_back('|');
      joined.append(label);
    }
    return joined;
  }
  return std::nullopt;
}

auto Tabs::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "active") {
    if (const auto index = parse_u64(value); index.has_value()) {
      set_active(static_cast<std::size_t>(*index), false);
      return true;
    }
    if (const auto index = index_of_label(value); index.has_value()) {
      set_active(*index, false);
      return true;
    }
    return false;
  }
  if (name == "label" || name == "text") {
    const auto index = index_of_label(value);
    if (!index.has_value()) return false;
    set_active(*index, false);
    return true;
  }
  if (name == "scroll") {
    if (const auto offset = parse_f64(value); offset.has_value()) {
      set_scroll_offset(static_cast<float>(*offset));
      return true;
    }
    return false;
  }
  if (name == "options") {
    std::vector<std::string> labels;
    for (const auto part : split(value, '|')) {
      labels.emplace_back(part);
    }
    set_tabs(std::move(labels));
    return true;
  }
  return false;
}

auto Tabs::property_names() const -> std::vector<std::string_view> {
  return {"active", "label", "options", "scroll"};
}

auto Tabs::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (labels_.empty()) return false;
  if (action == "next") {
    set_active((active_ + 1) % labels_.size(), true);
    return true;
  }
  if (action == "previous" || action == "prev") {
    set_active((active_ + labels_.size() - 1) % labels_.size(), true);
    return true;
  }
  if (action == "first") {
    set_active(0, true);
    return true;
  }
  if (action == "last") {
    set_active(labels_.size() - 1, true);
    return true;
  }
  if (action == "select") {
    if (const auto index = parse_u64(argument); index.has_value()) {
      set_active(static_cast<std::size_t>(*index), true);
      return true;
    }
    if (const auto index = index_of_label(argument); index.has_value()) {
      set_active(*index, true);
      return true;
    }
    return false;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
