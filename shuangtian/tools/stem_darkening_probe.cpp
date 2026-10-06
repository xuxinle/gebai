/// **对账探针**：把 FreeType（浏览器那条路）与霜天（自研栅格器）在同字号下的墨量放在一张表里。
///
/// 背景（2026-10-06 实测）：字符集全量对照——两侧**同一套字体**（逐行用 CDP 导出浏览器实际
/// 字体归属核对）、同一画布 DPI、同一前景/背景色——拉丁字母墨量比 ≈ 1.00，而汉字只有
/// **0.76**；换成灰度渲染（去掉亚像素彩边与低通滤波）差异仍在。即这不是抗锯齿模式造成的，
/// 而是**同一轮廓被栅格化成了不同的墨量**。
///
/// 本探针定位到机制：FreeType 对 **CFF（OTF）字体默认开启 stem darkening**——
/// 汉字 +26~29% 墨量，而拉丁（另在 DejaVu，TrueType）几乎不受影响。
///
/// 编译（FreeType 是**分析用**依赖，框架本体仍零第三方依赖）：
///   g++ -std=c++20 -O1 -I/usr/include/freetype2 tools/stem_darkening_probe.cpp \
///       -o build/probe/stem_darkening_probe -lfreetype
/// 用法：stem_darkening_probe [font.ttc] [--face N] [--sample 串] [ppem...]
///
/// 只跑 FreeType 侧；霜天侧由 `tools/text_ab_chars_probe` 出图、由测量脚本读像素。
/// 两侧单位一致（覆盖率之和；LCD 除以 3），所以可以直接比。
#include <cmath>
#include <cstdio>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_FONT_FORMATS_H
#include FT_MODULE_H

#include "st/core/print.hpp"

namespace {

const char* kSamples[] = {
    "中文测试字体渲染清晰组件画廊",
    "一二十丁厂七卜人入八九几儿了力乃刀又",
    "魏蜀黍龘龗爨蠹灩鬱麤齉齾",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "abcdefghijklmnopqrstuvwxyz",
};

double lcd_ink(const FT_Bitmap& bm) {
  // ⚠ FreeType 的 `LCD` 位图里 **`width` 是子像素数、不是像素数**（而 `pitch` 已是字节数）。
  // 最初按 `width` 当像素数读，每行多读 2/3 的越界字节 → 墨量虚高 3 倍
  // （实测「中」402.7 vs 真值 134.6）。量尺自身的错比渲染的错更贵：它会把你带偏方向。
  const bool lcd = bm.pixel_mode == FT_PIXEL_MODE_LCD;
  const unsigned pixels = lcd ? bm.width / 3U : bm.width;
  double sum = 0.0;
  for (unsigned y = 0; y < bm.rows; ++y) {
    const unsigned char* row = bm.buffer + static_cast<std::ptrdiff_t>(y) * bm.pitch;
    if (lcd) {
      for (unsigned x = 0; x < pixels; ++x) sum += row[x * 3] + row[x * 3 + 1] + row[x * 3 + 2];
    } else {
      for (unsigned x = 0; x < bm.width; ++x) sum += row[x];
    }
  }
  return sum / 255.0 / 3.0;
}

/// 解码 UTF-8 → 码点序列（不依赖 locale）。
std::vector<unsigned long> decode(const char* text) {
  std::vector<unsigned long> out;
  for (const char* p = text; *p != '\0';) {
    unsigned long cp = static_cast<unsigned char>(*p);
    int len = 1;
    if (cp >= 0xF0U) { cp &= 0x07U; len = 4; }
    else if (cp >= 0xE0U) { cp &= 0x0FU; len = 3; }
    else if (cp >= 0xC0U) { cp &= 0x1FU; len = 2; }
    for (int k = 1; k < len; ++k) cp = (cp << 6U) | (static_cast<unsigned char>(p[k]) & 0x3FU);
    p += len;
    out.push_back(cp);
  }
  return out;
}

struct Ink { double ink{0.0}; };

/// `ppem` 用 26.6 定点（支持 22.5 这种小数物理字号，与霜天口径一致）。
Ink render(FT_Face face, const char* text, double ppem, bool subpixel) {
  FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(std::lround(ppem * 64.0)), 0, 0);
  const FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT |
                         (subpixel ? FT_LOAD_TARGET_LCD : 0);
  Ink out;
  for (unsigned long cp : decode(text)) {
    if (FT_Load_Char(face, cp, flags) != 0) continue;
    FT_Render_Glyph(face->glyph, subpixel ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL);
    out.ink += lcd_ink(face->glyph->bitmap);
  }
  return out;
}

/// 两条路都设：CFF 引擎与自动 hinter（不同 FreeType 版本挂在不同属性上）。
void set_stem_darkening(FT_Library lib, FT_Bool disable) {
  (void)FT_Property_Set(lib, "cff", "no-stem-darkening", &disable);
  (void)FT_Property_Set(lib, "autofitter", "no-stem-darkening", &disable);
}

}  // namespace

int main(int argc, char** argv) {
  std::string path = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  long face_index = 2;
  std::vector<double> ppems;
  std::vector<std::string> samples;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--face") { face_index = std::strtol(argv[++i], nullptr, 10); }
    else if (a == "--sample") { samples.emplace_back(argv[++i]); }
    else if (a.rfind("--", 0) == 0) { continue; }
    else if (a[0] >= '0' && a[0] <= '9') { ppems.push_back(std::atof(a.c_str())); }
    else { path = a; }
  }
  if (ppems.empty()) ppems = {22.5};
  if (samples.empty()) { for (const char* s : kSamples) samples.emplace_back(s); }

  FT_Library lib = nullptr;
  if (FT_Init_FreeType(&lib) != 0) return 1;
  FT_Face face = nullptr;
  if (FT_New_Face(lib, path.c_str(), face_index, &face) != 0) {
    st::print("打不开 {} face {}\n", path, face_index);
    return 1;
  }
  st::print("字体 {} face={} 格式={}\n", path, face_index, FT_Get_Font_Format(face));
  st::print("{:<24}{:>8}{:>6}{:>12}{:>12}{:>8}{:>12}{:>12}{:>8}\n", "样本", "物理px", "字数",
            "LCDC墨量", "LCD关墨量", "比", "灰度墨量", "灰度关墨量", "比");
  for (double ppem : ppems) {
    for (const std::string& s : samples) {
      set_stem_darkening(lib, 0);
      const Ink l_on = render(face, s.c_str(), ppem, true);
      const Ink g_on = render(face, s.c_str(), ppem, false);
      set_stem_darkening(lib, 1);
      const Ink l_off = render(face, s.c_str(), ppem, true);
      const Ink g_off = render(face, s.c_str(), ppem, false);
      set_stem_darkening(lib, 0);
      st::print("{:<24}{:>8.4g}{:>6}{:>12.6g}{:>12.6g}{:>8.3}{:>12.6g}{:>12.6g}{:>8.3}\n", s, ppem,
                decode(s.c_str()).size(), l_on.ink, l_off.ink,
                l_off.ink > 0 ? l_on.ink / l_off.ink : 0.0, g_on.ink, g_off.ink,
                g_off.ink > 0 ? g_on.ink / g_off.ink : 0.0);
    }
  }
  FT_Done_Face(face);
  FT_Done_FreeType(lib);
  return 0;
}
