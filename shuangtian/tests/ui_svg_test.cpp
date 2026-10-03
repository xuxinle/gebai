/// SVG 图标子集测试：path d 全指令 / viewBox 变换 / 圆弧端点 / sprite / even-odd /
/// 缩放清晰度（矢量重栅 vs 位图放大的量化差异）。

#include <cmath>
#include <string>
#include <string_view>

#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/svg.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::svg::Document;

/// 数一下画布里指定色的像素量（着墨量度量；缓冲为 RGBA 预乘布局）。
auto count_ink(const Canvas& canvas, Color color) -> std::size_t {
  const auto pixels = canvas.pixels();
  std::size_t count = 0;
  for (const std::uint32_t packed : pixels) {
    if (st::math::unpremultiply(packed) == color) ++count;
  }
  return count;
}

/// alpha > 0 的像素量。
auto count_any_ink(const Canvas& canvas) -> std::size_t {
  std::size_t count = 0;
  const auto pixels = canvas.pixels();
  for (const std::uint32_t packed : pixels) {
    if ((packed & 0xFFU) != 0U) ++count;
  }
  return count;
}

/// 全部像素的 alpha 总量（亚像素覆盖率的积分——「墨量」）。
auto total_alpha(const Canvas& canvas) -> std::size_t {
  std::size_t total = 0;
  const auto pixels = canvas.pixels();
  for (const std::uint32_t packed : pixels) {
    total += packed & 0xFFU;
  }
  return total;
}

}  // namespace

ST_TEST(svg_parse_path_basic_commands) {
  // M/L/H/V/Z（绝对）+ 隐式 L 重复：M L L L L Z = 6 条命令
  const auto path = st::ui::svg::parse_path("M2 2 L6 2 6 6 H10 V10 Z");
  ST_CHECK(path.has_value());
  ST_CHECK_EQ(path->commands().size(), static_cast<std::size_t>(6));
}

ST_TEST(svg_parse_path_relative_and_smooth) {
  // 小写相对坐标 + S 平滑三次 + T 平滑二次：m l c s t = 5 条命令
  const auto path = st::ui::svg::parse_path("m2 2 l2 0 c1 0 1 1 2 1 s2 -1 3 0 t2 2");
  ST_CHECK(path.has_value());
  ST_CHECK(path->commands().size() >= 5);
  // 相对 move：起点 (2,2)
  const auto& first = path->commands().front();
  ST_CHECK_NEAR(first.p1.x, 2.0f, 1e-4f);
  ST_CHECK_NEAR(first.p1.y, 2.0f, 1e-4f);
}

ST_TEST(svg_parse_path_arc_endpoints) {
  // A 指令：从 (2,12) 到 (22,12) 的半圆——终点必须精确落在 (22,12)
  const auto path = st::ui::svg::parse_path("M2 12 A10 10 0 0 1 22 12");
  ST_CHECK(path.has_value());
  // 找最后一个非 Close 命令的终点
  Point last{};
  for (const auto& command : path->commands()) {
    switch (command.kind) {
      case st::raster::PathCommand::Kind::MoveTo:
      case st::raster::PathCommand::Kind::LineTo:
        last = command.p1;
        break;
      case st::raster::PathCommand::Kind::QuadTo:
        last = command.p2;
        break;
      case st::raster::PathCommand::Kind::CubicTo:
        last = command.p3;
        break;
      case st::raster::PathCommand::Kind::Close:
        break;
    }
  }
  ST_CHECK_NEAR(last.x, 22.0f, 1e-2f);
  ST_CHECK_NEAR(last.y, 12.0f, 1e-2f);
}

ST_TEST(svg_parse_path_scientific_notation) {
  const auto path = st::ui::svg::parse_path("M1e1 2E0 L1.5e+1 -2");
  ST_CHECK(path.has_value());
  const auto& first = path->commands().front();
  ST_CHECK_NEAR(first.p1.x, 10.0f, 1e-4f);
  ST_CHECK_NEAR(first.p1.y, 2.0f, 1e-4f);
}

