#include "st/ui/components/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/icon.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::paint_focus_ring;
using components_internal::text_port_of;

namespace {

// —— 尺度常量（颜色一律取自主题 token，此处只有几何与动效常量）——
constexpr float k_cursor_width = 2.0f;
constexpr float k_cursor_radius = 1.0f;
constexpr float k_icon_slot_width = 18.0f;
/// 前置图标绘制尺寸（比槽位小一点，四周留白；与 `IconView` 的 18 保持同一光学尺度）
constexpr float k_icon_size = 16.0f;
constexpr float k_input_min_width = 160.0f;
constexpr float k_textarea_default_lines = 4.0f;
constexpr double k_blink_period = 1.0;
constexpr double k_blink_duty = 0.55;

[[nodiscard]] constexpr auto is_continuation(char value) noexcept -> bool {
  return (static_cast<std::uint8_t>(value) & 0xC0) == 0x80;
}

/// 前一个码点起点（`index` 为字节位置，可等于 size）。
[[nodiscard]] auto utf8_prev(std::string_view utf8, std::size_t index) noexcept -> std::size_t {
  if (index == 0) return 0;
  std::size_t cursor = index < utf8.size() ? index - 1 : utf8.size();
  while (cursor > 0 && cursor < utf8.size() && is_continuation(utf8[cursor])) --cursor;
  return cursor;
}

/// 下一个码点起点（不超过 size）。
[[nodiscard]] auto utf8_next(std::string_view utf8, std::size_t index) noexcept -> std::size_t {
  std::size_t cursor = index + 1;
  while (cursor < utf8.size() && is_continuation(utf8[cursor])) ++cursor;
  return cursor > utf8.size() ? utf8.size() : cursor;
}

/// 夹取到合法码点边界。
[[nodiscard]] auto snap_index(std::string_view utf8, std::size_t index) noexcept -> std::size_t {
  if (index >= utf8.size()) return utf8.size();
  std::size_t cursor = index;
  while (cursor > 0 && is_continuation(utf8[cursor])) --cursor;
  return cursor;
}

/// 把主题 token 同步到 `style_`（仅填充未显式设置的项），供 `paint_box` 与 `visual` 树使用。
void sync_control_style(Element& element, const RenderContext& context) {
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  Style& style = element.style();
  if (style.radius <= 0.0f) style.radius = metrics.radius_sm;
  if (style.background.a == 0U) style.background = colors.surface;
  if (style.border_width <= 0.0f) style.border_width = metrics.border_width;
  if (style.border_color.a == 0U) style.border_color = colors.border;
}

void paint_border(raster::Surface& canvas, math::Rect rect, float radius, math::Color color,
                  float width) {
  if (width <= 0.0f || color.a == 0U) return;
  const float half = width * 0.5f;
  raster::Path outline;
  outline.add_rounded_rect(rect.inset(math::Insets::all(half)), radius > half ? radius - half : 0.0f);
  canvas.stroke_path(outline, raster::Paint::solid(color), width);
}

/// 光标闪烁相位（1s 周期，亮 55% / 灭 45%）。
[[nodiscard]] auto blink_visible(const RenderContext& context) noexcept -> bool {
  const double phase = std::fmod(context.time_seconds, k_blink_period);
  const double normalized = phase < 0.0 ? phase + k_blink_period : phase;
  return normalized < k_blink_period * k_blink_duty;
}

/// 一行文本内按水平偏移定位字节列（`offset_x` 相对行首）。
[[nodiscard]] auto column_at_offset(const TextPort& port, std::string_view line_text, float size,
                                    float offset_x) -> std::size_t {
  float accumulated = 0.0f;
  std::size_t index = 0;
  while (index < line_text.size()) {
    const std::size_t next = utf8_next(line_text, index);
    const float width = port.measure_width(line_text.substr(index, next - index), size);
    if (offset_x < accumulated + width * 0.5f) return index;
    accumulated += width;
    index = next;
  }
  return line_text.size();
}

}  // namespace

// ————————————————— Input —————————————————

Input::Input() { set_focusable(true); }

void Input::set_text(std::string text) {
  text_ = std::move(text);
  cursor_ = text_.size();
  mark_dirty();
}

void Input::set_placeholder(std::string text) {
  placeholder_ = std::move(text);
  mark_dirty();
}

