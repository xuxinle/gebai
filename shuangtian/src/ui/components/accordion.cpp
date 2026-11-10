#include "st/ui/components/accordion.hpp"

#include <algorithm>
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

using components_internal::text_port_of;

Accordion::Accordion() = default;

// ————————————————————————————————————————————————————————————————————————————
// 面板项
// ————————————————————————————————————————————————————————————————————————————

void Accordion::add_panel(std::string title, std::unique_ptr<Element> content) {
  Panel panel{};
  panel.title = std::move(title);
  panel.content = std::move(content);
  panel.progress = 0.0f;   // 新加的面板默认收起（`open` 才会展开它）
  panels_.push_back(std::move(panel));
  mark_layout_dirty();
}

void Accordion::clear_panels() {
  if (panels_.empty()) return;
  panels_.clear();
  mark_layout_dirty();
}

auto Accordion::title_at(std::size_t index) const -> std::string {
  return index < panels_.size() ? panels_[index].title : std::string{};
}

void Accordion::set_title(std::size_t index, std::string title) {
  if (index >= panels_.size() || panels_[index].title == title) return;
  panels_[index].title = std::move(title);
  mark_layout_dirty();
}

auto Accordion::content_at(std::size_t index) const noexcept -> Element* {
  if (index >= panels_.size()) return nullptr;
  return panels_[index].content.get();
}

void Accordion::set_content(std::size_t index, std::unique_ptr<Element> content) {
  if (index >= panels_.size()) return;
  panels_[index].content = std::move(content);
  mark_layout_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 展开状态
// ————————————————————————————————————————————————————————————————————————————

void Accordion::set_mode(Mode mode) {
  if (mode_ == mode) return;
  mode_ = mode;
  // 切到单选时只保留**第一个**已展开项（`Multiple` 下可能开了多个）。
  if (mode_ == Mode::Single) {
    int keep = -1;
    for (std::size_t i = 0; i < panels_.size(); ++i) {
      if (!panels_[i].open) continue;
      if (keep < 0) keep = static_cast<int>(i);
      else panels_[i].open = false;
    }
    (void)keep;
  }
  mark_layout_dirty();
}

void Accordion::set_allow_collapse(bool allow) {
  if (allow_collapse_ == allow) return;
  allow_collapse_ = allow;
  mark_dirty();
}

void Accordion::set_separated(bool separated) {
  if (separated_ == separated) return;
  separated_ = separated;
  mark_layout_dirty();
}

void Accordion::apply_mode(std::size_t opened_index) {
  if (mode_ != Mode::Single) return;
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    if (i == opened_index) continue;
    panels_[i].open = false;
    panels_[i].progress = 0.0f;
  }
}

void Accordion::set_open(std::size_t index, bool open) {
  if (index >= panels_.size()) return;
  if (panels_[index].open == open) return;
  panels_[index].open = open;
  if (open) {
    apply_mode(index);
    panels_[index].progress = 0.0f;   // 从 0 开始展开动画
  }
  mark_layout_dirty();
  if (on_change) on_change(open_indices());
}

void Accordion::open(std::size_t index) { set_open(index, true); }
void Accordion::close(std::size_t index) { set_open(index, false); }

void Accordion::toggle(std::size_t index) {
  if (index >= panels_.size()) return;
  if (!panels_[index].open) {
    set_open(index, true);
    return;
  }
  // 已展开：`allow_collapse` 决定能不能收起。
  // `Single` 且不允许收起时，保持展开（点它等于"再选一次当前项"）。
  if (mode_ == Mode::Single && !allow_collapse_) return;
  set_open(index, false);
}

void Accordion::close_all() {
  bool changed = false;
  for (auto& panel : panels_) {
    if (!panel.open) continue;
    panel.open = false;
    panel.progress = 0.0f;
    changed = true;
  }
  if (!changed) return;
  mark_layout_dirty();
  if (on_change) on_change(open_indices());
}

auto Accordion::is_open(std::size_t index) const -> bool {
  return index < panels_.size() && panels_[index].open;
}

auto Accordion::open_indices() const -> std::vector<std::size_t> {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    if (panels_[i].open) out.push_back(i);
  }
  return out;
}

// ————————————————————————————————————————————————————————————————————————————
// 主题、度量、落位
// ————————————————————————————————————————————————————————————————————————————

void Accordion::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  title_font_ = metrics.font_sm;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.radius = metrics.radius_md;
  style_.font_size = title_font_;
  style_.color = theme.colors().text;
  apply_text_overrides();
  title_font_ = style_.font_size;
}

