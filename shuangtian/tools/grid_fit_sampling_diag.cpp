/// 网格拟合**采样网格错位**诊断（仅验证用，不进框架构建/不进 st.pkg）。
///
/// 假设（2026-10-02 由覆盖率档位直方图定位）：
/// `grid_fit` 在 **supersample 坐标空间**里吸附到 `round(v)`——supersample=2 时
/// 对齐的是"半个物理像素"的格子，于是拟合**反而制造** 50% 覆盖的糊边
/// （实测「霜」@20.25px：off 的 0.5 档位 25 个 → normal 的 0.5 档位 **96** 个）。
/// 另外 `max_stem_width`/`max_shift` 也按采样单位比较，2 倍采样下阈值等于减半。
///
/// 本工具直接对字形轮廓做拟合（绕过 TextRenderer），逐 supersample 档打印：
/// 参与拟合的笔画数、是否生效、平均位移、以及**吸附后的边缘相位**
/// （落在物理像素整数网格上的比例——这才是要最大化的量）。
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/path.hpp"
#include "st/text/font.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

using st::raster::Path;
using st::text::FontFace;
using st::text::FontStack;
using st::text::GridFitMode;

namespace {

/// 复刻 `TextRenderer::glyph_bitmap` 的坐标变换（字体单位 → 采样空间，y 向下）。
[[nodiscard]] auto build_path(const FontFace& face, st::text::GlyphId glyph, float scale)
    -> Path {
  const auto outline = face.glyph_outline(glyph);
  Path path;
  if (!outline) return path;
  const auto map_point = [scale](st::math::Point point) noexcept -> st::math::Point {
    return st::math::Point{point.x * scale, -point.y * scale};
  };
  for (const auto& command : outline->commands()) {
    switch (command.kind) {
      case st::raster::PathCommand::Kind::MoveTo: path.move_to(map_point(command.p1)); break;
      case st::raster::PathCommand::Kind::LineTo: path.line_to(map_point(command.p1)); break;
      case st::raster::PathCommand::Kind::QuadTo:
        path.quad_to(map_point(command.p1), map_point(command.p2));
        break;
      case st::raster::PathCommand::Kind::CubicTo:
        path.cubic_to(map_point(command.p1), map_point(command.p2), map_point(command.p3));
        break;
      case st::raster::PathCommand::Kind::Close: path.close(); break;
    }
  }
  return path;
}

/// 从轮廓里抽取"近垂直直线边"的 x 位置（相位统计用）。
[[nodiscard]] auto vertical_edge_phases(const Path& path) -> std::vector<float> {
  std::vector<float> phases;
  const auto points = path.raw_points();
  const auto commands = path.commands();
  std::size_t cursor = 0;
  std::vector<std::pair<std::size_t, std::size_t>> segments;
  std::vector<std::size_t> offsets(commands.size(), 0);
  for (std::size_t index = 0; index < commands.size(); ++index) {
    offsets[index] = cursor;
    switch (commands[index].kind) {
      case st::raster::PathCommand::Kind::MoveTo:
      case st::raster::PathCommand::Kind::LineTo: cursor += 1; break;
      case st::raster::PathCommand::Kind::QuadTo: cursor += 2; break;
      case st::raster::PathCommand::Kind::CubicTo: cursor += 3; break;
      case st::raster::PathCommand::Kind::Close: break;
    }
  }
  std::size_t start = 0;
  bool has = false;
  for (std::size_t index = 0; index < commands.size(); ++index) {
    const auto kind = commands[index].kind;
    if (kind == st::raster::PathCommand::Kind::MoveTo) {
      start = offsets[index];
      has = true;
      continue;
    }
    if (kind == st::raster::PathCommand::Kind::Close) continue;
    std::size_t end = offsets[index];
    if (kind == st::raster::PathCommand::Kind::QuadTo) end += 1;
    if (kind == st::raster::PathCommand::Kind::CubicTo) end += 2;
    if (has && kind == st::raster::PathCommand::Kind::LineTo) {
      const auto& a = points[start];
      const auto& b = points[end];
      if (std::abs(b.y - a.y) > 2.0f && std::abs(b.x - a.x) < 0.5f) {
        phases.push_back(a.x);
      }
    }
    start = end;
  }
  return phases;
}

[[nodiscard]] auto on_grid_ratio(const std::vector<float>& phases, float grid) -> double {
  if (phases.empty()) return 0.0;
  std::size_t on = 0;
  for (const float value : phases) {
    const float distance = std::abs(value / grid - std::round(value / grid));
    if (distance < 0.03f) ++on;
  }
  return static_cast<double>(on) / static_cast<double>(phases.size());
}

}  // namespace

