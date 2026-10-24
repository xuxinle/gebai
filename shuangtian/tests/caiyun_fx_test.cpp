/// 裁云特效层单测：确定性、零开销、边界。
///
/// ## 为什么这几条值得钉住
///
/// 全仓的 e2e 判据是"同一时刻渲染两次逐像素相同"（`motion.hpp` 的纯函数约定）。
/// 特效层引入了三类**可能破坏它**的东西：
/// 1. 粒子（看起来就该随机——用的是哈希而不是随机引擎，这条要钉死）；
/// 2. 冲击震动（正弦叠加时间）；
/// 3. 逐像素逆映射（浮点运算顺序可能因实现而异，但同一进程内必须稳定）。
///
/// 另外两个容易踩的点：特效"关闭时零开销"（强度 0 不能被当成"画一层透明的"），
/// 以及运镜放大后**不能裁掉内容**（安全边距）。

#include <cmath>
#include <cstdint>
#include <vector>

// ⚠ 直接包含**实现**而不是只包含头：fx.cpp 在 examples/ 下，而测试构建只编译
// tests/*_test.cpp + 框架库源码，不会链接 examples 的目标文件。
//
// 这么做的前提是 fx.cpp 自身只依赖框架库（它没有别的 examples 依赖），
// 否则会在测试里牵出一串缺少符号。
#include "examples/caiyun/fx.cpp"

#include "st/test/test.hpp"
#include "st/raster/canvas.hpp"

namespace {

using caiyun::CameraMove;
using caiyun::GlowSpec;
using caiyun::ParticleField;
using st::math::Color;

/// 把画布像素拷成可比较的向量（`Canvas` 不可拷贝）。
[[nodiscard]] auto snapshot(const st::raster::Canvas& canvas) -> std::vector<std::uint32_t> {
  const auto pixels = canvas.pixels();
  return std::vector<std::uint32_t>(pixels.begin(), pixels.end());
}

/// 统计两幅同尺寸画布中不同的像素数。
[[nodiscard]] auto diff_pixels(const std::vector<std::uint32_t>& a,
                               const std::vector<std::uint32_t>& b) -> std::size_t {
  if (a.size() != b.size()) return a.size() + b.size();
  std::size_t count = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) ++count;
  }
  return count;
}

/// 一张有内容的测试画布（纯色 + 一块对比色，便于观察位移/缩放）。
[[nodiscard]] auto make_canvas() -> st::raster::Canvas {
  st::raster::Canvas canvas{160, 90, 1.0f};
  canvas.clear(Color{10, 14, 26, 255});
  canvas.fill_rect(st::math::Rect{60.0f, 30.0f, 40.0f, 30.0f},
                   st::raster::Paint::solid(Color{200, 220, 255, 255}));
  return canvas;
}

}  // namespace

// ── hash01：无状态确定性 ──

ST_TEST(fx_hash01_is_deterministic_and_well_distributed) {
  // 同一个 (a,b) 永远同一个值——粒子的不确定性全靠它。
  for (std::uint32_t i = 0; i < 64; ++i) {
    ST_CHECK(caiyun::hash01(i, 7u) == caiyun::hash01(i, 7u));
  }
  // 取值范围必须是 [0,1)（越界会让透明度/尺寸算出负数）。
  for (std::uint32_t i = 0; i < 256; ++i) {
    const float value = caiyun::hash01(i, 3u);
    ST_CHECK(value >= 0.0f);
    ST_CHECK(value < 1.0f);
  }
  // 相邻 index 不能给出相邻值：否则粒子会排成整齐的行列（一眼假）。
  // 判据：相邻差值的均值应当接近 1/3（均匀分布的期望），而不是接近 0。
  float sum_gap = 0.0f;
  for (std::uint32_t i = 0; i < 200; ++i) {
    sum_gap += std::fabs(caiyun::hash01(i + 1, 1u) - caiyun::hash01(i, 1u));
  }
  const float mean_gap = sum_gap / 200.0f;
  ST_CHECK(mean_gap > 0.22f);
  ST_CHECK(mean_gap < 0.45f);
}