auto Accordion::visible_content_height(std::size_t index) const -> float {
  if (index >= panels_.size()) return 0.0f;
  const Panel& panel = panels_[index];
  if (!panel.open && panel.progress <= 0.0f) return 0.0f;
  return panel.content_height * std::clamp(panel.progress, 0.0f, 1.0f);
}

void Accordion::measure(const RenderContext& context, const Constraints& constraints) {
  const float available =
      constraints.max_width < kUnbounded ? constraints.max_width : (style_.width > 0.0f ? style_.width : 360.0f);
  const float inner_width = std::max(available - kContentPadding * 2.0f, 20.0f);

  float height = 0.0f;
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    Panel& panel = panels_[i];
    height += kHeaderHeight;
    if (panel.content != nullptr) {
      Constraints inner{};
      inner.max_width = inner_width;
      inner.available_width = inner_width;
      inner.max_height = constraints.max_height;
      inner.available_height = constraints.max_height;
      panel.content->measure(context, inner);
      panel.content_height = panel.content->measured_size().height + kContentPadding * 2.0f;
    } else {
      panel.content_height = 0.0f;
    }
    height += visible_content_height(i);
    if (separated_ && i + 1U < panels_.size()) height += 0.5f;
  }
  height = std::clamp(height, style_.min_height, style_.max_height);
  float width = std::clamp(available, style_.min_width, style_.max_width);
  measured_ = math::Size{width, height};
}

void Accordion::relayout(const RenderContext& context, math::Rect rect) const {
  (void)context;
  header_boxes_.assign(panels_.size(), math::Rect{});
  content_boxes_.assign(panels_.size(), math::Rect{});
  if (rect.is_empty()) return;

  const float inner_width = std::max(rect.width - kContentPadding * 2.0f, 20.0f);
  float y = rect.y;
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    header_boxes_[i] = math::Rect{rect.x, y, rect.width, kHeaderHeight};
    y += kHeaderHeight;
    const float visible = visible_content_height(i);
    if (visible > 0.5f) {
      content_boxes_[i] =
          math::Rect{rect.x + kContentPadding, y, inner_width, visible};
      y += visible;
    }
    if (separated_ && i + 1U < panels_.size()) y += 0.5f;
  }
}

auto Accordion::advance_animation() const -> bool {
  bool animating = false;
  for (auto& panel : panels_) {
    const float target = panel.open ? 1.0f : 0.0f;
    if (std::abs(panel.progress - target) < 0.001f) {
      panel.progress = target;
      continue;
    }
    const float step = static_cast<float>(1.0 / (60.0 * static_cast<double>(kAnimSeconds)));
    panel.progress += panel.progress < target ? step : -step;
    panel.progress = std::clamp(panel.progress, 0.0f, 1.0f);
    animating = true;
  }
  return animating;
}

void Accordion::arrange(const RenderContext& context, math::Rect rect) {
  // ⚠ **动画必须先于几何推进**。
  //
  // 为何：`content_boxes_` 的高度取自 `progress`。若把推进放在 `paint_content` 里，
  // 那么 `arrange` 时进度恒为 0 → 内容区矩形为空 → 子内容**从不落位**，
  // 表现为“面板展开了、标题下面却是一片空白”（实测踩到）。
  // 几何（`arrange`）先于绘制（`paint`），动画要驱动几何，就必须在 `arrange` 里走。
  if (advance_animation()) request_animation();
  Element::arrange(context, rect);
  relayout(context, rect);
  // 内容 Element 由本组件持有（不是 children_），落位要显式给。
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    if (panels_[i].content == nullptr) continue;
    const math::Rect box = i < content_boxes_.size() ? content_boxes_[i] : math::Rect{};
    if (box.is_empty()) continue;
    panels_[i].content->arrange(
        context, math::Rect{box.x, box.y + kContentPadding, box.width,
                            std::max(box.height - kContentPadding * 2.0f, 0.0f)});
  }
}

auto Accordion::header_rect(std::size_t index) const -> math::Rect {
  return index < header_boxes_.size() ? header_boxes_[index] : math::Rect{};
}

