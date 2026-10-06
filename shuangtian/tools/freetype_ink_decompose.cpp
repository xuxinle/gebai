/// **机制拆解探针**：把 FreeType 侧相对霜天多出来的墨，拆到「hinting」与「stem darkening」两项。
///
/// 背景：同字体（Noto Sans CJK SC，CFF）、同字号（22.5 物理像素）、逐字形实测——
/// 霜天（恒等 gamma）的汉字墨量只有 FreeType 默认档的 **0.885~0.96**，拉丁却是 1.09。
/// FreeType 在这一档上有两个霜天没有的机制：
///   ① **CFF hints**（`hstem`/`vstem` + hintmask：把笔画边缘吸附到像素网格、并保证
///      笔画宽度不为零）——霜天的 Type2 解释器读了 charstring 但**不执行 hint 操作符**；
///   ② **stem darkening**（CFF 默认开启的加墨，随 ppem 衰减）。
/// 本探针用 2×2 组合（hinting 开/关 × 加墨开/关）把两者分开，给出**各自贡献**，
/// 从而决定修法（实现 hint 执行 vs 做覆盖率补偿）。
///
/// 编译（FreeType 是**分析用**依赖，框架本体仍零第三方依赖）：
///   g++ -std=c++20 -O1 -Iinclude -Ithird_party -I/usr/include/freetype2 \
///       tools/freetype_ink_decompose.cpp \
///       $(ls build/dev/obj/*.o | grep -E '_shuangtian_src_(core|math|raster|text)_' | grep -v -E 'raster_platform_|_test\.') \
///       -o build/probe/freetype_ink_decompose -lfreetype -lpthread -ldl -lm
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace {



struct Row {
  unsigned long cp;
  double ft_hint_dark;
  double ft_hint_nodark;
  double ft_nohint_dark;
  double ft_nohint_nodark;
  double st_ink;
};

double ft_ink(FT_Library lib, FT_Face face, unsigned long cp, double ppem, bool hint, bool dark) {
  FT_Bool disable = dark ? 0 : 1;
  (void)FT_Property_Set(lib, "cff", "no-stem-darkening", &disable);
  (void)FT_Property_Set(lib, "autofitter", "no-stem-darkening", &disable);
  FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(std::lround(ppem * 64.0)), 0, 0);
  FT_Int32 flags = FT_LOAD_TARGET_LCD;
  if (!hint) flags |= FT_LOAD_NO_HINTING;
  if (FT_Load_Char(face, cp, flags) != 0) return 0.0;
  FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
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

/// 霜天：用渲染器把单字画进画布（恒等 gamma，无拟合/补偿），读回覆盖率之和。
double st_ink(const st::text::TextRenderer& renderer, const std::string& utf8, float logical,
              float scale) {
  st::raster::Canvas canvas{300, 160, scale};
  canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
  (void)renderer.draw(canvas, utf8, st::math::Point{10.0f, 10.0f}, logical,
                      st::math::Color{0x00, 0x00, 0x00, 0xFF});
  const std::vector<std::uint8_t> px = canvas.to_rgba8();
  double ink = 0.0;
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    ink += (255.0 * 3.0 - px[i] - px[i + 1] - px[i + 2]) / (255.0 * 3.0);
  }
  return ink;
}

std::string utf8_of(unsigned long cp) {
  std::string s;
  if (cp < 0x80) s += static_cast<char>(cp);
  else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  // 字号用物理像素：与 A/B 对照（逻辑 15 × DPI 1.5）一致。
  std::vector<double> ppems{22.5};
  for (int i = 1; i < argc; ++i) ppems.push_back(std::atof(argv[i]));
  const std::string path = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  const int face_index = 2;
  const float scale = 1.5f;

  FT_Library library = nullptr;
  FT_Init_FreeType(&library);
  FT_Face ft = nullptr;
  if (FT_New_Face(library, path.c_str(), face_index, &ft) != 0) return 1;
  auto stack = st::text::FontStack::system_default();
  if (!stack) { st::print("无可用字体\n"); return 1; }

  const unsigned long cps[] = {0x4E00UL, 0x4E2DUL, 0x56FDUL, 0x971CUL, 0x6F22UL, 0x9F98UL,
                               0x0041UL, 0x0067UL};
  for (double ppem : ppems) {
    st::print("\n物理字号 {:.1f}px（逻辑 {:.1f} × DPI {:.1f}）\n", ppem,
              ppem / static_cast<double>(scale), static_cast<double>(scale));
    st::text::TextRenderer renderer{*stack, scale};
    renderer.set_subpixel(true);
    renderer.set_subpixel_filter(true);
    renderer.set_grid_fit(st::text::GridFitMode::Off);
    renderer.set_ink_compensation(false);
    renderer.set_coverage_gamma(1.0f);

    st::print("{:<8}{:>10}{:>10}{:>10}{:>10}{:>10}{:>12}{:>12}{:>12}\n", "码点", "FT默档",
              "FT关加墨", "FT关hint", "FT都关", "霜天", "hint贡献", "加墨贡献", "霜天/FT默档");
    for (unsigned long cp : cps) {
      const std::string u = utf8_of(cp);
      const double hd = ft_ink(library, ft, cp, ppem, true, true);
      const double hn = ft_ink(library, ft, cp, ppem, true, false);
      const double nd = ft_ink(library, ft, cp, ppem, false, true);
      const double nn = ft_ink(library, ft, cp, ppem, false, false);
      const double st = st_ink(renderer, u, static_cast<float>(ppem / scale), scale);
      st::print("U+{:04X}  {:>10.1f}{:>10.1f}{:>10.1f}{:>10.1f}{:>10.1f}"
                "{:>12.3f}{:>12.3f}{:>12.3f}\n",
                cp, hd, hn, nd, nn, st, nn > 0 ? nd / nn : 0.0, hn > 0 ? hd / hn : 0.0,
                hd > 0 ? st / hd : 0.0);
    }
  }
  FT_Done_Face(ft);
  FT_Done_FreeType(library);
  return 0;
}
