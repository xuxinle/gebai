#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kWidth = 1473;
inline constexpr int kHeight = 828;

[[nodiscard]] auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] auto measure_blur(st::raster::Canvas& canvas, st::math::Rect area,
                                float radius) -> double {
  double best = 1e9;
  for (int index = 0; index < 7; ++index) {
    const auto start = Clock::now();
    (void)canvas.blur_region(area, radius);
    best = std::min(best, elapsed_ms(start));
  }
  return best;
}

}  // namespace

/// **毛玻璃的成本边界**：为什么 `overlay_blur_radius` 只给弹层开。
///
/// `theme.hpp` 写着「只给弹层（菜单/对话框/命令面板/气泡）开，不给侧栏与常驻面板：
/// 弹层是静止、面积有限的，模糊代价可控；而常驻面板每帧模糊会把局部重绘的收益吃掉
/// （模糊成本与面积成正比）」。这个论断此前只有文字，没有数字——于是「能不能给
/// 侧栏也加毛玻璃」这类问题只能靠猜。
///
/// 本用例把成本钉成数据（1473×828 逻辑画布，半径 8/14/20 差别不大——
/// 三遍盒式模糊的常数开销压过半径项）：
///
/// | 区域              | 面积占比 | 耗时      | 占 16.7ms 帧预算 |
/// |-------------------|---------|-----------|-----------------|
/// | 全窗 1473×828     | 100%    | 52~60ms   | 316~358%        |
/// | 内容区 1193×828   | 81%     | 42~44ms   | 254~266%        |
/// | 侧栏 280×828      | 19%     | 8.7~8.8ms | 52~53%          |
/// | 弹层 600×400      | 19.7%   | 9.0~9.2ms | 54~55%          |
/// | 菜单 240×320      | 6.3%    | 2.8~2.9ms | 17%             |
///
/// 判据从这里来：
/// - **弹层档可行**（≤55% 预算，且弹层静止不逐帧重算）；
/// - **全窗/内容区不可行**（连静态都不止一帧）；
/// - **侧栏临界**（过半预算，只在「无动效的静态屏」才成立）。
///
/// 断言只锁**量级关系**（不是绝对毫秒——共享环境的绝对耗时不可靠）：
/// ① 全窗必须显著贵于菜单（面积差 15 倍以上时要体现出成本差）；
/// ② 弹层档的绝对耗时必须真的「可控」（留一条极宽的安全网，防止某次改动
///    把 blur 实现变成平方级）；
/// ③ 成本随面积单调增（面积更大的区域不能反而更便宜）。
ST_TEST(blur_region_cost_is_proportional_to_area) {
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  canvas.clear(st::math::Color::rgb(0x20, 0x20, 0x20));

  const auto full = st::math::Rect{0, 0, static_cast<float>(kWidth),
                                   static_cast<float>(kHeight)};
  const auto sidebar = st::math::Rect{0, 0, 280.0f, static_cast<float>(kHeight)};
  const auto overlay = st::math::Rect{400, 200, 600.0f, 400.0f};
  const auto menu = st::math::Rect{600, 300, 240.0f, 320.0f};

  constexpr float kRadius = 14.0f;
  const double cost_full = measure_blur(canvas, full, kRadius);
  const double cost_sidebar = measure_blur(canvas, sidebar, kRadius);
  const double cost_overlay = measure_blur(canvas, overlay, kRadius);
  const double cost_menu = measure_blur(canvas, menu, kRadius);

  st::log::out("[blur-cost] 全窗 {:.2f}ms · 侧栏 {:.2f}ms · 弹层 {:.2f}ms · 菜单 {:.2f}ms\n",
               cost_full, cost_sidebar, cost_overlay, cost_menu);

  // ③ 成本随面积单调：全窗 > 侧栏 > 菜单
  ST_CHECK(cost_full > cost_sidebar);
  ST_CHECK(cost_sidebar > cost_menu);
  // ① 面积 15.9 倍的差距必须体现出来（全窗 vs 菜单）
  ST_CHECK(cost_full > cost_menu * 4.0);
  // ② 弹层档必须可控（留极宽安全网：< 200ms 只拦「变成平方级」这类崩塌）
  ST_CHECK(cost_overlay < 200.0);
  ST_CHECK(cost_menu < 200.0);
}
