#include "st/ui/components/window_frame.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace st::ui {
namespace {

/// 边缘条按下时的提示色（低调：它只在用户真的去拖边时出现一瞬）。
constexpr float k_edge_press_alpha = 0.18f;

}  // namespace

WindowFrame::WindowFrame(std::string title) {
  // 窗框本身不参与 Tab 焦点环（与标题栏一致：它是外壳，键盘可达性由宿主负责）。
  set_focusable(false);
  // **总是**建标题栏（即使标题为空）：
  // ① 无参构造是声明式/工厂的硬要求（`make_element` 用 `make_unique<WindowFrame>()`），
  //    而标题由调用方随后 `set_title` 写；
  // ② 无边框窗口的"可拖动区"就在标题栏上——不建的话声明式里拿到的窗框拖不动；
  // ③ 想要"无标题栏的纯内容窗框"用 `set_show_title_bar(false)`（显式、可搜索）。
  auto bar = std::make_unique<TitleBar>(std::move(title));
  bar->set_id("titlebar");
  bar_ = bar.get();
  Element::add_child(std::move(bar));
  auto body = std::make_unique<Panel>(FlexDirection::Column);
  body->set_id("content");
  content_ = body.get();
  Element::add_child(std::move(body));
}

void WindowFrame::set_show_title_bar(bool show) {
  if (show_title_bar_ == show) return;
  show_title_bar_ = show;
  if (bar_ != nullptr) bar_->set_visible(show);
  mark_layout_dirty();
}

void WindowFrame::set_show_resize_edges(bool show) {
  if (show_resize_edges_ == show) return;
  show_resize_edges_ = show;
  mark_dirty();
}

void WindowFrame::set_window_control(WindowControl* control) noexcept {
  control_ = control;
  // 标题栏的窗口动作也经同一端口：一处注入，三按钮与边缘同源。
  if (bar_ != nullptr) bar_->set_window_control(control);
}

auto WindowFrame::resolve_control() const -> WindowControl* { return control_; }

auto WindowFrame::edge_of(std::size_t index) noexcept -> WindowEdge {
  switch (index) {
    case kEdgeLeft: return WindowEdge::Left;
    case kEdgeRight: return WindowEdge::Right;
    case kEdgeTop: return WindowEdge::Top;
    case kEdgeBottom: return WindowEdge::Bottom;
    case kEdgeTopLeft: return WindowEdge::TopLeft;
    case kEdgeTopRight: return WindowEdge::TopRight;
    case kEdgeBottomLeft: return WindowEdge::BottomLeft;
    case kEdgeBottomRight: return WindowEdge::BottomRight;
    default: break;
  }
  return WindowEdge::None;
}

auto WindowFrame::edge_rect_internal(std::size_t index, math::Rect frame) const -> math::Rect {
  const float band = edge_thickness();
  const float width = frame.width;
  const float height = frame.height;
  // 角块边长取 `band * 2`：太小则"角"与"边"分不开（斜向拖拽落到边上），
  // 太大则整条短边都成了角，拖上边也像拖角。2× 是 Win32 自己的比例。
  const float corner = std::min(band * 2.0f, std::min(width, height) * 0.5f);
  switch (index) {
    case kEdgeLeft: return math::Rect{frame.x, frame.y + band, band, height - band * 2.0f};
    case kEdgeRight:
      return math::Rect{frame.right() - band, frame.y + band, band, height - band * 2.0f};
    case kEdgeTop: return math::Rect{frame.x + band, frame.y, width - band * 2.0f, band};
    case kEdgeBottom:
      return math::Rect{frame.x + band, frame.bottom() - band, width - band * 2.0f, band};
    case kEdgeTopLeft: return math::Rect{frame.x, frame.y, corner, corner};
    case kEdgeTopRight: return math::Rect{frame.right() - corner, frame.y, corner, corner};
    case kEdgeBottomLeft: return math::Rect{frame.x, frame.bottom() - corner, corner, corner};
    case kEdgeBottomRight:
      return math::Rect{frame.right() - corner, frame.bottom() - corner, corner, corner};
    default: break;
  }
  return {};
}

auto WindowFrame::edge_rect(std::size_t index) const -> math::Rect {
  if (!show_resize_edges_ || index >= kEdgeCount) return {};
  return edge_rect_internal(index, bounds_);
}

auto WindowFrame::edge_at(math::Point point) const -> WindowEdge {
  if (!show_resize_edges_) return WindowEdge::None;
  // 判定**必须复用后端那份纯函数**：这里若自算一遍，Win32 的 `WM_NCHITTEST`
  // 与组件的命中就会各说各话（"看着能拖边、实际拖不动"这类幽灵问题的来源）。
  const float band = edge_thickness();
  const math::Point local{point.x - bounds_.x, point.y - bounds_.y};
  return resize_edge_at(local, math::Size{bounds_.width, bounds_.height}, band);
}

auto WindowFrame::content_child_count() const noexcept -> std::size_t {
  return content_ == nullptr ? 0 : content_->child_count();
}

auto WindowFrame::add_child(std::unique_ptr<Element> child) -> Element* {
  if (content_ == nullptr) return Element::add_child(std::move(child));
  return content_->add_child(std::move(child));
}

auto WindowFrame::insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element* {
  if (content_ == nullptr) return Element::insert_child(index, std::move(child));
  return content_->insert_child(index, std::move(child));
}

void WindowFrame::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  // 窗框自身不画底（标题栏与内容区各自有底）：它只是**容器**。
  // 但给一条边框：没有系统边界之后，这是"窗口到哪里为止"的视觉依据。
  style_.background = colors.bg;
  style_.border_width = 0.0f;
  style_.border_color = colors.border;
  style_.radius = 0.0f;
  style_.padding = {};
}