void Input::set_password(bool value) noexcept {
  if (password_ == value) return;
  password_ = value;
  mark_dirty();
}

void Input::set_icon_prefix(std::string_view icon_name) {
  icon_prefix_ = std::string(icon_name);
  mark_dirty();
}

void Input::set_cursor_index(std::size_t index) {
  cursor_ = snap_index(text_, index);
  mark_dirty();
}

// —— 属性面：协议 `set` / 脚本 `$('#x').set()` 的唯一入口 ——
// 不实现它，`{"value": "x"}` 会被静默忽略（实测踩过：对输入框 set value 一直无效且不报错）。
auto Input::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value" || name == "text") return text_;
  if (name == "placeholder") return placeholder_;
  if (name == "password") return password_ ? "true" : "false";
  if (name == "enabled") return enabled() ? "true" : "false";
  if (name == "visible") return visible() ? "true" : "false";
  return std::nullopt;
}

auto Input::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value" || name == "text") {
    set_text(std::string(value));
    return true;
  }
  if (name == "placeholder") {
    set_placeholder(std::string(value));
    return true;
  }
  if (name == "password") {
    set_password(value == "true" || value == "1");
    return true;
  }
  if (name == "enabled") {
    set_enabled(value == "true" || value == "1");
    return true;
  }
  if (name == "visible") {
    set_visible(value == "true" || value == "1");
    return true;
  }
  return false;
}

auto Input::property_names() const -> std::vector<std::string_view> {
  return {"value", "placeholder", "password", "enabled", "visible"};
}

// —— 动作面（与 `TextArea` 对齐）——
//
// 此前只存在于键盘路径（`Enter` → `on_submit`）与 `activate()`：自动化只能造一次真实
// 回车事件才能提交，`invoke submit` 返回 `unsupported`（DESIGN §8.1.1「API 存在但
// 动作面未实现」——`TextArea` 支持而单行 `Input` 没有，同族不对称）。
// 语义与 `Click` 路径的 `activate()`（Enter 同源）一致：提交是 **Input 自己的动作**，
// 不经由外部模拟键盘。
auto Input::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "clear") {
    set_text({});
    return true;
  }
  if (action == "submit" || action == "activate") {
    activate();
    return true;
  }
  return Element::invoke_action(action, argument);
}

auto Input::inner_box(const RenderContext& context) const -> math::Rect {
  const Metrics& metrics = context.theme.metrics();
  const float leading =
      metrics.space_md + (icon_prefix_.empty() ? 0.0f : k_icon_slot_width + metrics.space_sm);
  const math::Insets insets{leading, metrics.space_xs, metrics.space_md, metrics.space_xs};
  return bounds_.inset(style_.padding).inset(insets);
}

auto Input::display_text() const -> std::string {
  if (!password_) return text_;
  std::string masked;
  masked.reserve(text_.size());
  for (std::size_t index = 0; index < text_.size();) {
    const std::size_t next = utf8_next(text_, index);
    masked.append("•");
    index = next;
  }
  return masked;
}

auto Input::cursor_x(const RenderContext& context) const -> float {
  const TextPort& port = text_port_of(context);
  const std::string shown = display_text();
  // 掩码串与原文码点数一致，按码点推进即可定位光标显示位置。
  std::size_t shown_index = 0;
  std::size_t text_index = 0;
  while (text_index < cursor_ && shown_index < shown.size()) {
    shown_index = utf8_next(shown, shown_index);
    text_index = utf8_next(text_, text_index);
  }
  const float advance = port.measure_width(std::string_view(shown).substr(0, shown_index),
                                          style_.font_size);
  return inner_box(context).x + advance;
}

auto Input::index_at_x(const RenderContext& context, float x) const -> std::size_t {
  const TextPort& port = text_port_of(context);
  const std::string shown = display_text();
  const std::string_view text_view{text_};
  float cursor_position = inner_box(context).x;
  std::size_t shown_index = 0;
  std::size_t text_index = 0;
  while (shown_index < shown.size() && text_index < text_view.size()) {
    const std::size_t shown_next = utf8_next(shown, shown_index);
    const std::size_t text_next = utf8_next(text_view, text_index);
    const float width = port.measure_width(shown.substr(shown_index, shown_next - shown_index),
                                           style_.font_size);
    if (x < cursor_position + width * 0.5f) return text_index;
    cursor_position += width;
    shown_index = shown_next;
    text_index = text_next;
  }
  return text_view.size();
}

