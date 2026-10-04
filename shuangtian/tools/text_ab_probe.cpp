/// 文字**逐像素 A/B 对照**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 在同一画布 DPI 口径与**同一前景色/背景色**下渲染与浏览器对照页完全相同的文本，
/// PNG 按物理像素 1:1 落盘（不缩放），供 `tools/text_ab_report.py` 与浏览器截图
/// 逐像素比较。口径：同字体、同字号、同色、白底、物理像素。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
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

constexpr int kWidth = 700;
constexpr int kHeight = 120;
constexpr Color kBackground{0xFF, 0xFF, 0xFF, 0xFF};
/// 与主题正文色 `#0F172A` 同色；对照页用同一值，否则逐像素比对不成立。
constexpr Color kForeground{0x0F, 0x17, 0x2A, 0xFF};

/// 与 `ab.html` 逐字相同的对照串（**按真实 UI 字号**：12/13.5/14 逻辑 px）。
constexpr std::string_view kLine14 = "资源管理器 打开文件 设置";
constexpr std::string_view kLine135 = "Settings Open File 24 text";
constexpr std::string_view kLine12 = "src/raster/renderer.cpp";
constexpr std::string_view kLineMono = "const auto polylines = p";

void save(const std::vector<std::uint8_t>& rgba, float scale, const std::string& path) {
  st::codec::PngImage image;
  const int width = static_cast<int>(kWidth * scale);
  const int height = static_cast<int>(kHeight * scale);
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255U);
  // 尺寸一致性必须先查：`rgba` 与 `image.rgba` 不匹配时 `std::copy` 会直接写越界
  // （实测：scale 被 int 截断成 1 而像素缓冲是 1.5 倍，越界 32 万字节，
  //   表现为进程 0xC0000374 堆损坏——量尺自身的错与渲染结果的错必须分得开）。
  if (rgba.size() != image.rgba.size()) {
    st::print("  口径不一致：像素 {} vs 目标 {}，跳过 {}\n", rgba.size(), image.rgba.size(), path);
    return;
  }
  for (std::size_t index = 3; index < image.rgba.size(); index += 4U) image.rgba[index] = 255U;
  std::copy(rgba.begin(), rgba.end(), image.rgba.begin());
  if (const auto status = st::codec::png_write_file(path, image); !status.has_value()) {
    st::print("  写 {} 失败：{}\n", path, status.error().message);
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const float scale = argc > 2 ? std::stof(argv[2]) : 1.5f;
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  st::print("device_scale={}\n", scale);

  struct Mode {
    const char* tag;
    bool lcd;
    bool filter;
    GridFitMode fit;
  };
  const Mode modes[] = {
      {"gray-nofit", false, true, GridFitMode::Off},
      {"gray-normal", false, true, GridFitMode::Normal},
      {"lcdF-nofit", true, true, GridFitMode::Off},
      {"lcdF-normal", true, true, GridFitMode::Normal},
      {"lcdR-normal", true, false, GridFitMode::Normal},
  };
  for (const Mode& mode : modes) {
    std::vector<std::uint8_t> pixels;
    {
      Canvas canvas{static_cast<int>(kWidth * scale), static_cast<int>(kHeight * scale), scale};
      canvas.clear(kBackground);
      TextRenderer renderer(fonts, scale);
      renderer.set_subpixel(mode.lcd);
      renderer.set_subpixel_filter(mode.filter);
      renderer.set_grid_fit(mode.fit);
      (void)renderer.draw(canvas, kLine14, Point{4.0f, 2.0f}, 14.0f, kForeground,
                          FontRole::Proportional);
      (void)renderer.draw(canvas, kLine135, Point{4.0f, 30.0f}, 13.5f, kForeground,
                          FontRole::Proportional);
      (void)renderer.draw(canvas, kLine12, Point{4.0f, 58.0f}, 12.0f, kForeground,
                          FontRole::Proportional);
      (void)renderer.draw(canvas, kLineMono, Point{4.0f, 86.0f}, 13.5f, kForeground,
                          FontRole::Monospace);
      pixels = canvas.to_rgba8();
    }
    save(pixels, scale, out_dir + std::string("/ab-") + mode.tag + ".png");
    st::print("  已写 ab-{}.png（{}x{} 物理像素）\n", mode.tag,
              static_cast<int>(kWidth * scale), static_cast<int>(kHeight * scale));
  }
  return 0;
}
