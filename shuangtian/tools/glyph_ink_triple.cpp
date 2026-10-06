/// **墨量三方对账**：解析面积（真值） vs FreeType vs 霜天自研栅格器。
///
/// 为什么需要它：现有 `text_ink_conserve` 的"真值"是**霜天自己的 8× 超采样**——
/// 它只能证明"采样密度够"，**证不出覆盖率的算法对不对**（同一个错误在低/高采样下同样发生，
/// 比值照样 ≈1）。同字体轮廓已逐点一致（`glyph_outline_compare`：面积比 1.0000），
/// 所以现在要问的是：**覆盖率之和对不对得上轮廓面积**。
///
/// 判据：笔画的填充面积是几何量，可直接由轮廓算出（`glyph_outline_compare` 的鞋带公式）。
/// 单个字形在物理尺寸 S 下的理论覆盖率之和 = `面积(em²) × S²`。
/// 三方（解析 / FreeType / 霜天）在**同字号、同渲染模式**下比较，偏差即缺陷。
///
/// 编译：`tools/build_ink_triple.sh`（需 -lfreetype）
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_MODULE_H

#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace {

using st::math::Color;
using st::math::Point;

constexpr int kCurveSteps = 16;

/// 轮廓折线化（与 `glyph_outline_compare` 同口径），返回 em² 为单位的填充面积。
double outline_area_em2(FT_Face face, FT_UInt glyph, double units_per_em) {
  if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0) return 0.0;
  FT_Outline& o = face->glyph->outline;
  double signed_area2 = 0.0;
  double x0 = 0, y0 = 0;
  int start = 0;
  auto push = [&](double x, double y) {
    signed_area2 += x0 * y - x * y0;
    x0 = x;
    y0 = y;
  };
  for (int i = 0; i < o.n_points;) {
    const short tag = static_cast<short>(o.tags[i] & 0x03);
    const double x = o.points[i].x, y = o.points[i].y;
    // 追踪一条轮廓：从起点开始，按 tag 走（conic/cubic 展开成折线）。
    if (i == start) {
      x0 = x;
      y0 = y;
      ++i;
      continue;
    }
    if (tag == FT_CURVE_TAG_ON) {
      push(x, y);
      ++i;
    } else if (tag == FT_CURVE_TAG_CONIC) {
      const int next = (i + 1) % o.n_points;
      const double nx = o.points[next].x, ny = o.points[next].y;
      const bool next_on = (o.tags[next] & 0x03) == FT_CURVE_TAG_ON;
      if (next_on) {
        for (int k = 1; k <= kCurveSteps; ++k) {
          const double t = static_cast<double>(k) / kCurveSteps, u = 1.0 - t;
          push(u * u * x0 + 2 * u * t * x + t * t * nx, u * u * y0 + 2 * u * t * y + t * t * ny);
        }
        ++i;
      } else {
        // 两个连续控制点：隐式中点作为端点。
        const double mx = (x + nx) / 2.0, my = (y + ny) / 2.0;
        for (int k = 1; k <= kCurveSteps; ++k) {
          const double t = static_cast<double>(k) / kCurveSteps, u = 1.0 - t;
          push(u * u * x0 + 2 * u * t * x + t * t * mx, u * u * y0 + 2 * u * t * y + t * t * my);
        }
        i += 2;
      }
    } else {   // CUBIC
      const int n1 = (i + 1) % o.n_points, n2 = (i + 2) % o.n_points;
      const double x1 = o.points[n1].x, y1 = o.points[n1].y;
      const double x2 = o.points[n2].x, y2 = o.points[n2].y;
      for (int k = 1; k <= kCurveSteps; ++k) {
        const double t = static_cast<double>(k) / kCurveSteps, u = 1.0 - t;
        push(u * u * u * x0 + 3 * u * u * t * x + 3 * u * t * t * x1 + t * t * t * x2,
             u * u * u * y0 + 3 * u * u * t * y + 3 * u * t * t * y1 + t * t * t * y2);
      }
      i += 3;
    }
    if (i >= o.n_points) break;
    // 轮廓闭合：i 越过起点即结束（`FT_OUTLINE_DECOMPOSE` 里由 contour_end 界定；
    // 这里用简化的连续点流，面积口径与解析一致即可）。
  }
  return std::abs(signed_area2) * 0.5 / (units_per_em * units_per_em);
}



double ft_ink(FT_Library lib, FT_Face face, unsigned long cp, double ppem, bool subpixel, bool stem_darkening) {
  FT_Bool disable = stem_darkening ? 0 : 1;
  FT_Property_Set(lib, "cff", "no-stem-darkening", &disable);
  FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(std::lround(ppem * 64.0)), 0, 0);
  if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT |
                                  (subpixel ? FT_LOAD_TARGET_LCD : 0)) != 0) {
    return 0.0;
  }
  FT_Render_Glyph(face->glyph, subpixel ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL);
  const FT_Bitmap& bm = face->glyph->bitmap;
  const bool lcd = bm.pixel_mode == FT_PIXEL_MODE_LCD;
  const unsigned pixels = lcd ? bm.width / 3U : bm.width;
  double sum = 0.0;
  for (unsigned y = 0; y < bm.rows; ++y) {
    const unsigned char* row = bm.buffer + static_cast<std::ptrdiff_t>(y) * bm.pitch;
    if (lcd) {
      for (unsigned x = 0; x < pixels; ++x) sum += row[x * 3] + row[x * 3 + 1] + row[x * 3 + 2];
    } else {
      for (unsigned x = 0; x < pixels; ++x) sum += row[x];
    }
  }
  return sum / 255.0 / 3.0;
}

}  // namespace

