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

void SplitView::set_min_first_size(float value) {
  const float clamped = std::max(0.0f, value);
  if (clamped == min_first_size_) return;
  min_first_size_ = clamped;
  // 最小宽只影响**布局时的生效比例**（见 `layout_children`），不改 `ratio_` 本身——
  // 否则窗口放大后侧栏会停在“被抬过的比例”上，回不到用户设定的那个。
  mark_layout_dirty();
}

void SplitView::set_default_ratio(float value) {
  // 只记目标，**不动当前比例**（见头文件：声明式每帧都走 configure）。
  default_ratio_ = clamp_ratio(value);
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
  // 大小 = 父级**实际可用**空间：用 `fill_width/fill_height`（`max_*` 在行布局里是
  // `kUnbounded`——分栏既不 shrink-to-fit，也不该在无界空间里猜尺寸）。
  //
  // ⚠ 直接取 `max_width` 会让本组件以为自己在无界空间里：于是退化到
  // “两侧面板自然尺寸之和”，据此给子面板分宽度；而 `layout_children` 拿的是
  // **真实可用宽**（`arrange` 给的 rect）——两者分家，表现是「内容按窄宽排版、
  // 却画在宽控件里」（实测：精选文字高亮在行尾提前断开、行宽随窗口变化不重排）。
  float width = fill_width(constraints, style_.width, kUnbounded);
  float height = fill_height(constraints, style_.height, kUnbounded);

  // **有界时：给子面板**按比例算出的有限约束**（与 `layout_children` 同一几何）。
  //
  // 为何非做不可：旧实现在此处只有一个 `loose`（全 unbounded）分支——有界也拿它去
  // measure 子面板，于是里面的**内容型组件**（MarkdownView / CodeEditor / Text）
  // 拿不到可用宽，各自落回自己的兜底常量（MarkdownView 是 `kDefaultViewWidth`=640），
  // 排版宽因此永遠是 608。而 `arrange` 阶段又给了正确 rect——
  // 两者不一致的表现为「面板拉伸/拖分割条时文字不重排」（实测：侧栏 0.22→0.40，
  // 控件宽 957.8→736.8 而 `layout_width` 恒为 608）。
  //
  // 注意：arrange 只给 rect、**不会重 measure**，所以这里给错就永远错。
  // （`arrange` 阶段会用已确定的真实矩形再重测一次，见那里的说明。）
  const auto measure_tiled = [&]() { measure_with(context, width, height); };

  if (width < kUnbounded && height < kUnbounded) {
    // 两轴都有界：直接按比例给子面板有限约束（常规路径）。
    measure_tiled();
    measured_ = math::Size{width, height};
    return;
  }

  // 至少一轴无界：先用 unbounded 探子面板的自然尺寸（拿它推自身尺寸），
  // 再在**已知的那一轴**上按比例重测一遍——否则内容型组件会把兜底常量当可用宽。
  {
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
        // 无界主轴时不再加手柄尺寸：手柄不占布局（见 `handle_rect`）。
        width = horizontal ? first_w + second_w : std::max(first_w, second_w);
      }
      if (height >= kUnbounded) {
        height = horizontal ? std::max(first_h, second_h) : first_h + second_h;
      }
    }
    // 尺寸已定：**用有限约束重测子面板**（主轴上按 ratio 分）。
    // 少了这一步，无界那一轴上的子组件就永远停在探测期的兜底尺寸。
    measure_tiled();
  }
  measured_ = math::Size{width, height};
}

