// `NativeHost` 解除嵌入时的**几何还原**决策（`src/shell/child_view_geometry.hpp`）。
//
// ## 这些用例在钉什么
//
// 实测缺陷：解除嵌入后，外部窗口的标题栏落在屏幕上方之外——窗口看得见、拖不动。
// 根因是嵌入期为了让外部窗口的自绘标题栏退到宿主之外，把它的矩形上移了一个
// 标题栏高度并加高同样多；解除嵌入只恢复了**样式**，没恢复**几何**。
//
// 平台后端在任何 CI 环境里都跑不起来（要真窗口、要真浏览器），所以这里测的是
// 那段**纯几何决策**：给它"嵌入前的矩形 + 各显示器工作区"，它必须给出
// "送回去的那个矩形"。平台层只负责把结果 `SetWindowPos` 下去。

#include "st/test/test.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "../src/shell/child_view_geometry.hpp"

using st::shell::detail::PixelRect;
using st::shell::detail::cover_span;
using st::shell::detail::plan_detach_placement;
using st::shell::detail::title_bar_reachable;

namespace {

/// 常见显示器工作区：2560×1440 屏，扣掉 40px 任务栏。
constexpr PixelRect kWork1920{0, 0, 2560, 1400};
/// 副屏（在主屏右侧），同样扣掉任务栏。
constexpr PixelRect kWorkRight{2560, 0, 5120, 1400};

[[nodiscard]] auto rect_near(PixelRect a, PixelRect b) -> bool {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 第一组：可达性判据（"标题栏在屏外"就是拖不动）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(child_view_geometry_title_bar_reachable_uses_top_edge_midpoint) {
  const std::array<PixelRect, 1> work{kWork1920};
  // 正常窗口：顶边在屏内
  ST_CHECK(title_bar_reachable(PixelRect{100, 100, 1100, 800}, work));
  // 顶边正好在屏顶：仍算可达（顶边中点在 y=0，工作区含 0）
  ST_CHECK(title_bar_reachable(PixelRect{100, 0, 1100, 700}, work));
  // **实测缺陷形态**：顶边在屏幕上方之外（-107），标题栏整条看不见
  ST_CHECK(!title_bar_reachable(PixelRect{0, -107, 1955, 525}, work));
  // 只差一个像素也要判不可达：那不是"差不多"，是标题栏摸不到
  ST_CHECK(!title_bar_reachable(PixelRect{0, -1, 1000, 600}, work));
}

ST_TEST(child_view_geometry_horizontal_offscreen_is_unreachable) {
  // 窗口被拖到左边之外：顶边中点在屏外（标题栏右边还在屏内，但拖不动）。
  const std::array<PixelRect, 1> work{kWork1920};
  ST_CHECK(!title_bar_reachable(PixelRect{-3000, 100, -1000, 700}, work));
}

ST_TEST(child_view_geometry_work_area_excludes_taskbar) {
  // 标题栏缩到任务栏底下同样拖不动——判据必须用**工作区**而非显示器矩形。
  const std::array<PixelRect, 1> work{kWork1920};   // 底部 1400，屏高 1440
  ST_CHECK(!title_bar_reachable(PixelRect{100, 1410, 900, 2000}, work));
}

// ─────────────────────────────────────────────────────────────────────────────
// 第二组：可达时**原样送回**（含尺寸还原）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(child_view_geometry_restores_saved_rect_verbatim_when_reachable) {
  const std::array<PixelRect, 1> work{kWork1920};
  const PixelRect saved{150, 120, 1350, 920};
  // 当前形态 = 嵌入期（上移 107、加高 107）
  const PixelRect current{0, -107, 1955, 525};
  const PixelRect placed = plan_detach_placement(saved, current, work);
  // 位置与**尺寸**都必须回到嵌入前：只回位置会留下一个"高了一个标题栏"的窗口。
  ST_CHECK(rect_near(placed, saved));
}

ST_TEST(child_view_geometry_uses_current_when_saved_missing) {
  // 嵌入前没记到（`GetWindowRect` 失败）：拿当前矩形当基准，但不能留个够不着的。
  const std::array<PixelRect, 1> work{kWork1920};
  const PixelRect current{0, -107, 1955, 525};
  const PixelRect placed = plan_detach_placement(std::nullopt, current, work);
  ST_CHECK(title_bar_reachable(placed, work));   // 必须夹回可达处
  ST_CHECK(placed.width() == current.width());   // 尺寸不动（没别的信息可依）
  ST_CHECK(placed.height() == current.height());
}

// ─────────────────────────────────────────────────────────────────────────────
// 第三组：不可达时夹回工作区（只平移、不改尺寸）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(child_view_geometry_clamps_offscreen_top_into_work_area) {
  const std::array<PixelRect, 1> work{kWork1920};
  const PixelRect saved{0, -107, 1955, 525};   // 用户"记得"的位置也在屏外（真实现场）
  const PixelRect placed = plan_detach_placement(saved, saved, work);
  ST_CHECK(placed.top == 0);                   // 顶边贴到工作区顶
  ST_CHECK(placed.width() == saved.width());   // 尺寸不变
  ST_CHECK(placed.height() == saved.height());
  ST_CHECK(title_bar_reachable(placed, work));
}

