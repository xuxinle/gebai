#include "st/ui/components/viewport.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

namespace {

/// 中键的 `Event::button` 取值（与 `platform_win32` 的映射一致）。
constexpr int kMiddleButton{3};

}  // namespace

Viewport::Viewport() {
  // **必须开**：二维视口下内容会超出自己的矩形，不裁剪的话既会把邻居盖住，
  // 又会出现「看不见却抢点击」（`clip_children` 同时决定绘制与命中）。
  style_.clip_children = true;
}

auto Viewport::clamp_zoom(float zoom) -> float {
  if (!std::isfinite(zoom)) return 1.0f;
  return std::clamp(zoom, kMinZoom, kMaxZoom);
}

void Viewport::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  // 棋盘格**不硬编码颜色**：亮格用 `surface`（与"空"的背景同族）、暗格用 `surface_alt`
  // ——两者在亮/暗主题下都保持"能分辨但不抢眼"（硬编码灰在暗主题上会变成刺眼的亮块）。
  checker_light_ = colors.surface;
  checker_dark_ = colors.surface_alt;
  border_ = colors.border;
  if (checker_size_ <= 0.0f) checker_size_ = std::max(4.0f, metrics.space_sm);
  style_.background = colors.surface_sunken;   // 视口底：内容之外的"画布外"区域
}

auto Viewport::content_size() const -> math::Size {
  const Element* child = content();
  return child == nullptr ? math::Size{} : child->measured_size();
}

void Viewport::set_content(std::unique_ptr<Element> content) {
  children_.clear();
  if (content != nullptr) (void)Element::add_child(std::move(content));
  mark_layout_dirty();
}

void Viewport::set_zoom(float zoom) {
  const float clamped = clamp_zoom(zoom);
  if (clamped == zoom_) return;
  zoom_ = clamped;
  mark_layout_dirty();
  if (on_view_changed) on_view_changed();
}

void Viewport::zoom_at(math::Point view_point, float factor) {
  if (!std::isfinite(factor) || factor <= 0.0f) return;
  const float next = clamp_zoom(zoom_ * factor);
  if (next == zoom_) return;   // 已到边界：**不动 pan**（见头文件，那是"漂移"的来源）
  // 锚点不变量：`view = (content - pan) * zoom` 中该点的 content 保持不变。
  //   content = view / zoom_old + pan_old = view / zoom_new + pan_new
  // ⇒ pan_new = pan_old + view * (1/zoom_old - 1/zoom_new)
  const float delta = (1.0f / zoom_) - (1.0f / next);
  pan_.x += view_point.x * delta;
  pan_.y += view_point.y * delta;
  zoom_ = next;
  mark_layout_dirty();
  if (on_view_changed) on_view_changed();
}

void Viewport::zoom_step_at(math::Point view_point, bool in) {
  zoom_at(view_point, in ? kZoomStep : 1.0f / kZoomStep);
}

void Viewport::fit_to_view() {
  const math::Size size = content_size();
  if (size.width <= 0.0f || size.height <= 0.0f) return;
  if (bounds_.width <= 0.0f || bounds_.height <= 0.0f) return;
  // 「刚好看全」取两轴较小的那个倍率；不放大超过 1（小图居中显示更像看图工具）。
  const float fit = std::min(bounds_.width / size.width, bounds_.height / size.height);
  zoom_ = clamp_zoom(std::min(fit, 1.0f));
  // 居中：视口中心对应的内容点 = 内容中心。
  pan_.x = size.width * 0.5f - bounds_.width * 0.5f / zoom_;
  pan_.y = size.height * 0.5f - bounds_.height * 0.5f / zoom_;
  mark_layout_dirty();
  if (on_view_changed) on_view_changed();
}

auto Viewport::to_content(math::Point view_point) const -> math::Point {
  return math::Point{view_point.x / zoom_ + pan_.x, view_point.y / zoom_ + pan_.y};
}

