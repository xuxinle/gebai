#include "st/ui/components/scroll.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace st::ui {
namespace {

[[nodiscard]] auto clamp_scroll(float offset, float maximum) noexcept -> float {
  if (maximum <= 0.0f) return 0.0f;
  if (offset < 0.0f) return 0.0f;
  return offset > maximum ? maximum : offset;
}

}  // namespace

// —— ScrollBar ——

ScrollBar::ScrollBar() {
  style_.width = kBarWidth;
  style_.background = math::Color{0, 0, 0, 0};
}

auto ScrollBar::max_offset() const noexcept -> float {
  const float value = content_height_ - viewport_height_;
  return value > 0.0f ? value : 0.0f;
}

auto ScrollBar::thumb_ratio() const noexcept -> float {
  if (content_height_ <= 0.0f || viewport_height_ <= 0.0f) return 1.0f;
  return std::clamp(viewport_height_ / content_height_, 0.08f, 1.0f);
}

auto ScrollBar::thumb_length() const noexcept -> float {
  const float track = bounds_.height;
  if (track <= 0.0f) return 0.0f;
  const float length = track * thumb_ratio();
  return std::clamp(length, std::min(kMinThumbLength, track), track);
}

auto ScrollBar::thumb_rect() const noexcept -> math::Rect {
  if (bounds_.is_empty()) return math::Rect{};
  const float length = thumb_length();
  const float travel = bounds_.height - length;
  float y = bounds_.y;
  const float maximum = max_offset();
  if (travel > 0.0f && maximum > 0.0f) y += (offset_ / maximum) * travel;
  const float width = hovered_width();
  return math::Rect{bounds_.right() - width, y, width, length};
}

void ScrollBar::set_scroll_geometry(float content_height, float viewport_height,
                                    float offset) noexcept {
  content_height_ = content_height > 0.0f ? content_height : 0.0f;
  viewport_height_ = viewport_height > 0.0f ? viewport_height : 0.0f;
  offset_ = clamp_scroll(offset, max_offset());
  mark_dirty();
}

void ScrollBar::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float width = kBarWidth;
  if (style_.has_explicit_width()) width = style_.width;
  width = std::clamp(width, style_.min_width, style_.max_width);
  float height = constraints.max_height < kUnbounded ? constraints.max_height : 200.0f;
  if (style_.has_explicit_height()) height = style_.height;
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

void ScrollBar::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  bounds_ = rect;
  layout_dirty_ = false;
}

void ScrollBar::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const auto& colors = context.theme.colors();
  const float pill = context.theme.metrics().radius_pill;
  const bool active = hovered_ || dragging_;
  if (active) {
    canvas.fill_rect(bounds_, raster::Paint::solid(colors.surface_alt), pill,
                     raster::DrawOptions{.opacity = 0.7f});
  }
  canvas.fill_rect(thumb_rect(),
                   raster::Paint::solid(active ? colors.text_faint : colors.border_strong), pill);
}

void ScrollBar::apply_pointer(float pointer_y) {
  const float track = bounds_.height;
  const float length = thumb_length();
  const float travel = track - length;
  if (travel <= 0.0f) return;
  float ratio = (pointer_y - grab_ - bounds_.y) / travel;
  ratio = std::clamp(ratio, 0.0f, 1.0f);
  offset_ = ratio * max_offset();
  mark_dirty();
  if (on_scroll_) on_scroll_(offset_);
}

auto ScrollBar::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (event.kind == EventKind::MouseDown) {
    if (!bounds_.contains(event.position)) return false;
    const math::Rect thumb = thumb_rect();
    dragging_ = true;
    if (event.position.y >= thumb.y && event.position.y <= thumb.bottom()) {
      grab_ = event.position.y - thumb.y;
    } else {
      grab_ = thumb.height * 0.5f;
      apply_pointer(event.position.y);
    }
    mark_dirty();
    return true;
  }
  if (event.kind == EventKind::MouseMove) {
    if (!dragging_) return false;
    apply_pointer(event.position.y);
    return true;
  }
  if (event.kind == EventKind::MouseUp || event.kind == EventKind::Click) {
    if (!dragging_) return false;
    dragging_ = false;
    mark_dirty();
    return true;
  }
  return false;
}

auto ScrollBar::hit_test(math::Point point) const noexcept -> bool {
  if (!visible_ || !enabled_) return false;
  return bounds_.inflate(2.0f).contains(point);
}

auto ScrollBar::semantics_value() const -> std::string {
  return std::format("offset={:.0f}", offset_);
}

// —— ScrollView ——