// ── 粒子：确定性 + 时间会改变画面 ──

ST_TEST(fx_particles_are_deterministic_at_same_time) {
  ParticleField field{};
  field.count = 40;
  auto canvas_a = make_canvas();
  auto canvas_b = make_canvas();
  caiyun::paint_particles(canvas_a, field, 1.234, 1.0f);
  caiyun::paint_particles(canvas_b, field, 1.234, 1.0f);
  // 同一时刻两次渲染必须逐像素相同（全仓 e2e 判据）。
  ST_CHECK(diff_pixels(snapshot(canvas_a), snapshot(canvas_b)) == 0);
}

ST_TEST(fx_particles_change_with_time) {
  ParticleField field{};
  field.count = 60;
  field.drift_x = 40.0f;   // 放大漂移，让"时间不同 → 位置不同"可判
  auto canvas_a = make_canvas();
  auto canvas_b = make_canvas();
  caiyun::paint_particles(canvas_a, field, 0.0, 1.0f);
  caiyun::paint_particles(canvas_b, field, 2.5, 1.0f);
  // 时间推进后画面必须变化（否则"粒子"是贴图而不是尘）。
  ST_CHECK(diff_pixels(snapshot(canvas_a), snapshot(canvas_b)) > 0);
}

ST_TEST(fx_particles_off_has_zero_cost) {
  // 关闭（count=0 或 alpha=0）时不得改动任何像素。
  auto canvas = make_canvas();
  const auto before = snapshot(canvas);
  ParticleField off_count{};
  off_count.count = 0;
  caiyun::paint_particles(canvas, off_count, 1.0, 1.0f);
  ParticleField off_alpha{};
  off_alpha.count = 50;
  off_alpha.alpha = 0.0f;
  caiyun::paint_particles(canvas, off_alpha, 1.0, 1.0f);
  ST_CHECK(diff_pixels(before, snapshot(canvas)) == 0);
}

// ── 光晕：无可见环（这是实测踩过的坑）──

ST_TEST(fx_glow_has_no_visible_rings) {
  // 早先用"14 层同心圆"近似径向渐变，每层边缘都留下一个可见的环
  //（观感上比不加光晕还难看）。改为逐像素衰减后，沿半径方向的亮度
  // 必须单调递减——出现回升就说明有环。
  st::raster::Canvas canvas{200, 200, 1.0f};
  canvas.clear(Color{0, 0, 0, 255});
  GlowSpec spec{};
  spec.cx = 0.5f;
  spec.cy = 0.5f;
  spec.radius = 0.45f;
  spec.intensity = 0.9f;
  spec.color = Color{255, 255, 255, 255};
  caiyun::paint_glow(canvas, spec, 1.0f);

  // 判据一：沿半径方向亮度**单调不增**。
  int previous = 255;
  int increases = 0;
  for (int x = 100; x < 190; x += 2) {
    const auto pixel = canvas.pixel_at(x, 100);
    const int value = static_cast<int>(pixel.r);
    if (value > previous + 1) ++increases;   // +1 容忍取整噪点
    previous = value;
  }
  ST_CHECK(increases == 0);

  // 判据二（更严，覆盖"量化产生的台阶"）：相邻像素的**二阶差分不能有台阶**。
  //
  // 为什么需要它：只查单调性抓不住"半径量化"造成的阶梯——阶梯本身是单调不增的
  //（实测：把半径取整到 1/8 后，单调性判据仍然全绿，但画面上一圈圈环很明显）。
  // 环的本质是**亮度在局部出现平台再跳变**，即二阶差分在台阶处出现尖峰。
  int max_second_diff = 0;
  int prev_value = static_cast<int>(canvas.pixel_at(100, 100).r);
  int prev_diff = 0;
  for (int x = 101; x < 190; ++x) {
    const int value = static_cast<int>(canvas.pixel_at(x, 100).r);
    const int diff = value - prev_value;
    max_second_diff = std::max(max_second_diff, std::abs(diff - prev_diff));
    prev_diff = diff;
    prev_value = value;
  }
  // 真径向渐变的二阶差分很小（相邻最多差几个灰阶），台阶会到 10+。
  ST_CHECK(max_second_diff <= 8);
}

