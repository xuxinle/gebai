/// 单行字号渲染（仅验证用）：一页一行，供「整幅总墨量」比（不依赖裁剪/对齐）。
///
/// 与 `tools/gen_realwin_page.py` 生成的 `<size>.html` 同源（一页一行）。
/// 背景/前景色可用 `--bg=RRGGBB --fg=RRGGBB` 覆盖（默认浅色主题 token）；
/// **深色主题必须传深底浅字**，否则量出来的不是用户看到的那台机器。
/// 用法：size_line_probe.exe <输出> <scale> <gamma> <字号逻辑px> [--bg=hex] [--fg=hex]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 96;
constexpr float kTop = 8.0f;
constexpr Color kBackgroundDefault{0xFF, 0xFF, 0xFF, 0xFF};
constexpr Color kForegroundDefault{0x0F, 0x17, 0x2A, 0xFF};
/// 与 `gen_size_pages.py` 的 SAMPLE 完全相同。
constexpr std::string_view kSample = "组件画廊 Overview 24";

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_path = argc > 1 ? argv[1] : "out.png";
  const float scale = argc > 2 ? std::stof(argv[2]) : 1.5f;
  const float gamma = argc > 3 ? std::stof(argv[3]) : 0.6f;
  const float size = argc > 4 ? std::stof(argv[4]) : 15.0f;
  // 背景/前景色：默认浅色主题 token（`Theme::light`）；深色要显式传深底浅字。
  Color bg = kBackgroundDefault;
  Color fg = kForegroundDefault;
  const auto parse_hex = [](std::string_view s) -> Color {
    const auto v = static_cast<std::uint32_t>(std::stoul(std::string(s), nullptr, 16));
    return Color{static_cast<std::uint8_t>((v >> 16U) & 0xFFU),
                 static_cast<std::uint8_t>((v >> 8U) & 0xFFU),
                 static_cast<std::uint8_t>(v & 0xFFU), 0xFF};
  };
  bool subpixel = true;
  for (int i = 5; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a.starts_with("--bg=")) bg = parse_hex(a.substr(5));
    else if (a.starts_with("--fg=")) fg = parse_hex(a.substr(5));
    else if (a == "--gray") subpixel = false;
  }

  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  Canvas canvas{static_cast<int>(kWidth * scale), static_cast<int>(kHeight * scale), scale};
  canvas.clear(bg);
  TextRenderer renderer(fonts, scale);
  renderer.set_subpixel(subpixel);
  renderer.set_grid_fit(GridFitMode::Light);
  renderer.set_ink_compensation(true);
  renderer.set_coverage_gamma(gamma);
  (void)renderer.draw(canvas, kSample, Point{4.0f, kTop}, size, fg,
                      FontRole::Proportional);

  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  image.rgba = canvas.to_rgba8();
  for (std::size_t i = 3; i < image.rgba.size(); i += 4U) image.rgba[i] = 255U;
  if (const auto s = st::codec::png_write_file(out_path, image); !s.has_value()) {
    st::print("写 {} 失败\n", out_path);
    return 1;
  }
  return 0;
}