void SplitView::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  // **用已确定的几何重测子面板**（与 `ScrollView::arrange` 同一套路）。
  //
  // 为何必须：`measure` 阶段拿到的可用宽是**父级给的**，而 Row 容器给 grow 子节点的是
  // 无界上界（Flex 的 shrink-to-fit 语义）——于是分栏在同排还有兄弟元素时
  //（gbcode：活动栏 44px + 分栏）会**高估**自己的宽，据此给子面板分的宽度就偏大；
  // 而 `layout_children` 用的是这里的真实 rect。两边分家 = 「内容按一个宽排版、
  // 却画在另一个宽的控件里」（实测：选中高亮在行尾提前断开、行宽随窗口变化不重排）。
  //
  // 代价：每个可见子面板多一次 `measure`（排版，不绘制）——换来“排版宽 == 控件宽”。
  //
  // ⚠ **每帧无条件重测**，不能加“几何变了才测”的早退：`measure` 阶段拿到的可用宽
  // 是父级给的**粗口径**（Row 里 `available_width` 不扣同排固定兄弟，如 gbcode 的
  // 活动栏 44px），于是**每帧**都会把子面板按偏大的宽重排一次；若只在 rect 变化时
  // 重测，第二帧起就不修了→最终停在 measure 的错误宽度（实测：加了早退后
  // `layout_width` 仍为按 1280 算的 985.6，而控件宽是 982.4）。
  // 重测本身便宜：内容型组件在宽归一到同一值时跳过重排（见 MarkdownView::measure）。
  if (!bounds_.is_empty()) {
    measure_with(context, bounds_.width, bounds_.height);
  }
  layout_children(context);
  layout_dirty_ = false;
}

