// 对照工具（**仅测试用**，不参与框架构建）：用 FreeType 解析同一字形的轮廓，
// 与霜天自研 CFF 解释器的结果对比（轮廓数 / 点数 / 包围盒 / 面积）。
// 目的：在"某个字渲染错了"时快速判定是「cmap 映射错」还是「charstring 解释错」。
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "st/core/string.hpp"
#include "st/core/print.hpp"
#include "st/text/font.hpp"

namespace {

struct Stats {
  std::size_t contours{0};
  std::size_t points{0};
  double min_x{1e9}, min_y{1e9}, max_x{-1e9}, max_y{-1e9};
  double area{0.0};  // 有向面积的绝对值（轮廓围合面积的粗略量化）
};

Stats stats_from_ft(FT_Outline& outline) {
  Stats stats;
  stats.contours = static_cast<std::size_t>(outline.n_contours);
  stats.points = static_cast<std::size_t>(outline.n_points);
  for (int i = 0; i < outline.n_points; ++i) {
    const auto x = static_cast<double>(outline.points[i].x);
    const auto y = static_cast<double>(outline.points[i].y);
    stats.min_x = std::min(stats.min_x, x);
    stats.max_x = std::max(stats.max_x, x);
    stats.min_y = std::min(stats.min_y, y);
    stats.max_y = std::max(stats.max_y, y);
  }
  return stats;
}

Stats stats_from_path(const st::raster::Path& path) {
  Stats stats;
  st::math::Point previous{0.0f, 0.0f};
  st::math::Point start{0.0f, 0.0f};
  for (const auto& command : path.commands()) {
    switch (command.kind) {
      case st::raster::PathCommand::Kind::MoveTo:
        ++stats.contours;
        previous = command.p1;
        start = command.p1;
        break;
      case st::raster::PathCommand::Kind::LineTo:
        stats.area += static_cast<double>(previous.x) * command.p1.y - static_cast<double>(command.p1.x) * previous.y;
        previous = command.p1;
        ++stats.points;
        break;
      case st::raster::PathCommand::Kind::QuadTo:
      case st::raster::PathCommand::Kind::CubicTo: {
        const st::math::Point end = command.kind == st::raster::PathCommand::Kind::QuadTo ? command.p2 : command.p3;
        stats.area += static_cast<double>(previous.x) * end.y - static_cast<double>(end.x) * previous.y;
        previous = end;
        stats.points += command.kind == st::raster::PathCommand::Kind::QuadTo ? 2 : 3;
        break;
      }
      case st::raster::PathCommand::Kind::Close:
        stats.area += static_cast<double>(previous.x) * start.y - static_cast<double>(start.x) * previous.y;
        previous = start;
        break;
    }
    if (command.kind != st::raster::PathCommand::Kind::Close) {
      const st::math::Point p = command.kind == st::raster::PathCommand::Kind::MoveTo ? command.p1
                                : command.kind == st::raster::PathCommand::Kind::LineTo ? command.p1
                                                                                        : command.p3;
      if (command.kind != st::raster::PathCommand::Kind::MoveTo) {
        stats.min_x = std::min(stats.min_x, static_cast<double>(p.x));
        stats.max_x = std::max(stats.max_x, static_cast<double>(p.x));
        stats.min_y = std::min(stats.min_y, static_cast<double>(p.y));
        stats.max_y = std::max(stats.max_y, static_cast<double>(p.y));
      }
    }
  }
  return stats;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string path_text = argc > 1 ? argv[1] : "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  const int face_index = argc > 2 ? std::stoi(argv[2]) : 2;
  const std::string text = argc > 3 ? argv[3] : "叫响哨吗呢啊霜天中国";

  FT_Library library = nullptr;
  if (FT_Init_FreeType(&library) != 0) { st::print("FreeType 初始化失败\n"); return 1; }
  FT_Face ft_face = nullptr;
  if (FT_New_Face(library, path_text.c_str(), face_index, &ft_face) != 0) {
    st::print("FreeType 打开字体失败\n");
    return 1;
  }

  auto face = st::text::FontFace::load(path_text, face_index);
  if (!face) { st::print("霜天加载失败: {}\n", face.error()); return 1; }

  const double upem = static_cast<double>(ft_face->units_per_EM);
  st::print("upem: freetype={:.0f} 霜天={:.0f}\n", upem, static_cast<double>(face->metrics().units_per_em));
  st::print("{:<4} {:<8} {:<8} | {:<22} | {:<22} | 判定\n", "字", "FTgid", "我们gid", "FT(轮廓/点/bbox)",
              "霜天(轮廓/点/bbox)");

  const std::u32string codepoints = st::utf8_decode(text);
  int mismatches = 0;
  for (const char32_t cp : codepoints) {
    std::string one;
    st::encode_utf8(cp, one);
    const FT_UInt ft_glyph = FT_Get_Char_Index(ft_face, static_cast<FT_ULong>(cp));
    if (FT_Load_Glyph(ft_face, ft_glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0) continue;
    FT_Outline& outline = ft_face->glyph->outline;
    const Stats ft = stats_from_ft(outline);

    const auto our_glyph = face->glyph_index(cp);
    Stats ours;
    bool ok = false;
    if (our_glyph) {
      if (auto our_outline = face->glyph_outline(*our_glyph); our_outline) {
        ours = stats_from_path(*our_outline);
        ok = true;
      }
    }
    const bool same_gid = our_glyph.has_value() && *our_glyph == ft_glyph;
    const bool same_contours = ft.contours == ours.contours;
    const double dx = std::abs(ft.min_x - ours.min_x) + std::abs(ft.max_y - ours.max_y);
    const bool same_box = ok && dx < 4.0 && std::abs(ft.max_x - ours.max_x) < 4.0 &&
                          std::abs(ft.min_y - ours.min_y) < 4.0;
    const bool good = same_gid && same_contours && same_box;
    if (!good) ++mismatches;
    st::print("{:<4} %-8u %-8u | %4zu/%5zu/{:.0f},{:.0f},{:.0f},{:.0f} | %4zu/%5zu/{:.0f},{:.0f},{:.0f},{:.0f} | {}\n",
                one, ft_glyph, (our_glyph.has_value() ? *our_glyph : 0u), ft.contours, ft.points, ft.min_x, ft.min_y,
                ft.max_x, ft.max_y, ours.contours, ours.points, ours.min_x, ours.min_y, ours.max_x,
                ours.max_y, good ? "一致" : (same_gid ? "轮廓不一致" : "cmap 不一致"));
  }
  st::print("不一致字形: {} / {}\n", mismatches, codepoints.size());
  FT_Done_Face(ft_face);
  FT_Done_FreeType(library);
  return mismatches == 0 ? 0 : 1;
}
