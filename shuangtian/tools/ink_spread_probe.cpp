// 逐字墨量分布诊断：把每个字形的（拟合后墨量 / 不拟合基准墨量）逐条打出来。
//
// 用途：`text_ink_compensation_evens_out_glyph_weight` 判的是**极差收窄 ≥20%**，
// 而极差由最偏离的那个字决定——不知道是「哪个字、往哪个方向偏」就无法判断
// 残差是补偿的固有边界（只提亮不压暗）还是实现退化。
//
// 用法： ink_spread_probe [--compensate on|off|both]

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace {

constexpr std::string_view kText = "文件编辑选择查看运行帮助";
constexpr int kWidth = 320;
constexpr int kHeight = 60;

}  // namespace

auto main() -> int {
  auto loaded = st::text::FontStack::system_default();
  if (!loaded || loaded->empty()) {
    st::print("字体栈不可用\n");
    return 0;
  }
  const st::text::FontStack stack = std::move(*loaded);
  const float size = 20.25f;
  const auto ink_of = [&](std::string_view single, st::text::GridFitMode fit, bool comp) -> double {
    st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    st::text::TextRenderer renderer(stack, 1.0f);
    renderer.set_subpixel(true);
    renderer.set_grid_fit(fit);
    renderer.set_ink_compensation(comp);
    renderer.set_coverage_gamma(0.6f);
    (void)renderer.draw(canvas, single, st::math::Point{2.0f, 2.0f}, size,
                        st::math::Color{0, 0, 0, 0xFF});
    double ink = 0.0;
    for (const std::uint32_t pixel : canvas.pixels()) {
      const st::math::Color color = st::math::unpremultiply(pixel);
      ink += 1.0 - static_cast<double>(color.r) / 255.0;
    }
    return ink;
  };

  std::vector<double> without;
  std::vector<double> with;
  st::print("{:<6} {:>10} {:>10} {:>10} {:>8}\n", "字符", "基准", "拟合(无补偿)", "拟合+补偿", "补偿率");
  for (const char32_t codepoint : st::utf8_decode(kText)) {
    const std::string single = st::utf8_encode(std::u32string(1, codepoint));
    const double reference = ink_of(single, st::text::GridFitMode::Off, false);
    const double fitted = ink_of(single, st::text::GridFitMode::Normal, false);
    const double compensated = ink_of(single, st::text::GridFitMode::Normal, true);
    if (reference <= 0.0) continue;
    const double r_without = fitted / reference;
    const double r_with = compensated / reference;
    without.push_back(r_without);
    with.push_back(r_with);
    st::print("{:<6} {:>10.0f} {:>10.3f} {:>10.3f} {:>8.3f}\n", single, reference, r_without, r_with,
              r_with - r_without);
  }
  const auto spread = [](const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const auto [low, high] = std::minmax_element(v.begin(), v.end());
    return *high - *low;
  };
  const auto mean_of = [](const std::vector<double>& v) {
    double sum = 0.0;
    for (const double x : v) sum += x;
    return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
  };
  st::print("\n无补偿：极差 {:.3f}（min {:.3f} max {:.3f}）均值 {:.3f}\n", spread(without),
            *std::min_element(without.begin(), without.end()),
            *std::max_element(without.begin(), without.end()), mean_of(without));
  st::print("有补偿：极差 {:.3f}（min {:.3f} max {:.3f}）均值 {:.3f}\n", spread(with),
            *std::min_element(with.begin(), with.end()),
            *std::max_element(with.begin(), with.end()), mean_of(with));
  st::print("收窄比 = {:.4f}（阈值 < 0.8）\n", spread(with) / spread(without));
  return 0;
}