int main(int, char**) {
  const std::string path = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  const int face_index = 2;
  const double logical = 15.0;
  const double scale = 1.5;
  const double ppem = logical * scale;   // 22.5 物理像素

  FT_Library library = nullptr;
  FT_Init_FreeType(&library);
  FT_Library lib = library;
  FT_Face ft = nullptr;
  if (FT_New_Face(lib, path.c_str(), face_index, &ft) != 0) return 1;
  auto stack = st::text::FontStack::system_default();
  if (!stack) { st::print("无可用字体\n"); return 1; }

  const unsigned long cps[] = {0x4E00UL, 0x4E2DUL, 0x56FDUL, 0x971CUL, 0x0041UL, 0x0067UL};
  st::print("字号：逻辑 {:.1f} × DPI {:.1f} = 物理 {:.1f}px\n", logical, scale, ppem);
  st::print("{:<8}{:>12}{:>14}{:>14}{:>12}{:>14}{:>14}{:>12}\n", "码点", "解析面积", "FT LCD 默认",
            "FT LCD 关加墨", "霜天 LCD", "FT 灰度默认", "FT 灰度关加墨", "霜天 灰度");

  for (unsigned long cp : cps) {
    const FT_UInt glyph = FT_Get_Char_Index(ft, cp);
    // 解析面积按**轮廓**算（字体单位 → em²），再乘物理尺寸平方得到理论覆盖率之和。
    std::string text(1, '\0');
    // 码点 → UTF-8
    std::string utf8;
    if (cp < 0x80) utf8 += static_cast<char>(cp);
    else if (cp < 0x800) { utf8 += static_cast<char>(0xC0 | (cp >> 6)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { utf8 += static_cast<char>(0xE0 | (cp >> 12)); utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }

    auto face_st = st::text::FontFace::load(path, face_index);
    double area_em2 = 0.0;
    if (face_st.has_value()) {
      if (auto outline = face_st->glyph_outline(glyph); outline.has_value()) {
        double signed_area2 = 0.0;
        double px = 0, py = 0;
        for (const st::raster::PathCommand& cmd : outline->commands()) {
          auto push = [&](double x, double y) { signed_area2 += px * y - x * py; px = x; py = y; };
          switch (cmd.kind) {
            case st::raster::PathCommand::Kind::MoveTo: px = cmd.p1.x; py = cmd.p1.y; break;
            case st::raster::PathCommand::Kind::LineTo: push(cmd.p1.x, cmd.p1.y); break;
            case st::raster::PathCommand::Kind::QuadTo:
              for (int k = 1; k <= kCurveSteps; ++k) {
                const double t = static_cast<double>(k) / kCurveSteps, u = 1.0 - t;
                push(u * u * px + 2 * u * t * cmd.p1.x + t * t * cmd.p2.x,
                     u * u * py + 2 * u * t * cmd.p1.y + t * t * cmd.p2.y);
              }
              break;
            case st::raster::PathCommand::Kind::CubicTo:
              for (int k = 1; k <= kCurveSteps; ++k) {
                const double t = static_cast<double>(k) / kCurveSteps, u = 1.0 - t;
                push(u * u * u * px + 3 * u * u * t * cmd.p1.x + 3 * u * t * t * cmd.p2.x +
                         t * t * t * cmd.p3.x,
                     u * u * u * py + 3 * u * u * t * cmd.p1.y + 3 * u * t * t * cmd.p2.y +
                         t * t * t * cmd.p3.y);
              }
              break;
            case st::raster::PathCommand::Kind::Close: push(px, py); break;
          }
        }
        area_em2 = std::abs(signed_area2) * 0.5 / (1000.0 * 1000.0);
      }
    }
    const double analytic = area_em2 * ppem * ppem;

    // 霜天：用渲染器把单字画进画布，再按覆盖率之和读回。
    auto render_st = [&](bool subpixel) {
      st::raster::Canvas canvas{400, 200, static_cast<float>(scale)};
      canvas.clear(Color{0xFF, 0xFF, 0xFF, 0xFF});
      st::text::TextRenderer renderer{*stack, static_cast<float>(scale)};
      renderer.set_subpixel(subpixel);
      renderer.set_subpixel_filter(subpixel);
      renderer.set_grid_fit(st::text::GridFitMode::Off);
      renderer.set_coverage_gamma(1.0f);
      (void)renderer.draw(canvas, utf8, Point{10.0f, 10.0f}, static_cast<float>(logical),
                          Color{0x00, 0x00, 0x00, 0xFF});
      const std::vector<std::uint8_t> px = canvas.to_rgba8();
      double ink = 0.0;
      for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        // 覆盖率 = 1 − 亮度（前景纯黑、背景纯白，逐通道平均）
        ink += (255.0 * 3.0 - px[i] - px[i + 1] - px[i + 2]) / (255.0 * 3.0);
      }
      return ink;
    };

    st::print("U+{:04X}  {:>10.1f}{:>14.1f}{:>14.1f}{:>12.1f}{:>14.1f}{:>14.1f}{:>12.1f}\n", cp,
              analytic, ft_ink(library, ft, cp, ppem, true, true), ft_ink(library, ft, cp, ppem, true, false),
              render_st(true), ft_ink(library, ft, cp, ppem, false, true),
              ft_ink(library, ft, cp, ppem, false, false), render_st(false));
  }
  FT_Done_Face(ft);
  FT_Done_FreeType(lib);
  return 0;
}
