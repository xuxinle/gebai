#include "st/ui/element.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace st::ui {
namespace {

[[nodiscard]] auto text_port_or_null(const RenderContext& context) -> const TextPort& {
  static const NullTextPort fallback;
  return context.text != nullptr ? *context.text : fallback;
}

[[nodiscard]] auto main_size(FlexDirection direction, math::Size size) noexcept -> float {
  return direction == FlexDirection::Row ? size.width : size.height;
}

[[nodiscard]] auto cross_size(FlexDirection direction, math::Size size) noexcept -> float {
  return direction == FlexDirection::Row ? size.height : size.width;
}

}  // namespace

// —— NullTextPort ——

auto NullTextPort::instance() -> const NullTextPort& {
  static const NullTextPort port;
  return port;
}

auto NullTextPort::measure(std::string_view utf8, float size) const -> math::Size {
  (void)utf8;
  return math::Size{0.0f, size * 1.45f};
}

auto NullTextPort::measure_width(std::string_view utf8, float size) const -> float {
  (void)utf8;
  (void)size;
  return 0.0f;
}

auto NullTextPort::line_height(float size) const -> float { return size * 1.45f; }

void NullTextPort::draw(raster::Canvas& canvas, std::string_view utf8, math::Point origin, float size,
                        math::Color color) const {
  (void)canvas;
  (void)utf8;
  (void)origin;
  (void)size;
  (void)color;
}

auto NullTextPort::ellipsize(std::string_view utf8, float size, float max_width) const
    -> std::string {
  (void)size;
  (void)max_width;
  return std::string(utf8);
}

auto NullTextPort::wrap(std::string_view utf8, float size, float max_width) const
    -> std::vector<std::string_view> {
  (void)size;
  (void)max_width;
  return {utf8};
}

auto NullTextPort::wrap_limited(std::string_view utf8, float size, float max_width,
                                std::size_t max_lines) const -> std::vector<std::string> {
  (void)size;
  (void)max_width;
  (void)max_lines;
  return {std::string(utf8)};
}

// —— Element ——

Element::Element() = default;
Element::~Element() = default;

auto Element::child_at(std::size_t index) const noexcept -> Element* {
  return index < children_.size() ? children_[index].get() : nullptr;
}

auto Element::derived_id() const -> ElementId {
  if (!id_.empty()) return id_;
  // **必须是拥有型容器**：下面 `std::format(...)` 产生的是临时 `std::string`，
  // 若存成 `std::string_view` 会立刻悬垂，拼出来的 id 里就会出现垃圾字节
  // （实测：列表项 id 变成 `task-list/task-list/\x00`，且相邻项 id 相同——
  //   选择器/协议/脚本全都拿不到正确的元素）。
  std::vector<std::string> parts;
  const Element* current = this;
  std::size_t depth = 0;
  while (current != nullptr && depth < 64) {
    if (!current->id_.empty()) {
      parts.push_back(current->id_);
      break;
    }
    const Element* parent = current->parent_;
    if (parent == nullptr) {
      parts.push_back("root");
      break;
    }
    const auto& siblings = parent->children_;
    std::size_t index = 0;
    for (std::size_t position = 0; position < siblings.size(); ++position) {
      if (siblings[position].get() == current) {
        index = position;
        break;
      }
    }
    parts.push_back(std::format("{}[{}]", current->type(), index));
    current = parent;
    ++depth;
  }
  std::string out;
  for (auto iterator = parts.rbegin(); iterator != parts.rend(); ++iterator) {
    if (!out.empty()) out.push_back('/');
    out.append(*iterator);
  }
  return out;
}

auto Element::add_child(std::unique_ptr<Element> child) -> Element* {
  if (child == nullptr) return nullptr;
  child->parent_ = this;
  Element* raw = child.get();
  children_.push_back(std::move(child));
  mark_layout_dirty();
  return raw;
}

