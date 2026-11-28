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

// ————————————————————————————————————————————————————————————————
// 半透明圆角填充：**opacity 只能作用一次**
//
// 为何要单独一条：上面两条对照全走默认 `opacity = 1`，
// 而缺陷恰好只在 `opacity < 1` 时出现——它把 opacity 同时乘进了颜色和 alpha，
// 而后续 `premultiply()` 会再乘一次 alpha，于是透明度被**平方**。
//
// 实测（白 0.28 叠在 (229,234,248) 上）：期望 236，贴图缓存路径给出 **185**，
// 暗 51 级。界面上的表现是“圆角半透明气泡是一片灰块”，而**直角**不触发
// （直角走 `blend_span` 快路径，压根不进贴图缓存）。
//
// 同样关键的一点：**GPU 腿与软件腿必须一致**。同一个 `Style` 在小白的用户气泡上
// 曾出现软件腿 (182,187,200) vs GPU 腿 (233,239,251) 相差 51 级——本用例不从
// 像素上判 GPU（本文件只跑软件画布），而是钉死“缓存路径 == 不走缓存路径 == 线性期望”，
// 三者相等后两条腿就不再可能因这条而分岔。
ST_TEST(geometry_sprite_semi_transparent_opacity_applied_once) {
  struct Case {
    std::uint8_t base;
    float opacity;
    float radius;
  };
  const Case cases[] = {
      {229, 0.28F, 14.0F},   // 实测触发值（小白气泡）
      {229, 0.28F, 1.0F},    // 极小圆角也必须走同一支
      {229, 0.50F, 14.0F},
      {229, 0.10F, 14.0F},
      {64, 0.28F, 14.0F},    // 暗底（验证不是亮度偏置）
      {229, 1.00F, 14.0F},   // 不透明对照（不应受影响）
  };
  for (const auto& item : cases) {
    const auto render = [&item](bool cache_enabled) -> std::uint32_t {
      Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0F);
      canvas.set_geometry_cache_enabled(cache_enabled);
      canvas.clear(Color::rgb(item.base, 234, 248));
      canvas.fill_rect({20.0F, 20.0F, 120.0F, 70.0F},
                       Paint::solid(Color::rgb(255, 255, 255)), item.radius,
                       {.opacity = item.opacity});
      // 取矩形内部一个远离圆角弧的点：只测“半透明叠层”本身，不混 AA 讨论。
      return canvas.pixel_at_point({80.0F, 55.0F}).r;
    };
    const std::uint32_t cached = render(true);
    const std::uint32_t direct = render(false);
    const auto expect = static_cast<std::uint32_t>(
        item.opacity * 255.0F + (1.0F - item.opacity) * static_cast<float>(item.base) + 0.5F);
    st::log::out("[opacity-once] 底{} op{} r{}：缓存={} 不走缓存={} 期望={}\n", item.base,
                 item.opacity, item.radius, cached, direct, expect);
    // 容差 1 LSB（8 位量化），三路必须对齐。
    ST_CHECK(cached + 1U >= expect && cached <= expect + 1U);
    ST_CHECK(cached + 1U >= direct && cached <= direct + 1U);
  }
}

}  // namespace
