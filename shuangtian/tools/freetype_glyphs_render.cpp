/// **FreeType 侧逐字形渲染**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 用途：与 `tools/text_ab_allglyphs_page.html` **同一行表、同一字号、同一画布尺寸**
/// 渲染一份 PNG，供 `tools/freetype_hint_ceiling.py` 与浏览器截图逐带比对。
/// 输出的两份分别是 **hinting 开 / 关**——两者之差就是"复刻 FreeType 提示最多能补回多少"。
///
/// 为什么需要它：霜天在 TrueType 字体上比浏览器轻 6~9%（数字 8~10%），已归因到
/// "FreeType 执行字体自带提示指令、霜天不执行"。但做 TT 指令解释器代价很大，
/// 所以先量**上限**（这份工具就是量尺的一半）。
///
/// 编译（FreeType 是**分析用**依赖，框架本体仍零第三方依赖）：
///   tools/build_one_probe.sh freetype_glyphs_render   # 需在工具脚本里带 -lfreetype，见 README
/// 用法：freetype_glyphs_render <outdir> [--font 路径] [--face N] [--rows tools/text_ab_allglyphs_rows.json]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include "st/codec/png.hpp"
#include "st/core/print.hpp"

namespace {

/// 行表（由 `gen_text_ab.py` 生成；这里只取「文本 / 字号 / 颜色」三列）。
struct Line {
  std::string text;
  float top;
  float size;
  std::uint32_t rgb;
};

/// 从生成的 `.inc` 里抽 4 元组——**不重写一份解析器**：格式固定且由我们自己生成。
auto parse_rows(const std::string& path) -> std::vector<Line> {
  std::vector<Line> out;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return out;
  std::string content;
  {
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    content.resize(static_cast<std::size_t>(std::max(0L, size)));
    if (!content.empty()) {
      const std::size_t read = std::fread(content.data(), 1, content.size(), file);
      content.resize(read);
    }
  }
  std::fclose(file);
  // 行形如：    {"A", 4.0f, 10.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
  std::size_t pos = 0;
  const std::string key = "st::math::Color{0x";
  while ((pos = content.find(key, pos)) != std::string::npos) {
    const std::size_t line_start = content.rfind('\n', pos);
    const std::size_t brace = content.rfind('{', pos);
    const std::size_t quote0 = content.find('"', brace);
    const std::size_t quote1 = content.find('"', quote0 + 1);
    if (brace == std::string::npos || quote0 == std::string::npos ||
        quote1 == std::string::npos || line_start == std::string::npos) {
      pos += key.size();
      continue;
    }
    Line line{};
    line.text = content.substr(quote0 + 1, quote1 - quote0 - 1);
    // 两个浮点：top, size
    std::size_t p = quote1 + 1;
    const std::size_t comma1 = content.find(',', p);
    const std::size_t comma2 = content.find(',', comma1 + 1);
    line.top = std::strtof(content.substr(comma1 + 1, comma2 - comma1 - 1).c_str(), nullptr);
    line.size = std::strtof(content.substr(comma2 + 1, pos - comma2 - 1).c_str(), nullptr);
    line.rgb = static_cast<std::uint32_t>(std::strtoul(content.substr(pos + key.size(), 6).c_str(),
                                                       nullptr, 16));
    out.push_back(line);
    pos += key.size();
  }
  return out;
}

std::vector<unsigned long> decode(const std::string& text) {
  std::vector<unsigned long> out;
  for (std::size_t i = 0; i < text.size();) {
    unsigned long cp = static_cast<unsigned char>(text[i]);
    int len = 1;
    if (cp >= 0xF0U) { cp &= 0x07U; len = 4; }
    else if (cp >= 0xE0U) { cp &= 0x0FU; len = 3; }
    else if (cp >= 0xC0U) { cp &= 0x1FU; len = 2; }
    for (int k = 1; k < len; ++k) {
      cp = (cp << 6U) | (static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]) & 0x3FU);
    }
    i += static_cast<std::size_t>(len);
    out.push_back(cp);
  }
  return out;
}

