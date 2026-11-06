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


#include "st/core/log.hpp"

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

[[nodiscard]] auto build(const st::shell::WindowRingLayout& layout, const WindowChromeStyle& style,

                         std::uint32_t edge_rgb = 0xF7F7FAU) -> std::vector<std::uint32_t> {

  std::vector<std::uint32_t> pixels(

      static_cast<std::size_t>(layout.width) * static_cast<std::size_t>(layout.height), 0U);

  if (!st::shell::build_ring_pixels(layout, style, 0x0A0A12U, pixels, edge_rgb)) pixels.clear();

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


/// 柔和阴影的剖面：**贴窗口最浓，单调向外衰减，且贴窗口处不留空隙**。
///
/// ## 为何"贴窗口最浓"
///
/// 柔和阴影的本质是"光被窗口遮住"。紧贴窗口的那条线被遮得最彻底，所以最暗；
/// 向外光绕射进来，逐渐变亮。
///
/// 曾误把公式改成"升-降包络"（贴窗口为 0、向外先升后降），理由是想消掉
/// 弧上的黑点。结果实测贴窗口 alpha 只有 7、到第 8 像素才到峰值 102
/// —— 用户看到的是**窗口与阴影之间的一条空隙**。黑点的真正解法是
/// 窗口色/阴影的两路合成（见 `write_pixel` 里的 `has_window_color`），
/// 不是把阴影压成 0。
///
/// ## 判据
///
/// 1. **贴窗口（d=1）就是峰值** —— 否则就是空隙；
/// 2. **单调不增**向外 —— 否则剖面有起伏，看着不自然；
/// 3. 峰值够浓（>= 40）—— 否则等于没有阴影；
/// 4. 最外侧显著淡出（< 峰值一半）—— 否则是一条均匀灰带。
ST_TEST(window_chrome_ring_shadow_profile_is_soft) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(400, 300, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  const auto pixels = build(layout, style);
  ST_REQUIRE(!pixels.empty());

  const int cy = layout.height / 2;   // 左缘中线（远离圆角，量的是纯阴影）
  std::vector<int> alphas;
  for (int d = 1; d <= layout.margin; ++d) {
    alphas.push_back(alpha_at(pixels, layout.width, layout.margin - d, cy));
  }
  ST_REQUIRE(alphas.size() >= 8U);
  st::log::out("[shadow] d=1..{} 的 alpha：", alphas.size());
  for (const int a : alphas) st::log::out(" {} ", a);
  st::log::out("\n");

  const int at_edge = alphas.front();
  const int peak = *std::max_element(alphas.begin(), alphas.end());
  const int outermost = alphas.back();

  // ① 贴窗口就是峰值（不能留空隙）
  ST_CHECK(at_edge >= peak - 2);
  // ② 单调不增
  bool monotonic = true;
  for (std::size_t i = 1; i < alphas.size(); ++i) {
    if (alphas[i] > alphas[i - 1] + 1) monotonic = false;
  }
  ST_CHECK(monotonic);
  // ③ 峰值够浓
  ST_CHECK(peak >= 40);
  // ④ 外侧淡出
  ST_CHECK(outermost < peak / 2);
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


/// 圆角弧带必须有**多档部分覆盖**（抗锯齿），且贴窗口处不是最暗。
///
/// 判据的由来（两条都是实测踩出来的）：
///
/// 1. **档数 >= 3**：覆盖率公式写错时弧带只剩 1~2 档（弧变硬边）。
/// 2. **弧带里最亮的中间值必须接近窗口色**：若没有这一条，
///    单纯把阴影画到弧上也能凑出"多个中间值"，却全是暗的
///    （那是黑边，不是抗锯齿）。
///
/// 采样只取距圆心 `r-1 .. r+1` 的那一圈 —— 更远是纯阴影带，
/// 数进来会把阴影的连续衰减误当圆角过渡（第一版就是这样假绿的）。
ST_TEST(window_chrome_corner_edge_has_antialiasing_ramp) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(400, 300, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  const auto pixels = build(layout, style);
  ST_REQUIRE(!pixels.empty());

  const int m = layout.margin;
  const int r = layout.radius;
  const int cx = m + r;
  const int cy = m + r;
  std::vector<int> partial;
  for (int deg = 185; deg <= 265; deg += 5) {
    const double rad = static_cast<double>(deg) * 3.14159265358979 / 180.0;
    for (int dist = r - 1; dist <= r + 1; ++dist) {
      const int x = cx + static_cast<int>(std::lround(static_cast<double>(dist) * std::cos(rad)));
      const int y = cy + static_cast<int>(std::lround(static_cast<double>(dist) * std::sin(rad)));
      if (x < 0 || y < 0 || x >= layout.width || y >= layout.height) continue;
      const int a = alpha_at(pixels, layout.width, x, y);
      if (a > 4 && a < 251) partial.push_back(a);
    }
  }
  std::sort(partial.begin(), partial.end());
  partial.erase(std::unique(partial.begin(), partial.end()), partial.end());
  st::log::out("[corner-aa] 弧带部分覆盖值 {} 个：", partial.size());
  for (const int a : partial) st::log::out(" {} ", a);
  st::log::out("\n");

  ST_CHECK(partial.size() >= 3U);
  // 必须有一档接近窗口色（>= 200）：证明弧上画的是"窗口色×覆盖率"，
  // 而不是只有阴影。
  ST_CHECK(partial.back() >= 200);
}
/// AA 开关的**语义**：关掉后弧上不再有"窗口色半透"那一档。
///
/// 判据用**整角扫描**而不是固定几行采样——AA 带只有 1 像素宽
/// （`d ∈ (-0.5, 0.5)`），固定采样点大概率落空（实测：用 `dist=r±2` 的
/// 45° 采样时，开关两态差 0 个像素，看起来像"参数没生效"，其实是没采到）。
ST_TEST(window_chrome_ring_can_disable_corner_aa) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(400, 300, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  const auto make = [&](bool antialias) {
    std::vector<std::uint32_t> px(
        static_cast<std::size_t>(layout.width) * static_cast<std::size_t>(layout.height), 0U);
    (void)st::shell::build_ring_pixels(layout, style, 0x0A0A12U, px, 0xF7F7FAU, antialias);
    return px;
  };
  const auto off_pixels = make(false);
  const auto on_pixels = make(true);

  // 扫整个左上角区域（含弧与紧邻的阴影带），统计两态差异像素数。
  const int m = layout.margin;
  const int span = layout.radius + layout.margin + 4;
  int differing = 0;
  int on_partial = 0;
  for (int y = 0; y < span && y < layout.height; ++y) {
    for (int x = 0; x < span && x < layout.width; ++x) {
      const int a_off = alpha_at(off_pixels, layout.width, x, y);
      const int a_on = alpha_at(on_pixels, layout.width, x, y);
      if (a_off != a_on) ++differing;
      if (x >= m && y >= m && a_on > 4 && a_on < 251) ++on_partial;
    }
  }
  st::log::out("[corner-aa switch] 两态差异像素 {} 个；开态角内部分覆盖 {} 个\n", differing,
               on_partial);
  ST_CHECK(differing >= 1);
  ST_CHECK(on_partial >= 1);
}

/// 自绘圆角的过渡档数必须达到 **DWM 同款量级**（>= 5 档）。
///
/// ## 为何是"档数"而不是"有没有过渡"
///
/// 覆盖率公式写错时（`clamp(1.5 - d, 0, 1)`）弧上仍有中间值，只判"存在部分覆盖"
/// 会通过——而实际观感是硬边。必须判**档数**。
///
/// ## 采样范围必须限定在弧带内（踩过）
///
/// 第一版扫整个角区域（含旁边的阴影），结果把**阴影本身的连续衰减**也数进去了
/// （80 档），于是把公式回退成错的也照样绿（假绿）。
/// 现在只取"窗口角内、距圆心 ±(radius±1)"那一圈——那里才是圆角 AA 带。
///
/// ## 基线（真实窗口像素，背景 11 / 窗口 247）
///
/// | 实现 | 弧带过渡值 | 档数 |
/// |---|---|---|
/// | DWM（对齐目标） | 34/80/127/174/221 | 5 |
/// | 本框架自绘（修正后） | 24/86/100/132/148/163/196/228 | 8 |
/// | 本框架自绘（公式错，`1.5-d`） | 34/59/192 | 2~3 |
ST_TEST(window_chrome_corner_ramp_matches_dwm_grade) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(400, 300, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  const auto pixels = build(layout, style);
  ST_REQUIRE(!pixels.empty());

  const int m = layout.margin;
  const int r = layout.radius;
  const int cx = m + r;
  const int cy = m + r;
  // 只沿圆周取一圈样本：距圆心 r-1 .. r+1 的 8 个方向。
  // 这样排除阴影带（它在 r 之外更远处）与窗口内部（r 之内更深处）。
  std::vector<int> levels;
  for (int deg = 180; deg <= 270; deg += 5) {
    const double rad = static_cast<double>(deg) * 3.14159265358979 / 180.0;
    for (int dist = r - 1; dist <= r + 1; ++dist) {
      const int x = cx + static_cast<int>(std::lround(static_cast<double>(dist) * std::cos(rad)));
      const int y = cy + static_cast<int>(std::lround(static_cast<double>(dist) * std::sin(rad)));
      if (x < 0 || y < 0 || x >= layout.width || y >= layout.height) continue;
      const int a = alpha_at(pixels, layout.width, x, y);
      if (a > 4 && a < 251) levels.push_back(a);
    }
  }
  std::sort(levels.begin(), levels.end());
  levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
  st::log::out("[corner-ramp] 弧带过渡档数 = {}：", levels.size());
  for (const int a : levels) st::log::out(" {} ", a);
  st::log::out("\n");

  // 5 档是 DWM 的量级；低于它弧就是硬边。
  ST_CHECK(levels.size() >= 5U);
  // 正交量：必须覆盖"接近半透"那一档——那是 1px 抗锯齿的特征，
  // 也是 `clamp(1.5 - d, ...)` 那种错误公式给不出的。
  bool has_half = false;
  for (const int a : levels) {
    if (a > 100 && a < 165) has_half = true;
  }
  ST_CHECK(has_half);
}


/// **圆角与直线的交界处不得有孤立暗点**。
///
/// ## 这个缺陷长什么样
///
/// 用户报「白色背景上窗口圆角有两个黑点」。实测：圆角弧与左右直边的交界处，
/// 影子层画了几个 `alpha=107` 的纯阴影像素，紧邻却是窗口内容（合成亮度 246）。
/// 合成到白底后这几个点是 147 —— 孤立的一撮黑。
///
/// ## 成因
///
/// 阴影公式 `t = clamp((d-1)/fade, 0, 1)` 在 `d < 1` 时被 clamp 成 0，
/// 于是取到**满强度**。也就是「窗口边缘紧邻的一圈永远是全图最暗」，
/// 圆弧在这里刚好离开窗口本体，那几个像素就裸露出来。
///
/// 修法：阴影改成**升-降包络**（贴窗口为 0，向外先增后减）。
///
/// ## 判据
///
/// 沿弧带逐像素把它合成到**白底**上，检查有没有
/// 「比右邻与下邻都暗 60 级以上」的孤立点。有就是黑点。
ST_TEST(window_chrome_arc_has_no_isolated_dark_dots) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(400, 300, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  const auto pixels = build(layout, style);
  ST_REQUIRE(!pixels.empty());

  // 预乘 BGRA 合成到白底 247：out = R + 247 * (1 - a/255)
  const auto on_white = [&](int x, int y) -> int {
    const std::uint32_t p =
        pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(layout.width) + x];
    const int alpha = static_cast<int>((p >> 24) & 0xFFU);
    const int red = static_cast<int>((p >> 16) & 0xFFU);
    return red + 247 * (255 - alpha) / 255;
  };

  const int m = layout.margin;
  const int r = layout.radius;
  int dots = 0;
  for (int y = m - 2; y < m + r + 3; ++y) {
    for (int x = m - 2; x < m + r + 3; ++x) {
      if (x + 1 >= layout.width || y + 1 >= layout.height) continue;
      if (x < 0 || y < 0) continue;
      const int here = on_white(x, y);
      const int right = on_white(x + 1, y);
      const int down = on_white(x, y + 1);
      if (here + 60 < right && here + 60 < down) {
        st::log::out("[dark-dot] ({},{}) 合成亮度={} 右={} 下={}\n", x, y, here, right, down);
        ++dots;
      }
    }
  }
  st::log::out("[dark-dot] 左上角孤立暗点 {} 个（判据 == 0）\n", dots);
  ST_CHECK(dots == 0);
}