void Input::measure(const RenderContext& context, const Constraints& constraints) {
  sync_control_style(*this, context);
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);

  float width = style_.width;
  if (width == kAuto) {
    const std::string shown = display_text();
    const std::string_view sample = shown.empty() ? std::string_view(placeholder_)
                                                 : std::string_view(shown);
    const float leading =
        metrics.space_md + (icon_prefix_.empty() ? 0.0f : k_icon_slot_width + metrics.space_sm);
    width = port.measure_width(sample, style_.font_size) + leading + metrics.space_md +
            k_cursor_width + style_.padding.horizontal();
    width = std::max(width, k_input_min_width);
  }
  float height = style_.has_explicit_height() ? style_.height : metrics.control_height;

  width = std::clamp(width, style_.min_width, style_.max_width);
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Input::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const float radius = style_.radius > 0.0f ? style_.radius : metrics.radius_sm;
  const bool usable = enabled();

  math::Color background = colors.surface;
  math::Color border = colors.border;
  if (!usable) {
    background = colors.surface_alt;
    border = colors.border;
  } else if (focused()) {
    border = colors.primary;
  } else if (hovered() || pressed()) {
    border = colors.border_strong;
  }
  canvas.fill_rect(bounds_, raster::Paint::solid(background), radius);
  paint_border(canvas, bounds_, radius, border, metrics.border_width);
  if (usable && focused()) paint_focus_ring(context, canvas, bounds_, radius);

  const math::Rect box = inner_box(context);
  if (box.is_empty()) return;
  const TextPort& port = text_port_of(context);
  const float size = style_.font_size;
  const float line = port.line_height(size);
  const float text_y = box.y + (box.height - line) * 0.5f;
  const std::string shown = display_text();

  // 前置图标：`inner_box` 一直为它留了槽位，但**此前从未被绘制**——
  // 表现为“设了图标却看不见，文字还莫名右移一段”（空槽位仍然占宽）。
  if (!icon_prefix_.empty()) {
    const math::Rect slot{bounds_.x + style_.padding.left + metrics.space_md, bounds_.y,
                          k_icon_slot_width, bounds_.height};
    const math::Rect glyph{slot.x + (slot.width - k_icon_size) * 0.5f,
                           slot.y + (slot.height - k_icon_size) * 0.5f, k_icon_size, k_icon_size};
    const math::Color icon_color =
        usable ? (focused() ? colors.primary : colors.text_muted) : colors.text_faint;
    Icon::draw(canvas, icon_prefix_, glyph, icon_color, 0.0f);
  }

  canvas.push_clip_rect(box);
  if (shown.empty()) {
    if (!placeholder_.empty()) {
      port.draw(canvas, port.ellipsize(placeholder_, size, box.width),
                math::Point{box.x, text_y}, size, colors.text_faint);
    }
  } else {
    port.draw(canvas, port.ellipsize(shown, size, box.width), math::Point{box.x, text_y}, size,
              usable ? colors.text : colors.text_faint);
  }
  if (usable && focused() && blink_visible(context)) {
    const float height = std::min(line, box.height);
    const math::Rect caret{cursor_x(context), box.y + (box.height - height) * 0.5f, k_cursor_width,
                           height};
    canvas.fill_rect(caret, raster::Paint::solid(colors.primary), k_cursor_radius);
  }
  canvas.pop_clip();
}

auto Input::handle_key(const Event& event) -> bool {
  const std::string& key = event.key;
  if (key == "Enter") {
    if (on_submit) on_submit(text_);
    return true;
  }
  if (key == "Backspace") {
    if (cursor_ > 0) {
      const std::size_t start = utf8_prev(text_, cursor_);
      text_.erase(start, cursor_ - start);
      cursor_ = start;
      notify_change();
    }
    return true;
  }
  if (key == "Delete") {
    if (cursor_ < text_.size()) {
      const std::size_t end = utf8_next(text_, cursor_);
      text_.erase(cursor_, end - cursor_);
      notify_change();
    }
    return true;
  }
  if (key == "ArrowLeft") {
    cursor_ = cursor_ > 0 ? utf8_prev(text_, cursor_) : 0;
    mark_dirty();
    return true;
  }
  if (key == "ArrowRight") {
    cursor_ = std::min(utf8_next(text_, cursor_), text_.size());
    mark_dirty();
    return true;
  }
  if (key == "Home") {
    cursor_ = 0;
    mark_dirty();
    return true;
  }
  if (key == "End") {
    cursor_ = text_.size();
    mark_dirty();
    return true;
  }
  return false;
}

