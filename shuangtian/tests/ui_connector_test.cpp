/// `Connector`：连线组件的几何、路由、虚线、箭头与命中。
///
/// ## 守的是什么
///
/// 连线这类"只有几个像素宽"的自绘件，一旦算错**不会崩、不会报错**，只是看起来
/// "图有点怪"——所以每一条断言都必须落到**可复算的量**（路径点数、切向、像素墨量、
/// 命中判定），而不是"画出了东西"。
///
/// 四条最贵的：
///
/// 1. **曲线退化成直线**：`Curve` 的控制点沿"出线方向"外推，而固定坐标端点没有边、
///    也就没有法线。若不给默认方向，控制点正好落在端点上 ⇒ 三次贝塞尔退化成直线，
///    用户选"曲线"得到一根斜线。判据是**端点处切向**（首段方向必须接近水平，而不是
///    沿两点连线）。
/// 2. **端点双重偏移**：元素端点的 `bounds()` 是画布绝对坐标，而本组件的端点约定是
///    本地坐标（绘制时统一加回 `bounds_` 原点）。少了换算，偏移量随组件位置变化——
///    "部分卡片的线对得上、部分差一截"。判据是**把组件挪到别的位置，端点不变**。
/// 3. **命中吃掉下层点击**：线通常 `absolute` 铺满父级，若按整块 bounds 判定命中，
///    压在下面的按钮就点不动。判据是"线附近的点命中、同一矩形内的远处点**不**命中"。
/// 4. **虚线静默变实线**：光栅层没有 dash（`StrokeStyle` 只有 cap/join），虚线只能在
///    路径层切段。判据是**同一区块内虚线墨量显著少于实线**。

#include "st/test/test.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/connector.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Connector;
using st::ui::Element;
using st::ui::Theme;

constexpr int kCanvasW = 260;
constexpr int kCanvasH = 180;
constexpr std::uint32_t kBackdrop = 0xF7F8FA;

[[nodiscard]] auto make_backdrop() -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kCanvasW, kCanvasH, 1.0f);
  canvas.clear(Color::rgb(0xF7, 0xF8, 0xFA));
  return canvas;
}

/// 一块**不透明**的深色对比块，用来给"是否画了东西"做判据（背景是浅色）。
[[nodiscard]] auto is_ink(const Color& c) -> bool {
  const double lum =
      0.2126 * static_cast<double>(c.r) + 0.7152 * static_cast<double>(c.g) + 0.0722 * static_cast<double>(c.b);
  return lum < 200.0;   // 浅底 0xF7F8FA 的亮度约 248
}

/// 统计一块区域内的墨像素数（虚线/箭头/线宽的判据都是它）。
[[nodiscard]] auto ink_count(const Canvas& canvas, Rect region) -> int {
  int total = 0;
  for (int y = static_cast<int>(region.y); y < static_cast<int>(region.bottom()); ++y) {
    for (int x = static_cast<int>(region.x); x < static_cast<int>(region.right()); ++x) {
      if (is_ink(canvas.pixel_at(x, y))) ++total;
    }
  }
  return total;
}

/// 把组件摆到 `box` 并画到一张新画布上。
[[nodiscard]] auto render(Connector& connector, Rect box, const Theme& theme) -> Canvas {
  st::ui::RenderContext context{theme, nullptr, 0.0};
  connector.apply_theme(theme);
  connector.arrange(context, box);
  Canvas canvas = make_backdrop();
  connector.paint(context, canvas);
  return canvas;
}

/// 首段方向（用扁平化后的头两个点算），返回单位向量。
[[nodiscard]] auto first_direction(const st::math::Path& path) -> Point {
  const auto polylines = path.flatten(0.25f);
  if (polylines.empty() || polylines.front().points.size() < 2U) return Point{0.0f, 0.0f};
  const Point a = polylines.front().points[0];
  const Point b = polylines.front().points[1U];
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1.0e-6f) return Point{0.0f, 0.0f};
  return Point{dx / len, dy / len};
}

}  // namespace

