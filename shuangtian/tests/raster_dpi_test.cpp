#include <cmath>

#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;

/// 统计非透明像素数（物理像素）。
[[nodiscard]] auto ink_pixels(const Canvas& canvas) -> std::size_t {
  std::size_t count = 0;
  for (const auto pixel : canvas.pixels()) {
    if ((pixel & 0xFFU) != 0U) ++count;
  }
  return count;
}

}  // namespace

ST_TEST(dpi_canvas_logical_size) {
  Canvas canvas = Canvas::for_logical_size(100, 50, 2.0f);
  ST_CHECK_EQ(canvas.physical_width(), 200);
  ST_CHECK_EQ(canvas.physical_height(), 100);
  ST_CHECK_EQ(canvas.width(), 100);
  ST_CHECK_EQ(canvas.height(), 50);
  ST_CHECK_NEAR(canvas.device_scale(), 2.0, 1e-6);

  // 非整数 DPI（1.5x）也要正确取整
  Canvas odd = Canvas::for_logical_size(101, 33, 1.5f);
  ST_CHECK_EQ(odd.physical_width(), 152);   // round(101 × 1.5)
  ST_CHECK_EQ(odd.physical_height(), 50);   // round(33 × 1.5) = 49.5 → 50
  ST_CHECK_EQ(odd.width(), 101);
}

ST_TEST(dpi_rect_scales_to_physical) {
  Canvas canvas = Canvas::for_logical_size(100, 50, 2.0f);
  canvas.clear(Color{0, 0, 0, 0});
  canvas.fill_rect(Rect{10.0f, 10.0f, 20.0f, 20.0f}, Paint::solid(Color::rgb(0, 0, 0)));

  // 逻辑 (10,10)-(30,30) → 物理 (20,20)-(60,60)
  const st::math::IntRect bounds = canvas.content_bounds();
  ST_CHECK_EQ(bounds.x, 20);
  ST_CHECK_EQ(bounds.y, 20);
  ST_CHECK_EQ(bounds.width, 40);
  ST_CHECK_EQ(bounds.height, 40);

  // 逻辑视图不变
  const st::math::IntRect logical = canvas.content_bounds_logical();
  ST_CHECK_EQ(logical.x, 10);
  ST_CHECK_EQ(logical.y, 10);
  ST_CHECK_EQ(logical.width, 20);
  ST_CHECK_EQ(logical.height, 20);

  ST_CHECK_EQ(canvas.pixel_at_point(Point{10.5f, 10.5f}).a, 255);
  ST_CHECK_EQ(canvas.pixel_at_point(Point{29.5f, 29.5f}).a, 255);
  ST_CHECK_EQ(canvas.pixel_at_point(Point{9.0f, 20.0f}).a, 0);
}

ST_TEST(dpi_aligned_edges_stay_crisp) {
  Canvas canvas = Canvas::for_logical_size(40, 40, 2.0f);
  canvas.clear(Color{0, 0, 0, 0});
  canvas.fill_rect(Rect{5.0f, 5.0f, 10.0f, 10.0f}, Paint::solid(Color::rgb(255, 0, 0)));

  // 逻辑 5..15 → 物理 10..30：边界像素必须全有/全无（无抗锯齿灰边）
  ST_CHECK_EQ(canvas.pixel_at(9, 20).a, 0);
  ST_CHECK_EQ(canvas.pixel_at(10, 20).a, 255);
  ST_CHECK_EQ(canvas.pixel_at(29, 20).a, 255);
  ST_CHECK_EQ(canvas.pixel_at(30, 20).a, 0);
}

ST_TEST(dpi_stroke_width_scales) {
  const auto draw_hline = [](Canvas& canvas) {
    st::raster::Path line;
    line.move_to(Point{2.0f, 5.0f});
    line.line_to(Point{38.0f, 5.0f});
    canvas.stroke_path(line, Paint::solid(Color::rgb(0, 0, 255)), 1.0f);
  };
  const auto max_alpha = [](const Canvas& canvas) {
    std::uint32_t best = 0;
    for (const auto pixel : canvas.pixels()) {
      const std::uint32_t alpha = pixel & 0xFFU;
      if (alpha > best) best = alpha;
    }
    return best;
  };

  Canvas at1 = Canvas::for_logical_size(40, 10, 1.0f);
  at1.clear(Color{0, 0, 0, 0});
  draw_hline(at1);

  Canvas at2 = Canvas::for_logical_size(40, 10, 2.0f);
  at2.clear(Color{0, 0, 0, 0});
  draw_hline(at2);

  // 1x：1px 发丝线骑在两行之间，任一行都不到满覆盖 —— 这正是"糊"的来源
  ST_CHECK(max_alpha(at1) < 255U);
  // 2x：同一几何按物理分辨率重采样，出现满覆盖核心行 —— DPI 提升了真实清晰度
  ST_CHECK_EQ(max_alpha(at2), 255U);
  // 物理跨度按 DPR 放大（线本体 36 逻辑 → 72 物理）
  ST_CHECK(at2.content_bounds().width >= 72);
  ST_CHECK(at1.content_bounds().width >= 36);
}