auto Input::on_event(const RenderContext& context, Event& event) -> bool {
  if (!enabled()) return false;
  switch (event.kind) {
    case EventKind::MouseDown:
    case EventKind::MouseUp:
    case EventKind::Click:
      cursor_ = index_at_x(context, event.position.x);
      mark_dirty();
      return true;
    case EventKind::KeyDown:
      return handle_key(event);
    case EventKind::TextInput:
      if (event.text.empty()) return false;
      insert_text(event.text);
      return true;
    default: return false;
  }
}

void Input::insert_text(std::string_view inserted) {
  text_.insert(cursor_, inserted);
  cursor_ += inserted.size();
  notify_change();
}

void Input::notify_change() {
  mark_dirty();
  if (on_change) on_change(text_);
}

void Input::activate() {
  if (on_submit) on_submit(text_);
}

auto Input::semantics_text() const -> std::string { return placeholder_; }

auto Input::semantics_value() const -> std::string { return display_text(); }

auto Input::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.editable = enabled();
  return flags;
}

// ————————————————— TextArea —————————————————

TextArea::TextArea() { set_focusable(true); }

void TextArea::set_read_only(bool value) noexcept {
  if (read_only_ == value) return;
  read_only_ = value;
  // 只读时要确保焦点不在“正在编辑”的状态里：光标保留（可看/可选中），
  // 但不该再由键盘改动。此处只标脏——真正的拦截在 `on_event` 的编辑分支。
  mark_dirty();
}

void TextArea::set_text(std::string text) {
  text_ = std::move(text);
  // 光标回到文首：加载文档/程序化替换文本后应当从头展示，
  // 否则 `sync_scroll` 会把视图拉到末尾（编辑中插入文本时另有路径保持光标可见）。
  cursor_ = 0;
  scroll_ = 0.0f;
  mark_dirty();
}

void TextArea::set_placeholder(std::string text) {
  placeholder_ = std::move(text);
  mark_dirty();
}

void TextArea::set_cursor_index(std::size_t index) {
  cursor_ = snap_index(text_, index);
  mark_dirty();
}

void TextArea::set_scroll_offset(float offset) {
  scroll_ = offset > 0.0f ? offset : 0.0f;
  mark_dirty();
}

auto TextArea::inner_box(const RenderContext& context) const -> math::Rect {
  const Metrics& metrics = context.theme.metrics();
  const math::Insets insets{metrics.space_md, metrics.space_sm, metrics.space_md, metrics.space_sm};
  return bounds_.inset(style_.padding).inset(insets);
}

auto TextArea::line_height(const RenderContext& context) const -> float {
  return style_.font_size * context.theme.metrics().line_height_body;
}

auto TextArea::layout_lines(const RenderContext& context, float width) const -> std::vector<Line> {
  std::vector<Line> lines;
  const TextPort& port = text_port_of(context);
  const float size = style_.font_size;
  const float wrap_width = std::max(width, 1.0f);
  const std::string_view whole{text_};

  std::size_t paragraph_begin = 0;
  while (true) {
    const std::size_t newline = text_.find('\n', paragraph_begin);
    const std::size_t paragraph_end = newline == std::string::npos ? text_.size() : newline;
    const std::string_view paragraph = whole.substr(paragraph_begin, paragraph_end - paragraph_begin);
    std::size_t search = paragraph_begin;

    const std::vector<std::string_view> wrapped = port.wrap(paragraph, size, wrap_width);
    if (wrapped.empty()) {
      lines.push_back(Line{paragraph_begin, paragraph_end});
    } else {
      for (const std::string_view piece : wrapped) {
        std::size_t begin = search;
        if (!piece.empty()) {
          const std::size_t found = text_.find(piece, search);
          if (found != std::string::npos && found <= paragraph_end) begin = found;
        }
        std::size_t end = begin + piece.size();
        if (end > paragraph_end) end = paragraph_end;
        lines.push_back(Line{begin, end});
        search = end;
      }
    }

    if (newline == std::string::npos) break;
    paragraph_begin = newline + 1;
    if (paragraph_begin > text_.size()) break;
  }
  return lines;
}

