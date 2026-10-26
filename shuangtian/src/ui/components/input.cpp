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
#include "st/math/path.hpp"
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

// `utf8_prev` / `utf8_next` 现由骨架层提供（`st/core/string.hpp`）：单一真相源，
// 并修掉旧副本在**文末光标**下的缺陷（旧版 `utf8_prev(text, size) == size`，
// 导致打字后（光标=文末）退格什么都不删）。见
// `tests/ui_input_test.cpp: ui_text_area_backspace_at_end_deletes_last_codepoint`。

/// 夹取到合法码点边界。
[[nodiscard]] auto snap_index(std::string_view utf8, std::size_t index) noexcept -> std::size_t {
  if (index >= utf8.size()) return utf8.size();
  std::size_t cursor = index;
  while (cursor > 0 && is_continuation(utf8[cursor])) --cursor;
  return cursor;
}

/// 把主题 token 同步到 `style_`（仅填充未显式设置的项），供 `paint_box` 与 `visual` 树使用。
///
/// ⚠ 判据是 `a == 0`（“未显式设过”）：所以它**只能承担首次填充**，不能当主题刷新用——
/// 一旦填过，`a != 0` 就永远是旧值（那是 2026-10-18 修的第二个缺陷，
/// 见 `Input::apply_theme` 的注释）。
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
  // 走「环形填充」而不是描边：描边把每一段扩展成独立四边形，几何量是环形的两倍
  // （见 `make_rounded_border_ring` 的注释与实测）。
  canvas.fill_path(math::make_rounded_border_ring(rect, radius, width),
                   raster::Paint::solid(color));
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

Input::Input() {
  set_focusable(true);
  // 输入框 = 可编辑文字 → I-beam 指针（原生应用的通用约定：
  // 看到插入点光标就知道“这里能打字/能选字”）。
  set_cursor(MouseCursor::Text);
}

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
  if (name == "selection") {
    // 选中文本（只读快照）；无选区/密码框 → 空串。
    return selected_text();
  }
  return std::nullopt;
}

auto Input::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value" || name == "text") {
    // 属性面写入（多来自控制通道/自动化）**通知** `on_change`：宿主把它当
    // "文本变了"的唯一钩子——静默改会出现"get 读回来是新文本、宿主状态里还是
    // 旧的"（实测：gbcode 提交框 `set text` 成功、提交时报"提交信息为空"）。
    // C++ API `set_text`（应用内部逻辑，如 clear 动作）仍不通知——
    // "程序化写入不冒充用户编辑"的旧语义保留（见 ui_input_invoke_clear 测试）。
    const std::string before = text_;
    set_text(std::string(value));
    if (text_ != before && on_change) on_change(text_);
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
  // 只列**可读写**属性（契约见 `Element::property_names`）。
  return {"value", "placeholder", "password", "enabled", "visible"};
}