/// 直线路由：路径就是两点，且端点落在给定坐标（加自身原点换算）。
ST_TEST(connector_straight_route_has_two_points_and_hits_both_ends) {
  const Theme theme = Theme::dark();
  Connector connector;
  connector.set_route(Connector::Route::Straight);
  connector.set_points({20.0f, 20.0f}, {180.0f, 120.0f});
  const Canvas canvas = render(connector, Rect{0.0f, 0.0f, 220.0f, 160.0f}, theme);

  // 几何：两点、无控制点。
  ST_CHECK_EQ(connector.path_point_count(), 2U);
  // 长度 ≈ 两点距离 sqrt(160²+100²)=188.68（允许扁平化容差）。
  ST_CHECK(std::abs(connector.path_length() - 188.6796f) < 1.0f);
  // 端点解析：本地坐标（`resolved_from` 不加原点）。
  ST_CHECK(std::abs(connector.resolved_from().x - 20.0f) < 0.01f);
  ST_CHECK(std::abs(connector.resolved_to().y - 120.0f) < 0.01f);
  // 画面上：起点与终点附近必须有墨，框外没有。
  ST_CHECK(ink_count(canvas, Rect{16.0f, 16.0f, 12.0f, 12.0f}) > 0);
  ST_CHECK(ink_count(canvas, Rect{174.0f, 114.0f, 12.0f, 12.0f}) > 0);
  ST_CHECK_EQ(ink_count(canvas, Rect{0.0f, 0.0f, 10.0f, 10.0f}), 0);
}

/// **曲线路由在端点处的切向必须沿"出线方向"**，不能退化成两点连线。
///
/// 这是本组件最容易静默坏掉的一处：固定坐标端点没有边法线，而 `Curve` 的控制点沿法线
/// 外推——没有默认方向时控制点落在端点上，三次贝塞尔退化成直线。
ST_TEST(connector_curve_keeps_horizontal_tangent_at_endpoints) {
  const Theme theme = Theme::dark();
  Connector connector;
  connector.set_route(Connector::Route::Curve);
  // 明显斜着的一对点：若退化成直线，首段方向会是 (0.83, 0.55) 这样的斜向。
  connector.set_points({30.0f, 30.0f}, {200.0f, 140.0f});
  const Canvas canvas = render(connector, Rect{0.0f, 0.0f, 220.0f, 160.0f}, theme);
  (void)canvas;

  const Point head = first_direction(connector.build_path());
  st::log::out("[connector] 首段方向 ({:.3f}, {:.3f})\n", static_cast<double>(head.x),
               static_cast<double>(head.y));
  // 出线方向是水平（右）⇒ 首段 dx 必须接近 1、dy 接近 0。
  ST_CHECK(head.x > 0.9f);
  ST_CHECK(std::abs(head.y) < 0.2f);
  // 曲线比直线长（控制点外推后是有弧度的）。
  Connector straight;
  straight.set_route(Connector::Route::Straight);
  straight.set_points({30.0f, 30.0f}, {200.0f, 140.0f});
  straight.arrange(st::ui::RenderContext{theme, nullptr, 0.0}, Rect{0.0f, 0.0f, 220.0f, 160.0f});
  ST_CHECK(connector.path_length() > straight.path_length());
  // 曲线路径含控制点：点数 > 2。
  ST_CHECK(connector.path_point_count() > 2U);
}

/// 正交路由：水平出线时中间竖着走一段，且转角圆化后路径点数多于"三个折点"。
ST_TEST(connector_orthogonal_route_turns_through_mid_x) {
  const Theme theme = Theme::dark();
  Connector connector;
  connector.set_route(Connector::Route::Orthogonal);
  connector.set_points({20.0f, 40.0f}, {180.0f, 120.0f});
  const Canvas canvas = render(connector, Rect{0.0f, 0.0f, 220.0f, 160.0f}, theme);
  ST_CHECK(ink_count(canvas, Rect{0.0f, 30.0f, 220.0f, 100.0f}) > 0);

  // 水平出、水平入 ⇒ 中段竖线在 x 的中点（100）。
  bool found_vertical_mid = false;
  for (const auto& polyline : connector.build_path().flatten(0.25f)) {
    const auto& points = polyline.points;
    for (std::size_t i = 1; i < points.size(); ++i) {
      const bool vertical = std::abs(points[i].x - points[i - 1U].x) < 0.3f &&
                            std::abs(points[i].y - points[i - 1U].y) > 1.0f;
      if (vertical && std::abs(points[i].x - 100.0f) < 2.0f) found_vertical_mid = true;
    }
  }
  ST_CHECK(found_vertical_mid);
  // 圆角折线用的是 `quad_to`（控制点是转角本身）⇒ 点数 > 4。
  ST_CHECK(connector.path_point_count() > 4U);

  // 关掉圆化后应当只剩直角折点（点数 = 4：起点、两个转角、终点）。
  connector.set_smooth_corners(false);
  ST_CHECK_EQ(connector.path_point_count(), 4U);
}