auto Element::insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element* {
  if (child == nullptr) return nullptr;
  child->parent_ = this;
  Element* raw = child.get();
  const std::size_t position = index > children_.size() ? children_.size() : index;
  children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(position), std::move(child));
  mark_layout_dirty();
  return raw;
}

auto Element::remove_child(Element* child) -> std::unique_ptr<Element> {
  for (auto iterator = children_.begin(); iterator != children_.end(); ++iterator) {
    if (iterator->get() == child) {
      std::unique_ptr<Element> detached = std::move(*iterator);
      detached->parent_ = nullptr;
      children_.erase(iterator);
      mark_layout_dirty();
      return detached;
    }
  }
  return nullptr;
}

void Element::clear_children() {
  for (auto& child : children_) child->parent_ = nullptr;
  children_.clear();
  mark_layout_dirty();
}

auto Element::hit_test(math::Point point) const noexcept -> bool {
  if (!visible_) return false;
  return bounds_.contains(point);
}

auto Element::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags;
  flags.visible = visible_;
  flags.enabled = enabled_;
  flags.focused = focused_;
  flags.hovered = hovered_;
  flags.pressed = pressed_;
  return flags;
}

void Element::collect_semantics(SemanticsNode& node) const {
  node.id = derived_id();
  node.type = std::string(type());
  node.role = role();
  node.bounds = bounds_;
  node.text = semantics_text();
  node.value = semantics_value();
  node.flags = semantics_flags();
  for (const auto& child : children_) {
    if (!child->visible_) continue;
    SemanticsNode child_node;
    child->collect_semantics(child_node);
    node.children.push_back(std::move(child_node));
  }
}

void Element::collect_visual(VisualNode& node) const {
  node.id = derived_id();
  node.type = std::string(type());
  node.bounds = bounds_;
  node.visible = visible_;
  node.opacity = style_.opacity;
  node.fill = style_.background.a != 0U ? style_.background.to_css() : std::string{};
  node.radius = style_.radius;
  node.text = semantics_text();
  node.hit_target = true;
  for (const auto& child : children_) {
    if (!child->visible_) continue;
    VisualNode child_node;
    child_node.depth = node.depth + 1;
    child->collect_visual(child_node);
    node.children.push_back(std::move(child_node));
  }
}

auto Element::get_property(std::string_view name) const -> std::optional<std::string> {
  (void)name;
  return std::nullopt;
}

auto Element::set_property(std::string_view name, std::string_view value) -> bool {
  (void)name;
  (void)value;
  return false;
}

auto Element::property_names() const -> std::vector<std::string_view> { return {}; }

auto Element::invoke_action(std::string_view action, std::string_view argument) -> bool {
  (void)argument;
  if (action == "click" || action == "activate") {
    activate();
    return true;
  }
  if (action == "focus") {
    set_focused(true);
    return true;
  }
  if (action == "blur") {
    set_focused(false);
    return true;
  }
  return false;
}

void Element::mark_dirty() {
  dirty_ = true;
  for (Element* current = parent_; current != nullptr; current = current->parent_) {
    current->dirty_ = true;
  }
}

void Element::mark_layout_dirty() {
  layout_dirty_ = true;
  dirty_ = true;
  for (Element* current = parent_; current != nullptr; current = current->parent_) {
    current->layout_dirty_ = true;
    current->dirty_ = true;
  }
}

void Element::clear_dirty() noexcept {
  dirty_ = false;
  layout_dirty_ = false;
}

auto Element::content_box() const noexcept -> math::Rect { return bounds_.inset(style_.padding); }