auto Input::readonly_property_names() const -> std::vector<std::string_view> {
  // `selection` 只读（`get` 能读、`set` 不应生效）——与 TextArea / Terminal /
  // CodeEditor 同一组名字，自动化可用同一个判据。
  return {"selection"};
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
  if (action == "select_all") {
    if (password_) return true;   // 密码框不做选区
    select_anchor_ = 0U;
    cursor_ = text_.size();
    mark_dirty();
    return true;
  }
  if (action == "clear_selection") {
    clear_selection();
    mark_dirty();
    return true;
  }
  if (action == "copy") {
    // 有选区 → 拷选区；无选区 → 保持旧语义（拷全文，兼容既有调用方）。
    // 密码框拒绝：把真实文本写进剪贴板等于把密码泄露到任何能读剪贴板的地方。
    // 空文本/无宿主 → false（如实上报，不假装成功）。
    if (password_ || host() == nullptr) return false;
    const std::string text = has_selection() ? selected_text() : text_;
    if (text.empty()) return false;
    return host()->host_set_clipboard_text(text);
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
  return index_to_x(context, cursor_);
}

auto Input::index_to_x(const RenderContext& context, std::size_t text_index) const -> float {
  const TextPort& port = text_port_of(context);
  const std::string shown = display_text();
  // 掩码串与原文码点数一致，按码点推进即可定位显示位置。
  std::size_t shown_index = 0;
  std::size_t walk = 0;
  while (walk < text_index && shown_index < shown.size() && walk < text_.size()) {
    shown_index = utf8_next(shown, shown_index);
    walk = utf8_next(text_, walk);
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

void Input::apply_theme(const Theme& theme) {
  Element::apply_theme(theme);
  // **输入框的底色/描边必须随主题重落**（2026-10-18 修）。
  //
  // 缺陷形态（实测踩到）：`Input` 原本**没有** `apply_theme`，底色只在
  // `measure` 里由 `sync_control_style` 做一次“未设则填”。而那次填充之后
  // `style.background.a != 0`，后续每次 measure 都跳过——于是切主题时
  // `style().background` 永久停在首次主题上。
  //
  // 为何“画面看着对”却仍是缺陷：`paint_content` 直读 `colors.surface`，
  // 所以屏幕上的输入框会变；但 `style().background` 还供给 `visual` 树、
  // `paint_box` 与控制通道的属性快照——它停在旧主题就造成“屏幕一套、
  // 语义树另一套”。嵌在暗色命令面板里的输入框因此看着像一块没跟上的浅色区域。
  //
  // “显式设置优先”的保住方式：只在我方上一次写入的值**未被别人改过**时才覆盖。
  //
  // ⚠ 首轮特判不能省（实测踩到）：构造时 `applied_*` 是全透明，而 `style_` 可能
  // 已被 `measure` 的 “未设则填” 写成某个具体色。若只看“是否等于我方上次写的值”，
  // 首轮就会判为“被别人改过”→ 永远不更新 → 主题切换完全无效。
  // 用 `theme_style_owned_` 记“这份色是我们落的”这个事实。
  const math::Color next_bg = theme.colors().surface;
  const math::Color next_border = theme.colors().border;
  const auto untouched = [](math::Color current, math::Color mine) {
    return current.r == mine.r && current.g == mine.g && current.b == mine.b &&
           current.a == mine.a;
  };
  if (!theme_style_owned_ || untouched(style_.background, applied_theme_bg_)) {
    style_.background = next_bg;
    applied_theme_bg_ = next_bg;
    theme_style_owned_ = true;
  }
  if (!theme_style_owned_ || untouched(style_.border_color, applied_theme_border_)) {
    style_.border_color = next_border;
    applied_theme_border_ = next_border;
  }
  // ⚠ **不要在这里 `mark_dirty()`**（实测踩到：加了它会让
  // `script_write_takes_partial_damage_path` 这类“局部重绘”用例直接变红）：
  // `apply_theme` 现在每帧都会被调（主题传播与重排解耦，见 `UiRoot::layout`），
  // 无条件标脏等于“每帧整帧重绘”，把增量重绘整体作废。
  // 真正需要重画的情况（主题真的变了）由 `UiRoot::set_theme` 的
  // `mark_dirty_all()` 负责——那才是“全局变化”的正确入口。
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
  // —— 选区高亮（2026-10-14）——
  // 画在文本**之下**，且用与光标同一套「字节索引 → x」映射（`index_to_x`）——
  // 两处各算一份必然错位（掩码串与原文的索引对齐已经在那边处理过了）。
  // 垂直方向与光标一致（行框内居中），保证“高亮包住字”而不是贴着行底。
  if (usable && has_selection() && !password_) {
    const auto [from, to] = ordered_selection();
    const float x0 = index_to_x(context, from);
    const float x1 = index_to_x(context, to);
    const float band_h = std::min(line, box.height);
    const math::Rect band{std::min(x0, x1), box.y + (box.height - band_h) * 0.5f, x1 - x0,
                          band_h};
    canvas.fill_rect(band, raster::Paint::solid(colors.selection), k_cursor_radius);
  }
    if (shown.empty()) {
    if (!placeholder_.empty()) {
      port.draw(canvas, port.ellipsize(placeholder_, size, box.width),
                math::Point{box.x, text_y}, size, colors.text_faint);
    }
  } else {
    // 显式文字色覆盖（DSL `BoxProps::color/hex_color`）盖过可用/禁用色
    //（Input 不覆写 apply_theme，基类已把覆盖回放进 style_；这里读回）。
    math::Color text_color = usable ? colors.text : colors.text_faint;
    if (const auto& override_color = text_color_override(); override_color.has_value()) {
      text_color = *override_color;
    }
    port.draw(canvas, port.ellipsize(shown, size, box.width), math::Point{box.x, text_y}, size,
              text_color);
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
  const bool ctrl = event.ctrl || event.meta;
  const bool shift = event.shift;
  // —— 剪贴板与全选（2026-10-14：从“整框复制”升级到**选区语义**）——
  if (ctrl && (key == "c" || key == "C")) {
    // 有选区 → 拷选区；无选区 → 保持旧语义（拷全文，兼容既有调用方）。
    return invoke_action("copy", {});
  }
  if (ctrl && (key == "x" || key == "X")) {
    // 剪切：密码框拒绝（与 copy 同一安全边界）。
    if (password_) return true;
    if (!has_selection()) return true;   // 无选区：无事发生（不删全框）
    if (host() != nullptr) {
      const std::string picked = selected_text();
      if (!picked.empty()) (void)host()->host_set_clipboard_text(picked);
    }
    (void)erase_selection();
    return true;
  }
  if (ctrl && (key == "a" || key == "A")) {
    if (password_) return true;   // 密码框不做选区
    select_anchor_ = 0U;
    cursor_ = text_.size();
    mark_dirty();
    return true;
  }
  // —— 移动（Shift = 扩选）——
  // 扩选的核心：**不动锚点**，只移光标（`cursor_` 就是“焦点端”）。
  // 首次按 Shift+方向键时若还没有锚点，就把当前光标当作锚点（从当前位置开始扩）。
  const auto extend = [this, shift]() {
    if (shift && select_anchor_ == kNoSelection) select_anchor_ = cursor_;
    if (!shift) select_anchor_ = kNoSelection;   // 无 Shift 的移动 = 收起选区
  };
  if (key == "ArrowLeft") {
    if (has_selection() && !shift) {
      // 有选区且无 Shift → 光标跳到**选区左端**（浏览器行为），而不是左移一格。
      cursor_ = ordered_selection().first;
      select_anchor_ = kNoSelection;
    } else {
      extend();
      cursor_ = cursor_ > 0 ? utf8_prev(text_, cursor_) : 0;
    }
    mark_dirty();
    return true;
  }
  if (key == "ArrowRight") {
    if (has_selection() && !shift) {
      cursor_ = ordered_selection().second;
      select_anchor_ = kNoSelection;
    } else {
      extend();
      cursor_ = std::min(utf8_next(text_, cursor_), text_.size());
    }
    mark_dirty();
    return true;
  }
  if (key == "Home" || key == "End") {
    extend();
    cursor_ = key == "Home" ? 0U : text_.size();
    mark_dirty();
    return true;
  }
  // —— 删除（有选区时先删选区，这是通行行为）——
  if (key == "Backspace") {
    if (erase_selection()) return true;
    if (cursor_ > 0) {
      const std::size_t start = utf8_prev(text_, cursor_);
      text_.erase(start, cursor_ - start);
      cursor_ = start;
      notify_change();
    }
    return true;
  }
  if (key == "Delete") {
    if (erase_selection()) return true;
    if (cursor_ < text_.size()) {
      const std::size_t end = utf8_next(text_, cursor_);
      text_.erase(cursor_, end - cursor_);
      notify_change();
    }
    return true;
  }
  if (key == "Enter") {
    if (on_submit) on_submit(text_);
    return true;
  }
  return false;
}

auto Input::on_event(const RenderContext& context, Event& event) -> bool {
  if (!enabled()) return false;
  switch (event.kind) {
    case EventKind::MouseDown: {
      if (event.button != 1) return false;
      const std::size_t hit = index_at_x(context, event.position.x);
      // —— 连击判定（双击选词 / 三击整行）——
      // 时间取自 `RenderContext::time_seconds`（事件不带时间戳）。
      const std::int64_t now_ms = static_cast<std::int64_t>(context.time_seconds * 1000.0);
      const bool near = std::abs(event.position.x - last_click_x_) < 6.0f;
      const bool chained = last_click_ms_ >= 0 && (now_ms - last_click_ms_) < 500 && near;
      click_streak_ = chained ? std::min(click_streak_ + 1, 3) : 1;
      last_click_ms_ = now_ms;
      last_click_x_ = event.position.x;

      if (password_) {
        // 密码框：只有光标，没有选区（连双击也不给）。
        cursor_ = hit;
        select_anchor_ = kNoSelection;
        selecting_ = false;
        mark_dirty();
        return true;
      }
      if (click_streak_ == 2) {
        // 双击：选中该位置的**词**（词界由公共算法给）。
        const auto [begin, end] = components_internal::word_range_at(text_, hit);
        select_anchor_ = begin;
        cursor_ = end;
        selecting_ = false;
        mark_dirty();
        return true;
      }
      if (click_streak_ == 3) {
        // 三击：整行（编辑框通行行为）。
        select_anchor_ = 0U;
        cursor_ = text_.size();
        selecting_ = false;
        mark_dirty();
        return true;
      }
      // 单击：Shift = 扩展已有选区（保留锚点），否则重新开始拖选。
      if (event.shift && select_anchor_ != kNoSelection) {
        cursor_ = hit;
        selecting_ = false;
      } else {
        cursor_ = hit;
        select_anchor_ = hit;   // 锚点 = 按下处（拖了才有非空选区）
        selecting_ = true;
      }
      mark_dirty();
      return true;
    }
    case EventKind::MouseMove: {
      if (!selecting_) return false;
      cursor_ = index_at_x(context, event.position.x);
      mark_dirty();
      return true;
    }
    case EventKind::MouseUp:
      selecting_ = false;
      return true;
    case EventKind::Click: {
      // `Click` 与 `MouseDown` 可能同名事件重复送入：不重复做连击计数。
      return true;
    }
    case EventKind::KeyDown: return handle_key(event);
    case EventKind::TextInput:
      if (event.text.empty()) return false;
      insert_text(event.text);   // 走 `replace_selection_with`：有选区就替换
      return true;
    default: return false;
  }
}

void Input::insert_text(std::string_view inserted) {
  // **所有文本插入的唯一入口**（包括输入法）：有选区时**替换**，否则插入。
  // 为什么收敛到一个函数：旧实现直接 `text_.insert(cursor_, …)`，于是“有选区时
  // 打字应该覆盖选中”这个通行行为会在好几处（`TextInput`、粘贴、补全）各写一遍，
  // 漏一处就是一个难查的行为不一致。
  replace_selection_with(inserted);
}

void Input::replace_selection_with(std::string_view inserted) {
  if (has_selection()) {
    const auto [from, to] = ordered_selection();
    text_.erase(from, to - from);
    cursor_ = from;
    clear_selection();
  }
  text_.insert(cursor_, inserted);
  cursor_ += inserted.size();
  notify_change();
}

auto Input::ordered_selection() const -> std::pair<std::size_t, std::size_t> {
  if (select_anchor_ == kNoSelection) return {cursor_, cursor_};
  const std::size_t a = std::min(select_anchor_, cursor_);
  const std::size_t b = std::max(select_anchor_, cursor_);
  return {std::min(a, text_.size()), std::min(b, text_.size())};
}

auto Input::has_selection() const -> bool {
  // 密码框不做选区（连“看起来有选区”也不做：那会诱使用户以为能拷）。
  if (password_) return false;
  const auto [from, to] = ordered_selection();
  return to > from;
}

auto Input::selection() const -> std::pair<std::size_t, std::size_t> {
  if (!has_selection()) return {cursor_, cursor_};
  return ordered_selection();
}

auto Input::selected_text() const -> std::string {
  // 密码框：空（不给绕过掩码取明文的口子）。
  if (password_ || !has_selection()) return {};
  const auto [from, to] = ordered_selection();
  return text_.substr(from, to - from);
}

void Input::set_selection(std::size_t anchor, std::size_t focus) {
  // 两端都夹到**码点边界**与文本长度内（否则会切出半个 UTF-8 序列）。
  const auto clamp_index = [this](std::size_t index) -> std::size_t {
    const std::size_t bounded = std::min(index, text_.size());
    std::size_t pos = bounded;
    // 向后退到最近的码点边界（续字节 `10xxxxxx` 就继续退）。
    while (pos > 0U && (static_cast<unsigned char>(text_[pos]) & 0xC0U) == 0x80U) --pos;
    return pos;
  };
  select_anchor_ = clamp_index(anchor);
  cursor_ = clamp_index(focus);
  mark_dirty();
}

auto Input::erase_selection() -> bool {
  if (!has_selection()) return false;
  const auto [from, to] = ordered_selection();
  text_.erase(from, to - from);
  cursor_ = from;
  clear_selection();
  notify_change();
  return true;
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

TextArea::TextArea() {
  set_focusable(true);
  set_cursor(MouseCursor::Text);
}

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

/// 给定**字节索引** → 屏幕坐标（选区高亮每行两端用它）。
///
/// 与 `index_at_point` **同源**（同一份 `layout_lines` + 同一行高 + 同一 `scroll_`）：
/// 两处口径不一致会出现“点得到、高亮不上”这类难查的偏差。
auto TextArea::index_to_point(const RenderContext& context, std::size_t index) const
    -> math::Point {
  const math::Rect box = inner_box(context);
  const TextPort& port = text_port_of(context);
  const std::vector<Line> lines = layout_lines(context, box.width);
  if (lines.empty()) return math::Point{box.x, box.y};
  const std::size_t row = row_of(lines, index);
  const Line& target = lines[row];
  const std::size_t offset = index <= target.begin ? 0U
                            : std::min(index, target.end) - target.begin;
  const std::string_view content =
      std::string_view(text_).substr(target.begin, target.end - target.begin);
  const float x = box.x + port.measure_width(content.substr(0, offset), style_.font_size);
  const float y = box.y + static_cast<float>(row) * line_height(context) - scroll_;
  return math::Point{x, y};
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
  // —— 选区高亮（2026-10-14）——
  // **逐行分段**：每行取该行内的选中区间，换算成像素跳度（不是整行矩形——
  // 跨行选区若整行高亮，会让人以为多选了）。水平/垂直都用与光标同一套
  // `index_to_point`，避免“高亮与文字错开”。
  if (usable && has_selection()) {
    const auto [from, to] = ordered_selection();
    const float band_h = std::min(line, box.height);
    for (std::size_t row = 0; row < lines.size(); ++row) {
      const Line& entry = lines[row];
      // 该行与选区交集（行尾位置也算端点，故用 `<=` 比较左端）。
      const std::size_t seg_from = std::max(from, entry.begin);
      const std::size_t seg_to = std::min(to, entry.end);
      if (seg_to < seg_from) continue;
      if (seg_to == seg_from && from != to) {
        // 空行/行尾的零宽区段：给一个窄标记，让“选到了换行”可见。
        const math::Point at = index_to_point(context, seg_from);
        if (at.y + band_h < box.y || at.y > box.bottom()) continue;
        canvas.fill_rect(math::Rect{at.x, at.y, std::max(k_cursor_width, 2.0f), band_h},
                         raster::Paint::solid(colors.selection));
        continue;
      }
      if (seg_to == seg_from) continue;
      const math::Point head = index_to_point(context, seg_from);
      const math::Point tail = index_to_point(context, seg_to);
      if (head.y + band_h < box.y || head.y > box.bottom()) continue;
      const math::Rect band{head.x, head.y, std::max(tail.x - head.x, 1.0f), band_h};
      canvas.fill_rect(band, raster::Paint::solid(colors.selection));
    }
  }
  if (text_.empty() && !placeholder_.empty()) {
    port.draw(canvas, port.ellipsize(placeholder_, size, box.width),
              math::Point{box.x, box.y}, size, colors.text_faint);
  } else {
    const float first = std::max(0.0f, std::floor(scroll_ / line));
    const auto first_row = static_cast<std::size_t>(first);
    // 显式文字色覆盖（DSL `BoxProps::color/hex_color`）：与 Input 同口径
    //（TextArea 不覆写 apply_theme，基类已回放进 style_，这里读回）。
    math::Color text_color = usable ? colors.text : colors.text_faint;
    if (const auto& override_color = text_color_override(); override_color.has_value()) {
      text_color = *override_color;
    }
    for (std::size_t row = first_row; row < lines.size(); ++row) {
      const float y = box.y + static_cast<float>(row) * line - scroll_;
      if (y > box.bottom()) break;
      const Line& entry = lines[row];
      if (entry.end <= entry.begin) continue;
      const std::string_view content = whole.substr(entry.begin, entry.end - entry.begin);
      port.draw(canvas, content, math::Point{box.x, y}, size, text_color);
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
  // **所有文本插入的唯一入口**：只读模式忽略，有选区则**替换**。
  if (read_only_) return;
  replace_selection_with(inserted);
}

void TextArea::replace_selection_with(std::string_view inserted) {
  if (read_only_) return;
  if (has_selection()) {
    const auto [from, to] = ordered_selection();
    text_.erase(from, to - from);
    cursor_ = from;
    clear_selection();
  }
  text_.insert(cursor_, inserted);
  cursor_ += inserted.size();
  notify_change();
}

auto TextArea::ordered_selection() const -> std::pair<std::size_t, std::size_t> {
  if (select_anchor_ == kNoSelection) return {cursor_, cursor_};
  const std::size_t a = std::min(select_anchor_, cursor_);
  const std::size_t b = std::max(select_anchor_, cursor_);
  return {std::min(a, text_.size()), std::min(b, text_.size())};
}

auto TextArea::has_selection() const -> bool {
  const auto [from, to] = ordered_selection();
  return to > from;
}

auto TextArea::selection() const -> std::pair<std::size_t, std::size_t> {
  if (!has_selection()) return {cursor_, cursor_};
  return ordered_selection();
}

auto TextArea::selected_text() const -> std::string {
  if (!has_selection()) return {};
  const auto [from, to] = ordered_selection();
  return text_.substr(from, to - from);
}

void TextArea::set_selection(std::size_t anchor, std::size_t focus) {
  select_anchor_ = snap_index(text_, anchor);
  cursor_ = snap_index(text_, focus);
  mark_dirty();
}

auto TextArea::erase_selection() -> bool {
  if (!has_selection()) return false;
  const auto [from, to] = ordered_selection();
  text_.erase(from, to - from);
  cursor_ = from;
  clear_selection();
  notify_change();
  return true;
}

void TextArea::erase_backward() {
  // 有选区时 Backspace 删选区（通行行为），而不是再向前删一个字符。
  if (erase_selection()) return;
  if (cursor_ == 0) return;
  const std::size_t start = utf8_prev(text_, cursor_);
  text_.erase(start, cursor_ - start);
  cursor_ = start;
  notify_change();
}

void TextArea::erase_forward() {
  if (erase_selection()) return;
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
    case EventKind::MouseDown: {
      if (event.button != 1) return false;
      const std::size_t hit = index_at_point(context, event.position);
      // 连击判定（双击选词 / 三击选当前行），时间取自 `RenderContext`。
      const std::int64_t now_ms = static_cast<std::int64_t>(context.time_seconds * 1000.0);
      const bool near = std::abs(event.position.x - last_click_x_) < 6.0f;
      const bool chained = last_click_ms_ >= 0 && (now_ms - last_click_ms_) < 500 && near;
      click_streak_ = chained ? std::min(click_streak_ + 1, 3) : 1;
      last_click_ms_ = now_ms;
      last_click_x_ = event.position.x;

      if (click_streak_ == 2) {
        // 双击选词（词界由公共算法给；`\n` 不是词字符，所以不会越行）。
        const auto [begin, end] = components_internal::word_range_at(text_, hit);
        select_anchor_ = begin;
        cursor_ = end;
        selecting_ = false;
        sync_scroll(context);
        mark_dirty();
        return true;
      }
      if (click_streak_ == 3) {
        // 三击选**当前行**（多行编辑框的通行行为：选一段而不是整篇）。
        const std::vector<Line> lines = layout_lines(context, inner_box(context).width);
        const std::size_t row = row_of(lines, hit);
        if (row < lines.size()) {
          select_anchor_ = lines[row].begin;
          cursor_ = lines[row].end;
        }
        selecting_ = false;
        sync_scroll(context);
        mark_dirty();
        return true;
      }
      // Shift+点击：扩展已有选区；否则重新开始拖选。
      if (event.shift && select_anchor_ != kNoSelection) {
        cursor_ = hit;
        selecting_ = false;
      } else {
        cursor_ = hit;
        select_anchor_ = hit;
        selecting_ = true;
      }
      sync_scroll(context);
      mark_dirty();
      return true;
    }
    case EventKind::MouseMove: {
      if (!selecting_) return false;
      cursor_ = index_at_point(context, event.position);
      mark_dirty();
      return true;
    }
    case EventKind::MouseUp:
      selecting_ = false;
      return true;
    case EventKind::Click:
      return true;   // 与 MouseDown 不重复连击计数
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
      const bool ctrl = event.ctrl || event.meta;
      const bool shift = event.shift;
      // Ctrl+C：有选区 → 拷选区；无选区 → 拷全文（只读展示也能复制）。
      if (ctrl && (key == "c" || key == "C")) {
        return invoke_action("copy", {});
      }
      // Ctrl+X / Ctrl+A：与 `Input` 同一组语义（只读下不剪切）。
      if (ctrl && (key == "x" || key == "X")) {
        if (read_only_ || !has_selection()) return true;
        if (host() != nullptr) {
          const std::string picked = selected_text();
          if (!picked.empty()) (void)host()->host_set_clipboard_text(picked);
        }
        (void)erase_selection();
        return true;
      }
      if (ctrl && (key == "a" || key == "A")) {
        select_anchor_ = 0U;
        cursor_ = text_.size();
        sync_scroll(context);
        mark_dirty();
        return true;
      }
      // 编辑类按键在只读下**消费但不落字**（消费=不让它冒泡出去触发页面级快捷键；
      // 不落字=真的没改内容）。导航类按键（方向/Home/End/PageUp/PageDown）则照常。
      if (read_only_) {
        if (key == "Enter" || key == "Backspace" || key == "Delete") return true;
      }
      // Shift = 扩选（不动锚点，只移光标）；无 Shift 的移动收起选区。
      const auto extend = [this, shift]() {
        if (shift && select_anchor_ == kNoSelection) select_anchor_ = cursor_;
        if (!shift) select_anchor_ = kNoSelection;
      };
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
        if (has_selection() && !shift) {
          cursor_ = ordered_selection().first;
          select_anchor_ = kNoSelection;
        } else {
          extend();
          cursor_ = cursor_ > 0 ? utf8_prev(text_, cursor_) : 0;
        }
        sync_scroll(context);
        mark_dirty();
        return true;
      }
      if (key == "ArrowRight") {
        if (has_selection() && !shift) {
          cursor_ = ordered_selection().second;
          select_anchor_ = kNoSelection;
        } else {
          extend();
          cursor_ = std::min(utf8_next(text_, cursor_), text_.size());
        }
        sync_scroll(context);
        mark_dirty();
        return true;
      }
      if (key == "ArrowUp") {
        extend();
        move_vertical(context, -1);
        return true;
      }
      if (key == "ArrowDown") {
        extend();
        move_vertical(context, 1);
        return true;
      }
      if (key == "Home") {
        extend();
        move_to_line_edge(context, false);
        return true;
      }
      if (key == "End") {
        extend();
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
  if (name == "scroll") return std::format("{:.1f}", static_cast<double>(scroll_));
  if (name == "read_only") return read_only_ ? "true" : "false";
  if (name == "selection") return selected_text();
  return std::nullopt;
}

auto TextArea::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value" || name == "text") {
    // 属性面写入**通知** `on_change`（与 Input/CodeEditor 同一决议）：宿主把
    // `on_change` 当"文本变了"的唯一钩子，静默改会出现
    // "`get` 读回新文本、宿主状态还是旧的"。C++ API `set_text` 仍静默
    //（程序化写入不冒充用户编辑——2026-10-12 补齐，当年修 Input 时漏了本组件）。
    const std::string before = text_;
    set_text(std::string(value));
    if (text_ != before && on_change) on_change(text_);
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
  if (name == "scroll") {
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
  return {"value", "text", "placeholder", "cursor_index", "scroll", "read_only"};
}

auto TextArea::readonly_property_names() const -> std::vector<std::string_view> {
  return {"selection"};
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
  // —— 文本选择（2026-10-14）与 `Input` 同一组动作名 ——
  if (action == "select_all") {
    select_anchor_ = 0U;
    cursor_ = text_.size();
    mark_dirty();
    return true;
  }
  if (action == "clear_selection") {
    clear_selection();
    mark_dirty();
    return true;
  }
  if (action == "copy") {
    // 有选区 → 拷选区；无选区 → 拷全文（兼容既有调用方）。
    // 只读不拦：只读展示恰恰是复制的商频场景。
    if (host() == nullptr) return false;
    const std::string text = has_selection() ? selected_text() : text_;
    if (text.empty()) return false;
    return host()->host_set_clipboard_text(text);
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
