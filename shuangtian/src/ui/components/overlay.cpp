#include "st/ui/components/overlay.hpp"

#include <algorithm>
#include <format>
#include <span>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

/// 文本端口取用（`RenderContext::text` 可为空 → 退化为 no-op 端口）。
[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  return context.text != nullptr ? *context.text : NullTextPort::instance();
}

[[nodiscard]] auto tone_name(Tone tone) noexcept -> std::string_view {
  switch (tone) {
    case Tone::Default: return "default";
    case Tone::Muted: return "muted";
    case Tone::Faint: return "faint";
    case Tone::Primary: return "primary";
    case Tone::Accent: return "accent";
    case Tone::Success: return "success";
    case Tone::Warning: return "warning";
    case Tone::Danger: return "danger";
    case Tone::OnPrimary: return "on_primary";
  }
  return "default";
}

[[nodiscard]] auto tone_from_name(std::string_view name) -> std::optional<Tone> {
  if (name == "default") return Tone::Default;
  if (name == "muted") return Tone::Muted;
  if (name == "faint") return Tone::Faint;
  if (name == "primary") return Tone::Primary;
  if (name == "accent") return Tone::Accent;
  if (name == "success") return Tone::Success;
  if (name == "warning") return Tone::Warning;
  if (name == "danger") return Tone::Danger;
  if (name == "on_primary") return Tone::OnPrimary;
  return std::nullopt;
}

/// 单行文本（省略 + 左对齐；标题不折行）。
void draw_line(const RenderContext& context, raster::Surface& canvas, std::string_view text,
               math::Rect box, float size, math::Color color) {
  if (text.empty() || box.width <= 0.0f || box.height <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float line = port.line_height(size);
  const float y = box.y + (box.height - line) * 0.5f;
  port.draw(canvas, clipped, math::Point{box.x, y}, size, color);
}

/// 正文最多折行数（超出以省略号收尾）。
inline constexpr std::size_t kMaxBodyLines = 12;

/// 覆盖率取向归一（raster 层现况）：`Canvas::blend_coverage_row` 只接受**正绕向**覆盖率
/// （`rasterize_mask` 用 `abs(coverage)`，二者语义不一致），而 `Path` 工厂
/// （`add_rect`/`add_rounded_rect`/`add_circle`）产出的是反向绕向——直接 `fill_path` 会整块
/// 不可见。这里把路径按扁平化折线反转重建，使填充在两个语义下都真实落地；
/// raster 层统一为 `abs` 语义后本函数退化为等价直通（可安全移除）。
[[nodiscard]] auto oriented(const raster::Path& path) -> raster::Path {
  raster::Path out;
  for (const raster::Polyline& polyline : path.flatten(0.25f)) {
    const std::span<const math::Point> points = polyline.points;
    if (points.size() < 2U) continue;
    out.move_to(points.back());
    for (std::size_t index = points.size() - 1U; index > 0U; --index) {
      out.line_to(points[index - 1U]);
    }
    if (polyline.closed) out.close();
  }
  return out;
}

/// 矩形/圆角矩形填充（四角独立半径；经 `oriented` 归一后落盘）。
void fill_round_rect(raster::Surface& canvas, math::Rect rect, float top_left, float top_right,
                     float bottom_right, float bottom_left, const raster::Paint& paint) {
  if (rect.is_empty()) return;
  raster::Path path;
  path.add_rounded_rect(rect, top_left, top_right, bottom_right, bottom_left);
  canvas.fill_path(oriented(path), paint);
}

/// 四角同半径的纯色填充（`radius == 0` 即普通矩形）。
void fill_round_rect(raster::Surface& canvas, math::Rect rect, float radius, math::Color color) {
  fill_round_rect(canvas, rect, radius, radius, radius, radius, raster::Paint::solid(color));
}

}  // namespace

// —— Dialog ——

Dialog::Dialog(std::string title, std::string body)
    : title_(std::move(title)), body_(std::move(body)) {
  style_.background = math::Color{0, 0, 0, 0};
  set_focusable(true);  // Esc 需要键盘焦点
}

void Dialog::set_title(std::string title) {
  if (title_ == title) return;
  title_ = std::move(title);
  mark_layout_dirty();
}

void Dialog::set_body(std::string body) {
  if (body_ == body) return;
  body_ = std::move(body);
  mark_layout_dirty();
}

void Dialog::set_actions(std::vector<std::string> actions) {
  actions_ = std::move(actions);
  buttons_.clear();
  clear_children();
  const std::size_t count = actions_.size();
  for (std::size_t index = 0; index < count; ++index) {
    const bool primary = index + 1U == count;  // 末项为主按钮
    auto button = std::make_unique<Button>(
        actions_[index], primary ? Button::Variant::Primary : Button::Variant::Secondary,
        Button::Size::Medium);
    button->on_click = [this, index]() {
      if (on_action) on_action(index);
    };
    buttons_.push_back(add_child(std::move(button)));
  }
  mark_layout_dirty();
}

