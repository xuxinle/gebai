/// **逐字形渲染诊断**（仅验证用）：对一串文字**逐字**报「本次是否被拟合」、
/// 「抽取漏斗」、「墨量峰值」、「暗像素占比」——用来回答「为什么同一个菜单栏里
/// 有的字清晰、有的字发灰」这类**字间不一致**问题。
///
/// 动机（2026-10-04 用户实测观察）：同一菜单栏「文件 编辑 选择 查看 运行 帮助」，
/// 「运行」明显比其他字黑（平均暗度 0.683 vs 文件 0.511，**深 34%**）——
/// 同一字号、同一字重、同一背景，墨量本应接近。这说明渲染在**字间不稳定**，
/// 而此前所有量尺都只统计**字内**（或整屏），看不到字间差异。
///
/// 用法：`build/probe/glyph_weight_probe.exe [scale] [pixel_size] [文本...]`
/// 不传文本时用那六个菜单项。

#include <algorithm>
#include <array>
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

struct Weight {
  double ink{0.0};        ///< Σ覆盖率（真实墨量）
  double peak{0.0};       ///< 峰值覆盖率（有没有满黑像素）
  double dark_ratio{0.0}; ///< 覆盖率 > 0.75 的像素占比（“看起来实”的比例）
  int pixels{0};          ///< 有墨的像素数
};

}  // namespace

auto main(int argc, char** argv) -> int {
  float scale = 1.5f;
  float size = 20.25f;
  GridFitMode fit = GridFitMode::Normal;
  float gamma = 0.6f;
  std::vector<std::string> texts;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    if (arg.starts_with("--fit=")) {
      const std::string_view v = arg.substr(6);
      fit = v == "off" ? GridFitMode::Off : (v == "light" ? GridFitMode::Light : GridFitMode::Normal);
    } else if (arg.starts_with("--gamma=")) {
      gamma = std::stof(std::string(arg.substr(8)));
    } else if (arg == "--scale" && index + 1 < argc) {
      scale = std::stof(argv[++index]);
    } else if (arg == "--size" && index + 1 < argc) {
      size = std::stof(argv[++index]);
    } else {
      texts.emplace_back(arg);
    }
  }
  if (texts.empty()) {
    texts = {"文件编辑选择查看运行帮助"};
  }
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  st::print("scale={} 物理字号={} fit={} gamma={}\n", scale, size,
            fit == GridFitMode::Off ? "off" : (fit == GridFitMode::Light ? "light" : "normal"),
            gamma);
  st::print("\n{:<6s} {:>5s} {:>6s} {:>6s} {:>8s} {:>7s} {:>8s} {:>8s}\n", "字形", "拟合", "找到",
            "被拒", "候选边", "墨量", "峰值", "暗占比");
  for (const std::string& text : texts) {
    for (const char32_t codepoint : st::utf8_decode(text)) {
      TextRenderer renderer(*stack, static_cast<int>(std::lround(scale)));
      renderer.set_subpixel(true);
      renderer.set_grid_fit(fit);
      renderer.set_coverage_gamma(gamma);
      const auto bitmap = renderer.glyph_bitmap_of(codepoint, size);
      if (bitmap == nullptr || bitmap->coverage.empty()) continue;
      Weight weight;
      for (const float value : bitmap->coverage) {
        if (value <= 0.03f) continue;
        ++weight.pixels;
        weight.ink += static_cast<double>(value);
        weight.peak = std::max(weight.peak, static_cast<double>(value));
        if (value > 0.75f) weight.dark_ratio += 1.0;
      }
      weight.dark_ratio = weight.pixels ? weight.dark_ratio / weight.pixels : 0.0;
      // 用 UTF-8 打印单字（控制台按代码页可能显示为乱码——数值才是判据）。
      std::string label;
      const char32_t single = codepoint;
      label += static_cast<char>(' ');
      (void)single;
      st::print("U+{:04X} {:>6s} {:>6d} {:>6d} {:>8d} {:>7.1f} {:>8.3f} {:>7.1f}%\n",
                static_cast<unsigned>(codepoint), bitmap->fit_applied ? "是" : "否",
                bitmap->fit_stems, bitmap->fit_rejected_stems, bitmap->fit_funnel.straight,
                weight.ink, weight.peak, weight.dark_ratio * 100.0);
      (void)label;
    }
  }
  st::print("\n判读：同一字号/字重下**墨量应当接近**。哪个字被拟合（“拟合=是”）且\n"
            "墨量峰值高（接近 1.0），它就是“清晰的那个”——其余字是没被处理的那批。\n"
            "若同一串里拟合与否混杂，就是“字间不一致”的直接证据。\n");
  return 0;
}