auto TextArea::row_of(const std::vector<Line>& lines, std::size_t index) const -> std::size_t {
  std::size_t row = 0;
  for (std::size_t position = 0; position < lines.size(); ++position) {
    if (lines[position].begin <= index) row = position;
  }
  return row;
}

auto TextArea::index_at_point(const RenderContext& context, math::Point point) const
    -> std::size_t {
  const math::Rect box = inner_box(context);
  const TextPort& port = text_port_of(context);
  const std::vector<Line> lines = layout_lines(context, box.width);
  if (lines.empty()) return 0;
  const float line = line_height(context);
  const float relative = point.y - box.y + scroll_;
  const float raw_row = std::floor(relative / line);
  const float clamped_row = math::clampf(raw_row, 0.0f, static_cast<float>(lines.size() - 1));
  const auto row = static_cast<std::size_t>(clamped_row);
  const Line& target = lines[row];
  const std::string_view content = std::string_view(text_).substr(target.begin,
                                                                 target.end - target.begin);
  const std::size_t column = column_at_offset(port, content, style_.font_size, point.x - box.x);
  return target.begin + column;
}

void TextArea::sync_scroll(const RenderContext& context) {
  const math::Rect box = inner_box(context);
  if (box.is_empty()) {
    scroll_ = 0.0f;
    return;
  }
  const float line = line_height(context);
  const std::vector<Line> lines = layout_lines(context, box.width);
  if (lines.empty()) {
    scroll_ = 0.0f;
    return;
  }
  const std::size_t row = row_of(lines, cursor_);
  const float content_height = static_cast<float>(lines.size()) * line;
  const float max_scroll = std::max(0.0f, content_height - box.height);
  const float top = static_cast<float>(row) * line;
  if (top < scroll_) scroll_ = top;
  if (top + line > scroll_ + box.height) scroll_ = top + line - box.height;
  scroll_ = math::clampf(scroll_, 0.0f, max_scroll);
}

void TextArea::move_vertical(const RenderContext& context, int direction) {
  const math::Rect box = inner_box(context);
  const TextPort& port = text_port_of(context);
  const std::vector<Line> lines = layout_lines(context, box.width);
  if (lines.empty()) return;
  const std::size_t row = row_of(lines, cursor_);
  const Line& current = lines[row];
  const std::string_view current_text = std::string_view(text_).substr(
      current.begin, current.end - current.begin);
  const float offset = port.measure_width(
      current_text.substr(0, cursor_ - current.begin), style_.font_size);

  std::size_t target = row;
  if (direction < 0 && row > 0) target = row - 1;
  if (direction > 0 && row + 1 < lines.size()) target = row + 1;
  const Line& line = lines[target];
  const std::string_view line_text = std::string_view(text_).substr(line.begin,
                                                                   line.end - line.begin);
  cursor_ = line.begin + column_at_offset(port, line_text, style_.font_size, offset);
  sync_scroll(context);
  mark_dirty();
}

void TextArea::move_to_line_edge(const RenderContext& context, bool to_end) {
  const math::Rect box = inner_box(context);
  const std::vector<Line> lines = layout_lines(context, box.width);
  if (lines.empty()) return;
  const Line& line = lines[row_of(lines, cursor_)];
  cursor_ = to_end ? line.end : line.begin;
  sync_scroll(context);
  mark_dirty();
}

void TextArea::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  sync_scroll(context);
}

