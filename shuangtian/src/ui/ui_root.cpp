#include "st/ui/ui_root.hpp"

#include <algorithm>
#include <format>

#include "st/core/log.hpp"

namespace st::ui {

UiRoot::UiRoot() : theme_(Theme::light()) { dirty_rect_ = math::IntRect{}; }

UiRoot::~UiRoot() = default;

void UiRoot::set_content(std::unique_ptr<Element> content) {
  content_ = std::move(content);
  if (content_ != nullptr) {
    content_->mark_layout_dirty();
    assign_ids(*content_, "root");
  }
  focused_ = nullptr;
  hovered_ = nullptr;
  pressed_ = nullptr;
  mark_dirty_all();
}

void UiRoot::set_theme(Theme theme) {
  theme_ = std::move(theme);
  mark_dirty_all();
}

void UiRoot::set_text_port(const TextPort* port) {
  text_port_ = port;
  mark_dirty_all();
}

void UiRoot::set_viewport(math::Size size) {
  if (size.width == viewport_.width && size.height == viewport_.height) return;
  viewport_ = size;
  mark_dirty_all();
}

void UiRoot::assign_ids(Element& element, const std::string& prefix) {
  const std::string path = element.id().empty() ? prefix : element.id();
  if (element.id().empty()) element.set_id(path);
  for (std::size_t index = 0; index < element.children().size(); ++index) {
    Element* child = element.child_at(index);
    if (child == nullptr) continue;
    assign_ids(*child, std::format("{}/{}[{}]", path, child->type(), index));
  }
}

auto UiRoot::render_context() const -> RenderContext {
  RenderContext context{theme_, text_port_, time_seconds_};
  return context;
}

void UiRoot::layout(bool force) {
  if (content_ == nullptr) return;
  if (!force && !dirty_) return;
  const RenderContext context = render_context();
  const auto apply_theme_tree = [](auto&& self, Element& element, const Theme& theme) -> void {
    element.apply_theme(theme);
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      self(self, *element.child_at(index), theme);
    }
  };
  apply_theme_tree(apply_theme_tree, *content_, theme_);
  for (auto& overlay : overlays_) apply_theme_tree(apply_theme_tree, *overlay, theme_);
  Constraints constraints;
  constraints.max_width = viewport_.width;
  constraints.max_height = viewport_.height;
  constraints.available_width = viewport_.width;
  constraints.available_height = viewport_.height;

  content_->measure(context, constraints);
  const math::Size measured = content_->measured_size();
  // 根内容按"**至少铺满视口**"排布（内容更高时保持自然高度，不外溢裁剪）：
  // 这样根页面里的 `style().grow` 子项（如撑满窗口的编辑器/滚动区）能真正拿到剩余空间；
  // 若按内容自然高度排布，`grow` 在根层就退化成无效属性（页面永远"上半截"）。
  const float width = std::max(measured.width, viewport_.width);
  const float height = std::max(measured.height, viewport_.height);
  layout_subtree(*content_, math::Rect{0.0f, 0.0f, width, height});

  float overlay_offset = 0.0f;
  for (auto& overlay : overlays_) {
    overlay->measure(context, constraints);
    const math::Size overlay_size = overlay->measured_size();
    layout_subtree(*overlay,
                   math::Rect{0.0f, overlay_offset, overlay_size.width, overlay_size.height});
    overlay_offset += overlay_size.height;
  }

  dirty_ = false;
  dirty_rect_ = math::IntRect{0, 0, static_cast<int>(viewport_.width),
                              static_cast<int>(viewport_.height)};
  ++version_;
}

void UiRoot::layout_subtree(Element& element, math::Rect rect) {
  const RenderContext context = render_context();
  element.arrange(context, rect);
}

/// 绘制后扫一遍：还有元素在悬浮过渡中就请求下一帧。
///
/// 为什么要这一次遍历：过渡动画需要**连续帧**，而帧末 `clear_dirty()` 会清脏。
/// 遍历只做指针判读（元素数量级几百），相对一次绘制可以忽略；
/// 换来的是"淡入真的是淡入"而不是卡在第一格。
void UiRoot::collect_animation_requests() {
  animation_pending_ = false;
  const auto walk = [this](auto&& self, Element& element) -> void {
    if (element.hover_animating()) {
      animation_pending_ = true;
      element.clear_hover_animating();  // 消费：元素在下一次绘制里重新置位
      return;
    }
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      self(self, *element.child_at(index));
      if (animation_pending_) return;
    }
  };
  if (content_ != nullptr) walk(walk, *content_);
  for (const auto& overlay : overlays_) {
    if (animation_pending_) break;
    if (overlay != nullptr) walk(walk, *overlay);
  }
}