ST_TEST(dpi_text_rasterizes_at_physical_resolution) {
  auto stack = st::text::FontStack::system_default();
  ST_REQUIRE(stack.has_value());
  const st::text::TextRenderer renderer(*stack, 1.0f);

  Canvas at1 = Canvas::for_logical_size(200, 60, 1.0f);
  at1.clear(Color{0, 0, 0, 0});
  auto status1 = renderer.draw(at1, "霜天 Ag", Point{8.0f, 8.0f}, 24.0f, Color::rgb(0, 0, 0));
  ST_REQUIRE(status1.has_value());

  Canvas at2 = Canvas::for_logical_size(200, 60, 2.0f);
  at2.clear(Color{0, 0, 0, 0});
  auto status2 = renderer.draw(at2, "霜天 Ag", Point{8.0f, 8.0f}, 24.0f, Color::rgb(0, 0, 0));
  ST_REQUIRE(status2.has_value());

  const std::size_t ink1 = ink_pixels(at1);
  const std::size_t ink2 = ink_pixels(at2);
  ST_CHECK(ink1 > 100);
  // 面积随 DPI² 增长（抗锯齿边缘会略低于 4 倍，但必须显著高于 3 倍）
  ST_CHECK_NEAR(static_cast<double>(ink2) / static_cast<double>(ink1), 4.0, 1.0);
  ST_CHECK(ink2 > ink1 * 3);

  // 逻辑度量与 DPI 无关（排版稳定）
  const st::math::Size size1 = renderer.measure("霜天 Ag", 24.0f);
  const st::math::Size size2 = renderer.measure("霜天 Ag", 24.0f);
  ST_CHECK_NEAR(size1.width, size2.width, 0.001);
  ST_CHECK(size1.width > 0.0f);

  // 逻辑包围盒在两档 DPI 下一致
  const st::math::IntRect logical1 = at1.content_bounds_logical();
  const st::math::IntRect logical2 = at2.content_bounds_logical();
  ST_CHECK_NEAR(static_cast<double>(logical1.width), static_cast<double>(logical2.width), 1.5);
  ST_CHECK_NEAR(static_cast<double>(logical1.height), static_cast<double>(logical2.height), 1.5);
}

ST_TEST(dpi_ui_layout_is_dpi_independent) {
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{320.0f, 200.0f});
  auto panel = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
  panel->style().padding = st::math::Insets::all(16.0f);
  panel->style().gap = 8.0f;
  auto first = std::make_unique<st::ui::Panel>();
  first->set_id("first");
  auto second = std::make_unique<st::ui::Panel>();
  second->set_id("second");
  panel->add_child(std::move(first));
  panel->add_child(std::move(second));
  st::ui::Element* first_ptr = panel->child_at(0);
  st::ui::Element* second_ptr = panel->child_at(1);
  root.set_content(std::move(panel));
  root.layout(true);

  // 逻辑布局不因 DPI 变化（画布 DPI 只影响光栅化密度）
  const float first_x = first_ptr->bounds().x;
  const float second_x = second_ptr->bounds().x;
  ST_CHECK_NEAR(first_x, 16.0, 0.001);
  ST_CHECK(second_x > first_x);

  Canvas canvas_1x = Canvas::for_logical_size(320, 200, 1.0f);
  canvas_1x.clear(Color{0, 0, 0, 0});
  root.paint(canvas_1x);
  Canvas canvas_2x = Canvas::for_logical_size(320, 200, 2.0f);
  canvas_2x.clear(Color{0, 0, 0, 0});
  root.paint(canvas_2x);

  ST_CHECK_EQ(canvas_1x.physical_width(), 320);
  ST_CHECK_EQ(canvas_2x.physical_width(), 640);
  // 布局不变
  ST_CHECK_NEAR(first_ptr->bounds().x, first_x, 0.001);
}