auto main(int argc, char** argv) -> int {
  (void)argc;
  (void)argv;
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  const float pixel_size = 20.25f;  // 13.5 逻辑 @1.5
  const std::string sample = "霜天自绘概览组件三川目";
  for (const float supersample : {1.0f, 2.0f}) {
    st::print("\n========== supersample={} ==========\n", supersample);
    // 三种口径：现状默认（阈值按采样单位）、按物理像素换算的阈值
    struct Variant {
      const char* name;
      float grid;  ///< 1 = 旧口径（吸附采样单位）；supersample = 物理像素口径
    };
    for (const Variant& variant : {Variant{"旧口径 grid=1", 1.0f},
                                   Variant{"物理口径 grid=supersample", supersample}}) {
      std::size_t total_stems = 0;
      std::size_t applied = 0;
      double mean_shift_sum = 0.0;
      std::size_t glyphs = 0;
      std::size_t off_grid_before = 0;
      std::size_t off_grid_after = 0;
      std::size_t edges_before = 0;
      std::size_t edges_after = 0;
      for (const char32_t codepoint : st::utf8_decode(sample)) {
        const FontFace* face = fonts.find_face(codepoint);
        if (face == nullptr) continue;
        const auto glyph = face->glyph_index(codepoint);
        if (!glyph) continue;
        const float units = face->metrics().units_per_em;
        const float scale = pixel_size * supersample / units;
        const Path path = build_path(*face, *glyph, scale);
        if (path.is_empty()) continue;
        ++glyphs;
        // **复刻 `TextRenderer` 的口径**：先按未拟合包围盒定出位图原点，
        // 再平移到位图局部坐标后拟合（吸附格点锚在**位图自己的**物理像素边界上）。
        const auto bounds = path.flattened_bounds(0.2f);
        const float origin_x = std::floor(bounds.x) - 1.0f;
        const float origin_y = std::floor(bounds.y) - 1.0f;
        const Path local = path.translated(-origin_x, -origin_y);
        const auto before = vertical_edge_phases(local);
        // 目标网格：物理像素 = supersample 采样单位（位图局部坐标下与位图边界对齐）
        for (const float value : before) {
          ++edges_before;
          const float distance = std::abs(value / supersample - std::round(value / supersample));
          if (distance >= 0.03f) ++off_grid_before;
        }
        const st::text::GridFitResult result =
            st::text::grid_fit(local, {.mode = GridFitMode::Normal, .grid = variant.grid});
        total_stems += static_cast<std::size_t>(result.vertical_stems);
        if (result.applied) ++applied;
        mean_shift_sum += static_cast<double>(result.mean_shift);
        const auto after = vertical_edge_phases(result.path);
        for (const float value : after) {
          ++edges_after;
          const float distance = std::abs(value / supersample - std::round(value / supersample));
          if (distance >= 0.03f) ++off_grid_after;
        }
      }
      st::print("  [{}] 字形 {}：绘制的竖笔画 {}，生效 {}，平均位移 {:.3f}\n", variant.name, glyphs,
                total_stems, applied, glyphs > 0 ? mean_shift_sum / static_cast<double>(glyphs) : 0.0);
      st::print("     竖直线边落在**物理像素网格**上的比例：拟合前 {}/{} = {:.1f}% → 拟合后 {}/{} = {:.1f}%\n",
                edges_before - off_grid_before, edges_before,
                edges_before > 0 ? static_cast<double>(edges_before - off_grid_before) /
                                       static_cast<double>(edges_before) * 100.0
                                 : 0.0,
                edges_after - off_grid_after, edges_after,
                edges_after > 0 ? static_cast<double>(edges_after - off_grid_after) /
                                      static_cast<double>(edges_after) * 100.0
                                : 0.0);
    }
  }
  return 0;
}
