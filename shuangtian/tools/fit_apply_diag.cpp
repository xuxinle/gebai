/// 逐字形拟合生效性诊断（仅验证用）。
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/font.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

using st::raster::Path;
using st::text::FontFace;
using st::text::FontStack;
using st::text::GridFitMode;

namespace {

[[nodiscard]] auto build_path(const FontFace& face, st::text::GlyphId glyph, float scale, float dx,
                              float dy) -> Path {
  const auto outline = face.glyph_outline(glyph);
  Path path;
  if (!outline) return path;
  const auto map_point = [scale, dx, dy](st::math::Point point) noexcept -> st::math::Point {
    return st::math::Point{point.x * scale + dx, -point.y * scale + dy};
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

}  // namespace

auto main() -> int {
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  const float pixel_size = 20.25f;
  const std::string sample = "霜天自绘概览组件三川目口国回";
  for (const float supersample : {1.0f, 2.0f}) {
    st::print("\n===== supersample={} =====\n", supersample);
    for (const char32_t codepoint : st::utf8_decode(sample)) {
      const FontFace* face = fonts.find_face(codepoint);
      if (face == nullptr) continue;
      const auto glyph = face->glyph_index(codepoint);
      if (!glyph) continue;
      const float units = face->metrics().units_per_em;
      const float scale = pixel_size * supersample / units;
      const Path path = build_path(*face, *glyph, scale, 0.0f, 0.0f);
      if (path.is_empty()) continue;
      std::string utf8;
      st::encode_utf8(codepoint, utf8);
      for (const auto [mode_name, mode] : {std::pair{"light", GridFitMode::Light},
                                           std::pair{"normal", GridFitMode::Normal}}) {
        const auto result = st::text::grid_fit(
            path, {.mode = mode, .grid = supersample});
        st::print("  {} {}: applied={} vstems={} hstems={} mean_shift={:.4f} drift={:.4f}\n", utf8,
                  mode_name, result.applied ? "Y" : "N", result.vertical_stems,
                  result.horizontal_stems, static_cast<double>(result.mean_shift),
                  static_cast<double>(result.drift));
      }
    }
  }
  return 0;
}
