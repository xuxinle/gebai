#include "st/ui/components/list.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "st/raster/paint.hpp"

namespace st::ui {
namespace {

/// 文本端口取用（`RenderContext::text` 可为空 → 退化为 no-op 端口）。
[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  return context.text != nullptr ? *context.text : NullTextPort::instance();
}

/// 单行文本绘制（省略号截断 + 对齐），不修改 `style_`（同一节点可有多种字号/颜色）。
void draw_text(const RenderContext& context, raster::Surface& canvas, std::string_view text,
               math::Rect box, float size, math::Color color, TextAlign align) {
  if (text.empty() || box.width <= 0.0f || box.height <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float width = port.measure_width(clipped, size);
  float x = box.x;
  if (align == TextAlign::Center) {
    x = box.x + (box.width - width) * 0.5f;
  } else if (align == TextAlign::End) {
    x = box.right() - width;
  }
  const float line = port.line_height(size);
  const float y = box.y + (box.height - line) * 0.5f;
  port.draw(canvas, clipped, math::Point{x, y}, size, color);
}

constexpr float kTextInset{14.0f};  ///< 文本左边距 = space_md(12) + 指示条留白

}  // namespace

// —— ListItem ——

ListItem::ListItem(std::string label, std::string subtitle)
    : label_(std::move(label)), subtitle_(std::move(subtitle)) {
  style_.radius = 6.0f;
  // 列表项的悬浮反馈是"整行提亮"，不上浮（上浮会让行间跳动、列表看着在抖）
  // 悬浮特效：背景提亮 + 上浮（"可点"的手感）；参数由主题令牌统一给，
  // 组件只声明要哪几项——这样按钮/列表/表格的悬浮反馈不会各走一套。
  set_hover_effect(HoverEffect{.enabled = true, .background = true, .border = false, .lift = false, .glow = false, .cursor = true});

}

void ListItem::set_label(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  mark_layout_dirty();
}

void ListItem::set_subtitle(std::string subtitle) {
  if (subtitle_ == subtitle) return;
  subtitle_ = std::move(subtitle);
  mark_layout_dirty();
}

void ListItem::set_selected(bool value) {
  if (selected_ == value) return;
  selected_ = value;
  mark_dirty();
}

void ListItem::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const Metrics& metrics = context.theme.metrics();
  float width = port.measure_width(label_, metrics.font_base);
  if (!subtitle_.empty()) {
    width = std::max(width, port.measure_width(subtitle_, metrics.font_xs));
  }
  width += kTextInset * 2.0f;
  width = std::max(width, 80.0f);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void ListItem::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  bounds_ = rect;
  layout_dirty_ = false;
}

void ListItem::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const auto& colors = context.theme.colors();
  const auto& metrics = context.theme.metrics();

  if (selected_) {
    canvas.fill_rect(bounds_, raster::Paint::solid(colors.primary_soft), metrics.radius_sm);
    const float bar_height = std::max(0.0f, bounds_.height - metrics.space_lg);
    const math::Rect indicator{bounds_.x + 1.0f, bounds_.y + (bounds_.height - bar_height) * 0.5f,
                               kIndicatorWidth, bar_height};
    canvas.fill_rect(indicator, raster::Paint::solid(colors.primary), kIndicatorWidth * 0.5f);
  } else if (hovered_) {
    canvas.fill_rect(bounds_, raster::Paint::solid(colors.surface_alt), metrics.radius_sm);
  }

  const math::Color label_color = selected_ ? colors.primary : colors.text;
  const math::Rect text_area = bounds_.inset(math::Insets::symmetric(
      kTextInset, metrics.space_xs));
  if (subtitle_.empty()) {
    draw_text(context, canvas, label_, text_area, metrics.font_base, label_color, TextAlign::Start);
    return;
  }
  const float label_height = metrics.font_base * metrics.line_height_body;
  const math::Rect label_box{text_area.x, text_area.y, text_area.width, label_height};
  const math::Rect subtitle_box{text_area.x, label_box.bottom(), text_area.width,
                                text_area.bottom() - label_box.bottom()};
  draw_text(context, canvas, label_, label_box, metrics.font_base, label_color, TextAlign::Start);
  draw_text(context, canvas, subtitle_, subtitle_box, metrics.font_xs, colors.text_muted,
            TextAlign::Start);
}

void ListItem::activate() {
  if (on_activate_) on_activate_(index_);
}

auto ListItem::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (event.kind != EventKind::Click) return false;
  // 与 `invoke(click)` / 脚本 `ui_invoke` 共用同一条路径（见头文件注释）
  activate();
  event.handled = true;
  return true;
}

auto ListItem::semantics_text() const -> std::string {
  if (subtitle_.empty()) return label_;
  return std::format("{} · {}", label_, subtitle_);
}

auto ListItem::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = selected_;
  return flags;
}

// —— List ——

List::List() {
  style_.direction = FlexDirection::Column;
  style_.gap = 2.0f;
  set_focusable(true);
}

auto List::item_count() const noexcept -> std::size_t { return children_.size(); }