void TextArea::measure(const RenderContext& context, const Constraints& constraints) {
  sync_control_style(*this, context);
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);

  float width = style_.width;
  if (width == kAuto) {
    const float placeholder_width = port.measure_width(placeholder_, style_.font_size);
    width = std::max(placeholder_width, k_input_min_width) + metrics.space_md * 2.0f +
            style_.padding.horizontal();
  }
  float height = style_.height;
  if (height == kAuto) {
    height = k_textarea_default_lines * line_height(context) + metrics.space_sm * 2.0f +
             style_.padding.vertical();
  }

  width = std::clamp(width, style_.min_width, style_.max_width);
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void TextArea::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const float radius = style_.radius > 0.0f ? style_.radius : metrics.radius_sm;
  const bool usable = enabled();

  math::Color background = colors.surface;
  math::Color border = colors.border;
  if (!usable) {
    background = colors.surface_alt;
  } else if (focused()) {
    border = colors.primary;
  } else if (hovered() || pressed()) {
    border = colors.border_strong;
  }
  canvas.fill_rect(bounds_, raster::Paint::solid(background), radius);
  paint_border(canvas, bounds_, radius, border, metrics.border_width);
  if (usable && focused()) paint_focus_ring(context, canvas, bounds_, radius);

  const math::Rect box = inner_box(context);
  if (box.is_empty()) return;
  const TextPort& port = text_port_of(context);
  const float size = style_.font_size;
  const float line = line_height(context);
  const std::vector<Line> lines = layout_lines(context, box.width);
  const std::string_view whole{text_};

  canvas.push_clip_rect(box);
  if (text_.empty() && !placeholder_.empty()) {
    port.draw(canvas, port.ellipsize(placeholder_, size, box.width),
              math::Point{box.x, box.y}, size, colors.text_faint);
  } else {
    const float first = std::max(0.0f, std::floor(scroll_ / line));
    const auto first_row = static_cast<std::size_t>(first);
    for (std::size_t row = first_row; row < lines.size(); ++row) {
      const float y = box.y + static_cast<float>(row) * line - scroll_;
      if (y > box.bottom()) break;
      const Line& entry = lines[row];
      if (entry.end <= entry.begin) continue;
      const std::string_view content = whole.substr(entry.begin, entry.end - entry.begin);
      port.draw(canvas, content, math::Point{box.x, y}, size,
                usable ? colors.text : colors.text_faint);
    }
  }

  if (usable && focused() && blink_visible(context)) {
    const std::size_t row = row_of(lines, cursor_);
    const Line& entry = lines[row];
    const float caret_x = box.x + port.measure_width(
                                      whole.substr(entry.begin, cursor_ - entry.begin), size);
    const float caret_y = box.y + static_cast<float>(row) * line - scroll_;
    canvas.fill_rect(math::Rect{caret_x, caret_y, k_cursor_width, line},
                     raster::Paint::solid(colors.primary), k_cursor_radius);
  }
  canvas.pop_clip();

  // 内容超高时的滚动条提示（4px 圆角，纯视觉）。
  const float content_height = static_cast<float>(lines.size()) * line;
  if (content_height > box.height + 0.5f) {
    const float track_height = box.height;
    const float thumb_height = std::max(20.0f, track_height * (track_height / content_height));
    const float max_scroll = content_height - box.height;
    const float ratio = max_scroll > 0.0f ? math::clamp01(scroll_ / max_scroll) : 0.0f;
    const float thumb_y = box.y + ratio * (track_height - thumb_height);
    const float x = bounds_.right() - metrics.space_xs - 4.0f;
    canvas.fill_rect(math::Rect{x, thumb_y, 4.0f, thumb_height},
                     raster::Paint::solid(colors.border_strong), 2.0f);
  }
}

void TextArea::insert_text(std::string_view inserted) {
  text_.insert(cursor_, inserted);
  cursor_ += inserted.size();
  notify_change();
}

void TextArea::erase_backward() {
  if (cursor_ == 0) return;
  const std::size_t start = utf8_prev(text_, cursor_);
  text_.erase(start, cursor_ - start);
  cursor_ = start;
  notify_change();
}

void TextArea::erase_forward() {
  if (cursor_ >= text_.size()) return;
  const std::size_t end = utf8_next(text_, cursor_);
  text_.erase(cursor_, end - cursor_);
  notify_change();
}

void TextArea::notify_change() {
  mark_dirty();
  if (on_change) on_change(text_);
}