ST_TEST(fx_glow_off_changes_nothing) {
  auto canvas = make_canvas();
  const auto before = snapshot(canvas);
  GlowSpec spec{};
  spec.intensity = 0.0f;
  caiyun::paint_glow(canvas, spec, 1.0f);
  ST_CHECK(diff_pixels(before, snapshot(canvas)) == 0);
}

// ── 景深：强度 0 不动画面、强度大会丢失细节 ──

ST_TEST(fx_dof_off_is_identity) {
  auto source = make_canvas();
  const auto before = snapshot(source);
  const auto result = caiyun::depth_of_field(source, 0.0f);
  ST_CHECK(result.physical_width() == source.physical_width());
  ST_CHECK(result.physical_height() == source.physical_height());
  // 强度 0 = 不处理：与原图逐像素相同。
  ST_CHECK(diff_pixels(before, snapshot(result)) == 0);
}

ST_TEST(fx_dof_blurs_details) {
  // 造一张高频图（棋盘），模糊后相邻像素差应当显著变小。
  st::raster::Canvas source{120, 120, 1.0f};
  source.clear(Color{0, 0, 0, 255});
  for (int y = 0; y < 120; ++y) {
    for (int x = 0; x < 120; ++x) {
      if ((x / 3 + y / 3) % 2 == 0) {
        source.fill_rect(st::math::Rect{static_cast<float>(x), static_cast<float>(y), 1.0f, 1.0f},
                         st::raster::Paint::solid(Color{255, 255, 255, 255}));
      }
    }
  }
  const auto sharp = caiyun::depth_of_field(source, 0.0f);
  const auto blurred = caiyun::depth_of_field(source, 1.0f);
  const auto local_contrast = [](const st::raster::Canvas& canvas) {
    int total = 0;
    int count = 0;
    for (int y = 10; y < 110; y += 5) {
      for (int x = 10; x < 109; x += 5) {
        const int a = static_cast<int>(canvas.pixel_at(x, y).r);
        const int b = static_cast<int>(canvas.pixel_at(x + 1, y).r);
        total += std::abs(a - b);
        ++count;
      }
    }
    return count > 0 ? total / count : 0;
  };
  // 模糊后局部对比必须下降（这是"虚化"的可度量定义）。
  ST_CHECK(local_contrast(blurred) < local_contrast(sharp));
}

// ── 运镜：静止时不改画面、推镜会放大内容 ──

ST_TEST(fx_camera_static_is_identity) {
  auto source = make_canvas();
  const auto before = snapshot(source);
  CameraMove move{};   // 全部默认：无缩放/旋转/平移/震动
  const auto result = caiyun::apply_camera(source, move, 0.5f, 0.0, 1.0f);
  ST_CHECK(diff_pixels(before, snapshot(result)) == 0);
}

ST_TEST(fx_camera_zoom_in_enlarges_content) {
  // 推镜后，原来 40px 宽的白块应当覆盖更多像素。
  auto source = make_canvas();
  const auto count_bright = [](const st::raster::Canvas& canvas) {
    std::size_t count = 0;
    const auto pixels = canvas.pixels();
    for (const std::uint32_t value : pixels) {
      // 预乘 RGBA：亮块的 R 通道在高位
      if (((value >> 24U) & 0xFFU) > 150U) ++count;
    }
    return count;
  };
  const std::size_t before = count_bright(source);
  CameraMove move{};
  move.zoom_from = 1.0f;
  move.zoom_to = 1.5f;
  const auto zoomed = caiyun::apply_camera(source, move, 1.0f, 0.0, 1.0f);
  ST_CHECK(count_bright(zoomed) > before);
}