/// **元素端点坐标换算**：把组件本身挪位置，端点（画布绝对坐标）必须跟着挪同样的量。
///
/// 少了"绝对 → 本地"这一步时，偏移量恰为组件原点，且**随位置变化**——表现是
/// "同一个界面里有的线对得上、有的差一截"。
ST_TEST(connector_element_endpoints_follow_component_origin) {
  const Theme theme = Theme::dark();
  st::ui::RenderContext context{theme, nullptr, 0.0};

  // 一个"节点"元素：绝对定位、几何固定。
  Element from;
  Element to;
  from.arrange(context, Rect{100.0f, 60.0f, 40.0f, 20.0f});
  to.arrange(context, Rect{220.0f, 120.0f, 40.0f, 20.0f});

  Connector connector;
  connector.set_route(Connector::Route::Straight);
  connector.set_source(&from, Connector::Side::Right);
  connector.set_target(&to, Connector::Side::Left);

  // 组件摆到 (0,0)：本地坐标 == 绝对坐标。
  connector.arrange(context, Rect{0.0f, 0.0f, 300.0f, 180.0f});
  const Point at_origin = connector.resolved_from();
  st::log::out("[connector] 原点处 source=({:.1f},{:.1f})\n", static_cast<double>(at_origin.x),
               static_cast<double>(at_origin.y));
  ST_CHECK(std::abs(at_origin.x - 140.0f) < 0.01f);   // from 的右边中点
  ST_CHECK(std::abs(at_origin.y - 70.0f) < 0.01f);
  ST_CHECK(std::abs(connector.resolved_to().x - 220.0f) < 0.01f);   // to 的左边中点

  // 组件挪到 (37,-19)：端点**本地坐标**必须减去同样的偏移（画到画布上仍是同一处）。
  connector.arrange(context, Rect{37.0f, -19.0f, 300.0f, 180.0f});
  const Point moved = connector.resolved_from();
  st::log::out("[connector] 挪位后 source=({:.1f},{:.1f})\n", static_cast<double>(moved.x),
               static_cast<double>(moved.y));
  ST_CHECK(std::abs(moved.x - (140.0f - 37.0f)) < 0.01f);
  ST_CHECK(std::abs(moved.y - (70.0f + 19.0f)) < 0.01f);

  // 画到画布上的绝对坐标与组件位置无关（这才是"线跟着元素走"的真正含义）。
  const auto path = connector.build_path();
  ST_REQUIRE(!path.is_empty());
  ST_CHECK(std::abs(path.raw_points().front().x - 140.0f) < 0.01f);
  ST_CHECK(std::abs(path.raw_points().front().y - 70.0f) < 0.01f);
}

/// **端点元素不在场（不可见）时整条线不画**——而不是画到 (0,0)。
ST_TEST(connector_skips_line_when_endpoint_element_is_hidden) {
  const Theme theme = Theme::dark();
  st::ui::RenderContext context{theme, nullptr, 0.0};

  Element node;
  node.arrange(context, Rect{40.0f, 40.0f, 40.0f, 20.0f});
  Connector connector;
  // 顺序很重要：`set_points` 把**两端**都改成固定坐标（含清掉元素绑定），
  // 所以在它之后再 `set_source` 才不会被覆盖。
  connector.set_points({40.0f, 50.0f}, {200.0f, 150.0f});
  connector.set_source(&node, Connector::Side::Right);
  connector.arrange(context, Rect{0.0f, 0.0f, 240.0f, 170.0f});
  ST_CHECK(connector.source_ok());
  ST_CHECK(!connector.build_path().is_empty());

  node.set_visible(false);
  ST_CHECK(!connector.source_ok());   // 如实上报"端点此刻不存在"
  ST_CHECK(connector.build_path().is_empty());

  // 画面上也真的什么都没有（不能残留上一帧的线）。
  const Canvas canvas = render(connector, Rect{0.0f, 0.0f, 240.0f, 170.0f}, theme);
  ST_CHECK_EQ(ink_count(canvas, Rect{0.0f, 0.0f, 240.0f, 170.0f}), 0);
}