ST_TEST(child_view_geometry_clamps_bottom_and_right_edges) {
  const std::array<PixelRect, 1> work{kWork1920};
  // 右下越界：必须夹进来（否则标题栏在、但窗口大半在屏外）
  const PixelRect saved{2400, 1350, 3200, 1900};
  const PixelRect placed = plan_detach_placement(saved, saved, work);
  ST_CHECK(placed.right <= work.front().right);
  ST_CHECK(placed.bottom <= work.front().bottom);
  ST_CHECK(placed.left == work.front().right - saved.width());
  ST_CHECK(placed.top == work.front().bottom - saved.height());
}

ST_TEST(child_view_geometry_oversized_window_aligns_top_left) {
  const std::array<PixelRect, 1> work{kWork1920};
  // 比工作区还大：夹不进去时对齐左上边（标题栏与左边缘必定可见 = 拖得动）
  const PixelRect saved{-500, -500, 3500, 2500};
  const PixelRect placed = plan_detach_placement(saved, saved, work);
  ST_CHECK(placed.left == work.front().left);
  ST_CHECK(placed.top == work.front().top);
  ST_CHECK(placed.width() == saved.width());    // 不缩尺寸
  ST_CHECK(placed.height() == saved.height());
}

// ─────────────────────────────────────────────────────────────────────────────
// 第四组：多显示器（不能"搬到主屏"）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(child_view_geometry_picks_monitor_with_largest_overlap) {
  const std::array<PixelRect, 2> work{kWork1920, kWorkRight};
  // 用户把窗口放在右侧副屏，且顶部越界：必须夹回**副屏**，不能搬回主屏。
  const PixelRect saved{2700, -80, 3700, 620};
  const PixelRect placed = plan_detach_placement(saved, saved, work);
  ST_CHECK(placed.left >= kWorkRight.left);
  ST_CHECK(title_bar_reachable(placed, work));
}

ST_TEST(child_view_geometry_falls_back_to_first_work_area) {
  const std::array<PixelRect, 2> work{kWork1920, kWorkRight};
  // 完全在两屏之外（比如改过显示器布局）：取第一块，且保证可达。
  const PixelRect saved{9000, 5000, 9600, 5400};
  const PixelRect placed = plan_detach_placement(saved, saved, work);
  ST_CHECK(title_bar_reachable(placed, work));
}

// ─────────────────────────────────────────────────────────────────────────────
// 第五组：边界输入（不能崩、不能给出倒置矩形）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(child_view_geometry_handles_empty_and_invalid_inputs) {
  const std::array<PixelRect, 1> work{kWork1920};
  const PixelRect good{100, 100, 900, 700};

  // 没有任何工作区（极端环境）：原样交回，不慌不做坏事。
  const PixelRect no_work = plan_detach_placement(good, good, std::span<const PixelRect>{});
  ST_CHECK(rect_near(no_work, good));

  // 退化矩形（宽高 <= 0）：原样交回，绝不返回倒置矩形。
  const PixelRect degenerate{500, 500, 500, 500};
  const PixelRect placed = plan_detach_placement(degenerate, degenerate, work);
  ST_CHECK(placed.width() <= 0);
  ST_CHECK(!placed.is_valid());
}

// ─────────────────────────────────────────────────────────────────────────────
// 第六组：cover_span——分数 DPI 下的子窗口覆盖取整（实测缺陷：底部白线）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(cover_span_no_gap_at_fractional_scale) {
  // **实测缺陷形态**：1.5× DPI，WebView 逻辑底 = 窗口逻辑底（1247+34=1281），
  // 而物理客户区高 1922 = ceil(1281.33×1.5)——边缘截断算出 1870.5→1870，
  // 子窗口底边到 51+1870=1921，底部 1 物理像素露出背景色（那条白线）。
  // cover_span 必须：ceil 后的底边 ≥ ceil(逻辑底×scale)。
  const auto [y, bottom] = cover_span(34.0f, 1247.0f, 1.5f);
  ST_CHECK(y == 51);            // floor(51.0)
  ST_CHECK(bottom == 1922);     // ceil(1870.5 + 51) = ceil(1921.5) = 1922
  // 与客户区底对齐：客户区高 1922 = ceil(1281.333×1.5)，子窗口必须盖到 1922。
  const int client_bottom = static_cast<int>(std::ceil(1281.3333f * 1.5f - 0.001f));
  ST_CHECK(bottom >= client_bottom);
}

ST_TEST(cover_span_covers_full_logical_area) {
  // 整数边界不变（整倍 DPI 不该凭空多 1px）：
  const auto [a1, a2] = cover_span(0.0f, 100.0f, 2.0f);
  ST_CHECK(a1 == 0 && a2 == 200);
  // 非零起点同样覆盖到逻辑终点：
  const auto [b1, b2] = cover_span(10.0f, 90.0f, 1.0f);
  ST_CHECK(b1 == 10 && b2 == 100);
  // 浮点累积误差容差（+0.001 的用途）：1.5×下 34+1247=1281，1281×1.5=1921.5，
  // 但逐边算时 1281.0001 之类的值不应把底边多推到 1923。
  const auto [c1, c2] = cover_span(34.0f, 1247.0001f, 1.5f);
  ST_CHECK(c1 == 51 && c2 == 1922);
}

ST_TEST(cover_span_never_inverts) {
  // 极小跨度（浮点下界接近上界）：至少 1px，不产生倒置（右<左）。
  const auto [first, last] = cover_span(5.0f, 0.0f, 1.5f);
  ST_CHECK(last > first);
  ST_CHECK(last - first >= 1);
}