auto TextArea::on_event(const RenderContext& context, Event& event) -> bool {
  if (!enabled()) return false;
  switch (event.kind) {
    case EventKind::MouseDown:
    case EventKind::MouseUp:
    case EventKind::Click:
      cursor_ = index_at_point(context, event.position);
      sync_scroll(context);
      mark_dirty();
      return true;
    case EventKind::Wheel:
      scroll_ = scroll_ - event.wheel_delta;
      sync_scroll(context);
      mark_dirty();
      return true;
    case EventKind::TextInput:
      if (event.text.empty()) return false;
      // 只读：**不进字**（但落到底部的 KeyDown 导航分支仍放行——
      // 只读展示仍要能按方向键/Home/End 看内容，这正是它区别于 disabled 的地方）
      if (read_only_) return true;
      insert_text(event.text);
      return true;
    case EventKind::KeyDown: {
      const std::string& key = event.key;
      // 编辑类按键在只读下**消费但不落字**（消费=不让它冒泡出去触发页面级快捷键；
      // 不落字=真的没改内容）。导航类按键（方向/Home/End/PageUp/PageDown）则照常。
      if (read_only_) {
        if (key == "Enter" || key == "Backspace" || key == "Delete") return true;
      }
      if (key == "Enter") {
        insert_text("\n");
        return true;
      }
      if (key == "Backspace") {
        erase_backward();
        return true;
      }
      if (key == "Delete") {
        erase_forward();
        return true;
      }
      if (key == "ArrowLeft") {
        cursor_ = cursor_ > 0 ? utf8_prev(text_, cursor_) : 0;
        sync_scroll(context);
        mark_dirty();
        return true;
      }
      if (key == "ArrowRight") {
        cursor_ = std::min(utf8_next(text_, cursor_), text_.size());
        sync_scroll(context);
        mark_dirty();
        return true;
      }
      if (key == "ArrowUp") {
        move_vertical(context, -1);
        return true;
      }
      if (key == "ArrowDown") {
        move_vertical(context, 1);
        return true;
      }
      if (key == "Home") {
        move_to_line_edge(context, false);
        return true;
      }
      if (key == "End") {
        move_to_line_edge(context, true);
        return true;
      }
      if (key == "PageUp") {
        scroll_ -= inner_box(context).height;
        sync_scroll(context);
        mark_dirty();
        return true;
      }
      if (key == "PageDown") {
        scroll_ += inner_box(context).height;
        sync_scroll(context);
        mark_dirty();
        return true;
      }
      return false;
    }
    default: return false;
  }
}

void TextArea::activate() {
  if (on_change) on_change(text_);
}

auto TextArea::semantics_text() const -> std::string { return placeholder_; }

auto TextArea::semantics_value() const -> std::string { return text_; }

auto TextArea::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  // 只读仍是“可编辑控件”（只是不接受修改），仍可聚焦/选中/滚动——
  // 这与 disabled 在无障碍语义上是两件事（屏幕阅读器据此决定“可以读”还是“不可用”）。
  flags.editable = enabled() && !read_only_;
  flags.scrollable = true;
  return flags;
}

auto TextArea::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value" || name == "text") return text_;
  if (name == "placeholder") return placeholder_;
  if (name == "cursor_index") return std::format("{}", cursor_);
  if (name == "scroll_offset") return std::format("{:.1f}", static_cast<double>(scroll_));
  if (name == "read_only") return read_only_ ? "true" : "false";
  return std::nullopt;
}

auto TextArea::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value" || name == "text") {
    set_text(std::string(value));
    return true;
  }
  if (name == "placeholder") {
    set_placeholder(std::string(value));
    return true;
  }
  if (name == "cursor_index") {
    const auto parsed = st::parse_u64(value);
    if (!parsed.has_value()) return false;
    set_cursor_index(static_cast<std::size_t>(*parsed));
    return true;
  }
  if (name == "scroll_offset" || name == "scroll") {
    const auto parsed = st::parse_f64(value);
    if (!parsed.has_value()) return false;
    set_scroll_offset(static_cast<float>(*parsed));
    return true;
  }
  if (name == "read_only") {
    set_read_only(value == "true" || value == "1");
    return true;
  }
  return false;
}

auto TextArea::property_names() const -> std::vector<std::string_view> {
  return {"value", "text", "placeholder", "cursor_index", "scroll_offset", "read_only"};
}

auto TextArea::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "clear") {
    set_text({});
    return true;
  }
  if (action == "submit" || action == "activate") {
    activate();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