auto Accordion::content_rect(std::size_t index) const -> math::Rect {
  // ⚠ 返回**目标几何**（展开则非空），不受动画中间态影响。
  //
  // 为何：`content_boxes_` 是“本帧实际画在哪”，而动画刚起步时进度为 0、高度也为 0。
  // 若对外暴露那个值，调用方 `open(0)` 后立即查 `content_rect(0)` 会拿到空矩形，
  // 看起来像“没展开”（实测踩到）。几何查询回答的是“布局定下来以后它在哪”，
  // 动画只是绘制时的插值。
  if (index >= panels_.size()) return math::Rect{};
  if (!panels_[index].open && panels_[index].progress <= 0.0f) return math::Rect{};
  const math::Rect shown = index < content_boxes_.size() ? content_boxes_[index] : math::Rect{};
  if (!shown.is_empty()) return shown;
  // 动画未起步：按目标高度推算它的矩形（与 `relayout` 同一套累加规则）。
  float y = bounds_.y;
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    y += kHeaderHeight;
    if (i == index) {
      const float height = panels_[i].content_height;
      if (height <= 0.5f) return math::Rect{};
      const float width = std::max(bounds_.width - kContentPadding * 2.0f, 20.0f);
      return math::Rect{bounds_.x + kContentPadding, y, width, height};
    }
    y += visible_content_height(i);
    if (separated_ && i + 1U < panels_.size()) y += 0.5f;
  }
  return {};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Accordion::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || panels_.empty()) return;
  relayout(context, bounds_);
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);

  // 动画已在 `arrange` 里推进（几何要先于绘制就绪，见那里的注释）。

  // ⚠ 内容必须先画（在标题行之下会被标题行遮住的部分最少），
  // 但更关键的是：**内容要按可见高度裁剪**——动画期间内容超出的部分不能溢出。
  {
    canvas.push_clip_rect(bounds_);
    for (std::size_t i = 0; i < panels_.size(); ++i) {
      if (panels_[i].content == nullptr) continue;
      const math::Rect box = i < content_boxes_.size() ? content_boxes_[i] : math::Rect{};
      if (box.is_empty()) continue;
      canvas.push_clip_rect(box);
      panels_[i].content->paint(context, canvas);
      canvas.pop_clip();
    }
    canvas.pop_clip();
  }

  for (std::size_t i = 0; i < panels_.size(); ++i) {
    const math::Rect header = header_boxes_[i];
    if (header.is_empty()) continue;
    const bool hover = static_cast<int>(i) == hover_index_;
    if (hover) {
      canvas.fill_rect(header, raster::Paint::solid(colors.surface_hover));
    }
    // 箭头：展开时 `chevron-down`，收起时 `chevron-right`。
    const math::Rect chevron{header.x + kPaddingX, header.y + (header.height - kChevronSize) * 0.5f,
                             kChevronSize, kChevronSize};
    Icon::draw(canvas, panels_[i].open ? "chevron-down" : "chevron-right", chevron,
               colors.text_muted, Icon::effective_stroke("chevron-down", kChevronSize));
    // 标题
    const float text_x = chevron.right() + kGap;
    const float text_width = std::max(header.right() - kPaddingX - text_x, 0.0f);
    if (text_width <= 0.0f) continue;
    const std::string shown = port.ellipsize(panels_[i].title, title_font_, text_width);
    const float top = centered_line_top(port, shown, title_font_, header.y, header.height);
    port.draw(canvas, shown, math::Point{text_x, top}, title_font_, colors.text,
              text::FontRole::Proportional, 0.0f, panels_[i].open);
    // 分隔线（面板之间）
    if (separated_ && i + 1U < panels_.size()) {
      const float line_y = header.bottom() + visible_content_height(i);
      canvas.fill_rect(math::Rect{bounds_.x, line_y, bounds_.width, 0.5f},
                       raster::Paint::solid(colors.border));
    }
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

auto Accordion::on_event(const RenderContext& context, Event& event) -> bool {
  if (!enabled_) return false;
  // 先给内容层（展开的那一项），它吃掉的事件不该再触发标题行。
  for (std::size_t i = 0; i < panels_.size() && i < content_boxes_.size(); ++i) {
    if (panels_[i].content == nullptr) continue;
    const math::Rect box = content_boxes_[i];
    if (box.is_empty()) continue;
    if (event.kind == EventKind::MouseMove || event.kind == EventKind::Click ||
        event.kind == EventKind::DoubleClick || event.kind == EventKind::TripleClick) {
      if (!box.contains(event.position)) continue;
    }
    if (panels_[i].content->on_event(context, event)) return true;
  }

  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_index_ = -1;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      int found = -1;
      for (std::size_t i = 0; i < header_boxes_.size(); ++i) {
        if (header_boxes_[i].contains(event.position)) {
          found = static_cast<int>(i);
          break;
        }
      }
      if (found != hover_index_) {
        hover_index_ = found;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      // 整行可点（不是只有箭头可点）。
      for (std::size_t i = 0; i < header_boxes_.size(); ++i) {
        if (!header_boxes_[i].contains(event.position)) continue;
        toggle(i);
        event.handled = true;
        return true;
      }
      return false;
    }
    default: return false;
  }
}