void Dialog::set_viewport_rect(math::Rect rect) {
  viewport_ = rect;
  mark_layout_dirty();
}

auto Dialog::action_rect(std::size_t index) const noexcept -> math::Rect {
  if (index >= buttons_.size() || buttons_[index] == nullptr) return math::Rect{};
  return buttons_[index]->bounds();
}

void Dialog::dismiss() {
  if (on_dismiss) on_dismiss();
}

void Dialog::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.radius = metrics.radius_xl;
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_base;
  style_.padding = math::Insets{};
}

void Dialog::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float width = viewport_.width > 0.0f
                    ? viewport_.width
                    : (constraints.max_width < kUnbounded ? constraints.max_width : 640.0f);
  float height = viewport_.height > 0.0f
                     ? viewport_.height
                     : (constraints.max_height < kUnbounded ? constraints.max_height : 480.0f);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

auto Dialog::body_lines(const RenderContext& context, float width) const -> std::vector<std::string> {
  if (body_.empty() || width <= 0.0f) return {};
  const TextPort& port = text_port_of(context);
  return port.wrap_limited(body_, context.theme.metrics().font_base, width, kMaxBodyLines);
}

void Dialog::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);

  // 卡片宽：420~520（视口过窄时退让到视口内）。
  const float available_width = std::max(0.0f, rect.width - metrics.space_lg * 2.0f);
  float card_width = std::clamp(rect.width * 0.82f, kMinWidth, kMaxWidth);
  if (card_width > available_width) card_width = available_width;
  if (card_width <= 0.0f) card_width = rect.width;

  const float content_width = std::max(0.0f, card_width - kPadding * 2.0f);
  const float title_line = title_.empty() ? 0.0f : port.line_height(metrics.font_xl);
  const std::vector<std::string> lines = body_lines(context, content_width);
  const float body_line = port.line_height(metrics.font_base);
  const auto line_count = static_cast<float>(lines.size());
  const float body_height = body_line * line_count;
  const float actions_height = buttons_.empty() ? 0.0f : metrics.control_height;

  float card_height = kPadding * 2.0f + title_line + body_height + actions_height;
  if (!title_.empty() && !lines.empty()) card_height += metrics.space_md;
  if (!buttons_.empty() && (title_line > 0.0f || body_height > 0.0f)) {
    card_height += metrics.space_xl;
  }
  const float max_height = std::max(0.0f, rect.height - metrics.space_lg * 2.0f);
  card_height = std::min(card_height, max_height);
  card_ = math::Rect{rect.x + (rect.width - card_width) * 0.5f,
                     rect.y + (rect.height - card_height) * 0.5f, card_width, card_height};

  // 底部按钮行：右对齐、间距 space_sm（末项在最右）。
  const float button_y = card_.bottom() - kPadding - actions_height;
  float cursor = card_.right() - kPadding;
  for (std::size_t index = buttons_.size(); index > 0; --index) {
    Element* button = buttons_[index - 1U];
    if (button == nullptr) continue;
    const Constraints unbounded;
    button->measure(context, unbounded);
    const math::Size size = button->measured_size();
    const float x = cursor - size.width;
    button->arrange(context, math::Rect{x, button_y, size.width, size.height});
    cursor = x - metrics.space_sm;
  }
  layout_dirty_ = false;
}

void Dialog::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();

  // 遮罩（覆盖整个视口；半透明，底层内容仍可见）。
  canvas.fill_rect(bounds_, raster::Paint::solid(colors.overlay));
  if (card_.is_empty()) return;

  // 卡片：阴影 + surface 底 + radius_xl（底自绘，见 fill_round_rect 说明）。
  const Shadow shadow = shadow_lg(context.theme);
  canvas.draw_shadow(card_, metrics.radius_xl, shadow.blur, shadow.color,
                     math::Point{shadow.offset_x, shadow.offset_y});
  fill_round_rect(canvas, card_, metrics.radius_xl, colors.surface);

  const TextPort& port = text_port_of(context);
  const float content_width = std::max(0.0f, card_.width - kPadding * 2.0f);
  const float title_line = port.line_height(metrics.font_xl);
  const math::Rect title_box{card_.x + kPadding, card_.y + kPadding, content_width, title_line};
  // 注：TextPort 无字重概念，`font_xl` 的层级感由字号体现（权重语义在 style_ 中保留）。
  draw_line(context, canvas, title_, title_box, metrics.font_xl, colors.text);

  if (body_.empty()) return;
  const float body_line = port.line_height(metrics.font_base);
  float y = title_box.bottom() + metrics.space_md;
  const float limit = card_.bottom() - kPadding - (buttons_.empty() ? 0.0f : metrics.control_height) -
                      (buttons_.empty() ? 0.0f : metrics.space_xl);
  for (const std::string& line : body_lines(context, content_width)) {
    if (y + body_line > limit) break;
    port.draw(canvas, line, math::Point{card_.x + kPadding, y}, metrics.font_base, colors.text_muted);
    y += body_line;
  }
}

