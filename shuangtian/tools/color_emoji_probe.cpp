// 彩色 emoji（COLR/CPAL 分层）探针：把一批 emoji 用**真实的 TextRenderer** 画到画布上，
// 导出 PNG，并给出可复算的数字判据。
//
// ## 为什么要有这个探针（而不只是单元测试）
//
// 单元测试能钉住"层数对不对、层色对不对"（属性面），但钉不住**画面**：
// emoji 分层最常见的坏法是"层顺序反了 / 某层颜色取错但仍是合法颜色 / 层被裁掉一半"——
// 属性全对而画面是错的。所以这里出图，并把"有没有彩色"翻成一个可断言的数：
// **非灰像素占比**（彩色字形的色相散布远大于单色轮廓）。
//
// ## 判据（打印在输出里，可复算）
//
// 1. `font_layers`：字体里该字形的层数（>0 说明字体确实带 COLR v0 记录）；
// 2. `drawn_layers`：`TextRenderer::color_layers_of` 真的产出位图的层数；
// 3. `chroma_ratio`：饱和度 > 0.15 的墨迹像素占墨迹像素的比例——
//    单色轮廓（黑/白/主题色）恒为 0，彩色 emoji 应显著大于 0。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"

namespace {

using st::math::Color;

/// 把画布像素（预乘）转成 PNG 需要的非预乘 RGBA8。
auto canvas_to_rgba(const st::raster::Canvas& canvas) -> std::vector<std::uint8_t> {
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  std::vector<std::uint8_t> out(static_cast<std::size_t>(width) *
                                static_cast<std::size_t>(height) * 4U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                              static_cast<std::size_t>(x)) * 4U;
      const float alpha = static_cast<float>(pixel.a) / 255.0f;
      const auto unpremultiply = [alpha](std::uint8_t value) {
        if (alpha <= 0.0f) return static_cast<std::uint8_t>(0);
        const float scaled = static_cast<float>(value) / alpha;
        return static_cast<std::uint8_t>(std::lround(std::min(scaled, 255.0f)));
      };
      out[at + 0] = unpremultiply(pixel.r);
      out[at + 1] = unpremultiply(pixel.g);
      out[at + 2] = unpremultiply(pixel.b);
      out[at + 3] = pixel.a;
    }
  }
  return out;
}

struct Stats {
  int ink{0};
  int colored{0};
  [[nodiscard]] auto chroma_ratio() const -> double {
    return ink > 0 ? static_cast<double>(colored) / static_cast<double>(ink) : 0.0;
  }
};

/// 墨迹 = 与背景色差明显；彩色 = 在墨迹里且通道极差足够大（饱和度代理）。
auto measure(const st::raster::Canvas& canvas, Color background) -> Stats {
  Stats stats{};
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const int dr = static_cast<int>(pixel.r) - static_cast<int>(background.r);
      const int dg = static_cast<int>(pixel.g) - static_cast<int>(background.g);
      const int db = static_cast<int>(pixel.b) - static_cast<int>(background.b);
      if (dr * dr + dg * dg + db * db <= 900) continue;
      ++stats.ink;
      const int high = std::max({pixel.r, pixel.g, pixel.b});
      const int low = std::min({pixel.r, pixel.g, pixel.b});
      if (high - low > 38) ++stats.colored;
    }
  }
  return stats;
}

}  // namespace

