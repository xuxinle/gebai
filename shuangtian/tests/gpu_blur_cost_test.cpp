/// GPU 腿 `blur_region` 成本基准（与 `raster_blur_cost_test.cpp` 同口径）。
///
/// 背景：软件腿的量测（1473×828 · r14）把「常驻面板毛玻璃」判为不可行
/// （全窗 52~60ms = 3 倍帧预算）。但 GPU 腿实现（D3D11 离屏 ping-pong，
/// `platform_d3d11.cpp` `blur_region`）从未量过——若它在全窗档落到几 ms，
/// 「玻璃 = GPU 能力、软件腿自动降档」的架构就有了数据依据。
///
/// 判据（与软件腿用例同思路，只锁量级关系而非绝对 ms）：
/// ① 全窗档必须显著便宜于软件腿同面积档（GPU 若不便宜，架构就不成立）；
/// ② 弹层档绝对耗时可控（防平方级回归）；
/// ③ 成本随面积单调增。
/// 量级断言的阈值故意放宽（GPU 首帧含管线预热，取多轮最好成绩规避）。
///
/// ⚠ 判据 ③（成本随面积单调增）在本机长期为红，而**不是噪声**：实测三次稳定复现
/// （全窗 1.9~2.4ms / 侧栏 7.3~7.4ms / 弹层 7.3~7.4ms / 菜单 2.5~2.6ms），
/// 每次强制同步后仍如此。即“面积小 4 倍的侧栏比全窗慢 3 倍”——与
/// `blur_region` 的算法复杂度（3 pass × (2r+1) 采样，与面积线性）矛盾，
/// 指向驱动侧的小尺寸路径开销（待定位，见 `docs/BACKLOG.md`）。
/// 这一条保留在默认路径上：它是**真的不对**，不该用 `ST_TEST_SLOW` 藏起来。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/test/test.hpp"

namespace {

using Clock = std::chrono::steady_clock;

/// GPU 可用性（与 gpu_parity_test.cpp 同判据；本文件独立，不依赖其内部头）。
[[nodiscard]] auto gpu_ready() -> bool { return st::raster::gpu::available(); }

inline constexpr int kWidth = 1473;
inline constexpr int kHeight = 828;

[[nodiscard]] auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <class SurfaceT>
[[nodiscard]] auto measure_blur(SurfaceT& surface, st::math::Rect area, float radius) -> double {
  double best = 1e9;
  for (int index = 0; index < 9; ++index) {  // 多轮取最好：GPU 首轮含管线创建
    const auto start = Clock::now();
    (void)surface.blur_region(area, radius);
    best = std::min(best, elapsed_ms(start));
  }
  return best;
}

}  // namespace

ST_TEST(gpu_blur_region_cost_fits_frame_budget) {
  if (!gpu_ready()) return;  // 无 GPU 环境跳过（gpu_parity_test.hpp 提供）

  auto gpu = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  ST_CHECK(gpu.has_value());
  if (!gpu.has_value()) return;
  st::raster::Surface& target = **gpu;
  st::raster::Canvas software = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);

  const st::math::Color base{0x20, 0x20, 0x28, 0xFF};
  target.clear(base);
  software.clear(base);
  // 画一些结构（匀色区域的 blur 无从区分快慢，真实界面总有内容）。
  for (int i = 0; i < 8; ++i) {
    const float x = 40.0F + static_cast<float>(i) * 170.0F;
    target.fill_rect(st::math::Rect{x, 80.0F, 90.0F, 640.0F},
                     st::raster::Paint::solid({0x50, 0x60, 0x78, 0xFF}));
    software.fill_rect(st::math::Rect{x, 80.0F, 90.0F, 640.0F},
                       st::raster::Paint::solid({0x50, 0x60, 0x78, 0xFF}));
  }

  const auto full = st::math::Rect{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)};
  const auto sidebar = st::math::Rect{0, 0, 280.0F, static_cast<float>(kHeight)};
  const auto overlay = st::math::Rect{400, 200, 600.0F, 400.0F};
  const auto menu = st::math::Rect{600, 300, 240.0F, 320.0F};
  const float radius = 14.0F;

  const double gpu_full = measure_blur(target, full, radius);
  const double gpu_sidebar = measure_blur(target, sidebar, radius);
  const double gpu_overlay = measure_blur(target, overlay, radius);
  const double gpu_menu = measure_blur(target, menu, radius);
  // 软件腿只量全窗一档做对照（其余档已有 raster_blur_cost_test 的数据）。
  const double sw_full = measure_blur(software, full, radius);

  st::log::out("[gpu-blur-cost] 全窗 {:.2f}ms · 侧栏 {:.2f}ms · 弹层 {:.2f}ms · 菜单 {:.2f}ms"
              " ·（软件全窗对照 {:.2f}ms）\n",
              gpu_full, gpu_sidebar, gpu_overlay, gpu_menu, sw_full);

  // ① GPU 全窗必须显著便宜于软件腿（不便宜则「玻璃=GPU 能力」不成立）。
  ST_CHECK(gpu_full * 4.0 < sw_full);
  // ② 弹层档绝对耗时可控（宽安全网，防平方级回归）。
  ST_CHECK(gpu_overlay < 8.0);
  // ③ 成本随面积单调增。
  ST_CHECK(gpu_menu <= gpu_sidebar + 0.5);
  ST_CHECK(gpu_sidebar <= gpu_full + 0.5);
}