void UiRoot::paint(raster::Surface& canvas) {
  layout();
  const RenderContext context = render_context();
  for (auto& overlay : overlays_) overlay->paint(context, canvas);
  collect_animation_requests();
  if (content_ != nullptr) paint_subtree(context, *content_, canvas);
}

void UiRoot::paint_subtree(const RenderContext& context, Element& element, raster::Surface& canvas) {
  element.paint(context, canvas);
}

auto UiRoot::hit_test(math::Point point) -> Element* {
  for (auto iterator = overlays_.rbegin(); iterator != overlays_.rend(); ++iterator) {
    if (Element* hit = hit_test_subtree(**iterator, point); hit != nullptr) return hit;
  }
  if (content_ == nullptr) return nullptr;
  return hit_test_subtree(*content_, point);
}

auto UiRoot::hit_test_subtree(Element& element, math::Point point) -> Element* {
  if (!element.visible()) return nullptr;
  const auto children = element.children();
  for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator) {
    if (Element* hit = hit_test_subtree(**iterator, point); hit != nullptr) return hit;
  }
  return element.hit_test(point) ? &element : nullptr;
}

auto UiRoot::dispatch_to(Element& element, Event& event) -> bool {
  const RenderContext context = render_context();
  bool handled = false;
  for (Element* current = &element; current != nullptr; current = current->parent()) {
    if (current->on_event(context, event)) {
      handled = true;
      break;
    }
  }
  // 脚本桥：在**元素自身处理之后**通知观察者（各分支都经此函数，命中元素即 `element`）。
  //
  // 顺序很关键：若在 C++ 处理**之前**通知，脚本写入会被随后的 C++ 处理器覆盖，
  // 表现为"用 JS 改了界面却没生效"（实测踩过：语言标签点击后状态栏仍是 C++ 写的文案）。
  // 放在之后 = 脚本看到的是处理后的状态，且它的写入是最终态。
  if (event_observer_) event_observer_(event, element);
  return handled;
}

void UiRoot::update_hover(Element* target) {
  if (hovered_ == target) return;
  prune_stale_pointers();  // `hovered_` 可能已悬垂（子树被重建）
  const RenderContext context = render_context();
  if (hovered_ != nullptr) {
    hovered_->set_hovered(false);
    Event event;
    event.kind = EventKind::HoverOut;
    hovered_->notify_hover(false);  // 回调与事件并行：局部逻辑不必自己去解析事件流
    (void)dispatch_to(*hovered_, event);
  }
  hovered_ = target;
  if (hovered_ != nullptr) {
    hovered_->set_hovered(true);
    Event event;
    event.kind = EventKind::HoverIn;
    hovered_->notify_hover(true);
    (void)dispatch_to(*hovered_, event);
  }
  for (Element* current = target; current != nullptr; current = current->parent()) {
    current->mark_dirty();
  }
  ++version_;
  (void)context;
}

