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





/// 圆角边缘要有**标准 1 像素抗锯齿**（与 DWM 同款口径）。
///
/// 覆盖率 = `clamp(0.5 - d, 0, 1)`（d = 像素中心到弧的有符号距离）：
/// 弧上中心（d=0）覆盖 **0.5**，而不是 1.0。
///
/// 早先写的是 `clamp(1.5 - d, 0, 1)`，它在 d=0 时给覆盖 1.0，于是弧上
/// **只有一档覆盖**（要么满要么零）——那正是"圆角有台阶"的根因。
/// 因此判据是「弧附近必须出现**至少两个不同**的部分覆盖值」，
/// 而不是"存在某个高覆盖率像素"（后者用错的公式也能满足）。
///
/// 对照 DWM 真实像素：过渡值 34/80/127/174/221（等差 ~47），即多档均匀分布。
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
  for (int dist = r - 3; dist <= r + 3; ++dist) {
    if (dist <= 0) continue;
    const int off = (dist * 7071) / 10000;
    const int x = cx - off;
    const int y = cy - off;
    if (x < 0 || y < 0) continue;
    const int a = alpha_at(pixels, layout.width, x, y);
    if (a > 4 && a < 251) partial.push_back(a);
  }
  std::sort(partial.begin(), partial.end());
  partial.erase(std::unique(partial.begin(), partial.end()), partial.end());
  st::log::out("[corner-aa] 弧附近的部分覆盖值 {} 个：", partial.size());
  for (const int a : partial) st::log::out(" {} ", a);
  st::log::out("\n");

  ST_CHECK(partial.size() >= 2U);
  bool has_half = false;
  for (const int a : partial) {
    if (a > 90 && a < 175) has_half = true;
  }
  ST_CHECK(has_half);
}


ST_TEST(window_chrome_shadow_is_visible_at_the_edge) {
  WindowChromeStyle style;
  const WindowChromeGeometry geometry = make_geometry(400, 300, 1.0f);
  const auto layout = st::shell::ring_layout(geometry, style);
  const auto pixels = build(layout, style);
  ST_REQUIRE(!pixels.empty());

  // ## 采样必须避开 AA 带（踩过）
  //
  // 紧贴窗口的那一像素属于**圆角 AA 带**（用窗口色不透明覆盖，实测 alpha=255），
  // 它不随 `shadow_alpha` 变。第一版取 d=1，于是 alpha 改动测不出来（假绿）。
  // 正确取**中段** d=4..8：实测 alpha 50..17（0.42）vs 36..12（0.30）。
  const int m = layout.margin;
  const int y = layout.height / 2;   // 客户区中部：左右是直边，无圆角干扰
  std::vector<int> mid;
  for (int d = 4; d <= 8; ++d) {
    mid.push_back(alpha_at(pixels, layout.width, m - d, y));
  }
  st::log::out("[shadow] d=4..8 的 alpha：");
  for (const int a : mid) st::log::out(" {} ", a);
  st::log::out("\n");

  ST_CHECK(!mid.empty());
  if (!mid.empty()) {
    ST_CHECK(mid.front() >= 62);        // 实测 0.42 -> 73，0.30 -> 52：阈值取中间
    ST_CHECK(mid.front() > mid.back()); // 向外变淡
    const int outer = alpha_at(pixels, layout.width, 0, y);
    ST_CHECK(mid.front() > outer + 30); // 不能“一刀切”
  }
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