void Element::measure(const RenderContext& context, const Constraints& constraints) {
  const float horizontal_padding = style_.padding.horizontal();
  const float vertical_padding = style_.padding.vertical();
  const float available_width =
      std::max(0.0f, (style_.has_explicit_width() ? style_.width : constraints.max_width) -
                         horizontal_padding);
  const float available_height =
      style_.has_explicit_height() ? style_.height - vertical_padding : constraints.max_height;

  const bool row = style_.direction == FlexDirection::Row;
  float main_total = 0.0f;
  float cross_max = 0.0f;
  std::size_t visible_children = 0;

  for (auto& child : children_) {
    if (!child->visible_) continue;
    Constraints child_constraints;
    child_constraints.max_width = row ? kUnbounded : available_width;
    child_constraints.max_height = row ? available_height : kUnbounded;
    child_constraints.available_width = available_width;
    child_constraints.available_height = available_height;
    child->measure(context, child_constraints);
    const math::Size child_size = child->measured_size();
    main_total += main_size(style_.direction, child_size);
    cross_max = std::max(cross_max, cross_size(style_.direction, child_size));
    ++visible_children;
  }
  if (visible_children > 1) main_total += style_.gap * static_cast<float>(visible_children - 1);

  float width = 0.0f;
  float height = 0.0f;
  if (row) {
    width = main_total;
    height = cross_max;
  } else {
    width = cross_max;
    height = main_total;
  }

  if (style_.has_explicit_width()) {
    width = style_.width;
  } else {
    width += horizontal_padding;
  }
  if (style_.has_explicit_height()) {
    height = style_.height;
  } else {
    height += vertical_padding;
  }

  width = std::clamp(width, style_.min_width, style_.max_width);
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Element::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  layout_children(context, content_box());
  layout_dirty_ = false;
}

void Element::layout_children(const RenderContext& context, math::Rect content) {
  const bool row = style_.direction == FlexDirection::Row;
  std::vector<Element*> visible;
  visible.reserve(children_.size());
  for (auto& child : children_) {
    if (child->visible_) visible.push_back(child.get());
  }
  if (visible.empty()) return;

  const auto count = static_cast<float>(visible.size());
  const float total_gap = style_.gap * (count - 1.0f);
  const float main_available = row ? content.width : content.height;
  const float cross_available = row ? content.height : content.width;

  float fixed_main = 0.0f;
  float grow_weight = 0.0f;
  for (const auto* child : visible) {
    const auto& child_style = child->style();
    const float size = main_size(style_.direction, child->measured_size());
    const float explicit_size = row ? child_style.width : child_style.height;
    if (child_style.grow) {
      grow_weight += 1.0f;
      continue;
    }
    fixed_main += explicit_size == kAuto ? size : explicit_size;
  }

  float leftover = main_available - total_gap - fixed_main;
  if (leftover < 0.0f) leftover = 0.0f;
  const float grow_unit = grow_weight > 0.0f ? leftover / grow_weight : 0.0f;

  float cursor = row ? content.x : content.y;
  float used = total_gap + fixed_main;
  for (auto* child : visible) {
    const auto& child_style = child->style();
    float main_extent = main_size(style_.direction, child->measured_size());
    const float explicit_main = row ? child_style.width : child_style.height;
    if (explicit_main != kAuto) main_extent = explicit_main;
    if (child_style.grow) main_extent = grow_unit;

    float cross_extent = cross_size(style_.direction, child->measured_size());
    const float explicit_cross = row ? child_style.height : child_style.width;
    if (explicit_cross != kAuto) cross_extent = explicit_cross;

    const Align align = child_style.align_self != Align::Stretch ? child_style.align_self
                                                                 : style_.align_items;
    float cross_offset = 0.0f;
    if (align == Align::Stretch) {
      cross_extent = cross_available;
    } else if (align == Align::Center) {
      cross_offset = (cross_available - cross_extent) * 0.5f;
    } else if (align == Align::End) {
      cross_offset = cross_available - cross_extent;
    }

    math::Rect target;
    if (row) {
      target = math::Rect{cursor, content.y + cross_offset, main_extent, cross_extent};
    } else {
      target = math::Rect{content.x + cross_offset, cursor, cross_extent, main_extent};
    }
    target = target.inset(child_style.margin);
    child->arrange(context, target);

    cursor += main_extent + style_.gap;
    used += main_extent;
  }

  // 主轴剩余空间分配（无 grow 子节点时按 justify 处理）
  if (grow_weight == 0.0f && style_.justify != Justify::Start) {
    const float slack = main_available - used;
    if (slack > 0.0f) {
      float offset = 0.0f;
      float extra_gap = 0.0f;
      const std::size_t gaps = visible.size() > 1 ? visible.size() - 1 : 0;
      switch (style_.justify) {
        case Justify::Center: offset = slack * 0.5f; break;
        case Justify::End: offset = slack; break;
        case Justify::SpaceBetween:
          extra_gap = gaps > 0 ? slack / static_cast<float>(gaps) : 0.0f;
          break;
        case Justify::SpaceAround:
          extra_gap = slack / count;
          offset = extra_gap * 0.5f;
          break;
        case Justify::Start: break;
      }
      float shift = row ? content.x : content.y;
      shift += offset;
      for (auto* child : visible) {
        math::Rect moved = child->bounds();
        if (row) {
          moved.x += shift - (row ? content.x : content.y);
        } else {
          moved.y += shift - (row ? content.x : content.y);
        }
        child->arrange(context, moved);
        shift += (row ? moved.width : moved.height) + style_.gap + extra_gap;
      }
    }
  }
}

