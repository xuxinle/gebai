/// **轮廓对照探针**：霜天自研 CFF/TTF 解释器 vs FreeType，逐字形比几何。
///
/// 背景（2026-10-06）：同字体同字号（Noto Sans CJK SC，CFF）下，霜天汉字墨量只有
/// FreeType 的 **0.60~0.67**，而拉丁（DejaVu，TrueType）是 1.04——即差异**不是**抗锯齿/拟合，
/// 而是"CFF 字形的几何或覆盖率算少了"。FreeType 的 stem darkening 只能解释 1.08，
/// 排除它以后还剩 ~35%。本探针直接比**轮廓本身**：
///   轮廓一致 ⇒ 问题在栅格化/覆盖率；轮廓不一致 ⇒ 问题在 CFF charstring 解释器。
///
/// 做法：两侧都把轮廓离散成**同一形态的折线**（曲线按同一步数采样、末尾补隐式闭合边），
/// 再算三个与实现无关的量：**填充面积（em²）/ 周长（em）/ 紧包围盒**。
///
/// ⚠ 两个已踩过的口径坑（写在这里避免重犯）：
///   1. `FT_LOAD_NO_SCALE` 下轮廓坐标**就是字体单位整数**，不是 26.6 定点——再除 64
///      会让 FreeType 侧小 64 倍（实测「一」的周长 1997 被读成 31.2）；
///   2. 轮廓的**最后一条闭合边是隐式的**（CFF charstring 没有 close 操作），
///      只累加显式命令会漏掉一条边（实测「一」少 916 单位 ≈ 少一条底边）。
///
/// 编译（FreeType 是**分析用**依赖，框架本体仍零第三方依赖）：
///   g++ -std=c++20 -O1 -Iinclude -Ithird_party -I/usr/include/freetype2 \
///       tools/glyph_outline_compare.cpp $(ls build/dev/obj/*.o | grep -E '_shuangtian_src_(core|math|raster|text)_' | grep -v -E 'raster_platform_|_test\.') \
///       -o build/probe/glyph_outline_compare -lfreetype -lpthread -ldl -lm
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "st/core/print.hpp"
#include "st/raster/path.hpp"
#include "st/text/font.hpp"