auto main() -> int {
  const auto loaded = st::text::FontStack::system_default();
  if (!loaded) {
    std::printf("字体栈加载失败\n");
    return 1;
  }
  const st::text::FontStack stack = *loaded;
  st::text::TextRenderer renderer(stack);
  // 关掉会掩盖颜色的两个映射（γ / 对比度），让判据只看"颜色本身"。
  renderer.set_subpixel(false);
  renderer.set_coverage_gamma(1.0f);
  renderer.set_coverage_contrast_strength(0.0f);

  struct Sample {
    const char* text;
    char32_t codepoint;
    bool expect_color;
  };
  const std::vector<Sample> samples{{"😀", 0x1F600, true}, {"📁", 0x1F4C1, true},
                                    {"👍", 0x1F44D, true}, {"🎉", 0x1F389, true},
                                    {"🍦", 0x1F366, true}, {"A", U'A', false}};
  // **同一个字形要跨字号段看**：emoji 在界面里主要是 13~19px（正文/按钮），
  // 而分层在极小字号上容易暴露两个问题——层与层之间漏光（边缘抗锯齿各自摊开），
  // 以及包围盒小到让层错位。所有字号共用同一个判据（彩色占比），数字可直接对比。
  const std::vector<int> sizes{13, 15, 19, 32, 64};
  const int cell = 64 * 2 + 16;
  const Color background = Color::rgb(255, 255, 255);
  const std::string out_dir = "out/color_emoji";
  (void)st::fs::create_directories(out_dir);

  std::printf("%-6s %-5s %-12s %-13s %-7s %s\n", "字符", "字号", "font_layers", "drawn_layers",
              "ink", "chroma_ratio");
  int failures = 0;
  for (const Sample& sample : samples) {
    for (const int size : sizes) {
      st::raster::Canvas canvas(cell, cell);
      canvas.clear(background);
      const auto status = renderer.draw(canvas, sample.text, st::math::Point{8.0f, 8.0f},
                                        static_cast<float>(size), Color::rgb(0, 0, 0));
      if (!status) {
        std::printf("%-6s %-5d 绘制失败\n", sample.text, size);
        ++failures;
        continue;
      }
      std::size_t font_layers = 0;
      if (const st::text::FontFace* face = stack.find_face(sample.codepoint); face != nullptr) {
        if (const auto id = face->glyph_index(sample.codepoint); id.has_value()) {
          font_layers = face->color_layers(*id).size();
        }
      }
      const auto drawn_layers =
          renderer.color_layers_of(sample.codepoint, static_cast<float>(size)).size();
      const auto stats = measure(canvas, background);
      // 墨迹量随字号降（13px 的 emoji 本来就只百来个像素），所以墨迹阈值必须按字号缩放——
      // 用固定阈值会把小字号误判成“没画出来”。
      const int ink_floor = (size * size) / 8;
      const bool ok = sample.expect_color
                          ? (stats.chroma_ratio() > 0.10 && stats.ink > ink_floor &&
                             drawn_layers > 0)
                          : (stats.chroma_ratio() < 0.02 && stats.ink > ink_floor &&
                             drawn_layers == 0);
      if (!ok) ++failures;
      std::printf("%-6s %-5d %-12zu %-13zu %-7d %.3f%s\n", sample.text, size, font_layers,
                  drawn_layers, stats.ink, stats.chroma_ratio(), ok ? "" : "  <-- FAIL");

      const auto rgba = canvas_to_rgba(canvas);
      const st::codec::PngImage image{static_cast<std::uint32_t>(cell),
                                      static_cast<std::uint32_t>(cell),
                                      st::codec::Bytes(rgba.begin(), rgba.end())};
      (void)st::codec::png_write_file(out_dir + "/u" +
                                          std::format("{:X}",
                                                      static_cast<std::uint32_t>(
                                                          sample.codepoint)) +
                                          "_" + std::to_string(size) + ".png",
                                      image);
    }
  }
  std::printf("失败项：%d（图见 %s）\n", failures, out_dir.c_str());

  // —— 变体序列与组合序列 ——
  //
  // 这两类不在“单字形分层”的范围内，但用户看到的 emoji 文本常常带它们：
  //   · VS16（U+FE0F，emoji 变体选择符）：不应占宽度（默认忽略字符）；
  //   · ZWJ / 肤色修饰 / 区域指示符：需要 GSUB 合字才能变成单个字形。
  // 这里把它们**量出来**（宽度 + 彩色像素），而不是靠“应该没事”推断。
  struct Sequence {
    const char* label;
    const char* text;
  };
  const std::vector<Sequence> sequences{
      {"heart", "\xE2\x9D\xA4"},
      {"heart+VS16", "\xE2\x9D\xA4\xEF\xB8\x8F"},
      {"smile", "\xF0\x9F\x98\x80"},
      {"smile+VS16", "\xF0\x9F\x98\x80\xEF\xB8\x8F"},
      {"zwj-family", "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"},
      {"thumbsup+skin", "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"},
      {"flag-CN", "\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3"}};
  std::printf("\n%-16s %-10s %-10s %s\n", "序列", "width@32", "width@64", "说明");
  for (const Sequence& sequence : sequences) {
    const float w32 = renderer.measure_width(sequence.text, 32.0f);
    const float w64 = renderer.measure_width(sequence.text, 64.0f);
    std::printf("%-16s %-10.2f %-10.2f\n", sequence.label, static_cast<double>(w32),
                static_cast<double>(w64));
    st::raster::Canvas canvas(cell, cell);
    canvas.clear(background);
    (void)renderer.draw(canvas, sequence.text, st::math::Point{8.0f, 8.0f}, 32.0f,
                        Color::rgb(0, 0, 0));
    const auto rgba = canvas_to_rgba(canvas);
    const st::codec::PngImage image{static_cast<std::uint32_t>(cell),
                                    static_cast<std::uint32_t>(cell),
                                    st::codec::Bytes(rgba.begin(), rgba.end())};
    (void)st::codec::png_write_file(out_dir + "/seq_" + sequence.label + ".png", image);
  }

  return failures == 0 ? 0 : 1;
}
