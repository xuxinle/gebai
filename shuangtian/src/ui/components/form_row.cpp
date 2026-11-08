#include "st/ui/components/form_row.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

namespace {

/// 必填星号的宽（含一个额外间隔）。
constexpr float kAsteriskWidth = 8.0f;

}  // namespace

FormRow::FormRow() = default;

FormRow::FormRow(std::string label) : label_(std::move(label)) {}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void FormRow::set_label(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  mark_layout_dirty();
}

void FormRow::set_hint(std::string hint) {
  if (hint_ == hint) return;
  hint_ = std::move(hint);
  mark_layout_dirty();
}

void FormRow::set_error(std::string error) {
  if (error_ == error) return;
  error_ = std::move(error);
  mark_layout_dirty();
}

void FormRow::set_required(bool required) {
  if (required_ == required) return;
  required_ = required;
  mark_layout_dirty();
}

void FormRow::set_label_width(float width) {
  const float clamped = width > 0.0f ? width : 0.0f;
  if (label_width_ == clamped) return;
  label_width_ = clamped;
  mark_layout_dirty();
}

void FormRow::set_stacked(bool stacked) {
  if (stacked_ == stacked) return;
  stacked_ = stacked;
  mark_layout_dirty();
}

auto FormRow::add_child(std::unique_ptr<Element> child) -> Element* {
  // 表单行**只承载一个控件**。挂第二个会与标签列抢空间，且语义上「一行一个字段」。
  // 拒绝而不是静默替换：静默替换会让调用方持有的旧指针悬垂。
  if (child_count() > 0U) return nullptr;
  return Element::add_child(std::move(child));
}

auto FormRow::control() const noexcept -> Element* {
  for (std::size_t index = 0; index < child_count(); ++index) {
    if (Element* child = child_at(index); child != nullptr && !child->visible()) continue;
    return child_at(index);
  }
  return child_count() > 0U ? child_at(0) : nullptr;
}

auto FormRow::message_text() const -> const std::string& { return error_.empty() ? hint_ : error_; }

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void FormRow::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  label_font_ = metrics.font_sm;
  message_font_ = metrics.font_xs;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = label_font_;
  style_.color = theme.colors().text;
  apply_text_overrides();
  label_font_ = style_.font_size;
}

void FormRow::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const std::string& message = message_text();

  // 标签列宽：显式给值优先；否则按标签文本量（含必填星号）。
  const float label_text_width = label_.empty()
                                     ? 0.0f
                                     : port.measure_width(label_, label_font_) +
                                           (required_ ? kAsteriskWidth : 0.0f);
  effective_label_ = label_width_ > 0.0f ? label_width_ : std::max(label_text_width, 0.0f);
  if (!label_.empty() && effective_label_ <= 0.0f) effective_label_ = kDefaultLabelWidth;

  const float full_width =
      constraints.max_width < kUnbounded
          ? constraints.max_width
          : (style_.has_explicit_width() ? style_.width : 360.0f);
  const float total = style_.has_explicit_width() ? style_.width : full_width;

  // 控件可用宽。
  const float control_width =
      stacked_ || label_.empty()
          ? total
          : std::max(total - effective_label_ - kLabelGap, 40.0f);

  float control_height = 30.0f;
  if (Element* child = control(); child != nullptr) {
    Constraints inner{};
    inner.max_width = control_width;
    inner.available_width = control_width;
    inner.max_height = constraints.max_height;
    inner.available_height = constraints.max_height;
    child->measure(context, inner);
    control_height = child->measured_size().height;
  }

  const float message_height =
      message.empty() ? 0.0f : port.line_height(message_font_);
  float height = 0.0f;
  if (stacked_) {
    if (!label_.empty()) height += port.line_height(label_font_) + kHintGap;
    height += control_height;
  } else {
    height = control_height;
  }
  if (message_height > 0.0f) height += message_height + kHintGap;

  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  const float width = std::clamp(total, style_.min_width, style_.max_width);
  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void FormRow::relayout(const RenderContext& context, math::Rect rect) const {
  const TextPort& port = text_port_of(context);
  label_box_ = math::Rect{};
  control_box_ = math::Rect{};
  message_box_ = math::Rect{};
  if (rect.is_empty()) return;

  const Element* child = child_count() > 0U ? child_at(0) : nullptr;
  const float control_height = child != nullptr ? child->measured_size().height : 30.0f;
  const std::string& message = message_text();
  const float message_height = message.empty() ? 0.0f : port.line_height(message_font_);

  float y = rect.y;
  if (stacked_) {
    if (!label_.empty()) {
      const float label_height = port.line_height(label_font_);
      label_box_ = math::Rect{rect.x, y, rect.width, label_height};
      y += label_height + kHintGap;
    }
    control_box_ = math::Rect{rect.x, y, rect.width, control_height};
    y += control_height;
  } else {
    if (!label_.empty()) {
      // 标签**垂直居中于控件的首行**，而不是整行盒：多行控件（TextArea）
      // 下若按行盒居中，标签会漂到中间——表单里标签必须贴着它的第一行。
      const float label_height = port.line_height(label_font_);
      const float first_line_center = rect.y + std::min(label_height, control_height) * 0.5f;
      label_box_ =
          math::Rect{rect.x, first_line_center - label_height * 0.5f, effective_label_, label_height};
      control_box_ = math::Rect{rect.x + effective_label_ + kLabelGap, rect.y,
                                std::max(rect.width - effective_label_ - kLabelGap, 0.0f),
                                control_height};
    } else {
      control_box_ = math::Rect{rect.x, rect.y, rect.width, control_height};
    }
    y += control_height;
  }
  if (message_height > 0.0f) {
    y += kHintGap;
    message_box_ = math::Rect{control_box_.x, y, control_box_.width, message_height};
  }
}

