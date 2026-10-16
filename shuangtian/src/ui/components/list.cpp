#include "st/ui/components/list.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "st/raster/paint.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

namespace {

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
  label_width_ = -1.0F;  // 文案变了：宽度缓存作废（见 hpp 里 label_width_ 的说明）
  mark_layout_dirty();
}

void ListItem::set_subtitle(std::string subtitle) {
  if (subtitle_ == subtitle) return;
  subtitle_ = std::move(subtitle);
  subtitle_width_ = -1.0F;
  mark_layout_dirty();
}

void ListItem::set_selected(bool value) {
  if (selected_ == value) return;
  selected_ = value;
  mark_dirty();
}

void ListItem::apply_theme(const Theme& theme) {
  // 基类实现里已含 apply_text_overrides（本组件不重算字号/字色，主题态即默认态）；
  // 覆盖在 Element::apply_theme 末尾统一回放进 style_，measure/paint 从 style_ 取。
  //（容器 List 的下放覆盖会写进本项的 extras，见 List::apply_theme_to_items。）
  Element::apply_theme(theme);
}

void ListItem::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const Metrics& metrics = context.theme.metrics();
  // **文本宽度缓存**（见 hpp）：只量文案本身（不含内边距与夹取），
  // 它只取决于 `(文案, 字号)`。文案变由 `set_label`/`set_subtitle` 失效；
  // 字号变（主题换档 / `scale_fonts` / 排版覆盖）由这里比对字号失效。
  //
  // ⚠ 不从 style_.font_size 取：既有调用方（如 width-cache 测试）可不经过
  // apply_theme 直接 measure，style_ 停在构造默认值；度量口径必须与
  //「无覆盖 = 主题 font_base」保持一致（直接向意图侧问，不依赖回放时机）。
  const float label_font =
      text_size_override() > 0.0f ? text_size_override() : metrics.font_base;
  const float subtitle_font = label_font * (metrics.font_xs / metrics.font_base);
  if (label_width_ < 0.0F || label_width_font_size_ != label_font) {
    label_width_ = port.measure_width(label_, label_font);
    label_width_font_size_ = label_font;
  }
  float width = label_width_;
  if (!subtitle_.empty()) {
    if (subtitle_width_ < 0.0F || subtitle_width_font_size_ != subtitle_font) {
      subtitle_width_ = port.measure_width(subtitle_, subtitle_font);
      subtitle_width_font_size_ = subtitle_font;
    }
    width = std::max(width, subtitle_width_);
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

  const math::Color label_color = [this, &colors]() {
    // 显式文字色覆盖（DSL `BoxProps::color/hex_color`）盖过选中/常态色
    //（与 Tree 行文本同口径）。lambda 而非直写：选中色的优先级在覆盖之下，
    // 直接三元表达式会把覆盖写法搞成两分支都问一遍。
    if (const auto& override_color = text_color_override(); override_color.has_value()) {
      return *override_color;
    }
    return selected_ ? colors.primary : colors.text;
  }();
  const math::Rect text_area = bounds_.inset(math::Insets::symmetric(
      kTextInset, metrics.space_xs));
  // 与 measure 同一口径：向意图侧问字号（不依赖 apply_theme 回放时机）。
  const float label_size =
      text_size_override() > 0.0f ? text_size_override() : metrics.font_base;
  if (subtitle_.empty()) {
    draw_text(context, canvas, label_, text_area, label_size, label_color, TextAlign::Start);
    return;
  }
  const float label_height = label_size * metrics.line_height_body;
  const math::Rect label_box{text_area.x, text_area.y, text_area.width, label_height};
  const math::Rect subtitle_box{text_area.x, label_box.bottom(), text_area.width,
                                text_area.bottom() - label_box.bottom()};
  draw_text(context, canvas, label_, label_box, label_size, label_color, TextAlign::Start);
  draw_text(context, canvas, subtitle_, subtitle_box, label_size * 0.75f, colors.text_muted,
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

// —— 列表项的排版覆盖下放（与四先例的"各组件自己接"同口径）——

void ListItem::apply_container_text_overrides(const Element& container) {
  // 容器（List）记的排版覆盖镜像到本项（字是项画的，见 List::apply_theme_to_items
  // 的说明）。容器没设的那项镜像为空（`nullopt`/`-1`，即"无覆盖"），而不是保留
  // 旧值——每帧镜像一次，不会累积，也保证容器撤掉覆盖后项跟着回退。
  const auto& color = container.text_color_override();
  const float size = container.text_size_override();
  RenderExtras& state = extras();
  state.text_color_override = color;
  state.text_size_override = size;
  mark_layout_dirty();
}

// —— List（容器：自身不画字，排版覆盖下放给项）——

void List::apply_theme(const Theme& theme) {
  // 自身作为容器不画字：基类 apply_theme（含覆盖回放）跑完后，把覆盖镜像给每个
  // 项——apply_theme_tree 是父先子后，紧接着项的基类 apply_theme 会把镜像值
  // 回放进项的 style_（视觉树/get_property 的读回口都读 style_）。
  Element::apply_theme(theme);
  apply_theme_to_items();
}

void List::apply_theme_to_items() {
  // DSL 的 list(...) 把 BoxProps 应用在 List 容器上，而字是 ListItem 画的——
  // 不下放就是"容器记了覆盖、项不认"的静默不生效。
  //
  // 镜像语义：容器没设的那项镜像为"无覆盖"（nullopt/-1），而非保留旧值
  // ——每帧镜像一次不累积，也保证容器撤掉覆盖后项跟着回退。
  // 无覆盖时零开销（早退）；这里只镜像，真正的回放在项自己的 apply_theme。
  if (extras_if_any() == nullptr) return;   // 容器从没设过覆盖：项也不必动
  if (!text_color_override().has_value() && text_size_override() <= 0.0f) return;
  for (std::size_t index = 0; index < children_.size(); ++index) {
    Element* child = child_at(index);
    if (child == nullptr || child->type() != "ListItem") continue;
    static_cast<ListItem*>(child)->apply_container_text_overrides(*this);
  }
}

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
  // 复用查找用哈希索引（key → 池中下标）：旧实现每条 entry 在池里线性扫，
  // n 项列表一次同步是 O(n²) 字符串比较——几百项的搜索结果每帧同步一次
  //（声明式重组每帧调 sync_items）时在帧剖里排得进前几名。
  // 池只在本地存活，索引建一次用一次；key 理论上可重复（重复时后一个拿不到索引，
  // 走新建路径——与旧实现「先命中先拿」的语义一致）。
  std::unordered_map<std::string_view, std::size_t> pool_index;
  pool_index.reserve(pool.size() * 2);
  for (std::size_t at = 0; at < pool.size(); ++at) {
    if (pool[at] != nullptr) pool_index.emplace(pool[at]->key(), at);
  }
  for (const auto& entry : entries) {
    // 同 key 复用已有元素：**id、选中态、焦点都保持**（这正是 sync 与 clear+add 的区别）
    std::unique_ptr<ListItem> node;
    if (const auto found = pool_index.find(entry.key); found != pool_index.end()) {
      node = std::move(pool[found->second]);
      pool_index.erase(found);   // 每个元素只能被拿一次
    }
    if (node == nullptr) {
      node = std::make_unique<ListItem>(entry.label, entry.subtitle);
      node->set_key(entry.key);
    } else {
      node->set_label(entry.label);  // 文案可能变了，元素不变
      node->set_subtitle(entry.subtitle);
    }
    // 无论是新建还是复用，都要接上激活链路（漏了就是"点了没反应"）；
    // 数据层给了 `on_activate` 就用它（否则用 `set_on_select` 的默认行为）。
    if (entry.on_activate) {
      node->set_on_activate([callback = entry.on_activate](std::size_t) { callback(); });
    } else {
      bind_item(*node, next.size());
    }
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

// —— 列表类通用视口契约（与 `Tree` 同口径）——

auto List::visible_item_count() const noexcept -> std::size_t {
  const std::size_t total = children_.size();
  if (bounds_.is_empty()) return total == 0 ? 1 : total;
  const auto count = static_cast<std::size_t>(std::max(1.0f, bounds_.height / ListItem::kHeight));
  return std::max<std::size_t>(1, count);
}

auto List::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "items" || name == "count" || name == "rows") {
    return std::to_string(children_.size());
  }
  // 列表由外部 ScrollView 承载，自身不滚动（同 Tree）。
  if (name == "first_visible") return std::string("0");
  if (name == "visible_rows" || name == "visible_items") {
    return std::to_string(visible_item_count());
  }
  if (name == "scroll") return std::string("0.0");
  if (name == "selected") {
    return selected_ == kNoSelection ? std::string("-1") : std::to_string(selected_);
  }
  if (name == "selected_key") {
    if (selected_ == kNoSelection || selected_ >= children_.size()) return std::string();
    const Element* child = child_at(selected_);
    return child != nullptr ? child->key() : std::string();
  }
  return Element::get_property(name);
}

auto List::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.push_back("items");
  names.push_back("count");
  names.push_back("rows");
  names.push_back("first_visible");
  names.push_back("visible_items");
  names.push_back("visible_rows");
  names.push_back("scroll");
  names.push_back("selected");
  names.push_back("selected_key");
  return names;
}

}  // namespace st::ui