/// 按**给定几何**给两侧子面板按比例分有限约束（`measure` 与 `arrange` 共用一份）。
///
/// 抽出来的理由：分栏的“子面板该拿多宽”在测量与排布两处必须逐像素一致，
/// 而 `arrange` 阶段已知道真实矩形——它重测一次就是为了把**宽度定下来**，
/// 让内容型子组件（MarkdownView/CodeEditor）不再停在“宽度未定”时的结果。
void SplitView::measure_with(const RenderContext& context, float width, float height) {
  const bool horizontal = orientation_ == Orientation::Horizontal;
  if (first_hidden_ || second_hidden_) {
    Constraints whole;
    whole.max_width = width;
    whole.max_height = height;
    whole.available_width = width;
    whole.available_height = height;
    for (std::size_t index = 0; index < children_.size() && index < 2; ++index) {
      Element* child = children_[index].get();
      if (child == nullptr) continue;
      const bool hidden = (index == 0 && first_hidden_) || (index == 1 && second_hidden_);
      if (hidden) continue;
      child->measure(context, whole);
    }
    return;
  }
  // 主轴按 ratio 直接分（不再减手柄：手柄不占布局，与 `layout_children` 同几何）。
  const float usable = std::max(0.0f, horizontal ? width : height);
  const float first_main = usable * ratio_;
  const float second_main = usable - first_main;
  for (std::size_t index = 0; index < children_.size() && index < 2; ++index) {
    Element* child = children_[index].get();
    if (child == nullptr) continue;
    Constraints part;
    // 主轴按 ratio 分；交叉轴拿满（与 `layout_children` 的 rect 逐项对应）。
    part.max_width = horizontal ? (index == 0 ? first_main : second_main) : width;
    part.max_height = horizontal ? height : (index == 0 ? first_main : second_main);
    part.available_width = part.max_width;
    part.available_height = part.max_height;
    child->measure(context, part);
  }
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
  // ## 手柄**贴边重叠**，不占布局空间（2026-10-18 修）
  //
  // 旧实现让手柄占据 8px 布局空间（`usable = main - handle; 面板 + 手柄 + 面板`），
  // 而 `SplitView` 自身 `background` 是全透明——于是那 8px 露出的是**页面底色**，
  // 与两侧面板色都不同，在屏幕上就是一条“有宽度的填充带”；中间再叠 1px 线，
  // 观感就是“填充 + 边框”的条块（用户原话：分割线显示一条线就行，
  // 不要有填充宽度和边框）。实测像素：侧栏 (30,30,36) → 4px (17,17,20)
  // → 1px (76,76,84) → 4px (17,17,20) → 编辑区 (13,13,16)。
  //
  // 现在：两侧面板**直接相邻**（见 `layout_children`），手柄矩形以边界为中轴
  // 向两侧各张开一半——纯命中区，不参与布局。视觉上两面板紧贴，只剩那条 1px 线。
  const float boundary = bounds_.x + (horizontal ? main_extent() * ratio_ : 0.0f);
  const float half = handle_size_ * 0.5f;
  if (horizontal) {
    return math::Rect{boundary - half, bounds_.y, handle_size_, bounds_.height};
  }
  return math::Rect{bounds_.x, bounds_.y + main_extent() * ratio_ - half, bounds_.width,
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
  // 比例 → 主轴像素；再按**最小绝对宽度**抬一次（见 `set_min_first_size`）。
  //
  // 为何在布局时算而不是在 `set_ratio` 里算：比例是“占多大份额”，与窗口无关；
  // 而“至少得放得下多少”依赖当前主轴像素——窗口被高 DPI 夹小时，同一个 0.2
  // 算出的绝对宽会小到使侧栏标题变成“资…”。
  float effective_ratio = ratio_;
  const float total_main = main_extent();
  if (min_first_size_ > 0.0f && total_main > 0.0f) {
    const float low = std::clamp(min_ratio_, kMinRatioFloor, 0.5f);
    const float max_first = total_main * (1.0f - low);   // 不能把第二面板挤没
    const float wanted = std::min(min_first_size_, max_first);
    if (total_main * effective_ratio < wanted) effective_ratio = wanted / total_main;
  }
  const float first_main = total_main * effective_ratio;
  const float second_main = total_main - first_main;

  // **两侧面板直接相邻，不为手柄留缝**（2026-10-18）：
  // 手柄只存在于命中层（`handle_rect` 以边界为中轴张开），不参与布局。
  // 这样分割线在屏幕上就是“两个面板贴在一起 + 一条 1px 线”，而不是
  // “面板 + 一条 8px 的页面底色缝 + 面板”。
  if (horizontal) {
    first->arrange(context, math::Rect{bounds_.x, bounds_.y, first_main, bounds_.height});
    second->arrange(context,
                    math::Rect{bounds_.x + first_main, bounds_.y, second_main,
                               bounds_.height});
  } else {
    first->arrange(context, math::Rect{bounds_.x, bounds_.y, bounds_.width, first_main});
    second->arrange(context, math::Rect{bounds_.x, bounds_.y + first_main,
                                        bounds_.width, second_main});
  }
}

auto SplitView::ratio_at(float pointer_main) const noexcept -> float {
  const float usable = std::max(1.0f, main_extent());
  const float base = orientation_ == Orientation::Horizontal ? bounds_.x : bounds_.y;
  // 指针主轴坐标 → 比例。手柄不再占布局空间，所以分隔线就落在 `base + extent*ratio`，
  // 不需要再减那一半手柄（减了会让拖拽点与线错开半个手柄宽）。
  return clamp_ratio((pointer_main - base) / usable);
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
  // 聚焦不再画环：分割线聚焦时环画在手柄位置，视觉上就是分隔线上多出一圈边框
  //（用户要求分割线聚焦不显示边框）。键盘拖动（方向键）仍可用，只是无视觉环。
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
  // 双击/单击手柄归位到 `default_ratio_`（不是写死的 0.5）：
  // 面板的自然尺寸由应用决定（gbcode 侧栏是 1/5、底部面板 1/3），统一归到一半
  // 与“恢复默认布局”的手势语义不符。未设置 default_ratio 时仍为 0.5。
  set_ratio(default_ratio_, true);
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
  if (name == "default_ratio") return std::format("{}", default_ratio_);
  if (name == "min_ratio") return std::format("{}", min_ratio_);
  if (name == "step") return std::format("{}", step_);
  // 只读：手柄命中区尺寸。**给自动化用**——拖拽把手的位置 = 边界 ± 它的一半，
  // 不知道这个值就只能靠猜（旧 e2e 就写死了 `(height - 8) * ratio + 4`，
  // 而默认值从 8 改成 6 后直接抓不到。暴露真值比让每个调用方维护副本可靠）。
  if (name == "handle_size") return std::format("{}", handle_size_);
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
  if (name == "default_ratio") {
    const auto parsed = parse_f64(value);
    if (!parsed.has_value()) return false;
    set_default_ratio(static_cast<float>(*parsed));
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
  // `handle_size` 是**只读**的（不列在 `set_property` 里）：它供自动化计算
  // 拖拽把手的真实落点，改了它只会让命中区与视觉错位。
  return {"ratio",       "default_ratio", "min_ratio", "step",      "orientation",
          "first_hidden", "second_hidden", "handle_size"};
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
    set_ratio(default_ratio_, true);
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