/// **命中是沿线窄带，不是整块 bounds**——否则 `absolute` 铺满父级时会吃掉下层点击。
ST_TEST(connector_hit_is_a_narrow_band_along_the_line) {
  const Theme theme = Theme::dark();
  st::ui::RenderContext context{theme, nullptr, 0.0};
  Connector connector;
  connector.set_route(Connector::Route::Straight);
  connector.set_points({20.0f, 20.0f}, {200.0f, 20.0f});   // 横着一条
  connector.arrange(context, Rect{0.0f, 0.0f, 240.0f, 160.0f});
  connector.set_hit_tolerance(5.0f);

  ST_CHECK(connector.hit_test(Point{110.0f, 20.0f}));    // 线上
  ST_CHECK(connector.hit_test(Point{110.0f, 24.0f}));    // 带内（4px）
  ST_CHECK(!connector.hit_test(Point{110.0f, 31.0f}));   // 带外（11px）——整块 bounds 内但不命中
  ST_CHECK(!connector.hit_test(Point{110.0f, 120.0f}));  // 远离（仍在 bounds 内）
  // 距离本身可复算（测试与命中同源）。
  ST_CHECK(std::abs(connector.distance_to_line(Point{110.0f, 30.0f}) - 10.0f) < 0.2f);

  // 关掉容差后只剩线本身（线宽的一半 + 1）能命中。
  connector.set_hit_tolerance(0.0f);
  ST_CHECK(connector.hit_test(Point{110.0f, 20.0f}));
  ST_CHECK(!connector.hit_test(Point{110.0f, 26.0f}));
}

/// **虚线**：同一区块的墨量必须显著少于实线（否则就是"设了 dash 但画的是实线"）。
ST_TEST(connector_dash_reduces_ink_but_keeps_endpoints) {
  const Theme theme = Theme::dark();
  Connector solid;
  solid.set_points({20.0f, 90.0f}, {240.0f, 90.0f});
  solid.set_width(2.0f);
  const Canvas solid_canvas = render(solid, Rect{0.0f, 0.0f, 260.0f, 180.0f}, theme);
  const int solid_ink = ink_count(solid_canvas, Rect{10.0f, 84.0f, 240.0f, 13.0f});

  Connector dashed;
  dashed.set_points({20.0f, 90.0f}, {240.0f, 90.0f});
  dashed.set_width(2.0f);
  dashed.set_dash_csv("6 6");
  const Canvas dashed_canvas = render(dashed, Rect{0.0f, 0.0f, 260.0f, 180.0f}, theme);
  const int dashed_ink = ink_count(dashed_canvas, Rect{10.0f, 84.0f, 240.0f, 13.0f});

  st::log::out("[connector] 墨量 实线 {} / 虚线 {}\n", solid_ink, dashed_ink);
  ST_CHECK(solid_ink > 0);
  // 6/6 占空比 ⇒ 墨量应落在实线的一半附近（给抗锯齿留余量）。
  ST_CHECK(dashed_ink * 2 < solid_ink + solid_ink / 4);
  ST_CHECK(dashed_ink > 0);

  // 非法 dash 必须**被忽略**（保持上一次有效样式，而不是静默变实线）。
  dashed.set_dash_csv("6 0");
  ST_CHECK_EQ(dashed.dash_csv(), std::string("6 6"));
  dashed.set_dash_csv("");
  ST_CHECK_EQ(dashed.dash_csv(), std::string(""));
}

