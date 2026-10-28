// dw_ab_colors_probe.cpp —— 彩色字 / 彩色背景 / 深色主题 的渲染探针（仅验证用）。
//
// 为什么单独一个：主探针固定用「浅底深字 + 深底浅字」两套色调，
// 而这里要逐场景指定前景/背景，让亚像素与灰度在同一场景下逐像素可比。
//
// 场景取自 src/ui/theme.cpp 的真实 token：
//   浅底 bg #E8E8EE / text #141418 / primary #4655D8 / danger #D13B40
//   深底 bg #0A0F1A / text #F2F2F5 / primary #8B9AFF
//
// 用法：dw_ab_colors_probe <outdir> [--scale 1.0] [--lcd]
#include "text_ab_common.hpp"

#include "st/core/log.hpp"
#include <cstdio>
#include <string>

using ab::Point;
using ab::TextRenderer;

namespace {

struct Case {
  const char* name;
  unsigned bg;
  unsigned fg;
};

constexpr Case kCases[] = {
    {"light-text",    0xE8E8EE, 0x141418},
    {"light-muted",   0xE8E8EE, 0x4E4E5A},
    {"light-primary", 0xE8E8EE, 0x4655D8},
    {"light-danger",  0xE8E8EE, 0xD13B40},
    {"light-success", 0xE8E8EE, 0x1B8B55},
    {"light-onblue",  0xEDEEF9, 0x4655D8},
    {"dark-text",     0x0A0F1A, 0xF2F2F5},
    {"dark-primary",  0x0A0F1A, 0x8B9AFF},
    {"dark-danger",   0x0A0F1A, 0xFF7A7A},
    {"dark-onsoft",   0x2A2E45, 0x8B9AFF},
};

auto to_color(unsigned rgb) -> st::math::Color {
  return st::math::Color{static_cast<unsigned char>((rgb >> 16) & 0xFFU),
                         static_cast<unsigned char>((rgb >> 8) & 0xFFU),
                         static_cast<unsigned char>(rgb & 0xFFU), 255U};
}

constexpr const char* kText = "霜天渲染 中文 Abc 123";
constexpr float kSize = 15.0f;
constexpr int kRowH = 34;

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  bool lcd = false;
  float scale = 1.0f;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--lcd") lcd = true;
    else if (a.rfind("--scale=", 0) == 0) scale = std::stof(a.substr(8));
  }

  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    st::log::out("没找到可用字体\n");
    return 1;
  }

  const int n = static_cast<int>(sizeof(kCases) / sizeof(kCases[0]));
  const float logical_w = 420.0f;
  const float logical_h = static_cast<float>(n) * kRowH + 16.0f;
  const int width = static_cast<int>(logical_w * scale + 0.5f);
  const int height = static_cast<int>(logical_h * scale + 0.5f);

  st::raster::Canvas canvas{width, height, scale};
  canvas.clear(st::math::Color{255U, 255U, 255U, 255U});
  TextRenderer renderer{*stack, scale};
  renderer.set_subpixel(lcd);
  renderer.set_subpixel_filter(true);
  renderer.set_grid_fit(st::text::GridFitMode::Off);
  renderer.set_ink_compensation(false);

  for (int i = 0; i < n; ++i) {
    const auto bg = to_color(kCases[i].bg);
    const auto fg = to_color(kCases[i].fg);
    const float top = 8.0f + static_cast<float>(i) * kRowH;
    st::math::Rect band;
    band.x = 0.0f;
    band.y = top - 4.0f;
    band.width = logical_w;
    band.height = static_cast<float>(kRowH);
    canvas.fill_rect(band, st::raster::Paint::solid(bg));

    const bool is_dark = kCases[i].bg < 0x400000U;
    renderer.set_coverage_gamma(is_dark ? 0.60f : 1.10f);
    if (is_dark) {
      renderer.set_class_gamma(st::text::GlyphClass::Digit, 0.64f);
      renderer.set_class_gamma(st::text::GlyphClass::Letter, 0.62f);
      renderer.set_class_gamma(st::text::GlyphClass::Han, 0.75f);
    } else {
      renderer.set_class_gamma(st::text::GlyphClass::Digit, 0.84f);
      renderer.set_class_gamma(st::text::GlyphClass::Letter, 0.70f);
      renderer.set_class_gamma(st::text::GlyphClass::Han, 0.80f);
    }
    (void)renderer.draw(canvas, kText, Point{8.0f, top}, kSize, fg);
  }

  char path[512];
  sprintf_s(path, sizeof path, "%s/colors-%s.png", out_dir.c_str(), lcd ? "lcd" : "gray");
  ab::save(canvas.to_rgba8(), width, height, path);
  st::log::out("已写 {}（{}x{}，{}）\n", path, width, height, lcd ? "亚像素" : "灰度");
  return 0;
}
