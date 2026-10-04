/// 文字渲染**综合 A/B 探针**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 与 `tools/text_ab_page.html` **逐字、逐字号、逐颜色、逐位置**相同地渲染同一画布，
/// PNG 按**物理像素 1:1** 落盘（不缩放），供 `tools/text_ab_diff.py` 与浏览器截图比较。
///
/// 行表来自 `tools/text_ab_page_rows.inc`（由 `tools/gen_text_ab.py` 生成，与 HTML 同源）
///——**不手写两份表**：手工维护必然漂移，而漂移出来的差异会被误读成"渲染差异"。
///
/// 渲染档与**应用程序同一档**：LCD 亚像素 + 低通滤波 + light 拟合 + 墨量补偿 + 默认 gamma。
/// 关掉其中任何一项，对照出来的就不是用户看到的那台机器。
///
/// 口径：画布 = `kWidth`×`kHeight` 逻辑 px，`scale` = 截图时的 deviceScaleFactor。
#include <array>
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

constexpr Color kBackground{0xFF, 0xFF, 0xFF, 0xFF};

#include "text_ab_page_rows.inc"

void save(const std::vector<std::uint8_t>& rgba, float scale, const std::string& path) {
  st::codec::PngImage image;
  const int width = static_cast<int>(kWidth * scale);
  const int height = static_cast<int>(kHeight * scale);
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255U);
  // 尺寸一致性必须先查：不匹配时 `std::copy` 会直接写越界（实测过堆损坏）。
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
  // 覆盖率 gamma：0 = 用渲染器出厂默认（与应用程序同源，便于同台对比）。
  const float gamma = argc > 3 ? std::stof(argv[3]) : 0.0f;
  // 档位对照开关：定位“小字号偏粗”到底来自**栅格化本身**还是**网格拟合**。
  // 不提供这组开关时，任何调参都无法区分“笔画画宽了”与“拟合吸宽了”。
  std::string fit = "light";
  bool compensate = true;
  bool subpixel = true;
  // 小字号分档：单档 gamma 消不掉“小字比正文偏重”，需要单独压一档。
  float small_gamma = 0.0f;
  float small_max = 21.0f;
  for (int i = 4; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a.starts_with("--fit=")) fit = std::string(a.substr(6));
    else if (a.starts_with("--gamma-small=")) small_gamma = std::stof(std::string(a.substr(14)));
    else if (a.starts_with("--gamma-small-max=")) small_max = std::stof(std::string(a.substr(18)));
    else if (a == "--nocomp") compensate = false;
    else if (a == "--gray") subpixel = false;
  }
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  st::print("device_scale={}  画布={}x{} 逻辑  {} 行\n", scale, kWidth, kHeight, kLines.size());

  Canvas canvas{static_cast<int>(kWidth * scale), static_cast<int>(kHeight * scale), scale};
  canvas.clear(kBackground);
  TextRenderer renderer(fonts, scale);
  renderer.set_subpixel(subpixel);
  renderer.set_grid_fit(fit == "off" ? GridFitMode::Off
                        : fit == "normal" ? GridFitMode::Normal
                                           : GridFitMode::Light);
  renderer.set_ink_compensation(compensate);
  if (small_gamma > 0.0f) renderer.set_fitted_gamma(small_max, small_gamma);
  if (gamma > 0.0f) renderer.set_coverage_gamma(gamma);
  for (const Line& line : kLines) {
    (void)renderer.draw(canvas, line.text, Point{4.0f, line.top}, line.size, line.color,
                        line.role, 0.0f, line.bold);
  }
  const std::vector<std::uint8_t> pixels = canvas.to_rgba8();
  const std::string path = out_dir + "/ab-st.png";
  save(pixels, scale, path);
  st::print("  已写 {}（{}x{} 物理像素，coverage_gamma={}）\n", path,
            static_cast<int>(kWidth * scale), static_cast<int>(kHeight * scale),
            renderer.coverage_gamma());
  return 0;
}