auto Viewport::to_view(math::Point content_point) const -> math::Point {
  return math::Point{(content_point.x - pan_.x) * zoom_, (content_point.y - pan_.y) * zoom_};
}

void Viewport::measure(const RenderContext& context, const Constraints& constraints) {
  // 视口自己吃掉可用空间（同 `ScrollView`）。
  const float width = bounds_.width > 0.0f ? bounds_.width
                     : style_.has_explicit_width() ? style_.width
                                                   : constraints.max_width;
  const float height = bounds_.height > 0.0f ? bounds_.height
                      : style_.has_explicit_height() ? style_.height
                                                     : constraints.max_height;
  measured_ = math::Size{std::max(0.0f, width), std::max(0.0f, height)};
  // 内容按**不受约束**度量：它有自己的固有尺寸，视口只负责"看到哪一块"。
  // 给内容传视口约束会让它被压扁（那正是「缩放」与「重排」的区别）。
  if (Element* child = content(); child != nullptr) {
    child->measure(context, Constraints{kUnbounded, kUnbounded, kUnbounded, kUnbounded});
  }
}

void Viewport::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  Element* child = content();
  if (child == nullptr) return;
  const math::Size size = child->measured_size();
  // 内容的本征尺寸放进去，再整体平移 zoom/pan 的映射：
  //   view = (content - pan) * zoom  ⇒  content = 0 落在 (0 - pan) * zoom
  const math::Point origin = to_view(math::Point{0.0f, 0.0f});
  child->arrange(context,
                 math::Rect{bounds_.x + origin.x, bounds_.y + origin.y, size.width * zoom_,
                            size.height * zoom_});
}

void Viewport::paint_content(const RenderContext& /*context*/, raster::Surface& canvas) const {
  // 棋盘格**画在内容之下**（`paint_content` 是 `Element::paint` 里的"内容层"钩子，
  // 子元素会在它之后绘制）——这正是"透明底"的语义：内容有像素的地方盖住格子，
  // 没有像素的地方透出格子。
  //
  // 为何不自己 `push_clip_rect`：`Element::paint` 已按 `style_.clip_children` 做了裁剪
  // （与命中同一份口径），再裁一次是重复的，而重复裁剪一旦两层不一致就会出错。
  if (!checkerboard_ || checker_size_ <= 0.0f) return;
  // ⚠ 棋盘格**锚在内容坐标系**（而不是视口）：它是"内容下面那层透明底"的一部分，
  // 若锚在视口，平移时格子不动、内容在动——那看起来像"桌面在穿透图片"。
  const math::Point origin = to_view(math::Point{0.0f, 0.0f});
  const float cell = checker_size_ * zoom_;
  if (cell <= 0.5f) {
    // 缩得太小：格子会退化成摩尔纹。此时只铺亮色（比一片乱纹好看，也不费性能）。
    canvas.fill_rect(bounds_, raster::Paint::solid(checker_light_), 0.0f);
    return;
  }
  const int cols = static_cast<int>(std::ceil(bounds_.width / cell)) + 2;
  const int rows = static_cast<int>(std::ceil(bounds_.height / cell)) + 2;
  const int start_col = static_cast<int>(std::floor(-origin.x / cell));
  const int start_row = static_cast<int>(std::floor(-origin.y / cell));
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < cols; ++col) {
      if (((row + start_row) + (col + start_col)) % 2 == 0) continue;   // 只画暗格
      const float x = bounds_.x + origin.x + static_cast<float>(col + start_col) * cell;
      const float y = bounds_.y + origin.y + static_cast<float>(row + start_row) * cell;
      canvas.fill_rect(math::Rect{x, y, cell, cell}, raster::Paint::solid(checker_dark_), 0.0f);
    }
  }
}