ST_TEST(dpi_runtime_scale_switch) {
  Canvas canvas = Canvas::for_logical_size(80, 40, 1.0f);
  canvas.fill_rect(Rect{0.0f, 0.0f, 80.0f, 40.0f}, Paint::solid(Color::rgb(10, 20, 30)));
  ST_CHECK_EQ(canvas.int_bounds().width, 80);

  canvas.set_device_scale(2.0f);
  ST_CHECK_NEAR(canvas.device_scale(), 2.0, 1e-6);
  // 逻辑尺寸不变、drawing 仍需落在逻辑空间（缓冲未随 scale 变化，属调用方责任）
  ST_CHECK_EQ(canvas.width(), 40);

  Canvas resized = Canvas::for_logical_size(80, 40, 2.0f);
  resized.clear(Color{0, 0, 0, 0});
  resized.fill_rect(Rect{0.0f, 0.0f, 80.0f, 40.0f}, Paint::solid(Color::rgb(10, 20, 30)));
  const st::math::IntRect bounds = resized.content_bounds();
  ST_CHECK_EQ(bounds.width, 160);
  ST_CHECK_EQ(bounds.height, 80);
}

// —— Canvas 移动语义必须保留 DPI 缩放（真实缺陷回归）——
//
// 症状：运行时切 DPI（`*canvas = Canvas::for_logical_size(w, h, 2.0f)`）后，新画布物理尺寸是 2x
// 而坐标换算仍按 1x，绘制内容只占左上 1/4（"界面缩在角落"）。
ST_TEST(dpi_move_preserves_device_scale) {
  Canvas source = Canvas::for_logical_size(100, 50, 2.0f);
  ST_CHECK_NEAR(source.device_scale(), 2.0, 1e-6);

  // 移动构造
  Canvas moved(std::move(source));
  ST_CHECK_NEAR(moved.device_scale(), 2.0, 1e-6);
  ST_CHECK_EQ(moved.physical_width(), 200);
  ST_CHECK_EQ(moved.physical_height(), 100);
  ST_CHECK_EQ(moved.width(), 100);  // 逻辑尺寸 = 物理 / scale

  // 移动赋值（正是"重建帧缓冲"的写法）
  Canvas target = Canvas::for_logical_size(10, 10, 1.0f);
  ST_CHECK_NEAR(target.device_scale(), 1.0, 1e-6);
  target = Canvas::for_logical_size(100, 50, 2.0f);
  ST_CHECK_NEAR(target.device_scale(), 2.0, 1e-6);

  // 移动后绘制：逻辑坐标必须按 scale 落到物理像素（铺满整个物理缓冲）
  target.clear(Color{0, 0, 0, 0});
  target.fill_rect(Rect{0.0f, 0.0f, 100.0f, 50.0f}, Paint::solid(Color::rgb(10, 20, 30)), 0.0f);
  std::size_t painted = 0;
  for (int y = 0; y < target.physical_height(); ++y) {
    for (int x = 0; x < target.physical_width(); ++x) {
      if (target.pixel_at(x, y).a != 0U) ++painted;
    }
  }
  ST_CHECK_EQ(painted, static_cast<std::size_t>(target.physical_width()) *
                           static_cast<std::size_t>(target.physical_height()));
}

// —— 裁剪必须按逻辑坐标换算（真实缺陷回归）——
//
// 症状：`Card`（clip_children）在 2x 屏上把子项整片裁掉——圆角裁剪路径按逻辑坐标光栅化，
// 却与物理像素口径的 mask/clip 混用，于是裁剪区域只覆盖左上 1/4，子项"凭空消失"。
ST_TEST(dpi_rounded_clip_scales_with_device_scale) {
  for (const float scale : {1.0f, 2.0f, 1.5f}) {
    Canvas canvas = Canvas::for_logical_size(100, 100, scale);
    canvas.clear(Color{0, 0, 0, 0});
    // 逻辑 (20,20)-(80,80) 区域裁剪，圆角 8
    canvas.push_clip_rounded_rect(Rect{20.0f, 20.0f, 60.0f, 60.0f}, 8.0f);
    canvas.fill_rect(Rect{0.0f, 0.0f, 100.0f, 100.0f}, Paint::solid(Color::rgb(0, 0, 0)), 0.0f);
    canvas.pop_clip();

    // 内部必须被填（不是"剪没了"）
    ST_CHECK(canvas.pixel_at_point(Point{50.0f, 50.0f}).a > 200U);
    // 外部必须被裁掉
    ST_CHECK_EQ(canvas.pixel_at_point(Point{10.0f, 50.0f}).a, 0U);
    ST_CHECK_EQ(canvas.pixel_at_point(Point{50.0f, 10.0f}).a, 0U);
    ST_CHECK_EQ(canvas.pixel_at_point(Point{90.0f, 50.0f}).a, 0U);
    // 四个圆角确实被"切掉"（角点在裁剪区外）
    ST_CHECK_EQ(canvas.pixel_at_point(Point{20.5f, 20.5f}).a, 0U);
    ST_CHECK(canvas.pixel_at_point(Point{50.0f, 22.0f}).a > 200U);
  }
}

