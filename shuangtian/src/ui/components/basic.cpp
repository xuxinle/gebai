#include "st/ui/components/basic.hpp"

#include <algorithm>
#include <format>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

[[nodiscard]] auto port_of(const RenderContext& context) -> const TextPort& {
  static const NullTextPort fallback;
  return context.text != nullptr ? *context.text : fallback;
}


}  // namespace

// —— Text ——

Text::Text(std::string content) : content_(std::move(content)) {}

void Text::set_content(std::string content) {
  if (content_ == content) return;
  content_ = std::move(content);
  mark_layout_dirty();
}

void Text::set_multiline(bool multiline) {
  if (multiline_ == multiline) return;
  multiline_ = multiline;
  mark_layout_dirty();
}

void Text::set_max_lines(std::size_t lines) {
  max_lines_ = lines;
  mark_layout_dirty();
}

void Text::set_tone(Tone tone) {
  tone_ = tone;
  mark_dirty();
}

void Text::set_font_size(float size) {
  font_size_override_ = size;
  mark_layout_dirty();
}

void Text::set_weight(FontWeight weight) {
  weight_ = weight;
  mark_dirty();
}

void Text::apply_theme(const Theme& theme) {
  style_.color = tone_color(theme, tone_);
  style_.font_size = font_size_override_ > 0.0f ? font_size_override_ : theme.metrics().font_base;
  style_.font_weight = weight_;
  style_.background = math::Color{0, 0, 0, 0};
}

void Text::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  const float line_height = port.line_height(size);
  if (multiline_) {
    const float available = constraints.max_width < kUnbounded ? constraints.max_width : 480.0f;
    std::vector<std::string> lines = max_lines_ > 0
                                         ? port.wrap_limited(content_, size, available, max_lines_)
                                         : std::vector<std::string>{};
    if (max_lines_ == 0) {
      for (const auto& line : port.wrap(content_, size, available)) {
        lines.emplace_back(line);
      }
    }
    float widest = 0.0f;
    for (const auto& line : lines) widest = std::max(widest, port.measure_width(line, size));
    measured_ = math::Size{std::min(widest, available),
                           line_height * static_cast<float>(lines.empty() ? 1 : lines.size())};
    return;
  }
  const float width = port.measure_width(content_, size);
  measured_ = math::Size{std::min(width, constraints.max_width), line_height};
}

void Text::paint_content(const RenderContext& context, raster::Canvas& canvas) const {
  if (content_.empty()) return;
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  if (!multiline_) {
    paint_text(context, canvas, content_, bounds_);
    return;
  }
  const float available = bounds_.width;
  std::vector<std::string> lines;
  if (max_lines_ > 0) {
    lines = port.wrap_limited(content_, size, available, max_lines_);
  } else {
    for (const auto& line : port.wrap(content_, size, available)) lines.emplace_back(line);
  }
  const float line_height = port.line_height(size);
  float y = bounds_.y;
  for (const auto& line : lines) {
    const float width = port.measure_width(line, size);
    float x = bounds_.x;
    if (style_.text_align == TextAlign::Center) {
      x = bounds_.x + (bounds_.width - width) * 0.5f;
    } else if (style_.text_align == TextAlign::End) {
      x = bounds_.right() - width;
    }
    port.draw(canvas, line, math::Point{x, y}, size, style_.color);
    y += line_height;
  }
}

// —— Heading ——

auto Text::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "text" || name == "content") return content_;
  if (name == "multiline") return multiline_ ? "true" : "false";
  return std::nullopt;
}

auto Text::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "text" || name == "content") {
    set_content(std::string(value));
    return true;
  }
  if (name == "multiline") {
    set_multiline(value == "true" || value == "1");
    return true;
  }
  return false;
}

auto Text::property_names() const -> std::vector<std::string_view> {
  return {"text", "content", "multiline"};
}

Heading::Heading(std::string content, std::uint32_t level) : Text(std::move(content)) {
  set_level(level);
}

void Heading::set_level(std::uint32_t level) {
  level_ = level < 1 ? 1 : (level > 6 ? 6 : level);
  mark_layout_dirty();
}

void Heading::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  float size = metrics.font_2xl;
  switch (level_) {
    case 1: size = metrics.font_3xl; break;
    case 2: size = metrics.font_2xl; break;
    case 3: size = metrics.font_xl; break;
    case 4: size = metrics.font_lg; break;
    default: size = metrics.font_base; break;
  }
  style_.font_size = size;
  style_.font_weight = level_ <= 2 ? FontWeight::Bold : FontWeight::SemiBold;
  style_.color = theme.colors().text;
  style_.background = math::Color{0, 0, 0, 0};
}

// —— IconView ——

IconView::IconView(std::string name, float size) : icon_(std::move(name)), size_(size) {}

void IconView::set_icon(std::string name) {
  icon_ = std::move(name);
  mark_dirty();
}

void IconView::set_size(float size) {
  size_ = size;
  mark_layout_dirty();
}

void IconView::set_tone(Tone tone) {
  tone_ = tone;
  mark_dirty();
}

void IconView::apply_theme(const Theme& theme) { style_.color = tone_color(theme, tone_); }