ST_TEST(fx_camera_shake_decays_to_zero) {
  // 震动幅度随时间指数衰减：足够久之后必须完全静止（否则片尾会"发抖"）。
  auto source = make_canvas();
  CameraMove move{};
  move.shake = 0.02f;
  move.shake_decay = 8.0f;
  const auto still = caiyun::apply_camera(source, move, 0.0f, 100.0, 1.0f);
  const auto before = snapshot(source);
  ST_CHECK(diff_pixels(before, snapshot(still)) == 0);
}

ST_TEST(fx_camera_window_clamps_outside_and_advances_inside) {
  // 窗口语义：窗口外夹在 0/1，窗口内线性。
  //
  // 调的是**真实实现**（caiyun::camera_progress），不在测试里复算公式——
  // 复算只能证明“我抄的公式对”，实现写错也不红（本仓踩过的“恒绿测试”）。
  using caiyun::camera_progress;
  // 窗口 [3, 8]（5 秒）
  ST_CHECK_NEAR(camera_progress(0.0f, 12.0f, 3.0f, 8.0f), 0.0f, 1e-5f);
  ST_CHECK_NEAR(camera_progress(2.99f, 12.0f, 3.0f, 8.0f), 0.0f, 1e-5f);
  ST_CHECK_NEAR(camera_progress(5.5f, 12.0f, 3.0f, 8.0f), 0.5f, 1e-5f);
  ST_CHECK_NEAR(camera_progress(8.0f, 12.0f, 3.0f, 8.0f), 1.0f, 1e-5f);
  // 窗口之后必须**稳稳停在 1**：这是"钻到位就静止让观众读"的全部含义。
  ST_CHECK_NEAR(camera_progress(10.0f, 12.0f, 3.0f, 8.0f), 1.0f, 1e-5f);
  ST_CHECK_NEAR(camera_progress(12.0f, 12.0f, 3.0f, 8.0f), 1.0f, 1e-5f);
}

ST_TEST(fx_camera_window_disabled_matches_whole_span) {
  // 不填窗口（end <= start）时必须退回整段线性——老脚本逐像素不变。
  using caiyun::camera_progress;
  for (const float t : {0.0f, 3.0f, 6.0f, 12.0f}) {
    for (const float end : {0.0f, -1.0f}) {
      ST_CHECK_NEAR(camera_progress(t, 12.0f, 0.0f, end), t / 12.0f, 1e-5f);
    }
  }
}

ST_TEST(fx_camera_window_zero_span_is_finite) {
  // start == end 是"未启用"，但若有人写 [5,5] 也不能除零。
  const float value = caiyun::camera_progress(5.0f, 12.0f, 5.0f, 5.0f);
  ST_CHECK(std::isfinite(value));
  ST_CHECK(value >= 0.0f && value <= 1.0f);
}

ST_TEST(fx_camera_window_keeps_last_frames_identical) {
  // 端到端判据：窗口 [0,8] + 12s 幕 ⇒ 第 8s 与第 12s 的画面必须**逐像素相同**。
  // 这是"落定后静止"能被量出来的形态（而不是靠肉眼说"好像停了"）。
  auto source = make_canvas();
  CameraMove move{};
  move.zoom_from = 1.0f;
  move.zoom_to = 6.0f;
  move.anchor_x = 0.5f;
  move.anchor_y = 0.5f;
  move.zoom_curve = caiyun::ZoomCurve::ExpoIn;

  const float p8 = caiyun::camera_progress(8.0f, 12.0f, 0.0f, 8.0f);
  const float p12 = caiyun::camera_progress(12.0f, 12.0f, 0.0f, 8.0f);
  const auto a = caiyun::apply_camera(source, move, p8, 8.0, 1.0f);
  const auto b = caiyun::apply_camera(source, move, p12, 12.0, 1.0f);
  ST_CHECK(diff_pixels(snapshot(a), snapshot(b)) == 0);
}

