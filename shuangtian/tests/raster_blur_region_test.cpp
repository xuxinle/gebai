/// 区域毛玻璃（`Surface::blur_region`）的契约测试。
///
/// 这类"效果型"原语最容易写出**恒绿的假测试**：断言只检查"函数返回 true"，
/// 而内部没真模糊也照样通过。这里全部按**像素事实**断言：
///   ① 区域内的高频条纹必须被抹平（对比度下降）；
///   ② 区域外的像素必须**逐位不变**（模糊不能漏到外面）；
///   ③ 无变化的纯色区域模糊前后逐位相同（不能引入偏移/噪声）；
///   ④ 圆角不重要，但**裁剪域**必须生效（裁剪外的部分不模糊）。
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;

/// 画一张黑白相间的竖条纹（高频细节，模糊后对比度应当明显下降）。
void paint_stripes(Canvas& canvas, Rect area) {
  for (int x = 0; x < static_cast<int>(area.width); ++x) {
    const bool black = (x / 2) % 2 == 0;
    const Color color = black ? Color{0, 0, 0, 255} : Color{255, 255, 255, 255};
    canvas.fill_rect(Rect{area.x + static_cast<float>(x), area.y, 1.0f, area.height},
                     st::raster::Paint::solid(color));
  }
}

/// 区域内的水平亮度极差（max - min），用来衡量"还有多少对比度"。
auto luma_range(const Canvas& canvas, st::math::IntRect area) -> int {
  int lo = 255;
  int hi = 0;
  for (int y = area.y; y < area.y + area.height; ++y) {
    for (int x = area.x; x < area.x + area.width; ++x) {
      const Color c = canvas.pixel_at(x, y);
      const int luma = (c.r * 299 + c.g * 587 + c.b * 114) / 1000;
      lo = std::min(lo, luma);
      hi = std::max(hi, luma);
    }
  }
  return hi - lo;
}

}  // namespace

ST_TEST(blur_region_flattens_high_frequency_detail) {
  Canvas canvas(120, 80, 1.0f);
  canvas.clear(Color{128, 128, 128, 255});
  paint_stripes(canvas, Rect{0.0f, 0.0f, 120.0f, 80.0f});

  const st::math::IntRect inner{30, 20, 60, 40};
  const int before = luma_range(canvas, inner);
  ST_REQUIRE(before > 200);   // 条纹本身是纯黑白，极差应当接近 255

  const bool blurred = canvas.blur_region(Rect{0.0f, 0.0f, 120.0f, 80.0f}, 6.0f);
  ST_REQUIRE(blurred);

  const int after = luma_range(canvas, inner);
  // 判据：模糊后必须有**显著**下降（不是"降了一点"）。
  // 取 1/3 作为阈值：条纹周期 4px、半径 6px 时三遍盒式模糊会把它们抹成接近匀色。
  ST_CHECK(after < before / 3);
}

ST_TEST(blur_region_leaves_outside_pixels_untouched) {
  Canvas canvas(120, 80, 1.0f);
  canvas.clear(Color{128, 128, 128, 255});
  paint_stripes(canvas, Rect{0.0f, 0.0f, 120.0f, 80.0f});

  // 记录四条边外的一圈像素
  std::vector<Color> left;
  for (int y = 0; y < 80; ++y) left.push_back(canvas.pixel_at(2, y));

  ST_REQUIRE(canvas.blur_region(Rect{40.0f, 20.0f, 40.0f, 40.0f}, 5.0f));

  // 区域外逐位不变——这是"模糊不漏出去"的硬保证。
  // 曾经手写版本把整行当滑动窗口，结果区域外的行也被覆写。
  int changed = 0;
  for (int y = 0; y < 80; ++y) {
    const Color now = canvas.pixel_at(2, y);
    if (!(now == left[static_cast<std::size_t>(y)])) ++changed;
  }
  ST_CHECK(changed == 0);
}

ST_TEST(blur_region_is_identity_on_uniform_area) {
  Canvas canvas(60, 60, 1.0f);
  const Color flat{90, 120, 150, 255};
  canvas.clear(flat);

  ST_REQUIRE(canvas.blur_region(Rect{10.0f, 10.0f, 40.0f, 40.0f}, 4.0f));

  // 匀色区域模糊后必须**逐位不变**：任何偏移/噪声都会在这里现形
  // （滑动窗口的经典错误是"少加一个元素"导致整体偏暗）。
  int off = 0;
  for (int y = 12; y < 48; ++y) {
    for (int x = 12; x < 48; ++x) {
      const Color c = canvas.pixel_at(x, y);
      if (!(c == flat)) ++off;
    }
  }
  ST_CHECK(off == 0);
}

ST_TEST(blur_region_respects_clip) {
  Canvas canvas(120, 80, 1.0f);
  canvas.clear(Color{128, 128, 128, 255});
  paint_stripes(canvas, Rect{0.0f, 0.0f, 120.0f, 80.0f});

  // 只裁到右半边：左半边的条纹必须**保持原样**（模糊不该越过裁剪域）。
  canvas.push_clip_rect(Rect{60.0f, 0.0f, 60.0f, 80.0f});
  ST_REQUIRE(canvas.blur_region(Rect{0.0f, 0.0f, 120.0f, 80.0f}, 6.0f));
  canvas.pop_clip();

  const int left_range = luma_range(canvas, st::math::IntRect{10, 20, 40, 40});
  const int right_range = luma_range(canvas, st::math::IntRect{70, 20, 40, 40});
  ST_CHECK(left_range > 200);    // 裁剪外：条纹完好
  ST_CHECK(right_range < left_range / 3);   // 裁剪内：被抹平
}

