// 光栅化正确性回归测试：把「曾在真实界面里出问题」的几何场景全部固化为断言。
//
// 覆盖的三类真实缺陷（均已修复，此处防止回归）：
// ① **填充必须隐式闭合子路径**：字体轮廓普遍不含显式 Close，缺闭合边时环绕数不归零、
//    填充一路向右溢出（CJK 带口部首字形糊成黑块）。
// ② **覆盖率是带符号量**：非零环绕规则下孔洞处相邻轮廓符号相反、累加为 0——
//    画布侧必须取绝对值作为不透明度，否则逆时针轮廓整片丢失（圆角/阴影/字形全失效）。
// ③ **多轮廓相对方向决定孔洞**：外轮廓与内轮廓方向相反时中间必须镂空。

#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"

namespace {

/// 画布内非透明像素数。
[[nodiscard]] auto ink_count(const st::raster::Canvas& canvas) -> std::size_t {
  std::size_t count = 0;
  for (const auto pixel : canvas.pixels()) {
    if ((pixel & 0xFFU) != 0U) ++count;
  }
  return count;
}

[[nodiscard]] auto pixel_alpha(const st::raster::Canvas& canvas, int x, int y) -> unsigned {
  return canvas.pixel_at(x, y).a;
}

}  // namespace

// —— ① 未闭合子路径按填充语义隐式闭合 ——
ST_TEST(fill_closes_open_subpath_implicitly) {
  // 一个"开口"的三角形（缺最后一条边）：填充必须补上闭合边，面积等于闭合三角形
  st::raster::Path open_triangle;
  open_triangle.move_to({20.0f, 10.0f});
  open_triangle.line_to({60.0f, 10.0f});
  open_triangle.line_to({60.0f, 50.0f});
  // 故意不 close

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(80, 60, 1.0f);
  canvas.clear(st::math::Color{0, 0, 0, 0});
  canvas.fill_path(open_triangle, st::raster::Paint::solid(st::math::Color::rgb(0, 0, 0)));

  // 直角三角形：两直角边各 40px → 800px²（±抗锯齿容差）
  const std::size_t ink = ink_count(canvas);
  ST_CHECK(ink > 700);
  ST_CHECK(ink < 900);
  // 开口方向（右侧）不得溢出到 x>60 或 y>50 之外
  ST_CHECK_EQ(pixel_alpha(canvas, 70, 30), 0U);
  ST_CHECK_EQ(pixel_alpha(canvas, 40, 55), 0U);
  // 三角形内部必须被填充
  ST_CHECK(pixel_alpha(canvas, 50, 20) > 200U);
}

ST_TEST(fill_nested_contours_leave_hole) {
  // 外方框（顺时针）+ 内方框（逆时针）→ 中间镂空（非零环绕规则）
  st::raster::Path path;
  path.move_to({10.0f, 10.0f});
  path.line_to({70.0f, 10.0f});
  path.line_to({70.0f, 70.0f});
  path.line_to({10.0f, 70.0f});
  path.close();
  path.move_to({30.0f, 30.0f});
  path.line_to({30.0f, 50.0f});
  path.line_to({50.0f, 50.0f});
  path.line_to({50.0f, 30.0f});
  path.close();

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(80, 80, 1.0f);
  canvas.clear(st::math::Color{0, 0, 0, 0});
  canvas.fill_path(path, st::raster::Paint::solid(st::math::Color::rgb(0, 0, 0)));

  ST_CHECK_EQ(pixel_alpha(canvas, 40, 40), 0U);  // 孔洞
  ST_CHECK(pixel_alpha(canvas, 20, 40) > 200U);  // 边框
  ST_CHECK(pixel_alpha(canvas, 40, 20) > 200U);
}

// —— ② 带符号覆盖率不能整片丢弃 ——
ST_TEST(fill_counter_clockwise_contour_is_visible) {
  // 只画一个**逆时针**方框（覆盖率为负）：取绝对值后仍须正常填充
  st::raster::Path ccw;
  ccw.move_to({10.0f, 10.0f});
  ccw.line_to({10.0f, 50.0f});
  ccw.line_to({50.0f, 50.0f});
  ccw.line_to({50.0f, 10.0f});
  ccw.close();

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(60, 60, 1.0f);
  canvas.clear(st::math::Color{0, 0, 0, 0});
  canvas.fill_path(ccw, st::raster::Paint::solid(st::math::Color::rgb(0, 0, 0)));
  ST_CHECK(pixel_alpha(canvas, 30, 30) > 200U);
  ST_CHECK(ink_count(canvas) > 1400);  // ≈ 40×40
}