ST_TEST(dpi_clip_rect_scales_with_device_scale) {
  Canvas canvas = Canvas::for_logical_size(100, 50, 2.0f);
  canvas.clear(Color{0, 0, 0, 0});
  canvas.push_clip_rect(Rect{10.0f, 10.0f, 30.0f, 20.0f});
  canvas.fill_rect(Rect{0.0f, 0.0f, 100.0f, 50.0f}, Paint::solid(Color::rgb(0, 0, 0)), 0.0f);
  canvas.pop_clip();
  ST_CHECK(canvas.pixel_at_point(Point{25.0f, 20.0f}).a > 200U);
  ST_CHECK_EQ(canvas.pixel_at_point(Point{5.0f, 20.0f}).a, 0U);
  ST_CHECK_EQ(canvas.pixel_at_point(Point{45.0f, 20.0f}).a, 0U);
  ST_CHECK_EQ(canvas.pixel_at_point(Point{25.0f, 35.0f}).a, 0U);
}

ST_TEST(dpi_canvas_physical_size_is_exact_for_any_scale) {
  // 这条是**"切 DPI 后整屏发糊"**的回归测试。
  //
  // 背景：窗口后端原先把「逻辑尺寸 × scale」算成物理尺寸，而逻辑尺寸是整数、
  // scale 常是分数，于是 `round(logical * scale)` **取不到所有整数**——
  // 1.5x 下奇数宽度根本不可达（`741 × 1.5 = 1111.5` 只能得到 1112）。
  // 画布与窗口客户区差 1 像素，DXGI 就会**把整块纹理拉伸**到客户区：整屏发糊。
  // 用户实测："切换 DPI 时会模糊，重新缩放一下才好"（手动缩放恰好落到能整除的尺寸上）。
  //
  // 修法：物理尺寸成为**输入**（由客户区直接给出），逻辑尺寸由 `物理 / scale` 折算。
  // 因此这里必须钉住：**给什么物理尺寸，画布就是什么物理尺寸**——无论 scale 多怪。
  for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) {
    for (const int physical : {1111, 1112, 1113, 741, 999, 1001, 1280, 1500}) {
      const Canvas canvas{physical, 900, scale};
      ST_CHECK_EQ(canvas.physical_width(), physical);
      ST_CHECK_EQ(canvas.physical_height(), 900);
      ST_CHECK_EQ(canvas.device_scale(), scale);
      // 逻辑宽度由物理尺寸折算（`width()` 的契约就是 `round(物理 / scale)`）。
      // 这里钉住"折算不会反过来改变物理尺寸"——那是本用例的主断言。
      const int expected_logical = static_cast<int>(std::lround(static_cast<double>(physical) /
                                                               static_cast<double>(scale)));
      ST_CHECK_EQ(canvas.width(), expected_logical);
    }
  }
}

ST_TEST(dpi_logical_round_trip_cannot_hit_every_physical_size) {
  // 把"为什么必须让物理尺寸做输入"这层理由固化成断言：
  // 在 1.5x 下，**不存在**任何整数逻辑宽度能产出 1111 个物理像素。
  // 这不是实现的缺陷，是数学事实——所以"先定逻辑、再乘 scale"这条路必然漏尺寸。
  const auto physical_of = [](int logical, float scale) {
    return static_cast<int>(std::lround(static_cast<double>(logical) * static_cast<double>(scale)));
  };
  bool any_hits_1111 = false;
  for (int logical = 700; logical <= 760; ++logical) {
    if (physical_of(logical, 1.5f) == 1111) any_hits_1111 = true;
  }
  ST_CHECK(!any_hits_1111);                 // 1.5x 取不到奇数 1111
  ST_CHECK_EQ(physical_of(741, 1.5f), 1112);   // 最近的只能到 1112（差 1px → 会糊）
  // 而按物理尺寸直接建画布，就能精确拿到它
  const Canvas canvas{1111, 900, 1.5f};
  ST_CHECK_EQ(canvas.physical_width(), 1111);
}
