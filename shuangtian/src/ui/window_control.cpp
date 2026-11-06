#include "st/ui/window_control.hpp"

#include <algorithm>

namespace st::ui {

auto resize_edge_at(math::Point point, math::Size size, float border) noexcept -> WindowEdge {
  if (border <= 0.0f || size.width <= 0.0f || size.height <= 0.0f) return WindowEdge::None;
  // 两侧各留一个缩放带；带宽夹到半宽/半高：窗口很小时若带比一半还宽，
  // 整窗都成"边缘"，中间那块再也点不动（拖不动窗口、按钮也按不着）。
  const float band = std::min(border, std::min(size.width, size.height) * 0.5f);
  // 角带比边宽：角命中要求**同时**在水平带与垂直带内，可抓区是 `band×band`
  // 的小方块——按边宽的 6px 算，1.5× DPI 下只有 9×9 物理像素，鼠标很难正对。
  // 详见 `kWindowCornerResizeBorder` 的说明。
  const float corner_band =
      std::min(kWindowCornerResizeBorder,
               std::min(size.width, size.height) * 0.5f);
  const float reach = std::max(band, corner_band);
  // 上界必须给足一个带：贴边拖动时指针会短暂越出窗口边界（差 1~2px 是常态），
  // 一刀切在 [0, size] 上会让边缘拖动在越界那一帧"松手"。
  if (point.x < -reach || point.y < -reach || point.x > size.width + reach ||
      point.y > size.height + reach) {
    return WindowEdge::None;
  }
  // **先判角**：角区更宽，且角优先于边（否则永远只能命中单向缩放）。
  const bool near_left = point.x < corner_band;
  const bool near_right = point.x > size.width - corner_band;
  const bool near_top = point.y < corner_band;
  const bool near_bottom = point.y > size.height - corner_band;
  if (near_top && near_left) return WindowEdge::TopLeft;
  if (near_top && near_right) return WindowEdge::TopRight;
  if (near_bottom && near_left) return WindowEdge::BottomLeft;
  if (near_bottom && near_right) return WindowEdge::BottomRight;
  // 再判边（窄带）
  if (point.x < band) return WindowEdge::Left;
  if (point.x > size.width - band) return WindowEdge::Right;
  if (point.y < band) return WindowEdge::Top;
  if (point.y > size.height - band) return WindowEdge::Bottom;
  return WindowEdge::None;
}

}  // namespace st::ui