void IconView::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  (void)constraints;
  measured_ = math::Size{size_, size_};
}

void IconView::paint_content(const RenderContext& context, raster::Canvas& canvas) const {
  (void)context;
  if (icon_.empty()) return;
  Icon::draw(canvas, icon_, bounds_, style_.color, 0.0f);
}

// —— Button ——

auto IconView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "icon") return icon_;
  if (name == "size") return std::format("{}", size_);
  return std::nullopt;
}

auto IconView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "icon") {
    set_icon(std::string(value));
    return true;
  }
  if (name == "size") {
    if (const auto parsed = parse_f64(value); parsed.has_value()) {
      set_size(static_cast<float>(*parsed));
      return true;
    }
    return false;
  }
  return false;
}

Button::Button(std::string label, Variant variant, Size size)
    : label_(std::move(label)), variant_(variant), size_(size) {
  set_focusable(true);
}

void Button::set_label(std::string label) {
  label_ = std::move(label);
  mark_layout_dirty();
}

void Button::set_icon(std::string icon) {
  icon_ = std::move(icon);
  mark_layout_dirty();
}

void Button::set_variant(Variant variant) {
  variant_ = variant;
  mark_dirty();
}

void Button::set_size(Size size) {
  size_ = size;
  mark_layout_dirty();
}

void Button::set_tone(Tone tone) {
  tone_ = tone;
  mark_dirty();
}

void Button::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.radius = metrics.radius_md;
  style_.font_size = size_ == Size::Small ? metrics.font_sm
                                          : (size_ == Size::Large ? metrics.font_lg : metrics.font_base);
  style_.font_weight = FontWeight::Medium;
  style_.border_width = 0.0f;
  style_.align_items = Align::Center;
  style_.justify = Justify::Center;
  style_.direction = FlexDirection::Row;
  style_.text_align = TextAlign::Center;

  const bool inactive = !enabled_;
  switch (variant_) {
    case Variant::Primary:
      style_.background = inactive ? colors.primary_soft
                                   : (pressed_ ? colors.primary_active
                                               : (hovered_ ? colors.primary_hover : colors.primary));
      style_.color = inactive ? colors.primary : colors.on_primary;
      break;
    case Variant::Secondary:
      style_.background =
          pressed_ ? colors.surface_sunken : (hovered_ ? colors.surface_alt : colors.surface);
      style_.color = inactive ? colors.text_faint : colors.text;
      style_.border_width = metrics.border_width;
      style_.border_color = hovered_ ? colors.border_strong : colors.border;
      break;
    case Variant::Ghost:
      style_.background = pressed_ ? colors.surface_sunken
                                   : (hovered_ ? colors.surface_alt : math::Color{0, 0, 0, 0});
      style_.color = inactive ? colors.text_faint : colors.text_muted;
      break;
    case Variant::Soft:
      style_.background = pressed_ ? colors.primary_soft.darken(0.06f)
                                   : (hovered_ ? colors.primary_soft.lighten(0.04f)
                                               : colors.primary_soft);
      style_.color = inactive ? colors.text_faint : colors.primary;
      break;
    case Variant::Danger:
      style_.background = inactive ? colors.danger.with_alpha_f(0.2f)
                                   : (pressed_ ? colors.danger.darken(0.12f)
                                               : (hovered_ ? colors.danger.darken(0.06f)
                                                           : colors.danger));
      style_.color = colors.on_primary;
      break;
  }
  if (tone_ != Tone::Default) {
    if (tone_ == Tone::OnPrimary) {
      style_.color = colors.on_primary;
    } else if (variant_ == Variant::Ghost || variant_ == Variant::Secondary) {
      style_.color = tone_color(theme, tone_);
    }
  }
  if (style_.shadow.color.a == 0U && variant_ == Variant::Primary && !inactive) {
    // 主按钮的轻微投影提升层次
  }
}

void Button::measure(const RenderContext& context, const Constraints& constraints) {
  (void)constraints;
  const Metrics& metrics = context.theme.metrics();
  const float height = size_ == Size::Small ? metrics.control_height_sm
                                            : (size_ == Size::Large ? metrics.control_height_lg
                                                                    : metrics.control_height);
  const float horizontal_padding = size_ == Size::Small ? metrics.space_md : metrics.space_lg;
  const TextPort& port = port_of(context);
  float width = port.measure_width(label_, style_.font_size) + horizontal_padding * 2.0f;
  if (!icon_.empty()) width += style_.font_size + metrics.space_sm;
  measured_ = math::Size{std::max(width, height), height};
}