auto List::item(std::size_t index) const noexcept -> ListItem* {
  Element* child = child_at(index);
  if (child == nullptr || child->type() != "ListItem") return nullptr;
  return static_cast<ListItem*>(child);
}

void List::bind_item(ListItem& node, std::size_t index) {
  node.set_index(index);
  node.set_on_activate([this](std::size_t activated) { select(activated); });
}

void List::sync_items(const std::vector<Entry>& entries) {
  // 选中项按 **key** 记住（而不是索引）：顺序变了"选中的那一项"不应该变成别的数据
  std::string selected_key{};
  if (selected_ != kNoSelection && selected_ < children_.size()) {
    if (const auto* item = this->item(selected_); item != nullptr) selected_key = item->key();
  }

  // 现有项全部摘下（`remove_child` 会把父子关系清干净）——进入候选池，按 key 复用
  std::vector<std::unique_ptr<ListItem>> pool;
  std::vector<Element*> existing;
  existing.reserve(child_count());
  for (std::size_t position = 0; position < child_count(); ++position) {
    if (auto* entry = item(position); entry != nullptr) existing.push_back(entry);
  }
  pool.reserve(existing.size());
  for (Element* element : existing) {
    if (auto detached = remove_child(element); detached != nullptr) {
      pool.push_back(std::unique_ptr<ListItem>(static_cast<ListItem*>(detached.release())));
    }
  }

  std::vector<std::unique_ptr<Element>> next;
  next.reserve(entries.size());
  for (const auto& entry : entries) {
    // 同 key 复用已有元素：**id、选中态、焦点都保持**（这正是 sync 与 clear+add 的区别）
    std::unique_ptr<ListItem> node;
    for (auto& candidate : pool) {
      if (candidate != nullptr && candidate->key() == entry.key) {
        node = std::move(candidate);
        break;
      }
    }
    if (node == nullptr) {
      node = std::make_unique<ListItem>(entry.label, std::string{});
      node->set_key(entry.key);
    } else {
      node->set_label(entry.label);  // 文案可能变了，元素不变
    }
    // 无论是新建还是复用，都要接上激活链路（漏了就是"点了没反应"）
    bind_item(*node, next.size());
    next.push_back(std::move(node));
  }
  // 池里剩下的就是"数据里已消失的 key"：直接丢弃（不留在树里，否则选择器会查到幽灵元素）
  pool.clear();

  for (auto& node : next) add_child(std::move(node));

  // 重挑索引 + 恢复选中（按 key 找回，而不是按旧索引）
  std::size_t restored = kNoSelection;
  for (std::size_t position = 0; position < child_count(); ++position) {
    auto* entry = item(position);
    if (entry == nullptr) continue;
    entry->set_index(position);
    entry->set_selected(false);
    if (!selected_key.empty() && entry->key() == selected_key) restored = position;
  }
  if (restored != kNoSelection) {
    selected_ = restored;
    if (auto* entry = item(restored); entry != nullptr) entry->set_selected(true);
  } else {
    selected_ = kNoSelection;
    // 选中的项被移除时如实告知（而不是静默把选中挪到别的数据上）
    if (!selected_key.empty() && on_select_) on_select_(kNoSelection);
  }
  mark_layout_dirty();
}

void List::clear_items() {
  clear_children();
  selected_ = kNoSelection;
  mark_layout_dirty();
}

auto List::add_item(std::string label, std::string subtitle) -> ListItem* {
  auto node = std::make_unique<ListItem>(std::move(label), std::move(subtitle));
  bind_item(*node, children_.size());
  ListItem* raw = node.get();
  (void)add_child(std::move(node));
  return raw;
}

void List::select(std::size_t index, bool notify) {
  const bool valid = index < children_.size();
  selected_ = valid ? index : kNoSelection;
  for (std::size_t position = 0; position < children_.size(); ++position) {
    Element* child = child_at(position);
    if (child == nullptr || child->type() != "ListItem") continue;
    auto* entry = static_cast<ListItem*>(child);
    entry->set_index(position);
    entry->set_selected(valid && position == selected_);
  }
  mark_dirty();
  if (notify && on_select_) on_select_(selected_);
}

auto List::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (event.kind != EventKind::KeyDown) return false;
  const std::size_t count = children_.size();
  if (count == 0) return false;
  const std::size_t current = selected_ == kNoSelection ? 0 : selected_;
  if (event.key == "ArrowDown") {
    select(current + 1 < count ? current + 1 : count - 1);
    return true;
  }
  if (event.key == "ArrowUp") {
    select(current > 0 ? current - 1 : 0);
    return true;
  }
  if (event.key == "Home") {
    select(0);
    return true;
  }
  if (event.key == "End") {
    select(count - 1);
    return true;
  }
  return false;
}

auto List::semantics_value() const -> std::string {
  if (selected_ == kNoSelection) return "selected=none";
  return std::format("selected={}", static_cast<unsigned long long>(selected_));
}

auto List::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = selected_ != kNoSelection;
  return flags;
}

}  // namespace st::ui
