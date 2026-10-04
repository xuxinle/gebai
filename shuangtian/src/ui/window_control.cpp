#include "st/ui/window_control.hpp"

#include <algorithm>

namespace st::ui {

auto resize_edge_at(math::Point point, math::Size size, float border) noexcept -> WindowEdge {
  if (border <= 0.0f || size.width <= 0.0f || size.height <= 0.0f) return WindowEdge::None;
  // 两侧各留一个缩放带；带宽夹到半宽/半高：窗口很小时若带比一半还宽，
  // 整窗都成"边缘"，中间那块再也点不动（拖不动窗口、按钮也按不着）。
  const float band = std::min(border, std::min(size.width, size.height) * 0.5f);
  // 上界必须给足一个带：贴边拖动时指针会短暂越出窗口边界（差 1~2px 是常态），
  // 一刀切在 [0, size] 上会让边缘拖动在越界那一帧"松手"。
  if (point.x < -band || point.y < -band || point.x > size.width + band ||
      point.y > size.height + band) {
    return WindowEdge::None;
  }
  const bool left = point.x < band;
  const bool right = point.x > size.width - band;
  const bool top = point.y < band;
  const bool bottom = point.y > size.height - band;
  if (top && left) return WindowEdge::TopLeft;
  if (top && right) return WindowEdge::TopRight;
  if (bottom && left) return WindowEdge::BottomLeft;
  if (bottom && right) return WindowEdge::BottomRight;
  if (left) return WindowEdge::Left;
  if (right) return WindowEdge::Right;
  if (top) return WindowEdge::Top;
  if (bottom) return WindowEdge::Bottom;
  return WindowEdge::None;
}

}  // namespace st::ui