void FormRow::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context, rect);
  // 子控件的落位要显式跟随（`Element::layout_children` 用的是 padding 内的整块，
  // 而表单行要把控件放到标签列右侧）。
  for (std::size_t index = 0; index < child_count(); ++index) {
    if (Element* child = child_at(index); child != nullptr) {
      child->arrange(context, control_box_);
    }
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void FormRow::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout(context, bounds_);
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);

  if (!label_box_.is_empty() && !label_.empty()) {
    const std::string shown = port.ellipsize(label_, label_font_, label_box_.width -
                                                                    (required_ ? kAsteriskWidth : 0.0f));
    const float top =
        centered_line_top(port, shown, label_font_, label_box_.y, label_box_.height);
    port.draw(canvas, shown, math::Point{label_box_.x, top}, label_font_, colors.text_muted);
    // 必填星号：贴标签文本**右缘**（不是列右缘）——列宽对齐时星号跟着文字走。
    if (required_) {
      const float text_width = port.measure_width(shown, label_font_);
      port.draw(canvas, "*", math::Point{label_box_.x + text_width + 2.0f, top}, label_font_,
                colors.danger);
    }
  }

  const std::string& message = message_text();
  if (!message_box_.is_empty() && !message.empty()) {
    const bool is_error = !error_.empty();
    const std::string shown = port.ellipsize(message, message_font_, message_box_.width);
    const float top = centered_line_top(port, shown, message_font_, message_box_.y,
                                        message_box_.height);
    port.draw(canvas, shown, math::Point{message_box_.x, top}, message_font_,
              is_error ? colors.danger : colors.text_faint);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto FormRow::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "label" || name == "text") return label_;
  if (name == "hint") return hint_;
  if (name == "error") return error_;
  if (name == "required") return std::string(required_ ? kFlagTrue : "false");
  if (name == "label_width") return std::format("{}", label_width_);
  if (name == "stacked") return std::string(stacked_ ? kFlagTrue : "false");
  if (name == "effective_label_width") return std::format("{}", effective_label_);
  return std::nullopt;
}

auto FormRow::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "label" || name == "text") {
    set_label(std::string(value));
    return true;
  }
  if (name == "hint") {
    set_hint(std::string(value));
    return true;
  }
  if (name == "error") {
    set_error(std::string(value));
    return true;
  }
  if (name == "required") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_required(*parsed);
    return true;
  }
  if (name == "label_width") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_label_width(static_cast<float>(*parsed));
    return true;
  }
  if (name == "stacked") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_stacked(*parsed);
    return true;
  }
  return false;
}

auto FormRow::property_names() const -> std::vector<std::string_view> {
  return {"label", "hint", "error", "required", "label_width", "stacked"};
}

auto FormRow::readonly_property_names() const -> std::vector<std::string_view> {
  return {"effective_label_width"};
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::FormRow);