auto Dialog::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::KeyDown:
      if (event.key == "Escape" || event.key == "Esc") {
        event.handled = true;
        dismiss();
        return true;
      }
      return false;
    case EventKind::MouseDown:
    case EventKind::Click: {
      if (!card_.is_empty() && card_.contains(event.position)) {
        return true;  // 卡片内部：按钮已先行命中，其余区域不响应
      }
      event.handled = true;
      dismiss();
      return true;
    }
    case EventKind::MouseMove:
    case EventKind::Wheel:
      return true;  // 模态：吞掉指针与滚轮，避免穿透到底层内容
    default:
      return false;
  }
}

auto Dialog::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "title" || name == "text") return title_;
  if (name == "body" || name == "value") return body_;
  if (name == "actions") {
    std::string out;
    for (std::size_t index = 0; index < actions_.size(); ++index) {
      if (index > 0) out.push_back('|');
      out.append(actions_[index]);
    }
    return out;
  }
  if (name == "viewport") {
    return std::format("{},{},{},{}", viewport_.x, viewport_.y, viewport_.width, viewport_.height);
  }
  if (name == "card") {
    return std::format("{},{},{},{}", card_.x, card_.y, card_.width, card_.height);
  }
  return std::nullopt;
}

auto Dialog::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "title" || name == "text") {
    set_title(std::string(value));
    return true;
  }
  if (name == "body" || name == "value") {
    set_body(std::string(value));
    return true;
  }
  if (name == "actions") {
    std::vector<std::string> actions;
    if (value.empty()) {
      set_actions(std::move(actions));
      return true;
    }
    for (std::string_view part : st::split(value, '|')) actions.emplace_back(part);
    set_actions(std::move(actions));
    return true;
  }
  if (name == "viewport") {
    const std::vector<std::string_view> parts = st::split(value, ',');
    if (parts.size() != 4U) return false;
    float numbers[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (std::size_t index = 0; index < 4U; ++index) {
      const auto parsed = st::parse_f64(st::trim(parts[index]));
      if (!parsed.has_value()) return false;
      numbers[index] = static_cast<float>(*parsed);
    }
    set_viewport_rect(math::Rect{numbers[0], numbers[1], numbers[2], numbers[3]});
    return true;
  }
  return false;
}

auto Dialog::property_names() const -> std::vector<std::string_view> {
  return {"title", "body", "actions", "viewport", "card", "value"};
}

auto Dialog::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "dismiss" || action == "close") {
    dismiss();
    return true;
  }
  if (action == "action" || action == "invoke") {
    const auto parsed = st::parse_u64(argument);
    if (!parsed.has_value() || *parsed >= actions_.size()) return false;
    if (on_action) on_action(static_cast<std::size_t>(*parsed));
    return true;
  }
  return Element::invoke_action(action, argument);
}

// —— Toast ——

Toast::Toast(std::string message, Tone tone) : message_(std::move(message)), tone_(tone) {
  style_.background = math::Color{0, 0, 0, 0};
}

auto Toast::make(std::string message, Tone tone) -> std::unique_ptr<Toast> {
  return std::make_unique<Toast>(std::move(message), tone);
}

void Toast::set_message(std::string message) {
  if (message_ == message) return;
  message_ = std::move(message);
  mark_layout_dirty();
}

void Toast::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  mark_dirty();
}

void Toast::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};  // 卡片底自绘（见 fill_round_rect 说明）
  style_.color = theme.colors().text;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.radius = metrics.radius_md;
  style_.shadow = shadow_md(theme);
  style_.height = kHeight;
  style_.font_size = metrics.font_base;
  style_.text_align = TextAlign::Start;
  style_.padding = math::Insets{kAccentWidth + kPaddingX, 0.0f, kPaddingX, 0.0f};
}

