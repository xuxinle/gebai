/// 二维路径填充/描边的**语义**：非零环绕、自交、带洞、描边只画轮廓。
///
/// 为什么单独一个文件：这些语义原先只在已删除的 GL 测试里被测（测的是模板缓冲实现）。
/// 而真正需要被钉住的是**软件光栅器**——它是"跨平台可用"
/// 的那条腿，也是 GPU 路径的**等价性基准**。基准自己没有测试，等于没有基准。
///
/// 三条判据都是"能手算、能看图"的：
/// ① 反向环绕的内框必须是**空洞**（非零环绕的典型用法）；
/// ② 自交五角星的中心必须是**实心**（这一条能区分非零环绕与奇偶规则——奇偶规则下中心是空的）；
/// ③ 描边只该画轮廓，内部必须是**空的**。

#include "st/test/test.hpp"

#include <cmath>

#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::raster::Paint;
using st::raster::Path;

inline constexpr int kSize = 200;

/// 像素是否被画过（与背景不同）。
[[nodiscard]] auto is_inked(const Canvas& canvas, int x, int y, Color background) -> bool {
  return canvas.pixel_at(x, y) != background;
}

[[nodiscard]] auto count_inked(const Canvas& canvas, Color background) -> int {
  const std::uint32_t bg = st::math::premultiply(background);
  int count = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    if (pixel != bg) ++count;
  }
  return count;
}

}  // namespace

ST_TEST(path_fill_nonzero_winding_leaves_a_hole) {
  // 外框顺时针 + 内框逆时针 → 非零环绕下环绕数在孔洞处为 0 → 中间**必须是空的**。
  // 这是 `fill_path` 最常见的真实用法（环形、带孔图标、字母 O 这类形状）。
  Canvas canvas{kSize, kSize};
  const Color background{0x10, 0x14, 0x1E, 0xFF};
  canvas.clear(background);

  Path path;
  path.move_to(Point{20.0f, 20.0f});     // 外框：顺时针
  path.line_to(Point{180.0f, 20.0f});
  path.line_to(Point{180.0f, 180.0f});
  path.line_to(Point{20.0f, 180.0f});
  path.close();
  path.move_to(Point{60.0f, 60.0f});     // 内框：逆时针（反向 → 抵消）
  path.line_to(Point{60.0f, 140.0f});
  path.line_to(Point{140.0f, 140.0f});
  path.line_to(Point{140.0f, 60.0f});
  path.close();

  canvas.fill_path(path, Paint::solid(Color{0xFF, 0xFF, 0xFF, 0xFF}));

  ST_CHECK(is_inked(canvas, 100, 100, background) == false);   // 孔洞：空
  ST_CHECK(is_inked(canvas, 100, 40, background));             // 环带：实
  ST_CHECK(is_inked(canvas, 40, 100, background));             // 环带：实
  ST_CHECK(is_inked(canvas, 100, 190, background) == false);   // 框外：空
  // 孔洞面积应明显小于环带面积（防止"全都填了"也能通过前两条断言）
  const int inked = count_inked(canvas, background);
  const int outer_area = 160 * 160;
  const int hole_area = 80 * 80;
  ST_CHECK(inked < outer_area);
  ST_CHECK(inked > outer_area - hole_area - 200);
}

ST_TEST(path_fill_nonzero_winding_fills_self_intersection) {
  // 五角星（5 条交叉线一笔画）：**非零环绕的中心是实心的**，奇偶规则下才是空的。
  // 这条是"实现用的是哪一种环绕规则"的判别性证据——只测矩形是看不出来的。
  Canvas canvas{kSize, kSize};
  const Color background{0x10, 0x14, 0x1E, 0xFF};
  canvas.clear(background);

  Path star;
  constexpr float kPi = 3.14159265358979f;
  for (int i = 0; i <= 5; ++i) {
    // 每次跨 2 个顶点 → 形成自交的五角星
    const float angle = -kPi / 2.0f + static_cast<float>(i % 5) * (4.0f * kPi / 5.0f);
    const Point p{100.0f + 80.0f * std::cos(angle), 100.0f + 80.0f * std::sin(angle)};
    if (i == 0) {
      star.move_to(p);
    } else {
      star.line_to(p);
    }
  }
  star.close();
  canvas.fill_path(star, Paint::solid(Color{0xFF, 0xFF, 0xFF, 0xFF}));

  ST_CHECK(is_inked(canvas, 100, 100, background));   // 中心：非零环绕 → 实心
  ST_CHECK(is_inked(canvas, 100, 40, background));    // 上尖角：实心
  ST_CHECK(is_inked(canvas, 14, 14, background) == false);   // 角落：空
}

ST_TEST(path_stroke_draws_outline_only) {
  // 描边必须**只画轮廓**：内部留空。常见的实现错误是把描边也当填充（整块实心）。
  Canvas canvas{kSize, kSize};
  const Color background{0x10, 0x14, 0x1E, 0xFF};
  canvas.clear(background);

  Path rect;
  rect.move_to(Point{40.0f, 40.0f});
  rect.line_to(Point{160.0f, 40.0f});
  rect.line_to(Point{160.0f, 160.0f});
  rect.line_to(Point{40.0f, 160.0f});
  rect.close();
  canvas.stroke_path(rect, Paint::solid(Color{0xFF, 0xFF, 0xFF, 0xFF}), 6.0f);

  ST_CHECK(is_inked(canvas, 100, 40, background));            // 上边：有
  ST_CHECK(is_inked(canvas, 40, 100, background));            // 左边：有
  ST_CHECK(is_inked(canvas, 100, 100, background) == false);  // 内部：空
  ST_CHECK(is_inked(canvas, 8, 8, background) == false);      // 外部：空
  // 描边面积远小于填充面积（防止"其实填了整块"也通过上面两条）
  const int inked = count_inked(canvas, background);
  ST_CHECK(inked < 120 * 120 / 2);
}

ST_TEST(path_fill_zero_width_path_draws_nothing) {
  // 退化输入不该画东西，也不该崩（含空路径、单点、共线三点）。
  Canvas canvas{kSize, kSize};
  const Color background{0x10, 0x14, 0x1E, 0xFF};
  canvas.clear(background);
  const Paint paint = Paint::solid(Color{0xFF, 0xFF, 0xFF, 0xFF});

  canvas.fill_path(Path{}, paint);
  ST_CHECK_EQ(count_inked(canvas, background), 0);

  Path line;
  line.move_to(Point{10.0f, 10.0f});
  line.line_to(Point{120.0f, 120.0f});
  canvas.fill_path(line, paint);
  ST_CHECK_EQ(count_inked(canvas, background), 0);

  Path degenerate;
  degenerate.move_to(Point{10.0f, 10.0f});
  degenerate.line_to(Point{60.0f, 10.0f});
  degenerate.line_to(Point{110.0f, 10.0f});
  degenerate.close();
  canvas.fill_path(degenerate, paint);
  ST_CHECK_EQ(count_inked(canvas, background), 0);
}