/// **箭头**：加了箭头的那一端附近墨量必然增加（不改变另一端）。
ST_TEST(connector_arrow_adds_ink_at_the_selected_end) {
  const Theme theme = Theme::dark();
  Connector bare;
  bare.set_points({20.0f, 90.0f}, {230.0f, 90.0f});
  const Canvas bare_canvas = render(bare, Rect{0.0f, 0.0f, 260.0f, 180.0f}, theme);
  const int bare_tail = ink_count(bare_canvas, Rect{210.0f, 80.0f, 24.0f, 22.0f});
  const int bare_head = ink_count(bare_canvas, Rect{16.0f, 80.0f, 24.0f, 22.0f});

  Connector arrowed;
  arrowed.set_points({20.0f, 90.0f}, {230.0f, 90.0f});
  arrowed.set_arrow(Connector::Arrow::End);
  arrowed.set_arrow_size(10.0f);
  const Canvas arrowed_canvas = render(arrowed, Rect{0.0f, 0.0f, 260.0f, 180.0f}, theme);
  const int arrowed_tail = ink_count(arrowed_canvas, Rect{210.0f, 80.0f, 24.0f, 22.0f});
  const int arrowed_head = ink_count(arrowed_canvas, Rect{16.0f, 80.0f, 24.0f, 22.0f});

  st::log::out("[connector] 箭头端墨量 {}→{}（另一端 {}→{}）\n", bare_tail, arrowed_tail,
               bare_head, arrowed_head);
  ST_CHECK(arrowed_tail > bare_tail + 20);   // 三角面积 ~ 数十像素
  ST_CHECK(std::abs(arrowed_head - bare_head) <= 2);   // 另一端不受影响
}

/// 属性面：往返 + 非法值如实拒绝。
ST_TEST(connector_property_surface_round_trips_and_rejects_garbage) {
  Connector connector;
  ST_CHECK(connector.set_property("route", "curve"));
  ST_CHECK_EQ(connector.get_property("route").value_or(""), std::string("curve"));
  ST_CHECK(!connector.set_property("route", "wobble"));   // 非法枚举
  ST_CHECK_EQ(connector.get_property("route").value_or(""), std::string("curve"));

  ST_CHECK(connector.set_property("arrow", "both"));
  ST_CHECK_EQ(connector.get_property("arrow").value_or(""), std::string("both"));

  ST_CHECK(connector.set_property("from", "10.5, 20"));
  ST_CHECK(connector.set_property("to", "100,200"));
  ST_CHECK(std::abs(connector.source_point().x - 10.5f) < 0.01f);
  ST_CHECK(std::abs(connector.target_point().y - 200.0f) < 0.01f);
  ST_CHECK(!connector.set_property("from", "10.5"));   // 缺一项
  ST_CHECK(!connector.set_property("from", "a,b"));    // 非数字

  ST_CHECK(connector.set_property("dash", "6 4"));
  ST_CHECK_EQ(connector.dash_csv(), std::string("6 4"));
  ST_CHECK(connector.set_property("dash", ""));        // 空 = 实线
  ST_CHECK_EQ(connector.dash_csv(), std::string(""));

  ST_CHECK(connector.set_property("stroke_color", "#ff8800"));
  ST_CHECK_EQ(connector.color_hex(), std::string("#ff8800ff"));
  ST_CHECK(!connector.set_property("stroke_color", "not-a-color"));

  // 只读项必须拒绝写。
  ST_CHECK(!connector.set_property("path_length", "10"));
  ST_CHECK(connector.get_property("path_length").has_value());
  ST_CHECK(connector.get_property("source_ok").has_value());
}

/// 端点吸附的纯函数：`Side::Auto` 与四边中点（测试直接调，不经过绘制）。
ST_TEST(connector_anchor_picks_the_side_facing_the_other_end) {
  const Rect box{100.0f, 100.0f, 80.0f, 40.0f};   // 中心 (140,120)
  // 对方在右边 → 右侧边中点
  const Point right = Connector::anchor_of(box, Connector::Side::Auto, Point{400.0f, 120.0f});
  ST_CHECK(std::abs(right.x - 180.0f) < 0.01f);
  ST_CHECK(std::abs(right.y - 120.0f) < 0.01f);
  // 对方在上方 → 上侧边中点
  const Point top = Connector::anchor_of(box, Connector::Side::Auto, Point{140.0f, -50.0f});
  ST_CHECK(std::abs(top.y - 100.0f) < 0.01f);
  ST_CHECK(std::abs(top.x - 140.0f) < 0.01f);
  // 显式指定优先级高于 Auto 判据。
  const Point left = Connector::anchor_of(box, Connector::Side::Left, Point{400.0f, 120.0f});
  ST_CHECK(std::abs(left.x - 100.0f) < 0.01f);
  // 外向法线：吸附在右边 → (1,0)。
  const Point outward = Connector::outward_of(box, Point{180.0f, 120.0f});
  ST_CHECK(std::abs(outward.x - 1.0f) < 0.01f);
  ST_CHECK(std::abs(outward.y) < 0.01f);
}
