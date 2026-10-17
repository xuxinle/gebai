// `Surface::pixel_view()`（`std::mdspan` 二维像素视图）的回归测试。
//
// 这一层守的是三件事，每一件都对应一种「换实现会静默错」的形态：
// ① **同一块内存**：`pixel_view()` 不得拷贝。若它返回副本，写入视图就会丢——
//    而读路径看上去完全正常（最坏的一类缺陷）。
// ② **行列是（y, x）**：`view[y, x]` 与 `pixel_at(x, y)` 必须逐格对得上。
//    写反了不报错，只是读到邻行——正是换成 `mdspan` 要消灭的那类错。
// ③ **尺寸不一致时给空视图**：`mdspan` 对越界尺寸不做验证，传错即踩内存。
//    未初始化/尺寸与像素跨度不匹配的后端必须落到空视图，而不是 UB。
//
// ⚠ 两个容易把判据写错的地方（都实测踩过）：
// - `pixels()`/`pixel_view()` 里存的是**预乘打包像素**（`math::premultiply` 的产物），
//   不是某个颜色通道。要与 `pixel_at()` 对拍就得经 `unpremultiply` 走一圈；
// - 打包布局里 **alpha 在最低字节**（`premultiply` 读 `pixel & 0xFF` 即 alpha），
//   所以"不透明像素"的整数值形如 `0x??BBGGRR...` 而非 `0xRRGGBBAA`。
//
// ⚠ 写法注意：`view[y, x]` 里的逗号会被**函数式宏**当成参数分隔符（`ST_CHECK` 只收一个参数）。
// 把多维下标放进断言宏的地方要**整体加一层括号**：`ST_CHECK((view[y, x] == v))`。

#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"

namespace {

constexpr int kPhysicalWidth = 7;
constexpr int kPhysicalHeight = 5;

/// 红色通道存 `y * width + x`（0..34）：每个格子的**颜色都不相同**，
/// 于是"行列写反"必然读到不同的值，对拍才有分辨力。
[[nodiscard]] auto make_pattern_canvas() -> st::raster::Canvas {
  st::raster::Canvas canvas(kPhysicalWidth, kPhysicalHeight);
  for (int y = 0; y < kPhysicalHeight; ++y) {
    for (int x = 0; x < kPhysicalWidth; ++x) {
      const auto seed = static_cast<std::uint8_t>(y * kPhysicalWidth + x);
      canvas.set_pixel(x, y, st::math::Color{seed, 0, 0, 255});
    }
  }
  return canvas;
}

/// 该格在 `pixels()` 里的**打包**值（行主序下标）。
[[nodiscard]] auto packed_of(const st::raster::Canvas& canvas, int x, int y) -> std::uint32_t {
  return canvas.pixels()[static_cast<std::size_t>(y) * kPhysicalWidth +
                         static_cast<std::size_t>(x)];
}

/// 该格的红色通道（经 `pixel_at` 的非预乘值）。
[[nodiscard]] auto red_of(const st::raster::Canvas& canvas, int x, int y) -> std::uint32_t {
  return canvas.pixel_at(x, y).r;
}

}  // namespace

/// ① 二维视图与一维像素跨度**指向同一块内存**（写入视图必须反映到画布上）。
ST_TEST(raster_pixel_view_shares_memory_with_pixels) {
  auto canvas = make_pattern_canvas();
  // 挑一个**不会被下面写入**的格子，用于验证"未动的格保持原值"。
  const auto untouched = packed_of(canvas, 1, 4);

  auto view = canvas.pixel_view();
  ST_REQUIRE(view.extent(0) == static_cast<std::size_t>(kPhysicalHeight));
  ST_REQUIRE(view.extent(1) == static_cast<std::size_t>(kPhysicalWidth));
  // 地址必须一致：不一致就是拷贝（写入会丢）。
  ST_CHECK(view.data_handle() == canvas.pixels().data());

  // 写入视图的一个格（不透明纯红），应当立刻能从画布的打包像素里读到。
  const auto opaque_red = st::math::premultiply(st::math::Color{255, 0, 0, 255});
  view[2, 3] = opaque_red;
  ST_CHECK((view[2, 3] == opaque_red));
  ST_CHECK_EQ(packed_of(canvas, 3, 2), opaque_red);
  ST_CHECK_EQ(red_of(canvas, 3, 2), 255U);

  // 未动的格保持原值（不是整块被重建）。
  ST_CHECK_EQ(packed_of(canvas, 1, 4), untouched);
}