void WindowFrame::measure(const RenderContext& context, const Constraints& constraints) {
  apply_theme(context.theme);
  // 铺满父级：用 `available_*` 而不是 `max_*`（行布局里后者是 1e9 —— 见 `fill_width` 的说明）。
  const float width = fill_width(constraints, style_.width, 0.0f);
  const float height = fill_height(constraints, style_.height, 0.0f);
  measured_ = math::Size{std::max(width, 0.0f), std::max(height, 0.0f)};

  const float bar_height =
      (show_title_bar_ && bar_ != nullptr) ? bar_->bar_height() : 0.0f;
  const float content_height = std::max(0.0f, measured_.height - bar_height);
  if (show_title_bar_ && bar_ != nullptr) {
    bar_->measure(context, Constraints{measured_.width, bar_height, measured_.width, bar_height});
  }
  if (content_ != nullptr) {
    content_->measure(context, Constraints{measured_.width, content_height, measured_.width,
                                           content_height});
  }
}

void WindowFrame::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  const float bar_height = (show_title_bar_ && bar_ != nullptr) ? bar_->bar_height() : 0.0f;
  if (show_title_bar_ && bar_ != nullptr) {
    bar_->arrange(context, math::Rect{bounds_.x, bounds_.y, bounds_.width, bar_height});
  }
  if (content_ != nullptr) {
    content_->arrange(context, math::Rect{bounds_.x, bounds_.y + bar_height, bounds_.width,
                                          std::max(0.0f, bounds_.height - bar_height)});
  }
}

void WindowFrame::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  // 边缘条本身不常驻绘制（系统窗框也不画"拖拽把手"）；只在**按住/悬浮**时给一点提示，
  // 让用户确认"这一下抓的是边，不是内容"。
  const WindowEdge active = pressed_edge_ != WindowEdge::None ? pressed_edge_ : hovered_edge_;
  if (active == WindowEdge::None || !show_resize_edges_) return;
  for (std::size_t index = 0; index < kEdgeCount; ++index) {
    if (edge_of(index) != active) continue;
    const math::Rect band = edge_rect(index);
    if (band.is_empty()) continue;
    canvas.fill_rect(band, raster::Paint::solid(context.theme.colors().primary.with_alpha_f(
                                k_edge_press_alpha)));
  }
}

auto WindowFrame::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const WindowEdge edge = edge_at(event.position);
      if (edge != hovered_edge_) {
        hovered_edge_ = edge;
        mark_dirty();
      }
      return false;   // 不消费：移动事件继续冒泡（子元素/根仍要看到）
    }
    case EventKind::HoverOut: {
      if (hovered_edge_ != WindowEdge::None) {
        hovered_edge_ = WindowEdge::None;
        mark_dirty();
      }
      return false;
    }
    case EventKind::MouseDown: {
      if (event.button != 1) return false;
      const WindowEdge edge = edge_at(event.position);
      if (edge == WindowEdge::None) return false;   // 内容区/标题栏自己的事
      pressed_edge_ = edge;
      mark_dirty();
      // 实际开始缩放交给后端：Win32 由 `WM_NCHITTEST` 在窗口层接管（更跟手），
      // 这里返回 false 时**仍然消费事件**——边缘上的按下不该穿透到内容区去。
      if (WindowControl* control = resolve_control();
          control != nullptr && control->window_control_available()) {
        (void)control->window_begin_resize(edge);
      }
      return true;
    }
    case EventKind::MouseUp: {
      if (pressed_edge_ == WindowEdge::None) return false;
      pressed_edge_ = WindowEdge::None;
      mark_dirty();
      return true;
    }
    default: break;
  }
  return false;
}

auto WindowFrame::hit_test(math::Point point) const noexcept -> bool {
  return bounds_.contains(point);
}

auto WindowFrame::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "title" || name == "text") {
    return bar_ != nullptr ? std::optional<std::string>(bar_->title()) : std::nullopt;
  }
  if (name == "show_title_bar") return std::string(show_title_bar_ ? "true" : "false");
  if (name == "show_resize_edges") return std::string(show_resize_edges_ ? "true" : "false");
  if (name == "edge_band") return std::format("{}", static_cast<double>(edge_thickness()));
  return std::nullopt;
}

auto WindowFrame::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "title" || name == "text") {
    if (bar_ == nullptr) return false;
    bar_->set_title(std::string(value));
    return true;
  }
  if (name == "show_title_bar") {
    set_show_title_bar(value == "true" || value == "1" || value == "on");
    return true;
  }
  if (name == "show_resize_edges") {
    set_show_resize_edges(value == "true" || value == "1" || value == "on");
    return true;
  }
  return false;
}

auto WindowFrame::property_names() const -> std::vector<std::string_view> {
  return {"title", "text", "show_title_bar", "show_resize_edges", "edge_band"};
}

auto WindowFrame::invoke_action(std::string_view action, std::string_view argument) -> bool {
  (void)argument;
  WindowControl* control = resolve_control();
  // 转发给标题栏（动作面与属性面都只有一处定义：窗框是**外壳**，不是第二个实现）。
  if (bar_ != nullptr && bar_->invoke_action(action, argument)) return true;
  // 标题栏缺席（纯内容窗框）时，窗口动作仍应可用：按同一语义兜底。
  if (control == nullptr || !control->window_control_available()) return false;
  if (action == "minimize") return control->window_minimize();
  if (action == "maximize") return control->window_toggle_maximize();
  if (action == "close") return control->window_request_close();
  if (action == "move") return control->window_begin_move();
  return false;
}

}  // namespace st::ui