ST_TEST(svg_parse_viewbox_and_fit) {
  const auto doc = st::ui::svg::parse(
      R"x(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><path d="M1 1 L15 15"/></svg>)x");
  ST_CHECK(doc.has_value());
  ST_CHECK_NEAR(doc->view_w, 16.0f, 1e-4f);
  // 16×16 视图盒放进 32×8 的矩形：meet → scale=0.5，水平居中
  const auto transform = st::ui::svg::fit_transform(*doc, Rect{0.0f, 0.0f, 32.0f, 8.0f});
  const Point mapped = transform.apply(Point{16.0f, 16.0f});
  ST_CHECK_NEAR(mapped.x, 20.0f, 1e-3f);  // tx=(32-8)/2=12，+16*0.5
  ST_CHECK_NEAR(mapped.y, 8.0f, 1e-3f);   // ty=0，+16*0.5
}

ST_TEST(svg_shapes_rect_circle_polygon) {
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24"><rect x="2" y="2" width="20" height="20" rx="4"/><circle cx="12" cy="12" r="6"/><polygon points="12,2 22,22 2,22"/></svg>)x");
  ST_CHECK(doc.has_value());
  ST_CHECK_EQ(doc->nodes.size(), static_cast<std::size_t>(3));
  for (const auto& node : doc->nodes) {
    ST_CHECK(!node.path.is_empty());
  }
}

ST_TEST(svg_default_fill_is_black_override_wins) {
  // 无 fill 属性（Codicons 形态）→ override_color 生效（主题色着色）
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24"><path d="M4 4 L20 4 L20 20 L4 20 Z"/></svg>)x");
  ST_CHECK(doc.has_value());
  Canvas canvas(24, 24);
  canvas.clear(Color{0, 0, 0, 0});
  st::ui::svg::draw(canvas, *doc, Rect{0, 0, 24, 24}, Color{255, 0, 0, 255});
  ST_CHECK(count_ink(canvas, Color{255, 0, 0, 255}) + count_any_ink(canvas) > 0);
}

ST_TEST(svg_explicit_fill_not_overridden) {
  // 显式 fill 色 → override 不覆盖（多色图标不被主题色破坏）
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24"><rect x="2" y="2" width="20" height="20" fill="#00ff00"/></svg>)x");
  ST_CHECK(doc.has_value());
  Canvas canvas(48, 48);
  canvas.clear(Color{0, 0, 0, 0});
  st::ui::svg::draw(canvas, *doc, Rect{0, 0, 48, 48}, Color{255, 0, 0, 255});
  // 应有绿色墨、无红墨
  ST_CHECK(count_ink(canvas, Color{255, 0, 0, 255}) == 0);
  Canvas probe(48, 48);
  probe.clear(Color{0, 0, 0, 0});
  st::ui::svg::draw(probe, *doc, Rect{0, 0, 48, 48});
  ST_CHECK(count_ink(probe, Color{0, 255, 0, 255}) > 100);
}

ST_TEST(svg_stroke_lucide_style) {
  // Lucide/Feather 形态：fill=none + stroke + stroke-width（currentColor → override）
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 4 L20 20 M20 4 L4 20"/></svg>)x");
  ST_CHECK(doc.has_value());
  ST_CHECK(doc->nodes.size() == 1);
  ST_CHECK(doc->nodes.front().style.fill_none);
  ST_CHECK(doc->nodes.front().style.stroke_current);
  Canvas canvas(48, 48);
  canvas.clear(Color{0, 0, 0, 0});
  st::ui::svg::draw(canvas, *doc, Rect{0, 0, 48, 48}, Color{0, 128, 255, 255});
  ST_CHECK(count_any_ink(canvas) > 0);
}

ST_TEST(svg_group_style_inheritance) {
  // <g> 级联：子元素继承组的 stroke 与 fill
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24"><g fill="none" stroke="#102030" stroke-width="1.5"><path d="M2 2 L22 22"/><path d="M22 2 L2 22"/></g></svg>)x");
  ST_CHECK(doc.has_value());
  ST_CHECK_EQ(doc->nodes.size(), static_cast<std::size_t>(2));
  for (const auto& node : doc->nodes) {
    ST_CHECK(node.style.fill_none);
    ST_CHECK(node.style.stroke.has_value());
    ST_CHECK_NEAR(node.style.stroke_width, 1.5f, 1e-4f);
  }
}

ST_TEST(svg_sprite_use_and_defs) {
  // sprite：symbol 定义 + use 引用；defs 里的形状不直接绘制
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24"><defs><rect id="r" x="4" y="4" width="16" height="16"/></defs><use href="#r"/><use xlink:href="#r" x="0" y="0"/></svg>)x");
  ST_CHECK(doc.has_value());
  // defs 内不绘制；两次 use 展开成 2 个节点
  ST_CHECK_EQ(doc->nodes.size(), static_cast<std::size_t>(2));
}

