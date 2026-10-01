#include "st/ui/element.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/core/log.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace st::ui {
namespace {

[[nodiscard]] auto text_port_or_null(const RenderContext& context) -> const TextPort& {
  static const NullTextPort fallback;
  return context.text != nullptr ? *context.text : fallback;
}

/// 元素可能画到 `bounds_` 之外的**安全外扩量**（逻辑像素）。
///
/// 阴影的模糊与偏移、外发光、悬浮上浮都会溢出元素矩形；剔除时如果只按 `bounds_` 判断，
/// 这些行会被整块剔掉（表现为"卡片浮起来时边缘缺一块"这类难查的视觉回归）。
/// 因此这里按**当前主题**算实际外扩，而不是拍一个魔法数字。
[[nodiscard]] auto paint_margin(const RenderContext& context) -> float {
  const Shadow lg = shadow_lg(context.theme);
  const float shadow = std::max(lg.blur + std::abs(lg.offset_y),
                                lg.blur2 + std::abs(lg.offset2_y));
  const Metrics& metrics = context.theme.metrics();
  return shadow + metrics.hover_glow_width + metrics.hover_lift + 2.0f;
}

/// 子树里是否**可能**有元素与 `clip`（逻辑坐标）相交。
///
/// 为什么不能只看自己的矩形：框架允许子元素排到父容器之外（如外挂面板），
/// 按父矩形剔除会把可见的子元素一起剔掉。所以"自己有交集 → 直接画"，
/// "自己没交集 → 还要看后代"。这一步是纯矩形运算，相对绘制开销可忽略。
[[nodiscard]] auto subtree_may_paint(const Element& element, const math::Rect& clip,
                                     float margin) -> bool {
  const math::Rect inflated = element.bounds().inset(math::Insets::all(-margin));
  if (!inflated.is_empty() && !inflated.intersect(clip).is_empty()) return true;
  const auto children = element.children();
  for (const auto& child : children) {
    if (child == nullptr) continue;
    if (!child->visible()) continue;
    if (subtree_may_paint(*child, clip, margin)) return true;
  }
  return false;
}

[[nodiscard]] auto main_size(FlexDirection direction, math::Size size) noexcept -> float {
  return direction == FlexDirection::Row ? size.width : size.height;
}

[[nodiscard]] auto cross_size(FlexDirection direction, math::Size size) noexcept -> float {
  return direction == FlexDirection::Row ? size.height : size.width;
}

/// 把业务 key 转成 id 安全片段。
///
/// 为什么要转：id 会出现在**选择器**里（`#tasks/ListItem@task-42`），而选择器在
/// `#`/`.`/`:`/`[`/空白 处切词——业务 key 里带一个空格或点，选择器就再也定位不到这个元素。
/// 而 id 的用途本就是"被外部引用"，所以这里对不安全字符坚决替换，宁可变形不可坏用。
[[nodiscard]] auto sanitize_key(std::string_view key) -> std::string {
  std::string out;
  out.reserve(key.size());
  for (const char raw : key) {
    const auto value = static_cast<unsigned char>(raw);
    const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '_' || value == '-';
    out.push_back(safe ? raw : '-');
  }
  return out;
}

/// 兄弟间 key 撞车检测：撞了会生成**相同的 id**，于是按 id 查找可能命中另一个元素——
/// 这类错误静默且难查，所以在插入时就如实报出来。
void warn_on_duplicate_key(const Element& parent, const Element& child) {
  const std::string sanitized = sanitize_key(child.key());
  if (sanitized.empty()) return;
  for (const auto& sibling : parent.children()) {
    if (sibling.get() == &child || sibling->type() != child.type()) continue;
    if (sanitize_key(sibling->key()) != sanitized) continue;
    log::warn("同一父节点下 {} 的 key 重复（'{}'）：自动 id 会撞车，按 id 查找可能命中另一个元素",
              child.type(), child.key());
    return;
  }
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

auto NullTextPort::measure_width(std::string_view utf8, float size, text::FontRole role) const -> float {
  (void)role;  // 无字体环境：角色无意义
  (void)utf8;
  (void)size;
  return 0.0f;
}

auto NullTextPort::line_height(float size) const -> float { return size * 1.45f; }

void NullTextPort::draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
                        math::Color color, text::FontRole role) const {
  (void)role;  // 无字体环境：角色无意义
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
    // 有 key 就用 key：索引会随插入/删除/筛选整体位移，key 不会。
    // 这是"列表刷新后同一项仍能按 id 找到"的关键（选择器安全字符见 `sanitize_key`）。
    if (const std::string sanitized = sanitize_key(current->key_); !sanitized.empty()) {
      parts.push_back(std::format("{}@{}", current->type(), sanitized));
    } else {
      parts.push_back(std::format("{}[{}]", current->type(), index));
    }
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
  warn_on_duplicate_key(*this, *raw);
  children_.push_back(std::move(child));
  mark_layout_dirty();
  return raw;
}

auto Element::insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element* {
  if (child == nullptr) return nullptr;
  child->parent_ = this;
  Element* raw = child.get();
  const std::size_t position = index > children_.size() ? children_.size() : index;
  warn_on_duplicate_key(*this, *raw);
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
  // `hovered` 暴露给控制通道：悬浮是**只能看像素、看不出状态**的交互态，
  // 不给读取口就只能靠截图猜（自动化验证会很脆）。
  if (name == "hovered") return hovered_ ? "true" : "false";
  if (name == "hover_progress") return std::format("{:.3f}", static_cast<double>(hover_t_));
  if (name == "hover_effect") return hover_effect_.enabled ? "true" : "false";
  return std::nullopt;
}

auto Element::set_property(std::string_view name, std::string_view value) -> bool {
  (void)name;
  (void)value;
  return false;
}

auto Element::property_names() const -> std::vector<std::string_view> {
  return {"hovered", "hover_progress", "hover_effect"};
}

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
  // 增量重绘：把"我变了"上报给所在树的根（UiRoot 帧首收集）
  record_damage();
}