void Toast::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float width = port.measure_width(message_, style_.font_size) + style_.padding.horizontal();
  width = std::max(width, kHeight * 2.0f);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Toast::arrange(const RenderContext& context, math::Rect rect) {
  // FillViewport 挂载（分到整视口）：底部居中（习惯的 Toast 位），其余留空不遮内容。
  // 判据：分到的高度远超自身量得高度（Stack 形态分到的高度恰好等于自身）。
  if (measured_.height > 0.0f && rect.height > measured_.height * 2.0f) {
    const float width = std::min(measured_.width, rect.width);
    const float x = rect.x + (rect.width - width) * 0.5f;
    const float y = rect.bottom() - measured_.height - kBottomMargin;
    Element::arrange(context, math::Rect{x, y, width, measured_.height});
    return;
  }
  // 居中：UiRoot 对叠加层的默认排布是顶部左对齐，轻提示习惯上水平居中。
  // 宽度用自己量得的（rect 宽可能被拉满可用宽），保持胶囊尺寸后居中。
  const float width = std::min(measured_.width, rect.width);
  const float x = rect.x + (rect.width - width) * 0.5f;
  Element::arrange(context, math::Rect{x, rect.y, width, rect.height});
}

void Toast::paint(const RenderContext& context, raster::Surface& canvas) const {
  // —— 自动消失推演（先于一切绘制：连阴影都不落盘） ——
  // 不额外起线程/定时器：到期时刻由帧时间轴（context.time_seconds）推演，
  // 到期即置位并调 on_dismiss（宿主负责在下一帧前经 overlay_remove 摘除；
  // 与 Select::flush_dismiss 同一套"延迟摘除"约定：绘制路径不能在 UiRoot 遍历 overlays 时改动容器）。
  // 放在 paint_content 里做不到"什么都不画"——基类 Element::paint 会先画盒子（阴影），
  // 到期那帧就会残留一块阴影。
  if (auto_dismiss_ms_ > 0.0 && !expired_) {
    const double now = context.time_seconds;
    if (dismiss_at_ < 0.0) {
      dismiss_at_ = now + auto_dismiss_ms_ / 1000.0;  // 首次绘制起算
      request_animation();
    } else if (now >= dismiss_at_) {
      expired_ = true;
      record_damage();  // 摘除后需要一帧把旧内容抹掉（mark_dirty 非 const；绘制路径用损坏区上报）
      if (on_dismiss) on_dismiss();
      return;  // 到期：整组件不绘制（含阴影）
    } else {
      request_animation();  // 计时中：续帧，到期那帧才会真的触发
    }
  }
  Element::paint(context, canvas);
}

void Toast::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Metrics& metrics = context.theme.metrics();
  // 卡片底（自绘，见 fill_round_rect 说明）+ 左侧 4px 色条（左侧圆角随卡片）。
  fill_round_rect(canvas, bounds_, metrics.radius_md, context.theme.colors().surface);
  const math::Rect bar{bounds_.x, bounds_.y, kAccentWidth, bounds_.height};
  fill_round_rect(canvas, bar, metrics.radius_md, 0.0f, 0.0f, metrics.radius_md,
                  raster::Paint::solid(tone_color(context.theme, tone_)));
  paint_text(context, canvas, message_, content_box());
}

void Toast::set_auto_dismiss_ms(double milliseconds) noexcept {
  if (milliseconds < 0.0) milliseconds = 0.0;
  if (milliseconds == auto_dismiss_ms_) return;
  auto_dismiss_ms_ = milliseconds;
  dismiss_at_ = -1.0;  // 重置起算：下一次绘制重新起表
  expired_ = false;
  mark_dirty();
}

auto Toast::remaining_ms() const noexcept -> double {
  // 只读推算：无"当前时间"输入，返回配置上限；精确剩余看 expired()/绘制时刻。
  if (auto_dismiss_ms_ <= 0.0 || expired_ || dismiss_at_ < 0.0) return 0.0;
  return auto_dismiss_ms_;
}

auto Toast::semantics_value() const -> std::string { return std::string(tone_name(tone_)); }

auto Toast::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "message" || name == "text" || name == "value") return message_;
  if (name == "tone") return std::string(tone_name(tone_));
  if (name == "auto_dismiss_ms") return std::format("{:.0f}", auto_dismiss_ms_);
  if (name == "expired") return expired_ ? "true" : "false";
  return std::nullopt;
}

auto Toast::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "message" || name == "text" || name == "value") {
    set_message(std::string(value));
    return true;
  }
  if (name == "tone") {
    const auto parsed = tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  if (name == "auto_dismiss_ms") {
    const auto parsed = st::parse_f64(value);
    if (!parsed.has_value() || *parsed < 0.0) return false;
    set_auto_dismiss_ms(*parsed);
    return true;
  }
  return false;
}

auto Toast::property_names() const -> std::vector<std::string_view> {
  return {"message", "text", "value", "tone", "auto_dismiss_ms", "expired"};
}

}  // namespace st::ui
