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

/// `draw_canvas_at` 与 `draw_canvas`+逻辑坐标必须**得到同一张图**。
///
/// ## 为什么这条必须存在（2026-11-04 实测暴露）
///
/// 用户报“相期图标启动时光滑、启动后变粗糙”，连续截屏定位到 **782ms 处图标
/// 从 (18,15) 跳到 (17,14)**——那是 `defer_gpu` 把画布从软件换成 GPU 的时刻。
/// 两条路径对同一次贴图给出了不同的落点，于是同一个图标在切换前后“跳”一下。
///
/// 本用例不依赖 GPU（CI 上很可能没有）：它钉的是**软件侧两条 API 的等价性**——
/// `draw_canvas_at(src, px, py)` 必须与 `draw_canvas(src, {px/scale, py/scale,
/// src.width(), src.height()})` 像素级相同。任何一侧改了取整规则，这里立刻红。
///
/// 取**非整数 DPI + 非整数落点**：`12 * 1.5 = 18` 是整数，恰恰盖不到这个缺陷
///（既有的 `draw_canvas_at_places_scaled_source_exactly` 就是 18，所以它一直是绿的）。
ST_TEST(draw_canvas_at_matches_draw_canvas_at_fractional_scale) {
  constexpr float kScale = 1.25f;   // 非整数 DPI（Windows 常见的一档）
  constexpr int kSourcePx = 25;     // 物理 25 → 逻辑 20

  st::raster::Canvas source{kSourcePx, kSourcePx, kScale};
  source.clear(st::math::Color{0, 0, 0, 0});
  source.fill_path(st::math::Path{}.add_rect(st::math::Rect{2.0f, 2.0f, 16.0f, 16.0f}),
                   st::raster::Paint::solid(st::math::Color{255, 0, 0, 255}));

  // 同一个物理落点，两条写法：
  //   ① draw_canvas_at(src, px, py)            —— 物理坐标直给
  //   ② draw_canvas(src, {px/scale, py/scale, src.width(), src.height()})
  //      —— 逻辑坐标（draw_canvas_at 内部就是这么转的）
  // 它们必须逐像素相同；若不同，就说明转换里有取整不一致。
  for (const int px : {13, 17, 23}) {
    st::raster::Canvas via_at = st::raster::Canvas::for_logical_size(60, 60, kScale);
    via_at.clear(st::math::Color{0, 0, 0, 0});
    via_at.draw_canvas_at(source, px, px);

    st::raster::Canvas via_canvas = st::raster::Canvas::for_logical_size(60, 60, kScale);
    via_canvas.clear(st::math::Color{0, 0, 0, 0});
    via_canvas.draw_canvas(source,
                           st::math::Rect{static_cast<float>(px) / kScale,
                                          static_cast<float>(px) / kScale,
                                          static_cast<float>(source.width()),
                                          static_cast<float>(source.height())});

    const Box a = ink_box(via_at);
    const Box b = ink_box(via_canvas);
    ST_REQUIRE(a.valid());
    ST_REQUIRE(b.valid());
    // 差异信息里带上 px，红了能直接看出是哪一档偏移
    ST_CHECK_EQ(a.left, b.left);
    ST_CHECK_EQ(a.top, b.top);
    ST_CHECK_EQ(a.right, b.right);
    ST_CHECK_EQ(a.bottom, b.bottom);
  }
}

/// 非整数 DPI 下，`draw_canvas_at` 的落点必须严格等于传入的物理坐标。
///
/// 这是上一条的“单边断言”：即使两条 API 恰好错得一样（那样上一条会绿），
/// 物理坐标就是物理坐标——`draw_canvas_at(src, 17, 17)` 的墨迹必须从 17 开始，
/// 不能被 `floor(x/scale)` 拖到 16。
ST_TEST(draw_canvas_at_keeps_exact_physical_origin) {
  constexpr float kScale = 1.5f;
  st::raster::Canvas source{30, 30, kScale};   // 物理 30 → 逻辑 20
  source.clear(st::math::Color{0, 0, 0, 0});
  source.fill_path(st::math::Path{}.add_rect(st::math::Rect{0.0f, 0.0f, 20.0f, 20.0f}),
                   st::raster::Paint::solid(st::math::Color{255, 0, 0, 255}));

  // 17 / 1.5 = 11.333…（非整数）——正是相期标题栏图标在 DPI 1.5 下的实际落点。
  for (const int px : {17, 18, 19}) {
    st::raster::Canvas target = st::raster::Canvas::for_logical_size(80, 80, kScale);
    target.clear(st::math::Color{0, 0, 0, 0});
    target.draw_canvas_at(source, px, px);
    const Box box = ink_box(target);
    ST_REQUIRE(box.valid());
    ST_CHECK_EQ(box.left, px);
    ST_CHECK_EQ(box.top, px);
    ST_CHECK_EQ(box.right, px + 29);   // 铺满整张源（30 物理）
    ST_CHECK_EQ(box.bottom, px + 29);
  }
}