// —— ③ 真实字体：CJK 多轮廓字形（口部首）不得糊成实心块 ——
ST_TEST(font_cjk_glyph_renders_with_counters) {
  auto stack = st::text::FontStack::system_default();
  if (!stack) return;  // 无系统字体（精简环境）时跳过
  st::text::TextRenderer renderer(*stack);

  const auto render_alpha = [&renderer](std::string_view text, int width, int height) {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(width, height, 1.0f);
    canvas.clear(st::math::Color{0, 0, 0, 0});
    (void)renderer.draw(canvas, text, {8.0f, 8.0f}, 64.0f, st::math::Color::rgb(0, 0, 0));
    return canvas;
  };

  // 「口」：应当有镂空（内部像素为 0）——曾因缺闭合边渲染为实心块
  const auto mouth = render_alpha("口", 96, 96);
  const int cx = mouth.physical_width() / 2;
  const int cy = mouth.physical_height() / 2;
  ST_CHECK(pixel_alpha(mouth, cx, cy) < 40U);           // 内部镂空
  ST_CHECK(ink_count(mouth) > 100U);                    // 但确实画了笔画
  const auto solid_fraction = [&mouth]() {
    std::size_t dark = 0;
    for (const auto pixel : mouth.pixels()) {
      if ((pixel & 0xFFU) > 128U) ++dark;
    }
    return static_cast<double>(dark) / static_cast<double>(mouth.pixels().size());
  }();
  ST_CHECK(solid_fraction < 0.45);  // 不是"整块填满"

  // 「叫」（口 + 丩）：右侧部件也必须落墨——按"墨迹包围盒的右半部分"判定，
  // 这样与字号/边距无关（只要求右侧部件确实参与绘制，而不是整个字糊在左边）。
  const auto both = render_alpha("叫", 128, 96);
  int min_x = both.physical_width();
  int max_x = 0;
  for (int y = 0; y < both.physical_height(); ++y) {
    for (int x = 0; x < both.physical_width(); ++x) {
      if (pixel_alpha(both, x, y) > 40U) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
      }
    }
  }
  ST_CHECK(max_x > min_x);
  const int middle = min_x + (max_x - min_x) / 2;
  std::size_t right_ink = 0;
  for (int y = 0; y < both.physical_height(); ++y) {
    for (int x = middle; x <= max_x; ++x) {
      if (pixel_alpha(both, x, y) > 40U) ++right_ink;
    }
  }
  ST_CHECK(right_ink > 120);  // 右侧部件确实被绘制
  // 字形整体宽度应显著大于单个「口」（口约 1em 的 0.55 宽）
  ST_CHECK(static_cast<float>(max_x - min_x) > 30.0f);
}

ST_TEST(font_multi_contour_glyphs_keep_holes_across_scripts) {
  auto stack = st::text::FontStack::system_default();
  if (!stack) return;
  st::text::TextRenderer renderer(*stack);

  const auto dark_ratio = [&renderer](std::string_view text) {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(120, 120, 1.0f);
    canvas.clear(st::math::Color{0, 0, 0, 0});
    (void)renderer.draw(canvas, text, {8.0f, 8.0f}, 72.0f, st::math::Color::rgb(0, 0, 0));
    std::size_t dark = 0;
    for (const auto pixel : canvas.pixels()) {
      if ((pixel & 0xFFU) > 128U) ++dark;
    }
    return static_cast<double>(dark) / static_cast<double>(canvas.pixels().size());
  };

  // 典型"带孔"字形：Latin 的 O、B 与 CJK 的回、国——墨迹占比都不该接近满块
  for (const std::string_view text : {std::string_view{"O"}, std::string_view{"B"},
                                      std::string_view{"回"}, std::string_view{"国"}}) {
    const double ratio = dark_ratio(text);
    ST_CHECK(ratio > 0.02);   // 确实画了
    ST_CHECK(ratio < 0.45);   // 但没糊成实心
  }
}