namespace {

using Point = st::math::Point;

/// 曲线离散步数：两侧必须相同（否则折线长度不可比）。
constexpr int kCurveSteps = 16;

struct Geometry {
  int contours{0};
  int points{0};
  double area{0.0};      ///< em²（鞋带公式，多子路径求和后取绝对值）
  double perimeter{0.0}; ///< em
  double x0{1e9}, y0{1e9}, x1{-1e9}, y1{-1e9};
};

struct Contour {
  std::vector<Point> points;
};

/// 统一的几何分析（两侧共用；含隐式闭合边与鞋带公式）。
Geometry analyze(const std::vector<Contour>& contours, double units_per_em) {
  Geometry g;
  g.contours = static_cast<int>(contours.size());
  double signed_area2 = 0.0;
  for (const Contour& c : contours) {
    if (c.points.size() < 2) continue;
    g.points += static_cast<int>(c.points.size());
    for (std::size_t i = 0; i < c.points.size(); ++i) {
      const Point& a = c.points[i];
      const Point& b = c.points[(i + 1) % c.points.size()];   // 隐式闭合边
      g.perimeter += std::hypot(static_cast<double>(b.x) - a.x, static_cast<double>(b.y) - a.y);
      signed_area2 += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
      g.x0 = std::min<double>(g.x0, a.x);
      g.y0 = std::min<double>(g.y0, a.y);
      g.x1 = std::max<double>(g.x1, a.x);
      g.y1 = std::max<double>(g.y1, a.y);
    }
  }
  g.area = std::abs(signed_area2) * 0.5 / (units_per_em * units_per_em);
  g.perimeter /= units_per_em;
  return g;
}

void sample_cubic(Contour& c, double x0, double y0, double x1, double y1, double x2, double y2,
                  double x3, double y3) {
  for (int i = 1; i <= kCurveSteps; ++i) {
    const double t = static_cast<double>(i) / kCurveSteps;
    const double u = 1.0 - t;
    c.points.push_back(Point{static_cast<float>(u * u * u * x0 + 3 * u * u * t * x1 +
                                               3 * u * t * t * x2 + t * t * t * x3),
                             static_cast<float>(u * u * u * y0 + 3 * u * u * t * y1 +
                                                3 * u * t * t * y2 + t * t * t * y3)});
  }
}

void sample_quad(Contour& c, double x0, double y0, double x1, double y1, double x2, double y2) {
  for (int i = 1; i <= kCurveSteps; ++i) {
    const double t = static_cast<double>(i) / kCurveSteps;
    const double u = 1.0 - t;
    c.points.push_back(Point{static_cast<float>(u * u * x0 + 2 * u * t * x1 + t * t * x2),
                             static_cast<float>(u * u * y0 + 2 * u * t * y1 + t * t * y2)});
  }
}

/// 霜天侧：`raster::Path` 命令流 → 折线（字体单位）。
std::vector<Contour> contours_st(const st::raster::Path& path) {
  std::vector<Contour> out;
  for (const st::raster::PathCommand& cmd : path.commands()) {
    switch (cmd.kind) {
      case st::raster::PathCommand::Kind::MoveTo:
        out.push_back(Contour{});
        out.back().points.push_back(cmd.p1);
        break;
      case st::raster::PathCommand::Kind::LineTo:
        if (out.empty()) out.push_back(Contour{});
        out.back().points.push_back(cmd.p1);
        break;
      case st::raster::PathCommand::Kind::QuadTo:
        if (out.empty()) out.push_back(Contour{});
        sample_quad(out.back(), out.back().points.back().x, out.back().points.back().y, cmd.p1.x,
                    cmd.p1.y, cmd.p2.x, cmd.p2.y);
        break;
      case st::raster::PathCommand::Kind::CubicTo:
        if (out.empty()) out.push_back(Contour{});
        sample_cubic(out.back(), out.back().points.back().x, out.back().points.back().y, cmd.p1.x,
                     cmd.p1.y, cmd.p2.x, cmd.p2.y, cmd.p3.x, cmd.p3.y);
        break;
      case st::raster::PathCommand::Kind::Close:
        break;   // 隐式闭合由 analyze 处理
    }
  }
  return out;
}

struct CallbackState {
  std::vector<Contour>* out;
};

int move_to(const FT_Vector* to, void* user) {
  auto* st = static_cast<CallbackState*>(user);
  st->out->push_back(Contour{});
  st->out->back().points.push_back(Point{static_cast<float>(to->x), static_cast<float>(to->y)});
  return 0;
}

int line_to(const FT_Vector* to, void* user) {
  auto* st = static_cast<CallbackState*>(user);
  st->out->back().points.push_back(Point{static_cast<float>(to->x), static_cast<float>(to->y)});
  return 0;
}

int conic_to(const FT_Vector* control, const FT_Vector* to, void* user) {
  auto* st = static_cast<CallbackState*>(user);
  Contour& c = st->out->back();
  const Point last = c.points.back();
  sample_quad(c, last.x, last.y, static_cast<double>(control->x), static_cast<double>(control->y),
              static_cast<double>(to->x), static_cast<double>(to->y));
  return 0;
}

int cubic_to(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
  auto* st = static_cast<CallbackState*>(user);
  Contour& c = st->out->back();
  const Point last = c.points.back();
  sample_cubic(c, last.x, last.y, static_cast<double>(c1->x), static_cast<double>(c1->y),
               static_cast<double>(c2->x), static_cast<double>(c2->y),
               static_cast<double>(to->x), static_cast<double>(to->y));
  return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string path = argc > 1 ? argv[1]
                                    : "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  const int face_index = argc > 2 ? std::atoi(argv[2]) : 2;
  const unsigned long codepoints[] = {0x4E2DUL, 0x56FDUL, 0x4E00UL, 0x971CUL, 0x9F98UL,
                                      0x8C61UL, 0x0041UL, 0x0067UL};

  auto face = st::text::FontFace::load(path, face_index);
  if (!face.has_value()) {
    st::print("霜天侧加载失败：{}\n", face.error().message);
    return 1;
  }
  FT_Library lib = nullptr;
  FT_Init_FreeType(&lib);
  FT_Face ft = nullptr;
  if (FT_New_Face(lib, path.c_str(), face_index, &ft) != 0) return 1;
  // `NO_SCALE`：两侧都拿**字体单位**的原始轮廓（比的是解释器，不是 hinting/缩放）。
  const double upem = static_cast<double>(face->metrics().units_per_em);

  st::print("字体 {} face={}  units_per_em={:.0f}\n", path, face_index, upem);
  st::print("{:<8}{:>12}{:>22}{:>20}{:>10}{:>18}{:>18}\n", "码点", "子路径 霜/FT", "面积 霜/FT(em2)",
            "周长 霜/FT(em)", "面积比", "宽 霜/FT", "高 霜/FT");
  for (unsigned long cp : codepoints) {
    const auto gid = face->glyph_index(static_cast<char32_t>(cp));
    if (!gid.has_value()) continue;
    const auto outline = face->glyph_outline(*gid);
    if (!outline.has_value()) {
      st::print("U+{:04X} 霜天取轮廓失败：{}\n", cp, outline.error().message);
      continue;
    }
    const Geometry a = analyze(contours_st(*outline), upem);
    if (FT_Load_Glyph(ft, static_cast<FT_UInt>(*gid), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0) {
      continue;
    }
    std::vector<Contour> ft_contours;
    CallbackState state{&ft_contours};
    FT_Outline_Funcs funcs{};
    funcs.move_to = move_to;
    funcs.line_to = line_to;
    funcs.conic_to = conic_to;
    funcs.cubic_to = cubic_to;
    FT_Outline_Decompose(&ft->glyph->outline, &funcs, &state);
    const Geometry b = analyze(ft_contours, upem);
    st::print("U+{:04X}  {:>5}/{:<5}{:>10.4f}/{:<10.4f}{:>9.3f}/{:<9.3f}{:>10.4f}"
              "{:>9.4f}/{:<9.4f}{:>9.4f}/{:<9.4f}\n",
              cp, a.contours, b.contours, a.area, b.area, a.perimeter, b.perimeter,
              b.area > 0 ? a.area / b.area : 0.0, a.x1 - a.x0, b.x1 - b.x0, a.y1 - a.y0,
              b.y1 - b.y0);
  }
  FT_Done_Face(ft);
  FT_Done_FreeType(lib);
  return 0;
}