auto UiRoot::dispatch(Event& event) -> bool {
  // 分发前先清悬垂指针：界面每帧都可能重建子树（列表刷新、页面替换），
  // 而焦点/悬停/按压指针可能正指着已被销毁的元素
  prune_stale_pointers();
  layout();
  bool handled = false;

  switch (event.kind) {
    case EventKind::MouseMove: {
      Element* target = hit_test(event.position);
      update_hover(target);
      handled = dispatch_to(target != nullptr ? *target : *content_, event);
      break;
    }
    case EventKind::MouseDown: {
      Element* target = hit_test(event.position);
      pressed_ = target;
      if (target != nullptr) {
        if (target->focusable() && focused_ != target) set_focus(target);
        target->set_pressed(true);
        target->mark_dirty();
      }
      handled = target != nullptr && dispatch_to(*target, event);
      break;
    }
    case EventKind::MouseUp:
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      Element* target = hit_test(event.position);
      if (pressed_ != nullptr) {
        pressed_->set_pressed(false);
        pressed_->mark_dirty();
      }
      pressed_ = nullptr;
      handled = target != nullptr && dispatch_to(*target, event);
      if (!handled && target != nullptr && event.kind == EventKind::Click) {
        target->activate();
        handled = true;
      }
      break;
    }
    case EventKind::Wheel: {
      Element* target = hit_test(event.position);
      handled = target != nullptr && dispatch_to(*target, event);
      break;
    }
    case EventKind::KeyDown: {
      if (event.key == "Tab" && focused_ != nullptr) {
        focus_next(event.shift);
        handled = true;
        break;
      }
      handled = focused_ != nullptr && dispatch_to(*focused_, event);
      break;
    }
    case EventKind::KeyUp:
    case EventKind::TextInput: {
      handled = focused_ != nullptr && dispatch_to(*focused_, event);
      break;
    }
    case EventKind::FocusIn:
    case EventKind::FocusOut:
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      handled = false;
      break;
  }
  if (handled) ++version_;
  return handled;
}

auto UiRoot::find(std::string_view id) -> Element* {
  if (content_ == nullptr) return nullptr;
  Element* found = nullptr;
  const auto walk = [&](auto&& self, Element& element) -> void {
    if (found != nullptr) return;
    if (element.derived_id() == id) {
      found = &element;
      return;
    }
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      self(self, *element.child_at(index));
      if (found != nullptr) return;
    }
  };
  walk(walk, *content_);
  return found;
}

auto UiRoot::query(const Selector& selector, std::size_t limit) -> std::vector<Element*> {
  std::vector<Element*> matches;
  if (content_ == nullptr) return matches;
  const auto walk = [&](auto&& self, Element& element) -> void {
    if (limit != 0 && matches.size() >= limit) return;
    if (selector.matches_with_ancestors(element)) matches.push_back(&element);
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      self(self, *element.child_at(index));
      if (limit != 0 && matches.size() >= limit) return;
    }
  };
  walk(walk, *content_);
  return matches;
}

void UiRoot::prune_stale_pointers() {  if (focused_ == nullptr && hovered_ == nullptr && pressed_ == nullptr) return;
  // 只做指针相等比较：候选指针可能已指向销毁的元素，**绝不能解引用**
  const auto on_tree = [this](const Element* candidate) -> bool {
    if (candidate == nullptr) return false;
    const auto walk = [&candidate](auto&& self, const Element& node) -> bool {
      if (&node == candidate) return true;
      for (std::size_t index = 0; index < node.children().size(); ++index) {
        if (self(self, *node.child_at(index))) return true;
      }
      return false;
    };
    if (content_ != nullptr && walk(walk, *content_)) return true;
    for (const auto& overlay : overlays_) {
      if (overlay != nullptr && walk(walk, *overlay)) return true;
    }
    return false;
  };
  if (!on_tree(focused_)) focused_ = nullptr;
  if (!on_tree(hovered_)) hovered_ = nullptr;
  if (!on_tree(pressed_)) pressed_ = nullptr;
}

auto UiRoot::focused() -> Element* {
  // 协议/脚本/动作层都靠这个访问器拿焦点元素——它们会**直接解引用**返回值，
  // 所以悬垂判断必须在这里做（不能指望调用方自己检查）
  prune_stale_pointers();
  return focused_;
}

void UiRoot::set_focus(Element* element) {
  if (focused_ == element) return;
  prune_stale_pointers();  // `focused_` 可能已悬垂：清掉再走 FocusOut 通告
  const RenderContext context = render_context();
  if (focused_ != nullptr) {
    focused_->set_focused(false);
    Event event;
    event.kind = EventKind::FocusOut;
    (void)dispatch_to(*focused_, event);
    focused_->mark_dirty();
  }
  focused_ = element;
  if (focused_ != nullptr) {
    focused_->set_focused(true);
    Event event;
    event.kind = EventKind::FocusIn;
    (void)dispatch_to(*focused_, event);
    focused_->mark_dirty();
  }
  ++version_;
  (void)context;
}

void UiRoot::collect_focus_order(Element& element, std::vector<Element*>& order) {
  if (!element.visible() || !element.enabled()) return;
  if (element.focusable()) order.push_back(&element);
  for (std::size_t index = 0; index < element.children().size(); ++index) {
    collect_focus_order(*element.child_at(index), order);
  }
}