void Element::record_damage() const noexcept {
  const Element* top = this;
  for (const Element* current = parent_; current != nullptr; current = current->parent_) {
    top = current;
  }
  if (paint_margin_hint_ < 0.0f) {
    // 从未绘制过 → 绘制外扩（阴影/发光）未知：保守要求整帧，不能只重画 bounds。
    top->damage_needs_full_ = true;
    top->damage_valid_ = true;
    return;
  }
  const math::Rect extent = bounds_.inflate(paint_margin_hint_);
  top->damage_ = top->damage_valid_ ? top->damage_.union_with(extent) : extent;
  top->damage_valid_ = true;
}

auto Element::take_damage() const noexcept -> DamageReport {
  DamageReport report;
  report.rect = damage_;
  report.valid = damage_valid_;
  report.needs_full = damage_needs_full_;
  damage_ = math::Rect{};
  damage_valid_ = false;
  damage_needs_full_ = false;
  return report;
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
  // **wrap 只在有可用宽约束时才可能生效**：没有上界就永远装得下，自然只有一行。
  const bool wrapping = row && style_.wrap && available_width < kUnbounded;
  float main_total = 0.0f;
  float cross_max = 0.0f;
  std::size_t visible_children = 0;
  std::vector<math::Size> child_sizes;
  if (wrapping) child_sizes.reserve(children_.size());

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
    if (wrapping) child_sizes.push_back(child_size);
    ++visible_children;
  }
  if (visible_children > 1) main_total += style_.gap * static_cast<float>(visible_children - 1);

  float width = 0.0f;
  float height = 0.0f;
  if (row) {
    if (wrapping && !child_sizes.empty()) {
      // 换行容器的尺寸**不能**把小节点宽高"全累加"（那是单行语义）：
      // 宽度取最宽的一行、高度取各行高之和。缺了这段，容器高度只等于最高的一行，
      // 于是第二行起被父容器裁掉——图标全表这类界面会直接少一半（实测踩到）。
      float line_width = 0.0f;
      float line_height = 0.0f;
      float widest_line = 0.0f;
      float total_height = 0.0f;
      bool line_empty = true;
      for (const math::Size& size : child_sizes) {
        if (!line_empty && line_width + style_.gap + size.width > available_width) {
          widest_line = std::max(widest_line, line_width);
          total_height += line_height + style_.gap;
          line_width = size.width;
          line_height = size.height;
        } else {
          line_width += (line_empty ? 0.0f : style_.gap) + size.width;
          line_height = std::max(line_height, size.height);
        }
        line_empty = false;
      }
      widest_line = std::max(widest_line, line_width);
      total_height += line_height;
      // 行宽不会超过可用宽（超过就换行了）；夹一刀防止单个超宽子节点把容器撑破
      width = std::min(widest_line, available_width);
      height = total_height;
    } else {
      width = main_total;
      height = cross_max;
    }
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

  // —— 换行排布（`wrap`）——
  //
  // 实现方式：先分行（每行能装下多少），再逐行排。两趟是必要的——
  // 行高由该行**最高**的子元素决定，而"谁和谁同一行"又取决于行宽，
  // 一遍扫无法同时知道两件事（试过一遍扫，结果是与 `measure` 的估算不一致）。
  if (row && style_.wrap && content.width < kUnbounded) {
    struct Line {
      std::size_t begin{0};
      std::size_t end{0};
      float height{0.0f};
      float width{0.0f};
    };
    std::vector<Line> lines;
    for (std::size_t index = 0; index < visible.size(); ++index) {
      Element* child = visible[index];
      const auto& child_style = child->style();
      float main_extent = main_size(style_.direction, child->measured_size());
      if (child_style.width != kAuto) main_extent = child_style.width;
      const float cross_extent = cross_size(style_.direction, child->measured_size());
      if (lines.empty()) lines.push_back(Line{});
      Line& line = lines.back();
      const float needed = line.end == line.begin
                               ? main_extent
                               : line.width + style_.gap + main_extent;
      if (line.end > line.begin && needed > content.width) {
        lines.push_back(Line{.begin = index, .end = index + 1, .height = cross_extent,
                             .width = main_extent});
        continue;
      }
      line.end = index + 1;
      line.width = needed;
      line.height = std::max(line.height, cross_extent);
    }

    float cursor_y = content.y;
    for (const Line& line : lines) {
      float cursor_x = content.x;
      for (std::size_t index = line.begin; index < line.end; ++index) {
        Element* child = visible[index];
        const auto& child_style = child->style();
        float main_extent = main_size(style_.direction, child->measured_size());
        if (child_style.width != kAuto) main_extent = child_style.width;
        float cross_extent = cross_size(style_.direction, child->measured_size());
        if (child_style.height != kAuto) cross_extent = child_style.height;

        const Align align = child_style.align_self != Align::Stretch ? child_style.align_self
                                                                     : style_.align_items;
        float cross_offset = 0.0f;
        if (align == Align::Center) cross_offset = (line.height - cross_extent) * 0.5f;
        else if (align == Align::End) cross_offset = line.height - cross_extent;

        child->arrange(context, math::Rect{cursor_x, cursor_y + cross_offset, main_extent,
                                           cross_extent}
                                      .inset(child_style.margin));
        cursor_x += main_extent + style_.gap;
      }
      cursor_y += line.height + style_.gap;
    }
    return;
  }

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

/// 悬浮特效的**唯一实现点**（基类绘制）：背景提亮 / 描边变色 / 上浮 / 外发光。
///
/// 为什么放在基类而不是每个组件里：悬浮反馈是**跨组件一致性**问题——
/// 按钮、列表项、表格行、标签页各写一套，观感一定会散（间距、时长、色阶都不同）。
/// 组件只需声明"要哪些效果"（`HoverEffect`），参数由主题令牌统一给。
void Element::paint_box(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const auto& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  // 悬浮进度（0..1）：未开特效的组件恒为 0，走的是原有路径，零额外开销
  const float hover_t = hover_effect_.enabled ? advance_hover(context) : 0.0f;
  const bool hovering = hover_t > 0.001f;
  // 上浮：把整块框体（背景/描边/阴影）上移，配合阴影形成"抬起"感。
  // 只移框体、不移内容？——不移内容会显得文字"陷"在框里。
  // 因此这里把绘制用的框体整体上移，内容由组件自己的 `paint_content` 决定
  // （组件若要用上浮，从 `hover_lift_offset()` 取同一个位移）。
  const math::Rect box = hovering && hover_effect_.lift
                             ? bounds_.offset(0.0f, -metrics.hover_lift * hover_t)
                             : bounds_;
  math::Color background = style_.background;
  math::Color border = style_.border_color;
  if (hovering && hover_effect_.background && colors.surface_hover.a != 0U) {
    // 混合而非直接替换：`Ghost`/`Soft` 这类背景很淡的按钮也需要可见的反馈，
    // 而直接换成 surface_hover 会把它们的语气抹平（全部变成一个样子的方块）。
    background = background.a == 0U
                     ? math::Color{colors.surface_hover.r, colors.surface_hover.g,
                                   colors.surface_hover.b,
                                   static_cast<std::uint8_t>(colors.surface_hover.a * hover_t)}
                     : background.mix(colors.surface_hover, hover_t);
  }
  if (hovering && hover_effect_.border && colors.border_hover.a != 0U) {
    border = border.mix(colors.border_hover, hover_t);
  }
  // 外发光：把强调色以低不透明度铺一层**放大**的圆角矩形（v0.2 的阴影/模糊机制复用）
  if (hovering && hover_effect_.glow && colors.glow.a != 0U) {
    const float spread = metrics.hover_glow_width * hover_t;
    const math::Color glow{colors.glow.r, colors.glow.g, colors.glow.b,
                           static_cast<std::uint8_t>(colors.glow.a * hover_t)};
    canvas.fill_rect(box.inflate(spread), raster::Paint::solid(glow),
                     style_.radius + spread, raster::DrawOptions{.opacity = style_.opacity});
  }
  if (style_.shadow.visible()) {
    // 上浮时阴影跟着走（否则"抬起"会被阴影钉在原地，看着像两层错位）
    const math::Rect shadow_box = box.inset(style_.margin);
    const auto options = raster::DrawOptions{.opacity = style_.opacity};
    // 先环境层（大而淡）、后关键层（紧而实）：反过来的话紧层会被大层盖住，
    // 叠加后反而比单层更浑。
    if (style_.shadow.second_visible()) {
      canvas.draw_shadow(shadow_box, style_.radius, style_.shadow.blur2, style_.shadow.color2,
                         math::Point{style_.shadow.offset2_x, style_.shadow.offset2_y}, options);
    }
    canvas.draw_shadow(shadow_box, style_.radius, style_.shadow.blur, style_.shadow.color,
                       math::Point{style_.shadow.offset_x, style_.shadow.offset_y}, options);
  }
  if (background.a != 0U) {
    canvas.fill_rect(box, raster::Paint::solid(background), style_.radius,
                     raster::DrawOptions{.opacity = style_.opacity});
  }
  if (style_.border_width > 0.0f && border.a != 0U) {
    raster::Path outline;
    const float half = style_.border_width * 0.5f;
    outline.add_rounded_rect(box.inset(math::Insets::all(half)),
                             style_.radius > half ? style_.radius - half : 0.0f);
    canvas.stroke_path(outline, raster::Paint::solid(border), style_.border_width,
                       raster::DrawOptions{.opacity = style_.opacity});
  }
}

auto Element::paint_text(const RenderContext& context, raster::Surface& canvas, std::string_view text,
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

void Element::paint(const RenderContext& context, raster::Surface& canvas) const {
  if (!visible_) return;
  // **视口剔除**：与当前裁剪区无交集就整块不画。
  //
  // 为什么必须有：绘制是自顶向下的，一个排到屏幕外很远的元素（例如滚动到视野外的
  // 三维视图、长列表的尾部项）照样会被完整绘制——`SceneView` 因此每帧白付一次
  // 渲染+回读（实测 13.9 ms，而整帧其余部分才 4.3 ms）。事件、布局都不受影响，只有绘制跳过。
  //
  // 安全前提（两条，缺一就会"东西不见了"）：
  // ① 按**主题实际值**外扩（阴影/发光/上浮都会画到 bounds 之外，见 `paint_margin`）；
  // ② 自己无交集时还要看**后代**（框架允许子元素排到父容器之外）。
  {
    const math::IntRect physical = canvas.clip_rect();
    const float scale = canvas.device_scale();
    const float inv = scale > 0.0f ? 1.0f / scale : 1.0f;
    const math::Rect logical_clip{static_cast<float>(physical.x) * inv,
                                  static_cast<float>(physical.y) * inv,
                                  static_cast<float>(physical.width) * inv,
                                  static_cast<float>(physical.height) * inv};
    const float margin = paint_margin(context);
    // 留存本次绘制的外扩量：元素后续标脏时据此算损坏区（增量重绘）。
    paint_margin_hint_ = margin;
    if (!subtree_may_paint(*this, logical_clip, margin)) return;
  }
  if (context.painted_elements != nullptr) ++*context.painted_elements;
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
  // 基类不消费；但行为注入的 handler 仍要给机会（组件覆写若已消费则不会走到这里）。
  if (event_handler_) return event_handler_(event);
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
