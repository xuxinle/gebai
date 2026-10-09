#include "st/ui/components/split_view.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::paint_focus_ring;

namespace {

/// 最小占比的合法区间：低于 0.02 手柄自身都放不下；高于 0.5 两侧倒挂。
constexpr float kMinRatioFloor = 0.02f;
/// 键盘步长上限：一次挪不能超过一半（否则「步进」与「拖到底」没区别）。
constexpr float kMaxStep = 0.25f;
/// 手柄命中区下限：比视觉线宽宽，保证可抓（见头文件）。
constexpr float kMinHandleSize = 4.0f;
/// 手柄命中区上限：太宽会吃掉两侧面板的点击区（内容里的按钮点不到）。
constexpr float kMaxHandleSize = 24.0f;

}  // namespace

SplitView::SplitView() { set_focusable(true); }

SplitView::SplitView(Orientation orientation) : orientation_(orientation) { set_focusable(true); }

void SplitView::set_first(std::unique_ptr<Element> panel) {
  // 面板住在 `children_[0]`：先拿旧的、再替换（保持 child 数量不变）。
  if (child_count() > 0) {
    (void)remove_child(child_at(0));
    insert_child(0, std::move(panel));
  } else {
    add_child(std::move(panel));
  }
  mark_layout_dirty();
}

void SplitView::set_second(std::unique_ptr<Element> panel) {
  // 第二面板恒在 `children_[1]`（单面板时就是 `children_[0]` 的追加）。
  if (child_count() > 1) {
    (void)remove_child(child_at(1));
    insert_child(child_count() > 0 ? 1 : 0, std::move(panel));
  } else {
    add_child(std::move(panel));
  }
  mark_layout_dirty();
}

void SplitView::set_orientation(Orientation orientation) {
  if (orientation_ == orientation) return;
  orientation_ = orientation;
  mark_layout_dirty();
}

auto SplitView::clamp_ratio(float value) const noexcept -> float {
  const float low = std::clamp(min_ratio_, kMinRatioFloor, 0.5f);
  const float high = 1.0f - low;
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

void SplitView::set_ratio(float ratio, bool notify) {
  const float clamped = clamp_ratio(ratio);
  if (clamped == ratio_) return;
  ratio_ = clamped;
  mark_layout_dirty();
  if (notify && on_change) on_change(ratio_);
}

void SplitView::set_min_ratio(float value) {
  const float clamped = std::clamp(value, kMinRatioFloor, 0.45f);
  if (clamped == min_ratio_) return;
  min_ratio_ = clamped;
  // 新下限可能让当前比例非法：夹取（可能触发重排）
  set_ratio(ratio_, false);
}

void SplitView::set_step(float step) {
  const float clamped = std::clamp(step, 0.001f, kMaxStep);
  if (clamped == step_) return;
  step_ = clamped;
}

void SplitView::set_handle_size(float size) {
  const float clamped = std::clamp(size, kMinHandleSize, kMaxHandleSize);
  if (clamped == handle_size_) return;
  handle_size_ = clamped;
  mark_layout_dirty();
}

void SplitView::set_second_hidden(bool hidden) {
  if (hidden == second_hidden_) return;
  second_hidden_ = hidden;
  // 隐藏侧子元素的 `visible` 同步：退出绘制与命中（只不 arrange 不够——
  // 它上一帧的 bounds 还在，命中测试照样能点中它）。
  if (Element* second = this->second(); second != nullptr) second->set_visible(!hidden);
  mark_layout_dirty();
}

void SplitView::set_first_hidden(bool hidden) {
  if (hidden == first_hidden_) return;
  first_hidden_ = hidden;
  if (Element* first = this->first(); first != nullptr) first->set_visible(!hidden);
  mark_layout_dirty();
}

void SplitView::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  style_.background = math::Color{0, 0, 0, 0};
  style_.color = enabled_ ? colors.text : colors.text_faint;
  style_.radius = theme.metrics().radius_sm;
}