/// ② 索引顺序是 `view[y, x]`：逐格与打包像素对拍（行列写反必红）。
ST_TEST(raster_pixel_view_indexes_row_major_y_then_x) {
  auto canvas = make_pattern_canvas();
  const auto view = canvas.pixel_view();
  ST_REQUIRE(view.extent(0) == static_cast<std::size_t>(kPhysicalHeight));

  std::size_t mismatches = 0;
  for (int y = 0; y < kPhysicalHeight; ++y) {
    for (int x = 0; x < kPhysicalWidth; ++x) {
      const auto via_view = view[static_cast<std::size_t>(y), static_cast<std::size_t>(x)];
      if (via_view != packed_of(canvas, x, y)) ++mismatches;
    }
  }
  ST_CHECK_EQ(mismatches, 0U);

  // 抽一格做**非对称**核对：若实现按 x 为首维（写反），这一格会读到 (3, 2) 的值而非 (2, 3)。
  ST_CHECK_EQ(red_of(canvas, 3, 2), static_cast<std::uint32_t>(2 * kPhysicalWidth + 3));
  ST_CHECK_EQ(red_of(canvas, 2, 3), static_cast<std::uint32_t>(3 * kPhysicalWidth + 2));
  ST_CHECK((view[2, 3] == packed_of(canvas, 3, 2)));
  ST_CHECK((view[2, 3] != packed_of(canvas, 2, 3)));
}

/// ③ 只读视图（`const` 重载）内容与可写视图一致；且 const 画布也能取视图。
ST_TEST(raster_pixel_view_const_overload_matches) {
  const auto canvas = make_pattern_canvas();
  const auto view = canvas.pixel_view();
  ST_REQUIRE(view.extent(0) == static_cast<std::size_t>(kPhysicalHeight));
  ST_CHECK((view[4, 6] == packed_of(canvas, 6, 4)));
  ST_CHECK((view[0, 0] == packed_of(canvas, 0, 0)));
  // 角格确实是"不透明黑"：alpha 在最低字节，故最低字节为 255。
  ST_CHECK_EQ((view[0, 0] & 0xFFU), 255U);
}

/// ④ 退化尺寸（零宽/零高）给**空视图**而不是 UB。
ST_TEST(raster_pixel_view_empty_canvas_is_empty_view) {
  st::raster::Canvas canvas(0, 0);
  const auto view = canvas.pixel_view();
  ST_CHECK_EQ(view.size(), 0U);
  ST_CHECK_EQ(view.extent(0), 0U);
  ST_CHECK_EQ(view.extent(1), 0U);
}

/// ⑤ resize 之后视图必须跟着新尺寸（不是缓存的旧 mdspan）。
/// 这条守的是"视图是**派生**的，不是存下来的"——存下来就会在 resize 后指向已释放内存。
ST_TEST(raster_pixel_view_follows_resize) {
  auto canvas = make_pattern_canvas();
  const auto wide = canvas.pixel_view();
  ST_REQUIRE(wide.extent(1) == static_cast<std::size_t>(kPhysicalWidth));

  const auto resized = canvas.resize(11, 3);
  ST_REQUIRE(resized.has_value());
  const auto narrow = canvas.pixel_view();
  ST_CHECK_EQ(narrow.extent(0), 3U);
  ST_CHECK_EQ(narrow.extent(1), 11U);
  ST_CHECK_EQ(narrow.size(), 33U);
  ST_CHECK(narrow.data_handle() == canvas.pixels().data());
}
