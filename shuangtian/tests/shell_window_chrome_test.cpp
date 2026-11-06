/// 窗框几何的纯函数测试：环形阴影位图、圆角半径、最大化豁免。
///
/// 这些是**观感参数的可测部分**——`docs/perceptual_changes.md` 要求观感改动至少有一个
/// 可复算的量。这里的量是"位图里有多少不透明像素、它们在哪儿"：
///   · 窗口内部必须**一个像素都不画**（否则每帧多搬一圈没用的带宽，还在圆角处露馅）；
///   · 窗外必须**单调衰减**（近处最暗、远处归零），不能有硬边；
///   · 最大化必须**整张空**（用户明确要求：最大化时不绘制这些）。
///
/// 反向验证：把 `ring_layout` 的 `maximized` 早退删掉，"最大化为空"这条会立刻变红。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "st/shell/window_chrome.hpp"
#include "st/test/test.hpp"

namespace {

using st::shell::WindowChromeGeometry;
using st::shell::WindowChromeStyle;

/// 数位图里 alpha > 0 的像素（= 真正会被合成的像素）。
[[nodiscard]] auto opaque_count(const std::vector<std::uint32_t>& pixels) -> std::size_t {
  return static_cast<std::size_t>(
      std::count_if(pixels.begin(), pixels.end(), [](std::uint32_t p) { return (p >> 24) != 0; }));
}

[[nodiscard]] auto alpha_at(const std::vector<std::uint32_t>& pixels, int width, int x, int y)
    -> int {
  return static_cast<int>((pixels[static_cast<std::size_t>(y) * width + x] >> 24) & 0xFFU);
}

/// 按 layout 分配一块全透明的缓冲（模拟接入方的 DIB 像素），再让实现往里写环。
[[nodiscard]] auto build(const st::shell::WindowRingLayout& layout, const WindowChromeStyle& style)
    -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> pixels(
      static_cast<std::size_t>(layout.width) * static_cast<std::size_t>(layout.height), 0U);
  if (!st::shell::build_ring_pixels(layout, style, 0x0A0A12U, pixels)) pixels.clear();
  return pixels;
}

[[nodiscard]] auto make_geometry(int w, int h, float scale, bool maximized = false)
    -> WindowChromeGeometry {
  WindowChromeGeometry geometry;
  geometry.client_width = w;
  geometry.client_height = h;
  geometry.scale = scale;
  geometry.maximized = maximized;
  return geometry;
}

}  // namespace

ST_TEST(window_chrome_ring_skips_the_window_interior) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(400, 300, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  ST_REQUIRE(layout.visible);
  ST_CHECK_EQ(layout.width, 400 + 2 * layout.margin);
  ST_CHECK_EQ(layout.height, 300 + 2 * layout.margin);
  const auto pixels = build(layout, style);
  ST_REQUIRE(pixels.size() == static_cast<std::size_t>(layout.width) * layout.height);

  // 窗口正中心与四条边内侧若干像素：必须**完全透明**。
  // 这条守着"画内部纯属浪费"——400×300 的内部有两万多个像素，全画等于每帧白搬一遍。
  const int cx = layout.width / 2;
  const int cy = layout.height / 2;
  ST_CHECK_EQ(alpha_at(pixels, layout.width, cx, cy), 0);
  const int inset = layout.margin + 3;
  ST_CHECK_EQ(alpha_at(pixels, layout.width, inset, cy), 0);
  ST_CHECK_EQ(alpha_at(pixels, layout.width, layout.width - inset, cy), 0);
  ST_CHECK_EQ(alpha_at(pixels, layout.width, cx, inset), 0);
  ST_CHECK_EQ(alpha_at(pixels, layout.width, cx, layout.height - inset), 0);
}

ST_TEST(window_chrome_ring_fades_monotonically_outward) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(400, 300, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  const auto pixels = build(layout, style);
  // 沿左缘中线从窗口边缘向外走：越远越淡，且必须单调（阴影有硬边就是"一圈描边"而不是阴影）。
  const int cy = layout.height / 2;
  std::vector<int> alphas;
  for (int d = 1; d <= layout.margin; ++d) {
    alphas.push_back(alpha_at(pixels, layout.width, layout.margin - d, cy));
  }
  ST_REQUIRE(!alphas.empty());
  // 边缘那格必须有可见强度（否则等于没画）
  ST_CHECK(alphas.front() > 20);
  bool monotonic = true;
  for (std::size_t i = 1; i < alphas.size(); ++i) {
    if (alphas[i] > alphas[i - 1]) monotonic = false;
  }
  ST_CHECK(monotonic);
  // 最远处要衰减到接近 0（否则阴影在带宽边缘被硬切）
  ST_CHECK(alphas.back() < 20);
}

