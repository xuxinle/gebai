// **角区抓取范围必须比边宽**（用户实测："圆角没有斜向拖动的能力"）。
//
// 根因：角命中要求**同时**落在水平带与垂直带内，可抓区就是 `band × band`。
// 按边宽的 6 逻辑像素算，1.5× DPI 下只有 9×9 物理像素——鼠标很难正对那个
// 小方块，实操中几乎总是先命中上/左边，变成单向缩放。
//
// 修法：角用更宽的带（`kWindowCornerResizeBorder`），边的带宽保持不变
// （边一变宽，从侧边拖窗口就会误触缩放）。
#include <algorithm>
#include <cmath>

#include "st/core/log.hpp"
#include "st/test/test.hpp"
#include "st/ui/window_control.hpp"

namespace {
using st::math::Point;
using st::math::Size;
using st::ui::WindowEdge;
using st::ui::resize_edge_at;

constexpr Size kWin{800.0f, 600.0f};
}  // namespace

/// 角的可抓边长必须显著大于边长 —— 否则斜向拖不动。
ST_TEST(window_resize_corner_zone_is_wider_than_edge) {
  const float border = st::ui::kWindowResizeBorder;
  const float corner = st::ui::kWindowCornerResizeBorder;
  st::log::out("[resize] 边带={} 角带={}（角应更宽）\n", border, corner);
  ST_CHECK(corner > border);

  // 沿左上角的对角线往内走，找出"仍是 TopLeft"的最远距离。
  // 若角带 == 边带，这个距离只有 border*0.707；角带更宽则应接近 corner*0.707。
  float last_topleft = 0.0f;
  for (float d = 0.0f; d < 60.0f; d += 0.25f) {
    if (resize_edge_at(Point{d, d}, kWin, border) == WindowEdge::TopLeft) last_topleft = d;
  }
  const float diagonal_reach = last_topleft * std::sqrt(2.0f);
  st::log::out("[resize] 左上角沿对角线的可抓距离 = {}（投影 {}）\n", last_topleft,
               diagonal_reach);
  // 至少要有 1.5× 边带的投影，否则"太小的方块抓不住"这个问题没解决。
  ST_CHECK(diagonal_reach >= border * 1.5f);
}

/// 加宽角带**不能**让边变宽：从侧边中部抓，仍应是单向缩放。
ST_TEST(window_resize_edge_width_is_unchanged_by_corner_zone) {
  const float border = st::ui::kWindowResizeBorder;

  // 左边中部：只有 x 在 border 内才是 Left
  ST_CHECK(resize_edge_at(Point{border * 0.5f, 300.0f}, kWin, border) == WindowEdge::Left);
  ST_CHECK(resize_edge_at(Point{border * 1.5f, 300.0f}, kWin, border) == WindowEdge::None);

  // 上边中部同理
  ST_CHECK(resize_edge_at(Point{400.0f, border * 0.5f}, kWin, border) == WindowEdge::Top);
  ST_CHECK(resize_edge_at(Point{400.0f, border * 1.5f}, kWin, border) == WindowEdge::None);
}

/// 四个角都要能命中（含斜向），且优先级高于边。
ST_TEST(window_resize_all_four_corners_are_reachable) {
  const float border = st::ui::kWindowResizeBorder;
  const float probe = 4.0f;   // 典型的"往角上一戳"的位置
  struct Case {
    const char* name;
    Point point;
    WindowEdge expect;
  };
  const Case cases[] = {
      {"左上", {probe, probe}, WindowEdge::TopLeft},
      {"右上", {kWin.width - probe, probe}, WindowEdge::TopRight},
      {"左下", {probe, kWin.height - probe}, WindowEdge::BottomLeft},
      {"右下", {kWin.width - probe, kWin.height - probe}, WindowEdge::BottomRight},
  };
  for (const Case& c : cases) {
    const WindowEdge got = resize_edge_at(c.point, kWin, border);
    st::log::out("[resize] {} ({},{}) -> {}（期望 {}）\n", c.name, c.point.x, c.point.y,
                 static_cast<int>(got), static_cast<int>(c.expect));
    ST_CHECK(got == c.expect);
  }
}

/// 小窗口上角带要被夹到半宽/半高，**不能让整窗都变成角**。
///
/// 夹取后的正确行为是：角带 = 半宽，于是中心点恰好落在带**外**（返回 None），
/// 而紧贴角上仍能命中角。若忘了夹取，角带 12 > 半宽 8，整窗都会被判成角，
/// 用户就再也拖不动窗口了。
ST_TEST(window_resize_corner_zone_clamps_on_tiny_window) {
  const Size tiny{16.0f, 16.0f};   // 半宽 8 < 角带 12 → 必须夹到 8
  const float border = st::ui::kWindowResizeBorder;

  // 中心点 (8,8)：`x < corner_band` = `8 < 8` 为假 → 不在角内，也不在边带内
  const WindowEdge centre = resize_edge_at(Point{8.0f, 8.0f}, tiny, border);
  st::log::out("[resize] 16x16 窗口中心 -> {}（应为 None：带被夹到半宽）\n",
               static_cast<int>(centre));
  ST_CHECK(centre == WindowEdge::None);

  // 紧贴角上仍然能抓到角
  const WindowEdge corner = resize_edge_at(Point{1.0f, 1.0f}, tiny, border);
  st::log::out("[resize] 16x16 窗口左上 1px -> {}（应是 TopLeft）\n",
               static_cast<int>(corner));
  ST_CHECK(corner == WindowEdge::TopLeft);

  // 边中部仍能抓边（band 夹到 min(6, 8) = 6）
  const WindowEdge edge = resize_edge_at(Point{2.0f, 8.0f}, tiny, border);
  st::log::out("[resize] 16x16 窗口左边 -> {}（应是 Left 或角）\n", static_cast<int>(edge));
  ST_CHECK(edge != WindowEdge::None);
}