// ═══ ScriptShape.rotate / 多边形的几何 ═══
//
// ## 这组钉的是什么
//
// 两个能力之前都是**画了但不生效**、且都不报错：
//   · `rotate` 有字段、`layout.cpp` 也解析了，但 `paint_script_shapes` 的
//     switch 一个分支都不读它（脚本里写了不报错，只是画面是正的）；
//   · 没有多边形图元（kind 只到 3），倾斜/透视只能拿多个轴对齐矩形错位拼。
//
// 算术提到 `fx` 就是为了能在这里对着**真实实现**断言——`motion.cpp`
// 拖一整条依赖（scene/carry/kin_text/model3d），include 不进测试。

namespace {

using Point = st::math::Point;

/// 轴对齐包围盒：{min_x, max_x, min_y, max_y}
[[nodiscard]] auto bbox(const std::vector<Point>& pts) -> std::array<float, 4> {
  float min_x = pts[0].x, max_x = pts[0].x;
  float min_y = pts[0].y, max_y = pts[0].y;
  for (const Point& p : pts) {
    min_x = std::min(min_x, p.x);
    max_x = std::max(max_x, p.x);
    min_y = std::min(min_y, p.y);
    max_y = std::max(max_y, p.y);
  }
  return {min_x, max_x, min_y, max_y};
}

/// 多边形面积（鞋带公式）。
[[nodiscard]] auto polygon_area(const std::vector<Point>& pts) -> float {
  float sum = 0.0f;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const Point& a = pts[i];
    const Point& b = pts[(i + 1) % pts.size()];
    sum += a.x * b.y - b.x * a.y;
  }
  return std::abs(sum) * 0.5f;
}

[[nodiscard]] auto square(float x, float y, float size) -> std::vector<Point> {
  return {Point{x, y}, Point{x + size, y}, Point{x + size, y + size}, Point{x, y + size}};
}

}  // namespace

ST_TEST(fx_rotate_point_is_identity_at_zero_degrees) {
  const Point p{123.5f, 456.25f};
  const Point out = caiyun::rotate_point_deg(p, Point{100.0f, 400.0f}, 0.0f);
  ST_CHECK(out.x == p.x);
  ST_CHECK(out.y == p.y);
}

ST_TEST(fx_rotate_point_keeps_distance_to_center) {
  // 旋转的必要条件：到中心距离不变（这条错，图元会边走边缩）。
  const Point c{960.0f, 540.0f};
  const Point p{1100.0f, 700.0f};
  const float r0 = std::hypot(p.x - c.x, p.y - c.y);
  for (const float deg : {15.0f, 35.0f, 90.0f, 180.0f, -47.0f}) {
    const Point out = caiyun::rotate_point_deg(p, c, deg);
    ST_CHECK(std::abs(r0 - std::hypot(out.x - c.x, out.y - c.y)) < 0.01f);
  }
}

ST_TEST(fx_rotate_quarter_turn_is_clockwise) {
  // ## 为何不用 90°（本轮踩到的真漏洞）
  //
  // 原来这条用 90° 断言 `|x|≈0 && |y-100|≈0`，**方向反了也成立**：
  // cos(90°)≈0 使两种符号都得到 (0, ±100)，取绝对值后完全不可区分
  // （逆向验证实测：把 `-dy*s` 改成 `+dy*s` 后这条仍然绿）。
  //
  // 改用 30°：顺时针（y 向下）下 (100,0) 应到 (86.6, 50)；
  // 逆时针则到 (86.6, -50)——y 的符号把两者分开。
  constexpr float kDeg30 = 30.0f;
  const Point out = caiyun::rotate_point_deg(Point{100.0f, 0.0f}, Point{0.0f, 0.0f}, kDeg30);
  ST_CHECK(std::abs(out.x - 86.6025f) < 0.01f);
  ST_CHECK(out.y > 0.0f);                      // 顺时针：y 为正（向下）
  ST_CHECK(std::abs(out.y - 50.0f) < 0.01f);
}

