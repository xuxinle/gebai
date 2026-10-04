/// 字号阶梯 A/B 探针（仅验证用）：**同一段字、只变字号**，用于推导「字号 → gamma」映射。
///
/// 与 `tools/text_ab_sizes.html`（由 `tools/gen_text_ab_sizes.py` 生成）同源：
/// 文本/字号/位置完全一致，PNG 按物理像素 1:1 落盘。
///
/// 渲染档与应用程序同一档（LCD + 低通 + light 拟合 + 墨量补偿），可用参数覆盖以扫参：
///   `--gamma=<v>`        全局 gamma
///   `--gamma-min-size=<物理px> --gamma-min=<v>`   小于该物理字号用另一档（近似"分档"）
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
constexpr Color kForeground{0x0F, 0x17, 0x2A, 0xFF};

#include "text_ab_sizes_rows.inc"

void save(const std::vector<std::uint8_t>& rgba, float scale, const std::string& path) {
  st::codec::PngImage image;
  const int width = static_cast<int>(kWidth * scale);
  const int height = static_cast<int>(kHeight * scale);
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255U);
  if (rgba.size() != image.rgba.size()) {
    st::print("  口径不一致：{} vs {}，跳过\n", rgba.size(), image.rgba.size());
    return;
  }
  for (std::size_t i = 3; i < image.rgba.size(); i += 4U) image.rgba[i] = 255U;
  std::copy(rgba.begin(), rgba.end(), image.rgba.begin());
  (void)st::codec::png_write_file(path, image);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const float scale = argc > 2 ? std::stof(argv[2]) : 1.5f;
  float gamma = 0.0f;
  float min_size = 0.0f;   // 物理 px：小于它用 `gamma_min`
  float gamma_min = 0.0f;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a.starts_with("--gamma=")) gamma = std::stof(std::string(a.substr(8)));
    else if (a.starts_with("--gamma-min-size=")) min_size = std::stof(std::string(a.substr(17)));
    else if (a.starts_with("--gamma-min=")) gamma_min = std::stof(std::string(a.substr(12)));
  }
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  Canvas canvas{static_cast<int>(kWidth * scale), static_cast<int>(kHeight * scale), scale};
  canvas.clear(kBackground);
  TextRenderer renderer(fonts, scale);
  renderer.set_subpixel(true);
  renderer.set_grid_fit(GridFitMode::Light);
  renderer.set_ink_compensation(true);
  if (gamma > 0.0f) renderer.set_coverage_gamma(gamma);
  // **分档按物理字号**：`set_fitted_gamma` 的阈值就是物理 px，与这里口径一致。
  if (min_size > 0.0f && gamma_min > 0.0f) renderer.set_fitted_gamma(min_size, gamma_min);
  for (const Line& line : kLines) {
    (void)renderer.draw(canvas, kSample, Point{4.0f, line.top}, line.size, kForeground,
                        FontRole::Proportional);
  }
  const std::string path = out_dir + "/sizes-st.png";
  save(canvas.to_rgba8(), scale, path);
  st::print("  已写 {}（{} 档字号，gamma={} 小字({}px 以下)={}）\n", path, kLines.size(),
            gamma > 0.0f ? gamma : renderer.coverage_gamma(), min_size,
            gamma_min > 0.0f ? gamma_min : 0.0f);
  return 0;
}