void Element::paint_box(const RenderContext& context, raster::Canvas& canvas) const {
  if (bounds_.is_empty()) return;
  const auto& colors = context.theme.colors();
  (void)colors;
  if (style_.shadow.visible()) {
    canvas.draw_shadow(bounds_.inset(style_.margin), style_.radius, style_.shadow.blur,
                       style_.shadow.color,
                       math::Point{style_.shadow.offset_x, style_.shadow.offset_y},
                       raster::DrawOptions{.opacity = style_.opacity});
  }
  if (style_.background.a != 0U) {
    canvas.fill_rect(bounds_, raster::Paint::solid(style_.background), style_.radius,
                     raster::DrawOptions{.opacity = style_.opacity});
  }
  if (style_.border_width > 0.0f && style_.border_color.a != 0U) {
    raster::Path outline;
    const float half = style_.border_width * 0.5f;
    outline.add_rounded_rect(bounds_.inset(math::Insets::all(half)),
                             style_.radius > half ? style_.radius - half : 0.0f);
    canvas.stroke_path(outline, raster::Paint::solid(style_.border_color), style_.border_width,
                       raster::DrawOptions{.opacity = style_.opacity});
  }
}

auto Element::paint_text(const RenderContext& context, raster::Canvas& canvas, std::string_view text,
                         math::Rect box) const -> void {
  if (text.empty()) return;
  const TextPort& port = text_port_or_null(context);
  const float size = style_.font_size;
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float width = port.measure_width(clipped, size);
  float x = box.x;
  if (style_.text_align == TextAlign::Center) {
    x = box.x + (box.width - width) * 0.5f;
  } else if (style_.text_align == TextAlign::End) {
    x = box.right() - width;
  }
  const float height = port.line_height(size);
  const float y = box.y + (box.height - height) * 0.5f;
  port.draw(canvas, clipped, math::Point{x, y}, size, style_.color);
}

void Element::paint(const RenderContext& context, raster::Canvas& canvas) const {
  if (!visible_) return;
  paint_box(context, canvas);
  paint_content(context, canvas);
  if (style_.clip_children) {
    canvas.push_clip_rounded_rect(bounds_, style_.radius);
    for (const auto& child : children_) child->paint(context, canvas);
    canvas.pop_clip();
    return;
  }
  for (const auto& child : children_) child->paint(context, canvas);
}

auto Element::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  (void)event;
  return false;
}

// —— Panel / Spacer ——

Panel::Panel(FlexDirection direction) {
  style_.direction = direction;
  style_.background = math::Color{0, 0, 0, 0};
}

Spacer::Spacer(float size) {
  style_.width = size;
  style_.height = size;
  style_.grow = false;
}

}  // namespace st::ui