/// 按霜天那套「文本框顶 + ascender」放置基线，尽量与探针同口径。
void render(FT_Library library, FT_Face face, const std::vector<Line>& rows, int width, int height, float scale,
            bool hinting, const std::string& path) {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255U);
  /// 亚像素合成：三个子像素覆盖率分别作用于 R/G/B 三个通道（ClearType 口径）。
  const auto put_subpixel = [&](st::codec::PngImage& image, int x, int y, std::uint32_t rgb,
                               float ar, float ag, float ab) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const std::size_t base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                              static_cast<std::size_t>(x)) * 4U;
    const float fr = static_cast<float>((rgb >> 16U) & 0xFFU);
    const float fg = static_cast<float>((rgb >> 8U) & 0xFFU);
    const float fb = static_cast<float>(rgb & 0xFFU);
    const float cl[3] = {std::clamp(ar, 0.0f, 1.0f), std::clamp(ag, 0.0f, 1.0f),
                         std::clamp(ab, 0.0f, 1.0f)};
    const float fgl[3] = {fr, fg, fb};
    for (int c = 0; c < 3; ++c) {
      image.rgba[base + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(
          std::lround(255.0f + (fgl[c] - 255.0f) * cl[c]));
    }
    image.rgba[base + 3U] = 255U;
  };

  const auto put = [&](int x, int y, std::uint32_t rgb, float alpha) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const std::size_t base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                              static_cast<std::size_t>(x)) * 4U;
    const float r = static_cast<float>((rgb >> 16U) & 0xFFU);
    const float g = static_cast<float>((rgb >> 8U) & 0xFFU);
    const float b = static_cast<float>(rgb & 0xFFU);
    const float a = std::clamp(alpha, 0.0f, 1.0f);
    image.rgba[base + 0U] = static_cast<std::uint8_t>(std::lround(255.0f + (r - 255.0f) * a));
    image.rgba[base + 1U] = static_cast<std::uint8_t>(std::lround(255.0f + (g - 255.0f) * a));
    image.rgba[base + 2U] = static_cast<std::uint8_t>(std::lround(255.0f + (b - 255.0f) * a));
    image.rgba[base + 3U] = 255U;
  };

  for (const Line& row : rows) {
    const FT_F26Dot6 ppem = static_cast<FT_F26Dot6>(std::lround(row.size * scale * 64.0f));
    FT_Set_Char_Size(face, 0, ppem, 0, 0);
    // **必须用 LCD（亚像素）**：浏览器与霜天都是亚像素渲染，用灰度渲染 FreeType
    // 会得到完全不同的墨量（实测：灰度下"FreeType 关提示 / 浏览器"≈1.02，
    // 而霜天/浏览器 = 0.88~0.92——模式不同，比出来的差值没有意义）。
    const FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD |
                           (hinting ? 0 : FT_LOAD_NO_HINTING);
    // 基线：与该字号下的 ascender 对齐（与霜天 `TextRenderer` 的口径一致）
    const float ascent = static_cast<float>(face->size->metrics.ascender) / 64.0f;
    float pen_x = 4.0f * scale;
    const float baseline = (row.top + ascent) * scale;
    for (unsigned long cp : decode(row.text)) {
      if (FT_Load_Char(face, cp, flags) != 0) continue;
      FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
      const FT_Bitmap& bm = face->glyph->bitmap;
      const int left = static_cast<int>(std::lround(pen_x)) + face->glyph->bitmap_left / 3;
      const int top = static_cast<int>(std::lround(baseline)) - face->glyph->bitmap_top;
      // LCD 位图：`width` 是**子像素数**（= 像素数 × 3），每像素 3 字节分别是 R/G/B 覆盖率。
      const unsigned pixels = bm.width / 3U;
      for (unsigned y = 0; y < bm.rows; ++y) {
        const unsigned char* src = bm.buffer + static_cast<std::ptrdiff_t>(y) * bm.pitch;
        for (unsigned x = 0; x < pixels; ++x) {
          put_subpixel(image, left + static_cast<int>(x), top + static_cast<int>(y), row.rgb,
                       static_cast<float>(src[x * 3U + 0U]) / 255.0f,
                       static_cast<float>(src[x * 3U + 1U]) / 255.0f,
                       static_cast<float>(src[x * 3U + 2U]) / 255.0f);
        }
      }
      pen_x += static_cast<float>(face->glyph->advance.x) / 64.0f;
    }
  }
  (void)st::codec::png_write_file(path, image);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  std::string out_dir = ".";
  std::string font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
  std::string rows_path = "tools/text_ab_allglyphs_rows.inc";
  int face_index = 0;
  int width = 1350;
  int height = 7037;
  float scale = 1.5f;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--font") font = argv[++i];
    else if (a == "--face") face_index = std::atoi(argv[++i]);
    else if (a == "--rows") rows_path = argv[++i];
    else if (a == "--width") width = std::atoi(argv[++i]);
    else if (a == "--height") height = std::atoi(argv[++i]);
    else if (a == "--scale") scale = std::strtof(argv[++i], nullptr);
    else out_dir = a;
  }
  const auto rows = parse_rows(rows_path);
  if (rows.empty()) {
    st::print("行表为空：{}\n", rows_path);
    return 1;
  }
  FT_Library library = nullptr;
  FT_Init_FreeType(&library);
  FT_Face face = nullptr;
  if (FT_New_Face(library, font.c_str(), face_index, &face) != 0) {
    st::print("打不开 {} face {}\n", font, face_index);
    return 1;
  }
  st::print("字体 {} face={} 行数 {} 画布 {}x{} scale={}\n", font, face_index, rows.size(), width,
            height, scale);
  render(library, face, rows, width, height, scale, true, out_dir + "/ft-hint-on.png");
  render(library, face, rows, width, height, scale, false, out_dir + "/ft-hint-off.png");
  st::print("已写 {}/ft-hint-on.png 与 ft-hint-off.png\n", out_dir);
  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return 0;
}
