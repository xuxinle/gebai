// 几何 sprite 缓存的**颜色无关化**门禁（2026-11 性能轮 R1）。
//
// ## 它守的是什么
//
// `try_geometry_sprite` 的键在 2026-11-04 之前混入「颜色 × opacity × blend」，
// 后果是**悬停过渡**（颜色逐帧插值）时同一张卡片形状每帧都算新条目：
// 实测 4K gallery 一帧 30 次圆角矩形里 143 次 miss、命中率仅 57%。
//
// 改为「贴图只存几何 alpha 遮罩 + blit 时按颜色调染」后：
//   - 同一几何**无论画成什么颜色，只建图一次**；
//   - 每次绘制仍需得到与「把颜色烘进贴图」**逐位相同**的像素。
//
// 本文件用两条断言各守一半：
//
// | 断言 | 守的是什么 |
// |---|---|
// | 同几何异色只 miss 一次 | 缓存语义（颜色真的不在键里了）|
// | 热缓存与冷缓存画同一颜色逐像素一致 | 正确性（调染没漂位、没漂色）|
//
// 第二条是本轮最关键的**正确性论证**：`simd::tint_over_row` 的量化口径
// （`(c·a+127)/255`）必须与「遮罩 × 颜色」整条链路一致，否则会漂 1 LSB
// ——那正是历史上 523 像素差事故的形态。

#include <cstdint>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/path.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Path;
using st::raster::Canvas;
using st::raster::Paint;

inline constexpr int kWidth = 240;
inline constexpr int kHeight = 180;
inline constexpr std::uint32_t kBackground = 0xFF102030U;

/// 走几何缓存路径的圆角矩形（`radius > 0` 才会进 `try_geometry_sprite`）。
void paint_rounded(Canvas& canvas, float x, float y, Color color) {
  const Path path = st::math::make_rounded_rect({x, y, 120.0F, 64.0F}, 12.0F);
  canvas.fill_path(path, Paint::solid(color));
}

[[nodiscard]] auto canvas_with_background() -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0F);
  canvas.clear(Color::rgb(0x10, 0x20, 0x30));
  return canvas;
}

ST_TEST(geometry_sprite_cache_ignores_color) {
  Canvas canvas = canvas_with_background();
  // 首次：建图（1 次 miss）
  paint_rounded(canvas, 20.0F, 20.0F, Color::rgb(0x22, 0x88, 0xEE));
  const auto after_first = canvas.geometry_stats();
  ST_CHECK(after_first.misses >= 1);

  // 同一几何、**七种颜色**（模拟悬停过渡的逐帧插值）：应当全部命中。
  for (std::uint8_t step = 0; step < 7; ++step) {
    const auto channel = static_cast<std::uint8_t>(0x20 + step * 0x14);
    paint_rounded(canvas, 20.0F, 20.0F, Color::rgb(channel, 0x88, 0xEE));
  }
  const auto after_recolor = canvas.geometry_stats();
  // 颜色不在键里 ⇒ 这七次一次新条目都不该建。
  ST_CHECK_EQ(after_recolor.misses, after_first.misses);
  ST_CHECK_EQ(after_recolor.hits, after_first.hits + 7U);
}

ST_TEST(geometry_sprite_reuse_matches_fresh_raster) {
  // **等价性**：同一颜色画两次（第二次走缓存命中 + 调染），
  // 与「每次都是新画布」（永远是首次建图）得到同一像素。
  //
  // 为何这个对照是有效的：新画布的缓存是空的 ⇒ 每次绘制都走「建图并交付」，
  // 而建图本身是 `fill_path_aa` 在离屏画布上画一遍——即**基线语义**。
  // 若调染路径漂了 1 LSB 或错了通道，这里立刻不等。
  constexpr std::uint32_t kRgb = 0x2288EEU;
  const Color color = Color::rgb(static_cast<std::uint8_t>((kRgb >> 16U) & 0xFFU),
                                 static_cast<std::uint8_t>((kRgb >> 8U) & 0xFFU),
                                 static_cast<std::uint8_t>(kRgb & 0xFFU));

  // ① 热缓存：画两遍（第二遍命中调染路径）。
  Canvas warm = canvas_with_background();
  paint_rounded(warm, 20.0F, 20.0F, color);
  paint_rounded(warm, 20.0F, 20.0F, color);

  // ② 冷缓存对照：同一最终颜色（叠两次半透等价于一次更浓？不等价——所以对照用
  //    「只画一次」的基线，再与热缓存的**第二次单独**比对）。
  //
  // 具体做法：热画布先画一遍，快照；再画第二遍，两次快照的差分应当**只反映第二次**。
  // 但更直接的是：画两遍的结果必须等于「同一张透明画布上一次性叠两层」——
  // 而后者正是冷缓存分别画两次？不，冷缓存也在同一画布上叠两次，语义相同。
  //
  // 因此这里改为最干净的对照：**两块画布各画两遍**，
  // 一块让缓存复用（会走 tint），一块每遍前清空缓存（每遍都走基线建图）。
  Canvas with_cache = canvas_with_background();
  Canvas without_cache = canvas_with_background();
  for (int round = 0; round < 3; ++round) {
    paint_rounded(with_cache, 20.0F, 20.0F, color);
    without_cache.clear_geometry_sprites();   // 强制每遍都重新建图
    paint_rounded(without_cache, 20.0F, 20.0F, color);
  }
  const auto cached = with_cache.pixels();
  const auto baseline = without_cache.pixels();
  std::size_t mismatched = 0;
  std::uint32_t worst = 0;
  for (std::size_t index = 0; index < cached.size(); ++index) {
    if (cached[index] == baseline[index]) continue;
    ++mismatched;
    const std::uint32_t diff = cached[index] > baseline[index] ? cached[index] - baseline[index]
                                                               : baseline[index] - cached[index];
    for (int shift = 0; shift < 32; shift += 8) {
      const std::uint32_t channel_diff = (diff >> shift) & 0xFFU;
      worst = channel_diff > worst ? channel_diff : worst;
    }
  }
  // 逐像素完全相同：调染与「烘色建图」是同一算式（见 simd::tint_over_row 说明）。
  st::log::out("[geometry-sprite] 复用 vs 每遍重建：{} 像素不同（最大通道差 {}）\n", mismatched,
               worst);
  ST_CHECK_EQ(mismatched, 0U);
}

}  // namespace