ST_TEST(window_chrome_ring_is_drawn_only_outside) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(200, 160, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  const auto pixels = build(layout, style);
  // 环的理论面积 = 全体积 - 窗口体积（不含圆角让出的部分，所以实际应**略多**于或不小于差值）
  const std::size_t total = static_cast<std::size_t>(layout.width) * layout.height;
  const std::size_t interior = static_cast<std::size_t>(200) * 160;
  const std::size_t painted = opaque_count(pixels);
  ST_CHECK(painted > 0);
  ST_CHECK(painted < total - interior + 4 * layout.radius * layout.radius + 8);
}

ST_TEST(window_chrome_maximized_draws_nothing) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(1920, 1080, 1.0f, /*maximized=*/true);
  const auto layout = st::shell::ring_layout(geometry, style);
  // 用户要求：最大化时不绘制外围阴影与圆角。两条都验。
  ST_CHECK(!layout.visible);
  ST_CHECK(build(layout, style).empty());
  ST_CHECK_EQ(st::shell::corner_radius_physical(geometry, style), 0);
}

ST_TEST(window_chrome_corner_radius_scales_with_dpi_and_clamps) {
  WindowChromeStyle style;
  style.corner_radius = 10;
  ST_CHECK_EQ(st::shell::corner_radius_physical(make_geometry(800, 600, 1.0f), style), 10);
  ST_CHECK_EQ(st::shell::corner_radius_physical(make_geometry(800, 600, 2.0f), style), 20);
  // 窗口比 2×半径还小时，半径必须夹到短边一半——否则圆角退化成胶囊（肉眼上是"窗口变圆"）。
  const int tiny = st::shell::corner_radius_physical(make_geometry(12, 8, 1.0f), style);
  ST_CHECK_EQ(tiny, 4);
  // 直角配置
  style.corner_radius = 0;
  ST_CHECK_EQ(st::shell::corner_radius_physical(make_geometry(800, 600, 1.0f), style), 0);
}

ST_TEST(window_chrome_shadow_can_be_disabled) {
  WindowChromeStyle style;
  style.shadow = false;
  const WindowChromeGeometry geometry = make_geometry(400, 300, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  ST_CHECK(!layout.visible);
  ST_CHECK(build(layout, style).empty());
  // 关掉阴影不该连带关掉圆角（两者是独立开关：用户要的正是"圆角保留、阴影可关"）
  style.corner_radius = 12;
  ST_CHECK_EQ(st::shell::corner_radius_physical(geometry, style), 12);
}

ST_TEST(window_chrome_round_rect_distance_signs) {
  // 圆心 (50,50)，半尺寸 (40,40)，半径 10 → 角上的弧心在 (40,40)/(60,40)/(40,60)/(60,40 之外)。
  const float d_inside = st::shell::round_rect_distance(50.0f, 50.0f, 50.0f, 50.0f, 40.0f, 40.0f, 10.0f);
  ST_CHECK(d_inside < 0.0f);
  const float d_far = st::shell::round_rect_distance(200.0f, 200.0f, 50.0f, 50.0f, 40.0f, 40.0f, 10.0f);
  ST_CHECK(d_far > 0.0f);
  // 边中点处距离 = 到**边**的距离：右边在 x = 50+40 = 90，所以 (80,50) 的距离是 10（在内部）。
  const float d_edge =
      st::shell::round_rect_distance(80.0f, 50.0f, 50.0f, 50.0f, 40.0f, 40.0f, 10.0f);
  ST_CHECK(std::abs(d_edge + 10.0f) < 0.01f);
  // 边界上（x = 90）距离为 0；再往外一像素为正。
  const float d_on_edge =
      st::shell::round_rect_distance(90.0f, 50.0f, 50.0f, 50.0f, 40.0f, 40.0f, 10.0f);
  ST_CHECK(std::abs(d_on_edge) < 0.01f);
  const float d_just_out =
      st::shell::round_rect_distance(91.0f, 50.0f, 50.0f, 50.0f, 40.0f, 40.0f, 10.0f);
  ST_CHECK(std::abs(d_just_out - 1.0f) < 0.01f);
  // **圆角的判据**：直角矩形的角点 (89,89) 在直角里算"内侧"（离边皆 1px），
  // 而在圆角矩形里必须落在外面（> 0）——这正是"圆角让出去的那块"。
  const float d_corner =
      st::shell::round_rect_distance(88.0f, 88.0f, 50.0f, 50.0f, 40.0f, 40.0f, 10.0f);
  ST_CHECK(d_corner > 0.0f);
  // 同一位置在半径 0（直角）时必须落在**内部**——证明上面那条 > 0 确实来自圆角而不是别的原因。
  const float d_corner_square =
      st::shell::round_rect_distance(88.0f, 88.0f, 50.0f, 50.0f, 40.0f, 40.0f, 0.0f);
  ST_CHECK(d_corner_square < 0.0f);
}