ScrollView::ScrollView() {
  style_.direction = FlexDirection::Column;
  // 滚动视口**裁子元素**（含越出视口的那部分）。
  //
  // 这个开关同时决定绘制与**命中**：`ScrollView::paint` 一直在自己压裁剪矩形，
  // 但命中侧的裁剪读的是 `style_.clip_children`（声明式与手搭元素同一处口径）。
  // 不声明它，滚动后的长内容（如终端滚回）就会“看不见却抢点击”——
  // 实测该文本 bounds 为 y=−119 / 高 851，把**标题栏与菜单栏**的点击全吃掉了。
  style_.clip_children = true;
  set_focusable(true);
  auto bar = std::make_unique<ScrollBar>();
  bar_ = bar.get();
  bar_->set_on_scroll([this](float offset) { scroll_to(offset); });
  bar_->set_visible(false);
  (void)add_child(std::move(bar));
}

auto ScrollView::add_child(std::unique_ptr<Element> child) -> Element* {
  // 内容的插入位置恒在**滚动条之前**（约定见 `content_child_count` 注释）
  if (bar_ == nullptr) return Element::add_child(std::move(child));
  const std::size_t position = children_.empty() ? 0 : children_.size() - 1;
  return Element::insert_child(position, std::move(child));
}

auto ScrollView::insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element* {
  // 外部索引语义只覆盖内容子元素（滚动条不可见）——夹取到 [0, content_child_count()]
  const std::size_t content = content_child_count();
  return Element::insert_child(std::min(index, content), std::move(child));
}

auto ScrollView::reserved_width() const noexcept -> float {
  // 滚动条占位：条宽 + 4px 间隙（与 ScrollBar 的视觉规格保持一致）
  return show_bar_ ? ScrollBar::kBarWidth + 4.0f : 0.0f;
}

auto ScrollView::content_extent(const Element& child) const noexcept -> float {
  const float explicit_height = child.style().height;
  return explicit_height != kAuto ? explicit_height : child.measured_size().height;
}

void ScrollView::recompute_content_height() noexcept {
  float total = style_.padding.vertical();
  bool first = true;
  for (const auto& child : children_) {
    if (child.get() == bar_ || !child->visible()) continue;
    if (!first) total += style_.gap;
    total += content_extent(*child);
    first = false;
  }
  content_height_ = total;
}

auto ScrollView::max_scroll() const noexcept -> float {
  const float value = content_height_ - view_height_;
  return value > 0.0f ? value : 0.0f;
}

void ScrollView::scroll_to(float offset) {
  const float target = clamp_scroll(offset, max_scroll());
  if (target == offset_) return;
  offset_ = target;
  mark_layout_dirty();
  if (on_scroll_) on_scroll_(offset_);
}

void ScrollView::set_show_scrollbar(bool show) noexcept {
  if (show_bar_ == show) return;
  show_bar_ = show;
  if (bar_ != nullptr) bar_->set_visible(show && content_height_ > view_height_ + 0.5f);
  mark_layout_dirty();
}

void ScrollView::measure(const RenderContext& context, const Constraints& constraints) {
  const float reserved = reserved_width();
  const float outer_width =
      style_.has_explicit_width() ? style_.width : constraints.max_width;
  float content_width = kUnbounded;
  if (outer_width < kUnbounded) {
    content_width = std::max(0.0f, outer_width - style_.padding.horizontal() - reserved);
  }

  float total = style_.padding.vertical();
  float child_width = 0.0f;
  bool first = true;
  for (auto& child : children_) {
    if (child.get() == bar_ || !child->visible()) continue;
    Constraints child_constraints;
    child_constraints.max_width = content_width;
    child_constraints.max_height = kUnbounded;
    child_constraints.available_width = content_width;
    child_constraints.available_height = kUnbounded;
    child->measure(context, child_constraints);
    const math::Size size = child->measured_size();
    if (!first) total += style_.gap;
    total += size.height;
    child_width = std::max(child_width, size.width);
    first = false;
  }
  content_height_ = total;

  float width = style_.has_explicit_width() ? style_.width : outer_width;
  if (width >= kUnbounded) width = child_width + style_.padding.horizontal() + reserved;
  width = std::clamp(width, style_.min_width, style_.max_width);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);

  float height = 0.0f;
  if (style_.has_explicit_height()) {
    height = style_.height;
  } else if (constraints.max_height < kUnbounded) {
    height = std::min(content_height_, constraints.max_height);
  } else {
    height = content_height_;
  }
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);

  measured_ = math::Size{width, height};
}