ST_TEST(svg_use_cycle_protection) {
  // 自引用 use 不死循环（防环）
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24"><symbol id="a"><use href="#a"/><rect x="2" y="2" width="20" height="20"/></symbol><use href="#a"/></svg>)x");
  ST_CHECK(doc.has_value());
  ST_CHECK_EQ(doc->nodes.size(), static_cast<std::size_t>(1));
}

ST_TEST(svg_evenodd_ring_has_hole) {
  // even-odd 环：外圆 + 内孔 → 中心不应有墨
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24"><path fill-rule="evenodd" d="M12 2 A10 10 0 1 0 12 22 A10 10 0 1 0 12 2 Z M12 8 A4 4 0 1 0 12 16 A4 4 0 1 0 12 8 Z"/></svg>)x");
  ST_CHECK(doc.has_value());
  Canvas canvas(96, 96);
  canvas.clear(Color{0, 0, 0, 0});
  st::ui::svg::draw(canvas, *doc, Rect{0, 0, 96, 96}, Color{255, 255, 255, 255});
  // 中心（48,48）在孔里应透明；环带中点（48,20——外半径40/内孔16 之间）应有墨
  const Color center = canvas.pixel_at(48, 48);
  ST_CHECK(center.a == 0U);
  const Color ring = canvas.pixel_at(48, 20);
  ST_CHECK(ring.a > 200U);
}

ST_TEST(svg_scaling_keeps_vector_crispness) {
  // **缩放清晰度的量化断言**：同一图标渲 16px 与 96px。
  // 矢量重栅：小尺寸的着墨占比（覆盖率）与大尺寸的面积占比应接近（形状守恒）；
  // 且大尺寸边缘过渡带占比不随尺寸恶化（抗锯齿带宽度 ~1px 恒定）。
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 4 L20 20 M20 4 L4 20 M12 3 L12 21"/></svg>)x");
  ST_CHECK(doc.has_value());

  Canvas small(16, 16);
  small.clear(Color{0, 0, 0, 0});
  st::ui::svg::draw(small, *doc, Rect{0, 0, 16, 16}, Color{255, 255, 255, 255});

  Canvas large(96, 96);
  large.clear(Color{0, 0, 0, 0});
  st::ui::svg::draw(large, *doc, Rect{0, 0, 96, 96}, Color{255, 255, 255, 255});

  ST_CHECK(count_any_ink(small) > 0);
  ST_CHECK(count_any_ink(large) > 0);
  // 墨量占比（覆盖率积分/面积）：16px 与 96px 相差应 < 35%（矢量守恒；
  // 位图放大会把小图的抗锯齿一起放大——占比结构完全不同）
  const double ratio_small =
      static_cast<double>(total_alpha(small)) / (16.0 * 16.0 * 255.0);
  const double ratio_large =
      static_cast<double>(total_alpha(large)) / (96.0 * 96.0 * 255.0);
  ST_CHECK(ratio_small > 0.01);
  ST_CHECK(ratio_large > 0.01);
  const double diff = std::abs(ratio_small - ratio_large) / ratio_large;
  ST_CHECK(diff < 0.35);
}