void SplitView::measure(const RenderContext& context, const Constraints& constraints) {
  const bool horizontal = orientation_ == Orientation::Horizontal;
  // 大小 = 约束给的可得空间（分栏容器自身不做 shrink-to-fit——它的尺寸由父容器决定）；
  // 没有显式尺寸且无上界时取两侧面板的自然尺寸之和（独立摆放时也有个合理值）。
  float width = style_.has_explicit_width() ? style_.width : constraints.max_width;
  float height = style_.has_explicit_height() ? style_.height : constraints.max_height;
  if (width >= kUnbounded || height >= kUnbounded) {
    Constraints loose;
    loose.max_width = kUnbounded;
    loose.max_height = kUnbounded;
    loose.available_width = kUnbounded;
    loose.available_height = kUnbounded;
    float first_w = 0.0f;
    float first_h = 0.0f;
    float second_w = 0.0f;
    float second_h = 0.0f;
    for (std::size_t index = 0; index < children_.size() && index < 2; ++index) {
      Element* child = children_[index].get();
      if (child == nullptr) continue;
      child->measure(context, loose);
      if (index == 0) {
        first_w = child->measured_size().width;
        first_h = child->measured_size().height;
      } else {
        second_w = child->measured_size().width;
        second_h = child->measured_size().height;
      }
    }
    if (child_count() == 0) {
      width = width >= kUnbounded ? 0.0f : width;
      height = height >= kUnbounded ? 0.0f : height;
    } else if (child_count() == 1) {
      if (width >= kUnbounded) width = first_w;
      if (height >= kUnbounded) height = first_h;
    } else {
      if (width >= kUnbounded) {
        width = horizontal ? first_w + handle_size_ + second_w : std::max(first_w, second_w);
      }
      if (height >= kUnbounded) {
        height = horizontal ? std::max(first_h, second_h) : first_h + handle_size_ + second_h;
      }
    }
  }
  measured_ = math::Size{width, height};
}

void SplitView::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  layout_children(context);
  layout_dirty_ = false;
}

auto SplitView::main_extent() const noexcept -> float {
  const float extent =
      orientation_ == Orientation::Horizontal ? bounds_.width : bounds_.height;
  return std::max(0.0f, extent);
}

auto SplitView::handle_rect() const -> math::Rect {
  if (bounds_.is_empty()) return math::Rect{};
  // 任一侧隐藏 ⇒ 无手柄（单面板退化，与 `layout_children` 的几何一致；
  // 空矩形同时让绘制与命中自然失效）。
  if (first_hidden_ || second_hidden_) return math::Rect{};
  const bool horizontal = orientation_ == Orientation::Horizontal;
  // 手柄中心 = 首面板宽 + 手柄一半（与 layout_children 同一份几何：两处必须逐像素一致，
  // 否则「拖到零界」与「画在零界」错位）。首面板宽 = 可用空间 × ratio。
  const float usable = std::max(0.0f, main_extent() - handle_size_);
  const float center = usable * ratio_ + handle_size_ * 0.5f;
  if (horizontal) {
    return math::Rect{bounds_.x + center - handle_size_ * 0.5f, bounds_.y, handle_size_,
                      bounds_.height};
  }
  return math::Rect{bounds_.x, bounds_.y + center - handle_size_ * 0.5f, bounds_.width,
                    handle_size_};
}

void SplitView::layout_children(const RenderContext& context) {
  const bool horizontal = orientation_ == Orientation::Horizontal;
  Element* first = child_count() > 0 ? child_at(0) : nullptr;
  Element* second = child_count() > 1 ? child_at(1) : nullptr;
  // 隐藏侧等同"不存在"（但子元素留在树上）：可见侧单面板退化占满。
  if (first != nullptr && first_hidden_) first = nullptr;
  if (second != nullptr && second_hidden_) second = nullptr;
  // **单面板退化**：只有一侧时它占**全部"主轴空间（不保留手柄、不再按比例分半）——
  // 条件分支里抽掉一侧面板后分栏自然变成单栏，而不是留半屏空白。
  if (first == nullptr || second == nullptr) {
    Element* only = first != nullptr ? first : second;
    if (only != nullptr) only->arrange(context, bounds_);
    return;
  }
  const float grabbed = handle_size_;
  // 两侧可用空间按 ratio 分配（ratio 是「首面板占**可用**空间的比例」）：
  // 含手柄时几何也保持「ratio=0.5 两侧等宽」的直觉。
  const float usable = std::max(0.0f, main_extent() - grabbed);
  const float first_main = usable * ratio_;
  const float second_main = usable - first_main;

  if (horizontal) {
    first->arrange(context, math::Rect{bounds_.x, bounds_.y, first_main, bounds_.height});
    second->arrange(context,
                    math::Rect{bounds_.x + first_main + grabbed, bounds_.y, second_main,
                               bounds_.height});
  } else {
    first->arrange(context, math::Rect{bounds_.x, bounds_.y, bounds_.width, first_main});
    second->arrange(context, math::Rect{bounds_.x, bounds_.y + first_main + grabbed,
                                        bounds_.width, second_main});
  }
}

auto SplitView::ratio_at(float pointer_main) const noexcept -> float {
  const float usable = std::max(1.0f, main_extent() - handle_size_);
  const float base = orientation_ == Orientation::Horizontal ? bounds_.x : bounds_.y;
  // 指针主轴坐标 → 手柄中心 → 比例（手柄中心 = usable·ratio + H/2，解出 ratio）。
  const float center = pointer_main - base - handle_size_ * 0.5f;
  return clamp_ratio(center / usable);
}