ST_TEST(fx_rotated_square_bbox_grows_and_area_is_conserved) {
  // 200×200 正方旋转 35°：包围盒 = 200·(cos35+sin35) ≈ 278，面积不变。
  // 两条判据必须同时：只有包围盒变胖时，“宽高一起乘 cos”的错误实现能蒙过；
  // 只有面积守恒时，“根本没转”也守恒。
  auto pts = square(560.0f, 340.0f, 200.0f);
  const float area_before = polygon_area(pts);
  caiyun::rotate_points_about_bounds(pts.data(), pts.size(), 35.0f);
  const auto b = bbox(pts);
  const float rad = 35.0f * 3.14159265358979f / 180.0f;
  const float expect = 200.0f * (std::cos(rad) + std::sin(rad));
  ST_CHECK(std::abs((b[1] - b[0]) - expect) < 1.0f);  ST_CHECK(std::abs((b[3] - b[2]) - expect) < 1.0f);
  ST_CHECK((b[1] - b[0]) > 255.0f);
  ST_CHECK(std::abs(polygon_area(pts) - area_before) < 1.0f);
}

ST_TEST(fx_rotate_about_bounds_keeps_bbox_center_fixed) {
  // 旋转中心必须是**包围盒中心**：对手画的不均匀多边形，用顶点均值会让
  // 图元“转的同时整体飘一下”，而脚本作者不会预期位置会变。
  //
  // 判据要小心：旋转后**包围盒**中心本来就会变（转了之后外包框换了），
  // 不变的是**旋转中心**。所以这里验两条:
  //   ① 顶点到“原包围盒中心”的距离在旋转后不变（中心真的就是它）；
  //   ② 包围盒中心的新位置，恰好等于“原中心绕自身旋转”（即不动）——
  //      用 180° 验证：此时包围盒必定围绕原中心对称，中心不变。
  std::vector<Point> pts{Point{100.0f, 100.0f}, Point{300.0f, 140.0f},
                         Point{180.0f, 260.0f}};
  const auto before = bbox(pts);
  const Point center{(before[0] + before[1]) * 0.5f, (before[2] + before[3]) * 0.5f};

  // ① 每个顶点到中心的距离守恒
  std::vector<float> dist_before;
  for (const Point& p : pts) dist_before.push_back(std::hypot(p.x - center.x, p.y - center.y));
  caiyun::rotate_points_about_bounds(pts.data(), pts.size(), 40.0f);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const float d = std::hypot(pts[i].x - center.x, pts[i].y - center.y);
    ST_CHECK(std::abs(d - dist_before[i]) < 0.01f);
  }

  // ② 180° 后包围盒中心回到原处
  std::vector<Point> half{Point{100.0f, 100.0f}, Point{300.0f, 140.0f},
                          Point{180.0f, 260.0f}};
  caiyun::rotate_points_about_bounds(half.data(), half.size(), 180.0f);
  const auto turned = bbox(half);
  ST_CHECK(std::abs((turned[0] + turned[1]) * 0.5f - center.x) < 0.01f);
  ST_CHECK(std::abs((turned[2] + turned[3]) * 0.5f - center.y) < 0.01f);
}

ST_TEST(fx_rotate_zero_is_noop_and_null_is_safe) {
  auto pts = square(540.0f, 260.0f, 200.0f);
  const auto copy = pts;
  caiyun::rotate_points_about_bounds(pts.data(), pts.size(), 0.0f);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    ST_CHECK(pts[i].x == copy[i].x);
    ST_CHECK(pts[i].y == copy[i].y);
  }
  // 不足 3 个顶点的多边形不会被绘制，但旋转函数也不该越界。
  caiyun::rotate_points_about_bounds(nullptr, 0, 30.0f);
  std::vector<Point> one{Point{10.0f, 20.0f}};
  caiyun::rotate_points_about_bounds(one.data(), one.size(), 30.0f);
  ST_CHECK(one[0].x == 10.0f);
  ST_CHECK(one[0].y == 20.0f);
}

