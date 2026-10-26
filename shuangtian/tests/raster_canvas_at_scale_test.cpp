/// `draw_canvas_at` 在**源位图与目标画布缩放比相同且 > 1** 时，贴图位置必须精确。
///
/// 这条守的是一个真实缺陷（用户报「标题栏 logo 位置偏下」的根因）：
/// `IconSetPainter` 开一张 `size_px × size_px` 的**物理**位图（24×24 @ scale 1.5，
/// 即 16 逻辑）画图标，再 `draw_canvas_at(bitmap, box.x*scale, box.y*scale)` 贴回。
/// 若贴回时把该位图当成**物理**尺寸去铺，内容会整体偏右偏下（实测 8 物理像素）。
///
/// 判据取**墨迹包围盒**而不是单个像素：单点断言会被抗锯齿边缘晃过去。

#include "st/test/test.hpp"

#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

/// 在画布上找一个实心方块，返回它的物理像素包围盒。
struct Box {
  int left{-1};
  int top{-1};
  int right{-1};
  int bottom{-1};
  [[nodiscard]] auto valid() const -> bool { return left >= 0; }
};

[[nodiscard]] auto ink_box(const st::raster::Surface& surface) -> Box {
  Box box{};
  for (int y = 0; y < surface.physical_height(); ++y) {
    for (int x = 0; x < surface.physical_width(); ++x) {
      if (surface.pixel_at(x, y).a <= 40U) continue;
      if (box.left < 0 || x < box.left) box.left = x;
      if (box.top < 0 || y < box.top) box.top = y;
      if (x > box.right) box.right = x;
      if (y > box.bottom) box.bottom = y;
    }
  }
  return box;
}

}  // namespace

ST_TEST(draw_canvas_at_places_scaled_source_exactly) {
  constexpr float kScale = 1.5f;
  // 目标：逻辑 40×40 的画布（物理 60×60）。
  st::raster::Canvas target = st::raster::Canvas::for_logical_size(40, 40, kScale);
  target.clear(st::math::Color{0, 0, 0, 0});

  // 源：**物理 24×24**、scale 1.5 → 逻辑 16×16（与 IconSetPainter 的建法一致）。
  st::raster::Canvas source{24, 24, kScale};
  source.clear(st::math::Color{0, 0, 0, 0});
  // 在源里填满整张（不留边），贴回后墨迹应正好覆盖目标框。
  source.fill_path(st::math::Path{}.add_rect(st::math::Rect{0.0f, 0.0f, 16.0f, 16.0f}),
                   st::raster::Paint::solid(st::math::Color{255, 0, 0, 255}));

  // 贴到逻辑 (12,12)：物理落点 = round(12*1.5) = 18，覆盖物理 18..41（24 物理）。
  target.draw_canvas_at(source, 18, 18);

  const Box box = ink_box(target);
  ST_REQUIRE(box.valid());
  ST_CHECK_EQ(box.left, 18);
  ST_CHECK_EQ(box.top, 18);
  ST_CHECK_EQ(box.right, 41);    // 18 + 24 - 1
  ST_CHECK_EQ(box.bottom, 41);
}