ST_TEST(blur_region_respects_rounded_clip) {
  // 圆角裁剪是**遮罩**而不是矩形（`clip_rect()` 只给矩形）。
  // 如果写回时只看矩形，模糊会溢出到圆角**之外**，弹层四角会多出一圈脏边。
  // 实测（对话框左上圆角 10 倍放大）：圆角外多出一块矩形灰块。
  //
  // 两张图同构：`reference` 不模糊（量"模糊前"），`canvas` 模糊（量"模糊后"）。
  // 不能用单张图里两个点对比——那量的是"位置差"而不是"模糊前后差"。
  const Rect card{10.0f, 10.0f, 80.0f, 80.0f};
  const auto paint_scene = [&](Canvas& target) {
    target.clear(Color{128, 128, 128, 255});
    // 只在圆角**角落**放洋红（卡片矩形内、圆角外）。
    // 中心 (26,26)、半径 16：(11,11) 到圆心 sqrt(15²+15²)≈21 > 16 ⇒ 在圆角外。
    // 裸 memcpy 写回会把这里盖成模糊后的值；正确实现必须保留原样。
    target.fill_rect(Rect{10.0f, 10.0f, 4.0f, 4.0f},
                     st::raster::Paint::solid(Color{255, 0, 255, 255}));
  };

  Canvas reference(100, 100, 1.0f);
  paint_scene(reference);
  Canvas canvas(100, 100, 1.0f);
  paint_scene(canvas);

  // ① 圆角**角落**（在卡片矩形内、圆角外）的像素必须原样保留。
  // 这一条就是逆向验证能抓住的：把写回改回裸 memcpy 时它会红。
  // （11,11) 在圆角 16 的外侧：与圆心 (26,26) 的距离 sqrt(15²+15²)≈21 > 16。
  const Color corner_before = canvas.pixel_at(11, 11);
  ST_CHECK(corner_before.r > 200 && corner_before.g < 60);   // 确认探针真落在洋红上

  canvas.push_clip_rounded_rect(card, 16.0f);
  ST_REQUIRE(canvas.blur_region(card, 8.0f));
  canvas.pop_clip();

  const Color corner_after = canvas.pixel_at(11, 11);
  ST_CHECK(corner_after == corner_before);

  // ② 反向侧：模糊**真的发生了**。因为场景里只有洋红块，就量"洋红向卡片内扩散"：
  // 紧邻洋红的内部一点（15,15，在圆角内）原本纯灰，模糊后应当染上洋红（r、b 高于 g）。
  // 这样两个断言针对**同一个色源**，不需要引入第二个颜色去担心互相污染。
  const Color inner = canvas.pixel_at(15, 15);
  const Color inner_ref = reference.pixel_at(15, 15);
  ST_CHECK_EQ(static_cast<int>(inner_ref.r), 128);   // 基准确实是纯灰
  ST_CHECK(static_cast<int>(inner.r) - static_cast<int>(inner.g) > 8);
}

ST_TEST(blur_region_preserves_color_channels) {
  // **彩色模糊必须四个通道都模糊**。
  //
  // ⚠ **局限：这条只能抓住软件端的同类缺陷**。用户实际碰到的是 GPU 端的：
  // `BoxBlur` 着色器原本为阴影遮罩而写，只采样 `.r` 并输出 `(r, 0, 0, 1)`；
  // 直接拿它模糊彩色图像会丢掉 G/B 并把 alpha 写成 1——贴回后只剩红色分量。
  // 实测（暗色弹窗）：右侧一条“红绿色块”竖带，R 在 44~62 波动而
  // **G 恒为 30、B 恒为 38**。修法：`g_mode.w = 1` 时走全色分支。
  // 软件端的对应实现（`box_blur_rgba_pass`）本来就地模糊 RGBA，
  // 这条用例保证它将来不会退化成单通道——GPU 端的回归靠实机截图 + 彩点扫描。
  Canvas canvas(80, 60, 1.0f);
  // 一块**纯绿**区域：如果 G 通道被丢弃，模糊后它就会变黑/发红。
  canvas.clear(Color{20, 200, 60, 255});

  ST_REQUIRE(canvas.blur_region(Rect{10.0f, 10.0f, 50.0f, 40.0f}, 4.0f));

  // 匀色区域模糊后必须仍是同一个色（模糊不改变匀色块）。
  const Color center = canvas.pixel_at(35, 30);
  ST_CHECK(center.g > 180);   // 绿色通道保住
  ST_CHECK(center.b > 40);    // 蓝色通道保住
  ST_CHECK(center.r < 60);    // 红色没有被抬起来
}

ST_TEST(blur_region_reports_unsupported_size_gracefully) {
  Canvas canvas(20, 20, 1.0f);
  canvas.clear(Color{0, 0, 0, 255});
  // 退化输入（空矩形 / 半径 0 / 超出画布）必须返回 false 而不是崩溃。
  ST_CHECK(!canvas.blur_region(Rect{0.0f, 0.0f, 0.0f, 0.0f}, 4.0f));
  ST_CHECK(!canvas.blur_region(Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.0f));
  ST_CHECK(!canvas.blur_region(Rect{500.0f, 500.0f, 10.0f, 10.0f}, 4.0f));
}
