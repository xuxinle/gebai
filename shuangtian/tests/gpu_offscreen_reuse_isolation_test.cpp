/// GPU 腿**离屏缓存/复用**的独立性回归（同类缺陷的横向排查）。
///
/// ## 这份文件要回答的问题
///
/// 2026-11-28 修掉的那两个缺陷（毛玻璃采样没进子矩形、核越界采到清屏色）
/// 有同一个形状：**同一块离屏资源被不同尺寸/不同内容复用，而消费方按“只有我
/// 一个人用”推坐标**。既然这个形状在毛玻璃上出过两次，就必须横向问一遍：
/// 其它走离屏的绘制原语（阴影遮罩、路径遮罩）有没有同样的假设？
///
/// 结论（本文件用像素钉住）：
///   · 阴影遮罩：**按内容尺寸分配并整块渲染**，缓存键含宽度/高度/半径/模糊/偏移
///     ⇒ 不存在“大目标装小内容”，无此缺陷；
///   · 路径遮罩：按 `area` 建 **IMMUTABLE** 纹理且键含 area 四元
///     ⇒ 同样不存在。
///
/// 所以这里不写“修了什么”，而是写**不许退化成共享**：一旦有人把这两者改成
/// “复用一块大纹理”，下面两条立刻变红。
#include <cmath>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Rect;

[[nodiscard]] auto gpu_ready() -> bool { return st::raster::gpu::available(); }

/// 区域内最大通道差（`0xRRGGBBAA` 打包）。
[[nodiscard]] auto max_delta_in(const st::raster::Surface& left, const st::raster::Surface& right,
                                st::math::IntRect area) -> int {
  int worst = 0;
  for (int y = area.y; y < area.y + area.height; ++y) {
    for (int x = area.x; x < area.x + area.width; ++x) {
      const Color a = left.pixel_at(x, y);
      const Color b = right.pixel_at(x, y);
      worst = std::max({worst, std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
                        std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
                        std::abs(static_cast<int>(a.b) - static_cast<int>(b.b))});
    }
  }
  return worst;
}

}  // namespace

ST_TEST(gpu_shadow_is_independent_of_previous_shadow_sizes) {
  // 先画一个**远大于**目标的小尺寸阴影，再画小阴影；小阴影必须与
  // “一开始就只画小阴影”逐位一致。
  //
  // 若哪天阴影遮罩改成“复用一块只增不减的离屏目标”，这条会红——那正是
  // 毛玻璃踩过的坑（大目标装小内容、消费方仍按内容尺寸推坐标）。
  if (!gpu_ready()) return;
  constexpr int kWidth = 400;
  constexpr int kHeight = 300;

  auto fresh = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  auto warm = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  ST_CHECK(fresh.has_value());
  ST_CHECK(warm.has_value());
  if (!fresh.has_value() || !warm.has_value()) return;

  const Color base{0xF0, 0xF0, 0xF0, 0xFF};
  const Color shadow{0x00, 0x00, 0x00, 0x80};
  (*fresh)->clear(base);
  (*warm)->clear(base);

  const Rect small{40.0f, 40.0f, 120.0f, 80.0f};
  // ① 对照组：只画小阴影。
  (*fresh)->draw_shadow(small, 10.0f, 12.0f, shadow, st::math::Point{0.0f, 4.0f});
  // ② 实验组：先画一块尺寸完全不同的阴影（把离屏缓存撑满/打乱），再画同一个。
  (*warm)->draw_shadow(Rect{180.0f, 90.0f, 200.0f, 180.0f}, 16.0f, 28.0f, shadow,
                       st::math::Point{0.0f, 10.0f});
  (*warm)->draw_shadow(Rect{0.0f, 0.0f, 40.0f, 40.0f}, 6.0f, 6.0f, shadow,
                       st::math::Point{2.0f, 2.0f});
  (*warm)->clear(base);
  (*warm)->draw_shadow(small, 10.0f, 12.0f, shadow, st::math::Point{0.0f, 4.0f});

  // 小阴影的影响范围：矩形 + 偏移 + 2×模糊 + 余量。
  const auto reach = st::math::IntRect{30, 34, 140, 100};
  const int delta = max_delta_in(**fresh, **warm, reach);
  st::log::out("[shadow-reuse] 小阴影区最大 Δ{}\n", delta);
  ST_CHECK(delta == 0);
}

ST_TEST(gpu_path_mask_is_independent_of_previous_path_sizes) {
  // 路径遮罩同理：大路径先画、小路径后画，小路径的结果不得受前一次影响。
  if (!gpu_ready()) return;
  constexpr int kWidth = 400;
  constexpr int kHeight = 300;

  auto fresh = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  auto warm = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  ST_CHECK(fresh.has_value());
  ST_CHECK(warm.has_value());
  if (!fresh.has_value() || !warm.has_value()) return;

  const Color base{0xF0, 0xF0, 0xF0, 0xFF};
  const Color ink{0x20, 0x40, 0x80, 0xFF};
  (*fresh)->clear(base);
  (*warm)->clear(base);

  const auto build = [](Rect r) {
    st::math::Path path;
    path.move_to(st::math::Point{r.x, r.y});
    path.line_to(st::math::Point{r.x + r.width, r.y + r.height * 0.4f});
    path.line_to(st::math::Point{r.x + r.width * 0.3f, r.y + r.height});
    path.close();
    return path;
  };
  const Rect small{40.0f, 40.0f, 120.0f, 80.0f};
  (*fresh)->fill_path(build(small), st::raster::Paint::solid(ink));
  (*warm)->fill_path(build(Rect{150.0f, 60.0f, 220.0f, 200.0f}), st::raster::Paint::solid(ink));
  (*warm)->clear(base);
  (*warm)->fill_path(build(small), st::raster::Paint::solid(ink));

  const auto reach = st::math::IntRect{35, 35, 135, 95};
  const int delta = max_delta_in(**fresh, **warm, reach);
  st::log::out("[path-mask-reuse] 小路径区最大 Δ{}\n", delta);
  ST_CHECK(delta == 0);
}
