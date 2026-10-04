/// **DPI 缩放路径验证**（仅验证用）：回答「先渲染再放大，还是按缩放后大小直接渲染」。
///
/// 判据（两个可测量的区别）：
/// 1. **字形物理宽 ÷ 缩放 = 常数**（= 逻辑排版宽）——说明排版在逻辑单位、栅格化在物理单位；
/// 2. **「过渡像素 / 墨像素」随缩放递减（≈ 1/s）** ⇒ 边缘过渡**恒定约一个物理像素宽**，
///    即覆盖率是在物理像素空间**算出来的**。
///    若「先按逻辑尺寸渲染、再整体放大」，则过渡带与字身**同步**放大
///    （该比例**保持不变**），且放大是插值 ⇒ 大缩放下边缘反而更糊。
///
/// 用法：`build/probe/dpi_raster_probe.exe [逻辑字号] [文本...]`

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

struct Shape {
  double ink{0.0};        ///< Σ 覆盖率
  int edge_pixels{0};     ///< 过渡像素（0.05 < α < 0.95）——边缘“软”的量
  int ink_pixels{0};      ///< 有墨像素
  int width{0};
  int height{0};
};

}  // namespace

auto main(int argc, char** argv) -> int {
  float logic_size = 20.0f;
  std::vector<std::string> texts;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    if (arg == "--size" && index + 1 < argc) logic_size = std::stof(argv[++index]);
    else texts.emplace_back(arg);
  }
  if (texts.empty()) texts = {"运行 霜天 R"};
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  st::print("逻辑字号={}  文本：{}\n\n", logic_size, texts.front());
  st::print("{:<6s} {:>8s} {:>8s} {:>10s} {:>10s} {:>12s} {:>14s}\n", "缩放", "字形宽", "字形高",
            "墨像素", "过渡像素", "过渡/墨像素", "宽÷缩放(=逻辑)");
  for (const float scale : {1.0f, 1.5f, 2.0f, 3.0f}) {
    // 画布按**物理分辨率**建（device_scale = scale），排版用逻辑字号——
    // 这正是真实路径（见 `TextRenderer::draw`：pixel_size = size × device_scale）。
    st::raster::Canvas canvas{600, 120, scale};
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    TextRenderer renderer(*stack, 1.0f);
    renderer.set_subpixel(false);   // 灰度口径：边缘过渡量的判定更干净
    renderer.set_grid_fit(GridFitMode::Light);
    renderer.set_ink_compensation(true);
    renderer.set_coverage_gamma(0.6f);
    (void)renderer.draw(canvas, texts.front(), st::math::Point{4.0f, 4.0f}, logic_size,
                        st::math::Color{0, 0, 0, 0xFF});
    Shape shape;
    int min_x = 1 << 30;
    int min_y = 1 << 30;
    int max_x = -1;
    int max_y = -1;
    // 画布是 RGBA：**要读暗度而不是 alpha**（白底的 alpha 也是 FF，读 alpha 会得到
    // “整块画布都有墨”——本探针第一版就这么错了，被 72000 这个数一眼识破）。
    const int width = static_cast<int>(canvas.physical_width());
    const int height = static_cast<int>(canvas.physical_height());
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        // `pixel_at` 已是 `Color`（内部已解预乘）——不要再套 unpremultiply（那是 uint32 重载）。
        const st::math::Color color = canvas.pixel_at(x, y);
        const float dark = 1.0f - static_cast<float>(color.r) / 255.0f;
        if (dark <= 0.05f) continue;
        shape.ink += static_cast<double>(dark);
        ++shape.ink_pixels;
        if (dark < 0.95f) ++shape.edge_pixels;
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
      }
    }
    shape.width = max_x - min_x + 1;
    shape.height = max_y - min_y + 1;
    st::print("{:<6.1f} {:>8d} {:>8d} {:>10d} {:>10d} {:>12.3f} {:>14.1f}\n", scale, shape.width,
              shape.height, shape.ink_pixels, shape.edge_pixels,
              shape.ink_pixels ? static_cast<double>(shape.edge_pixels) /
                                     static_cast<double>(shape.ink_pixels)
                               : 0.0,
              static_cast<double>(shape.width) / static_cast<double>(scale));
  }
  st::print("\n判读：**直接按物理尺寸栅格化** ⇒ 「宽÷缩放」恒定（=逻辑宽），"
            "且「过渡/墨像素」随缩放**递减（≈1/s）**。\n"
            "      **先渲染再放大** ⇒ 「过渡/墨像素」**保持不变**（边缘与字身同步被放大）。\n");
  return 0;
}