void SplitView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  // 单面板：无分界线也无手柄（与 `layout_children` 的退化路径一致——
  // 画一条悬空分隔线会让用户以为还能拖）。`handle_rect` 在隐藏侧时空矩形，
  // 下面的几何自然退化，但先退出省一笔。
  if (child_count() < 2 || first_hidden_ || second_hidden_) return;
  const auto& colors = context.theme.colors();
  const float width = context.theme.metrics().border_width;
  const bool horizontal = orientation_ == Orientation::Horizontal;
  // 视觉线位置与命中区同一几何（handle_rect 以命中区中心定位；线画在中心）。
  const math::Rect handle = handle_rect();
  const float center = horizontal ? handle.center().x : handle.center().y;
  // 视觉线宽三档：常态 1px 发丝（border）、悬停 2px（border_strong）、拖拽 2px 主色。
  // 手柄命中区（handle_size_ 宽）比视觉线宽得多——看起来一条线、抓起来不费劲。
  const float line = dragging_ ? std::max(width * 2.0f, 2.0f)
                               : (hovered_ ? std::max(width * 2.0f, 2.0f) : width);
  const math::Color color =
      dragging_ ? colors.primary : (hovered_ ? colors.border_strong : colors.border);
  // ⚠ `center` 已经是**绝对坐标**（`handle_rect()` 返回的矩形已含 `bounds_.x/y`）。
  // 这里再写 `bounds_.x + center` 就是**加了两次**：分栏落在 x=44 时，线被画到
  // 361.66 而非 317.66——整整偏出一个 `bounds_.x`，**穿进编辑区的行号栏**，
  // 看起来就是"标签下方多了一条莫名其妙的竖线"（用户实测报的就是这条）。
  // 纵向分支同理（`bounds_.y + center`）。
  if (horizontal) {
    const math::Rect line_rect{center - line * 0.5f, bounds_.y, line, bounds_.height};
    canvas.fill_rect(line_rect, raster::Paint::solid(color));
  } else {
    const math::Rect line_rect{bounds_.x, center - line * 0.5f, bounds_.width, line};
    canvas.fill_rect(line_rect, raster::Paint::solid(color));
  }
  // 拖拽把手（居中的短粗条）：hover/拖拽时出现，静态时隐藏（安静的分隔线形态）。
  if (hovered_ || dragging_) {
    const float grip = 24.0f;
    const float thickness = 3.0f;
    const math::Color grip_color = dragging_ ? colors.primary : colors.border_strong;
    if (horizontal) {
      const math::Rect grip_rect{center - thickness * 0.5f,
                                 bounds_.center().y - grip * 0.5f, thickness, grip};
      canvas.fill_rect(grip_rect, raster::Paint::solid(grip_color),
                       thickness * 0.5f);
    } else {
      const math::Rect grip_rect{bounds_.center().x - grip * 0.5f,
                                 center - thickness * 0.5f, grip, thickness};
      canvas.fill_rect(grip_rect, raster::Paint::solid(grip_color),
                       thickness * 0.5f);
    }
  }
  if (focused_) paint_focus_ring(context, canvas, handle_rect(), 0.0f);
}

auto SplitView::on_event(const RenderContext& context, Event& event) -> bool {
  if (!enabled_) return false;
  // 任一侧隐藏时无手柄：不做拖拽（`handle_rect` 空矩形，命中判断自然不过）。
  const bool horizontal = orientation_ == Orientation::Horizontal;
  const auto pointer_main = [&](math::Point point) {
    return horizontal ? point.x : point.y;
  };
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseDown: {
      // 点击手柄（命中区内）→ 开始拖拽；点在面板上不拦截（交给面板内的子元素）。
      const math::Rect handle = handle_rect();
      if (!handle.contains(event.position)) return false;
      dragging_ = true;
      hovered_ = true;
      // 抓取点相对手柄中心的偏移：拖拽保持相对位置（不跳变到指针正中）
      const float center_main =
          pointer_main(math::Point{handle.x + handle.width * 0.5f,
                                   handle.y + handle.height * 0.5f});
      grab_offset_ = pointer_main(event.position) - center_main;
      mark_dirty();
      return true;
    }
    case EventKind::MouseMove: {
      if (!dragging_) return false;
      (void)context;
      // 拖拽归属（UiRoot 的 pressed_ 锁定本元素）保证拖出手柄也不断——
      // 与 ScrollBar 拖滑块同一契约。`grab_offset_` 保持抓取点相对位置（不跳变），
      // 几何一律经 `ratio_at`（与 handle_rect/paint 同一份公式，不重复推导）。
      const float target = pointer_main(event.position) - grab_offset_;
      set_ratio(ratio_at(target), true);
      return true;
    }
    case EventKind::MouseUp:
    case EventKind::Click: {
      if (!dragging_) return false;
      dragging_ = false;
      mark_dirty();
      return true;
    }
    case EventKind::KeyDown: {
      const std::string& key = event.key;
      // 方向键语义跟随分栏方向：左右分栏用 ←/→，上下分栏用 ↑/↓（两向都接也不冲突）。
      const bool forward = horizontal ? (key == "ArrowRight") : (key == "ArrowDown");
      const bool backward = horizontal ? (key == "ArrowLeft") : (key == "ArrowUp");
      if (forward) {
        set_ratio(ratio_ + step_, true);
        return true;
      }
      if (backward) {
        set_ratio(ratio_ - step_, true);
        return true;
      }
      if (key == "Home") {
        set_ratio(0.0f, true);
        return true;
      }
      if (key == "End") {
        set_ratio(1.0f, true);
        return true;
      }
      return false;
    }
    default: return false;
  }
}