void Button::paint_content(const RenderContext& context, raster::Canvas& canvas) const {
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = port_of(context);
  const float font_size = style_.font_size;
  const float icon_size = font_size + 2.0f;
  const float text_width = port.measure_width(label_, font_size);
  const float gap = (!icon_.empty() && !label_.empty()) ? metrics.space_sm : 0.0f;
  const float total = text_width + (icon_.empty() ? 0.0f : icon_size + gap);
  float cursor = bounds_.x + (bounds_.width - total) * 0.5f;
  const float center_y = bounds_.center().y;

  if (!icon_.empty() && icon_leading) {
    Icon::draw(canvas, icon_, math::Rect{cursor, center_y - icon_size * 0.5f, icon_size, icon_size},
               style_.color);
    cursor += icon_size + gap;
  }
  if (!label_.empty()) {
    const float line_height = port.line_height(font_size);
    port.draw(canvas, label_, math::Point{cursor, center_y - line_height * 0.5f}, font_size,
              style_.color);
  }
  if (!icon_.empty() && !icon_leading) {
    Icon::draw(canvas, icon_, math::Rect{cursor, center_y - icon_size * 0.5f, icon_size, icon_size},
               style_.color);
  }
  if (focused_) {
    raster::Path ring;
    const float inset = 2.0f;
    ring.add_rounded_rect(bounds_.inflate(-inset), style_.radius > inset ? style_.radius - inset : 0.0f);
    canvas.stroke_path(ring, raster::Paint::solid(context.theme.colors().focus_ring),
                       metrics.focus_width);
  }
}

auto Button::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::HoverIn:
      mark_dirty();
      return false;
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseDown:
      mark_dirty();
      return true;
    case EventKind::Click:
    case EventKind::DoubleClick:
      mark_dirty();
      activate();
      return true;
    case EventKind::KeyDown: {
      if (event.key == "Enter" || event.key == " " || event.key == "Space") {
        activate();
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

void Button::activate() {
  if (!enabled_) return;
  if (on_click) on_click();
}

auto Button::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.pressed = pressed_;
  return flags;
}

auto Button::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "label" || name == "text") return label_;
  if (name == "icon") return icon_;
  return std::nullopt;
}

auto Button::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "label" || name == "text") {
    set_label(std::string(value));
    return true;
  }
  if (name == "icon") {
    set_icon(std::string(value));
    return true;
  }
  return false;
}

auto Button::property_names() const -> std::vector<std::string_view> {
  return {"label", "text", "icon"};
}

auto Button::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "click" || action == "activate") {
    activate();
    return true;
  }
  return Element::invoke_action(action, argument);
}

// —— Card ——

Card::Card(float padding) : padding_(padding) {
  style_.padding = math::Insets::all(padding);
}

void Card::set_padding(float padding) {
  padding_ = padding;
  style_.padding = math::Insets::all(padding);
  mark_layout_dirty();
}

void Card::set_radius(float radius) {
  radius_override_ = radius;
  mark_dirty();
}

void Card::set_shadow_level(std::uint8_t level) {
  shadow_level_ = level;
  mark_dirty();
}

void Card::set_elevated(bool elevated) {
  elevated_ = elevated;
  mark_dirty();
}

void Card::apply_theme(const Theme& theme) {
  style_.background = theme.colors().surface;
  style_.border_color = theme.colors().border;
  style_.border_width = theme.metrics().border_width;
  style_.radius = radius_override_ > 0.0f ? radius_override_ : theme.metrics().radius_lg;
  style_.padding = math::Insets::all(padding_);
  style_.shadow = shadow_level_ == 0 ? Shadow{} : (elevated_ ? shadow_lg(theme) : shadow_sm(theme));
}

// —— Divider ——

Divider::Divider(bool vertical) : vertical_(vertical) {}

void Divider::apply_theme(const Theme& theme) {
  style_.background = theme.colors().border;
  style_.height = vertical_ ? kAuto : theme.metrics().border_width;
  style_.width = vertical_ ? theme.metrics().border_width : kAuto;
}

void Divider::measure(const RenderContext& context, const Constraints& constraints) {
  if (vertical_) {
    measured_ = math::Size{context.theme.metrics().border_width, constraints.max_height};
    return;
  }
  measured_ = math::Size{constraints.max_width, context.theme.metrics().border_width};
}

void Divider::paint_content(const RenderContext& context, raster::Canvas& canvas) const {
  (void)context;
  canvas.fill_rect(bounds_, raster::Paint::solid(style_.background));
}

// —— KeyValueRow ——

KeyValueRow::KeyValueRow(std::string key, std::string value)
    : key_(std::move(key)), value_(std::move(value)) {
  style_.direction = FlexDirection::Row;
}

void KeyValueRow::set_value(std::string value) {
  value_ = std::move(value);
  mark_dirty();
}

void KeyValueRow::apply_theme(const Theme& theme) {
  style_.font_size = theme.metrics().font_sm;
  style_.background = math::Color{0, 0, 0, 0};
}

void KeyValueRow::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = port_of(context);
  const float line_height = port.line_height(style_.font_size);
  measured_ = math::Size{constraints.max_width, line_height + 6.0f};
}

void KeyValueRow::paint_content(const RenderContext& context, raster::Canvas& canvas) const {
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  const float line_height = port.line_height(size);
  const float y = bounds_.y + (bounds_.height - line_height) * 0.5f;
  port.draw(canvas, key_, math::Point{bounds_.x, y}, size, context.theme.colors().text_muted);
  const float value_width = port.measure_width(value_, size);
  port.draw(canvas, value_, math::Point{bounds_.right() - value_width, y}, size,
            context.theme.colors().text);
}

}  // namespace st::ui