void ScrollView::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  view_height_ = std::max(0.0f, rect.height - style_.padding.vertical());

  const float content_width =
      std::max(0.0f, rect.width - style_.padding.horizontal() - reserved_width());
  // **用确定的内容宽重测内容**（宽度定下来后才测）。
  //
  // 滚动容器常被放进 Row 容器（典型的"侧栏 + 内容区"），而 Row 容器给 grow 子节点的
  // 是**无宽度约束**（那是 Flex 的 shrink-to-fit 语义）。于是 `measure` 阶段算出的尺寸
  // 是"宽度未定时"的结果：换行容器（`Style::wrap`）会被误判成单行、高度只有一行，
  // 内容被裁掉一半（实测：图标全表 72 个只显示 13 个，看着像"列数不够"）。
  // 到 arrange 时宽度已确定，重测一次即可让依赖宽度的布局得到正确尺寸。
  //
  // 代价：每个可见子节点多一次 `measure`（排版阶段，不做绘制）。
  // 换来的是"内容布局不依赖父容器是 Row 还是 Column"这个稳定语义。
  for (auto& child : children_) {
    if (child.get() == bar_ || !child->visible()) continue;
    Constraints constraints;
    constraints.max_width = content_width;
    constraints.max_height = kUnbounded;
    constraints.available_width = content_width;
    constraints.available_height = kUnbounded;
    child->measure(context, constraints);
  }
  recompute_content_height();
  offset_ = clamp_scroll(offset_, max_scroll());
  // 跟随模式：内容量完、偏移归一化后立即回到底（见 `set_follow_end` 的说明——
  // 关键就在这个时刻：`content_height_` 刚更新，`max_scroll()` 已是新值）。
  if (follow_end_) offset_ = max_scroll();

  float cursor = rect.y + style_.padding.top - offset_;
  for (auto& child : children_) {
    if (child.get() == bar_ || !child->visible()) continue;
    const float extent = content_extent(*child);
    child->arrange(context, math::Rect{rect.x + style_.padding.left, cursor, content_width, extent});
    cursor += extent + style_.gap;
  }

  if (bar_ != nullptr) {
    const math::Rect bar_rect{rect.right() - style_.padding.right - ScrollBar::kBarWidth,
                              rect.y + style_.padding.top, ScrollBar::kBarWidth, view_height_};
    bar_->arrange(context, bar_rect);
    bar_->set_visible(show_bar_ && content_height_ > view_height_ + 0.5f);
    bar_->set_scroll_geometry(content_height_, view_height_, offset_);
  }
  layout_dirty_ = false;
}

void ScrollView::paint(const RenderContext& context, raster::Surface& canvas) const {
  if (!visible_) return;
  paint_box(context, canvas);
  paint_content(context, canvas);
  if (style_.radius > 0.0f) {
    canvas.push_clip_rounded_rect(bounds_, style_.radius);
  } else {
    canvas.push_clip_rect(bounds_);
  }
  for (const auto& child : children_) {
    if (child.get() == bar_ || !child->visible()) continue;
    child->paint(context, canvas);
  }
  canvas.pop_clip();
  if (bar_ != nullptr && bar_->visible()) bar_->paint(context, canvas);
}

auto ScrollView::on_event(const RenderContext& context, Event& event) -> bool {
  if (event.kind == EventKind::Wheel) {
    if (max_scroll() <= 0.0f) return false;
    // **滚轮方向**：`wheel_delta` 与系统一致——向上为正、向下为负（Win32 的 WM_MOUSEWHEEL
    // 向上给 +120，X11 的按钮 4 也是上）。偏移量随内容向下增大，所以要取反。
    // 取错符号的表现是“往下滚滚不动”，且**不报错**：事件仍被处理（handled=true），
    // 只是偏移一直被夹在 0——页面下半截因此永远看不到。
    scroll_by(-event.wheel_delta * step_);
    event.handled = true;
    return true;
  }
  if (event.kind == EventKind::KeyDown) {
    if (event.key == "PageDown") {
      scroll_by(view_height_ * kPageRatio);
      return true;
    }
    if (event.key == "PageUp") {
      scroll_by(-view_height_ * kPageRatio);
      return true;
    }
    if (event.key == "Home") {
      scroll_to(0.0f);
      return true;
    }
    if (event.key == "End") {
      scroll_to(max_scroll());
      return true;
    }
    return false;
  }
  if (event.kind == EventKind::MouseMove) {
    if (bar_ != nullptr && bar_->dragging()) return bar_->on_event(context, event);
  }
  return false;
}

auto ScrollView::semantics_value() const -> std::string {
  return std::format("offset={:.0f}", offset_);
}

auto ScrollView::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.scrollable = true;
  return flags;
}

// —— 滚动容器的视口属性（列表类契约的权威来源）——

auto ScrollView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "scroll" || name == "offset") {
    return std::format("{:.1f}", static_cast<double>(offset_));
  }
  if (name == "max_scroll") {
    return std::format("{:.1f}", static_cast<double>(max_scroll()));
  }
  if (name == "viewport_height") {
    return std::format("{:.1f}", static_cast<double>(bounds_.height));
  }
  if (name == "content_height") {
    return std::format("{:.1f}", static_cast<double>(content_height_));
  }
  return Element::get_property(name);
}

auto ScrollView::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.push_back("scroll");
  names.push_back("offset");
  names.push_back("max_scroll");
  names.push_back("viewport_height");
  names.push_back("content_height");
  return names;
}

}  // namespace st::ui