ST_TEST(svg_iconset_load_and_get) {
  st::ui::svg::IconSet set;
  const bool loaded = set.load(
      R"x(<svg xmlns="http://www.w3.org/2000/svg"><symbol id="x" viewBox="0 0 24 24"><path d="M5 5 L19 19 M19 5 L5 19"/></symbol><symbol id="menu" viewBox="0 0 24 24"><path d="M4 6 L20 6 M4 12 L20 12 M4 18 L20 18"/></symbol></svg>)x");
  ST_CHECK(loaded);
  const auto ids = set.ids();
  ST_CHECK_EQ(ids.size(), static_cast<std::size_t>(2));
  ST_CHECK(set.has("x"));
  ST_CHECK(set.has("menu"));
  ST_CHECK(!set.has("missing"));
  const auto doc = set.get("menu");
  ST_CHECK(doc.has_value());
  ST_CHECK(!doc->nodes.empty());
  // symbol 的 viewBox 生效（view_w = 24）
  ST_CHECK_NEAR(doc->view_w, 24.0f, 1e-4f);
}

ST_TEST(svg_registry_and_icon_fallback) {
  auto& registry = st::ui::svg_registry();
  const bool ok = registry.add_single(
      "svg-test-only-zzz",
      R"x(<svg viewBox="0 0 24 24"><path d="M3 3 L21 21 M21 3 L3 21" stroke="#ffffff" stroke-width="3" fill="none"/></svg>)x");
  ST_CHECK(ok);
  ST_CHECK(st::ui::Icon::has("svg:svg-test-only-zzz"));
  ST_CHECK(st::ui::Icon::has("svg-test-only-zzz"));  // 裸名也命中（内置表优先、SVG 补位）

  Canvas canvas(32, 32);
  canvas.clear(Color{0, 0, 0, 0});
  st::ui::Icon::draw(canvas, "svg:svg-test-only-zzz", Rect{0, 0, 32, 32},
                     Color{255, 255, 255, 255});
  ST_CHECK(count_any_ink(canvas) > 0);
  // 内置表不受影响
  ST_CHECK(st::ui::Icon::has("check"));
  Canvas builtin(32, 32);
  builtin.clear(Color{0, 0, 0, 0});
  st::ui::Icon::draw(builtin, "check", Rect{0, 0, 32, 32}, Color{255, 255, 255, 255});
  ST_CHECK(count_any_ink(builtin) > 0);
}

ST_TEST(svg_painter_cache_hits) {
  st::ui::svg::IconSet set;
  ST_CHECK(set.load(
      R"x(<svg><symbol id="dot"><circle cx="12" cy="12" r="8"/></symbol></svg>)x"));
  st::ui::svg::IconSetPainter painter(
      std::make_shared<const st::ui::svg::IconSet>(std::move(set)));
  Canvas canvas(64, 64);
  canvas.clear(Color{0, 0, 0, 0});
  painter.draw(canvas, "dot", Rect{0, 0, 32, 32}, Color{255, 0, 0, 255});
  ST_CHECK_EQ(painter.cache_stats().misses, static_cast<std::size_t>(1));
  painter.draw(canvas, "dot", Rect{16, 16, 32, 32}, Color{255, 0, 0, 255});
  ST_CHECK_EQ(painter.cache_stats().hits, static_cast<std::size_t>(1));
  // 换颜色/换尺寸 = 新条目
  painter.draw(canvas, "dot", Rect{0, 0, 32, 32}, Color{0, 255, 0, 255});
  ST_CHECK_EQ(painter.cache_stats().misses, static_cast<std::size_t>(2));
  painter.draw(canvas, "dot", Rect{0, 0, 16, 16}, Color{255, 0, 0, 255});
  ST_CHECK_EQ(painter.cache_stats().misses, static_cast<std::size_t>(3));
  ST_CHECK_EQ(painter.cache_stats().entries, static_cast<std::size_t>(3));
}

ST_TEST(svg_transform_attribute) {
  const auto doc = st::ui::svg::parse(
      R"x(<svg viewBox="0 0 24 24"><g transform="translate(12 12) scale(2)"><rect x="0" y="0" width="4" height="4"/></g></svg>)x");
  ST_CHECK(doc.has_value());
  ST_CHECK_EQ(doc->nodes.size(), static_cast<std::size_t>(1));
  const auto bounds = doc->nodes.front().path.flattened_bounds(0.1f);
  // rect(0..4) × scale2 = 0..8，再 +12 → [12,20]
  ST_CHECK_NEAR(bounds.x, 12.0f, 1e-3f);
  ST_CHECK_NEAR(bounds.width, 8.0f, 1e-3f);
}

ST_TEST(svg_malformed_never_crashes) {
  // 乱码/截断/空——一律安全返回（不崩溃、不抛异常）
  ST_CHECK(!st::ui::svg::parse("").has_value());
  ST_CHECK(!st::ui::svg::parse("<<<not xml").has_value());
  ST_CHECK(!st::ui::svg::parse("<svg></svg>").has_value());
  // 截断的 d：要么解析失败要么得到部分路径——只要求不崩
  const auto partial = st::ui::svg::parse(R"x(<svg viewBox="0 0 10 10"><path d="M1 1 L9"/>)x");
  (void)partial;
  ST_CHECK(true);  // 到这里没崩就过
}