void SplitView::activate() {
  // 双击手柄归位（0.5）：与 Slider 的「点击即跳」不同——分栏的位置由拖拽/键盘决定，
  // 双击是「回到默认」的常用手势。
  set_ratio(0.5f, true);
}

auto SplitView::semantics_text() const -> std::string {
  return orientation_ == Orientation::Horizontal ? "左右分栏" : "上下分栏";
}

auto SplitView::semantics_value() const -> std::string {
  return std::format("{:.0f}%", static_cast<double>(ratio_ * 100.0f));
}

auto SplitView::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.focused = focused_;
  return flags;
}

auto SplitView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "ratio") return std::format("{}", ratio_);
  if (name == "min_ratio") return std::format("{}", min_ratio_);
  if (name == "step") return std::format("{}", step_);
  if (name == "first_hidden") return first_hidden_ ? std::string("true") : std::string("false");
  if (name == "second_hidden") return second_hidden_ ? std::string("true") : std::string("false");
  if (name == "orientation") {
    return orientation_ == Orientation::Horizontal ? std::string("horizontal")
                                                   : std::string("vertical");
  }
  return std::nullopt;
}

auto SplitView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "first_hidden" || name == "second_hidden") {
    const bool hidden = value == "true" || value == "1";
    if (name == "first_hidden") {
      set_first_hidden(hidden);
    } else {
      set_second_hidden(hidden);
    }
    return true;
  }
  if (name == "ratio") {
    const auto parsed = parse_f64(value);
    if (!parsed.has_value()) return false;
    set_ratio(static_cast<float>(*parsed), false);
    return true;
  }
  if (name == "min_ratio") {
    const auto parsed = parse_f64(value);
    if (!parsed.has_value()) return false;
    set_min_ratio(static_cast<float>(*parsed));
    return true;
  }
  if (name == "step") {
    const auto parsed = parse_f64(value);
    if (!parsed.has_value()) return false;
    set_step(static_cast<float>(*parsed));
    return true;
  }
  if (name == "orientation") {
    set_orientation(value == "vertical" ? Orientation::Vertical : Orientation::Horizontal);
    return true;
  }
  return false;
}

auto SplitView::property_names() const -> std::vector<std::string_view> {
  return {"ratio", "min_ratio", "step", "orientation", "first_hidden", "second_hidden"};
}

auto SplitView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  // 面板显隐的快捷动作（无参数 = 切换；"true"/"false" = 显式设置）。
  if (action == "toggle_first") {
    const bool explicit_value = argument == "true" || argument == "1";
    const bool hidden = argument.empty() ? !first_hidden_ : !explicit_value;
    set_first_hidden(hidden);
    return true;
  }
  if (action == "toggle_second" || action == "toggle_panel") {
    const bool explicit_value = argument == "true" || argument == "1";
    const bool hidden = argument.empty() ? !second_hidden_ : !explicit_value;
    set_second_hidden(hidden);
    return true;
  }
  if (action == "step_forward" || action == "increase" || action == "next") {
    set_ratio(ratio_ + step_, true);
    return true;
  }
  if (action == "step_backward" || action == "decrease" || action == "previous") {
    set_ratio(ratio_ - step_, true);
    return true;
  }
  if (action == "reset" || action == "center") {
    set_ratio(0.5f, true);
    return true;
  }
  if (action == "set") {
    const auto parsed = parse_f64(argument);
    if (!parsed.has_value()) return false;
    set_ratio(static_cast<float>(*parsed), true);
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