auto Accordion::hit_test(math::Point point) const noexcept -> bool {
  return bounds_.contains(point);
}

auto Accordion::cursor_at(math::Point point) const noexcept -> MouseCursor {
  for (const auto& header : header_boxes_) {
    if (header.contains(point)) return MouseCursor::Hand;
  }
  return MouseCursor::Arrow;
}

auto Accordion::semantics_text() const -> std::string {
  std::string out;
  for (std::size_t i = 0; i < panels_.size(); ++i) {
    if (i > 0) out += ", ";
    out += panels_[i].title;
    if (panels_[i].open) out += "（展开）";
  }
  return out;
}

auto Accordion::semantics_value() const -> std::string {
  const auto open_set = open_indices();
  std::string out;
  for (std::size_t i = 0; i < open_set.size(); ++i) {
    if (i > 0) out += '|';
    out += std::to_string(open_set[i]);
  }
  return out;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto Accordion::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "items" || name == "titles") {
    std::string out;
    for (std::size_t i = 0; i < panels_.size(); ++i) {
      if (i > 0) out += '|';
      out += panels_[i].title;
    }
    return out;
  }
  if (name == "open" || name == "expanded") return semantics_value();
  if (name == "mode") {
    return std::string(mode_ == Mode::Single ? "single" : "multiple");
  }
  if (name == "allow_collapse") return std::string(allow_collapse_ ? kFlagTrue : "false");
  if (name == "separated") return std::string(separated_ ? kFlagTrue : "false");
  if (name == "count") return std::to_string(panels_.size());
  return Element::get_property(name);
}

auto Accordion::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "open" || name == "expanded") {
    const std::string text(st::trim(value));
    // 空串 = 全收起。
    if (text.empty()) {
      close_all();
      return true;
    }
    std::vector<std::size_t> targets;
    for (const auto& part : st::split(text, '|')) {
      const auto parsed = st::parse_u64(st::trim(part));
      if (!parsed.has_value()) return false;
      targets.push_back(static_cast<std::size_t>(*parsed));
    }
    close_all();
    for (const auto index : targets) open(index);
    return true;
  }
  if (name == "mode") {
    const std::string text(st::trim(value));
    if (text == "single" || text == "one") set_mode(Mode::Single);
    else if (text == "multiple" || text == "many" || text == "multi") set_mode(Mode::Multiple);
    else return false;
    return true;
  }
  if (name == "allow_collapse") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_allow_collapse(*parsed);
    return true;
  }
  if (name == "separated") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_separated(*parsed);
    return true;
  }
  if (name == "items" || name == "titles") {
    // 改标题只改文本，**不动内容与回**调——内容 Element 是有主之物，
    // 按字符串重建会丢掉它。
    std::vector<std::string> titles;
    for (const auto& part : st::split(value, '|')) titles.emplace_back(st::trim(part));
    for (std::size_t i = 0; i < titles.size() && i < panels_.size(); ++i) {
      panels_[i].title = titles[i];
    }
    while (panels_.size() < titles.size()) {
      add_panel(titles[panels_.size()]);
    }
    mark_layout_dirty();
    return true;
  }
  return Element::set_property(name, value);
}

auto Accordion::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"items", "open", "mode", "allow_collapse", "separated"});
}

auto Accordion::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"count"});
}

auto Accordion::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "open" || action == "expand") {
    const auto parsed = st::parse_u64(st::trim(argument));
    if (!parsed.has_value()) return false;
    open(static_cast<std::size_t>(*parsed));
    return true;
  }
  if (action == "close" || action == "collapse") {
    const auto parsed = st::parse_u64(st::trim(argument));
    if (!parsed.has_value()) return false;
    close(static_cast<std::size_t>(*parsed));
    return true;
  }
  if (action == "toggle") {
    const auto parsed = st::parse_u64(st::trim(argument));
    if (!parsed.has_value()) return false;
    toggle(static_cast<std::size_t>(*parsed));
    return true;
  }
  if (action == "close_all") {
    close_all();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::Accordion);