void UiRoot::focus_next(bool backwards) {
  if (content_ == nullptr) return;
  std::vector<Element*> order;
  collect_focus_order(*content_, order);
  if (order.empty()) return;
  if (focused_ == nullptr) {
    set_focus(order.front());
    return;
  }
  const auto iterator = std::ranges::find(order, focused_);
  if (iterator == order.end()) {
    set_focus(order.front());
    return;
  }
  const auto index = static_cast<std::ptrdiff_t>(std::distance(order.begin(), iterator));
  const auto size = static_cast<std::ptrdiff_t>(order.size());
  const std::ptrdiff_t next = backwards ? (index - 1 + size) % size : (index + 1) % size;
  set_focus(order[static_cast<std::size_t>(next)]);
}

auto UiRoot::semantics(std::uint32_t max_depth) const -> SemanticsNode {
  SemanticsNode root;
  root.id = "root";
  root.type = "Root";
  root.role = Role::Panel;
  root.bounds = math::Rect{0.0f, 0.0f, viewport_.width, viewport_.height};
  root.flags = SemanticsFlags{};

  if (content_ != nullptr) {
    SemanticsNode node;
    content_->collect_semantics(node);
    root.children.push_back(std::move(node));
  }
  for (const auto& overlay : overlays_) {
    SemanticsNode node;
    overlay->collect_semantics(node);
    root.children.push_back(std::move(node));
  }

  if (max_depth > 0) {
    const auto prune = [](auto&& self, SemanticsNode& node, std::uint32_t depth,
                          std::uint32_t limit) -> void {
      if (depth + 1 >= limit) {
        node.children.clear();
        return;
      }
      for (auto& child : node.children) self(self, child, depth + 1, limit);
    };
    prune(prune, root, 0, max_depth);
  }
  return root;
}

auto UiRoot::visual_tree() const -> VisualNode {
  VisualNode root;
  root.id = "root";
  root.type = "Root";
  root.bounds = math::Rect{0.0f, 0.0f, viewport_.width, viewport_.height};
  root.fill = theme_.colors().bg.to_css();
  if (content_ != nullptr) {
    VisualNode node;
    node.depth = 1;
    content_->collect_visual(node);
    root.children.push_back(std::move(node));
  }
  for (const auto& overlay : overlays_) {
    VisualNode node;
    node.depth = 1;
    overlay->collect_visual(node);
    root.children.push_back(std::move(node));
  }
  return root;
}

void UiRoot::mark_dirty_all() {
  dirty_ = true;
  ++version_;
  if (content_ != nullptr) {
    const auto walk = [](auto&& self, Element& element) -> void {
      element.mark_layout_dirty();
      for (std::size_t index = 0; index < element.children().size(); ++index) {
        self(self, *element.child_at(index));
      }
    };
    walk(walk, *content_);
  }
}

void UiRoot::clear_dirty() noexcept {
  // 动画未结束就**保持脏**：元素在本次绘制里声明的"还在动"是下一帧的依据。
  // 清掉它会让动画停在第一帧；而每帧重新声明，所以动画结束后重绘会自然停下。
  //
  // `animation_pending_` 由 `paint()` 在绘制后汇总（遍历一次），这里只消费。
  dirty_ = animation_pending_;
  animation_pending_ = false;
  dirty_rect_ = math::IntRect{};
}

void UiRoot::add_overlay(std::unique_ptr<Element> overlay) {
  if (overlay == nullptr) return;
  assign_ids(*overlay, std::format("overlay[{}]", overlays_.size()));
  overlays_.push_back(std::move(overlay));
  mark_dirty_all();
}

auto UiRoot::overlay_at(std::size_t index) const noexcept -> Element* {
  return index < overlays_.size() ? overlays_[index].get() : nullptr;
}

void UiRoot::remove_overlay(Element* overlay) {
  for (auto iterator = overlays_.begin(); iterator != overlays_.end(); ++iterator) {
    if (iterator->get() == overlay) {
      overlays_.erase(iterator);
      mark_dirty_all();
      return;
    }
  }
}

void UiRoot::clear_overlays() {
  overlays_.clear();
  mark_dirty_all();
}

}  // namespace st::ui