auto Viewport::on_event(const RenderContext& /*context*/, Event& event) -> bool {
  switch (event.kind) {
    case EventKind::Wheel: {
      last_pointer_ = event.position;
      if (event.wheel_delta == 0.0f) return false;
      // 视口内的点换算成视口坐标（`to_content` 收的是视口坐标）。
      const math::Point local{event.position.x - bounds_.x, event.position.y - bounds_.y};
      // 滚轮向上（正 delta）= 放大。Ctrl 与否都缩放：本组件唯一的交互就是看大图。
      zoom_step_at(local, event.wheel_delta > 0.0f);
      event.handled = true;
      return true;
    }
    case EventKind::MouseDown: {
      // 中键拖拽平移（图像编辑器惯例）。左键留给内容自己用——视口不抢。
      if (event.button != kMiddleButton) return false;
      dragging_ = true;
      drag_origin_view_ = event.position;
      drag_origin_pan_ = pan_;
      event.handled = true;
      return true;
    }
    case EventKind::MouseMove: {
      last_pointer_ = event.position;
      if (!dragging_) return false;
      // `pressed_` 语义已由框架保证（按下后移出元素仍收到 Move），这里只管跟手。
      pan_.x = drag_origin_pan_.x - (event.position.x - drag_origin_view_.x) / zoom_;
      pan_.y = drag_origin_pan_.y - (event.position.y - drag_origin_view_.y) / zoom_;
      mark_layout_dirty();
      event.handled = true;
      return true;
    }
    case EventKind::MouseUp: {
      if (!dragging_) return false;
      dragging_ = false;
      event.handled = true;
      if (on_view_changed) on_view_changed();
      return true;
    }
    default:
      return false;
  }
}

auto Viewport::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "zoom") return std::format("{:.4f}", static_cast<double>(zoom_));
  if (name == "pan_x") return std::format("{:.2f}", static_cast<double>(pan_.x));
  if (name == "pan_y") return std::format("{:.2f}", static_cast<double>(pan_.y));
  if (name == "checkerboard") return checkerboard_ ? "true" : "false";
  return Element::get_property(name);
}

auto Viewport::set_property(std::string_view name, std::string_view value) -> bool {
  const auto parse_float = [&value]() -> std::optional<float> {
    try {
      std::size_t used = 0;
      const float parsed = std::stof(std::string(value), &used);
      if (used != value.size()) return std::nullopt;
      return parsed;
    } catch (...) {
      return std::nullopt;
    }
  };
  if (name == "zoom") {
    if (const auto parsed = parse_float(); parsed.has_value()) {
      set_zoom(*parsed);
      return true;
    }
    return false;
  }
  if (name == "pan_x" || name == "pan_y") {
    if (const auto parsed = parse_float(); parsed.has_value()) {
      // ⚠ 两轴**分开设**是有意的：一次设两轴需要组合语法，而控制通道的 `set` 是
      // 「一个名字一个值」——分开设让调用方能只动一轴（测试与脚本都更方便）。
      if (name == "pan_x") pan_.x = *parsed;
      else pan_.y = *parsed;
      mark_layout_dirty();
      return true;
    }
    return false;
  }
  if (name == "checkerboard") {
    // 只认明确的两个词：把乱值当 false 会让「设错了」看起来像「功能坏了」。
    if (value == "true") {
      set_checkerboard(true);
      return true;
    }
    if (value == "false") {
      set_checkerboard(false);
      return true;
    }
    return false;
  }
  return Element::set_property(name, value);
}

auto Viewport::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.insert(names.end(), {"zoom", "pan_x", "pan_y", "checkerboard"});
  return names;
}

auto Viewport::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "zoom_in" || action == "zoom_out") {
    // 无鼠标位置时以视口中心为锚（键盘/脚本路径）。
    const math::Point center{bounds_.width * 0.5f, bounds_.height * 0.5f};
    zoom_step_at(center, action == "zoom_in");
    return true;
  }
  if (action == "fit") {
    fit_to_view();
    return true;
  }
  if (action == "reset") {
    zoom_ = 1.0f;
    pan_ = math::Point{};
    mark_layout_dirty();
    if (on_view_changed) on_view_changed();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
