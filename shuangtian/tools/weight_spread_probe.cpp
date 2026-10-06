/// **字间一致性 / 锐度 Pareto 扫描**（仅验证用）。
///
/// 目标（用户 2026-10-04 明确：**全面优化、不要顾此失彼**）：
/// 不是让某个字更锐，也不是让整屏平均更锐，而是让**同一串文字里各字的墨量接近**
/// （字间一致），**同时不牺牲锐度**（过渡带别变宽）。
///
/// 为什么需要新口径（此前的量尺全都看不见这个问题）：
/// - 旧量尺统计**字内**离散或**整屏**平均——字内是自洽的，字间差异被平均掉；
/// - 实测（用户线索：「运行」比其他菜单项清晰）：同一菜单栏墨量极差
///   fit=off 18% → fit=normal **34%**，而逐字看是“每个字都被拟合减墨，
///   幅度从 −2% 到 −26% 不等”（两端各动 0.25px ⇒ 1.5px 笔画 ±33% 墨量）。
///
/// 本探针的回答方式：**对每个字形分别量 fit=off 与 fit=<目标> 两次**，
/// 报「同一个字的墨量变化率」的分布。
///
/// 为什么不能直接比“不同字的墨量极差”：设计上各字墨量本就差很远（一 vs 藏），
/// 实测 fit=off 时极差已达 **72.9%**——那是字形设计的差异，不是渲染缺陷。
/// 真正要问的是：**拟合对每个字的改变是否一致**？
/// 实测答案是“不一致”（−2% ~ −26%），那才是“有的字清晰、有的发灰”的来源。
///
/// 三个数一起看（缺一个就会被“顾此失彼”骗过去）：
///   `ratio_spread`  逐字变化率的极差——**字间一致性**，越小越好；
///   `band`          过渡带占比（0.5~0.9 覆盖率）——**锐度**，越小越锐；
///   `mean_ratio`    平均变化率——**整体墨量**，别为了均匀而整体变轻/变重。
///
/// 用法：`build/probe/weight_spread_probe.exe [--scale N] [--size N] [--gamma N] [--fit off|light|normal] [文本]`

#include <algorithm>
#include <cmath>
#include <numeric>
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

struct GlyphStat {
  double ink{0.0};
  int ink_pixels{0};
  int band_pixels{0};
};

auto measure(const st::text::TextRenderer::GlyphBitmap& bitmap) -> GlyphStat {
  GlyphStat stat;
  for (const float value : bitmap.coverage) {
    if (value <= 0.03f) continue;
    stat.ink += static_cast<double>(value);
    if (value < 0.95f) ++stat.ink_pixels;
    if (value >= 0.5f && value < 0.9f) ++stat.band_pixels;
  }
  return stat;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  float scale = 1.5f;
  float size = 20.25f;
  float gamma = 0.6f;
  GridFitMode fit = GridFitMode::Normal;
  std::vector<std::string> texts;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    if (arg == "--scale" && index + 1 < argc) scale = std::stof(argv[++index]);
    else if (arg == "--size" && index + 1 < argc) size = std::stof(argv[++index]);
    else if (arg == "--gamma" && index + 1 < argc) gamma = std::stof(argv[++index]);
    else if (arg.starts_with("--fit=")) {
      const std::string_view v = arg.substr(6);
      fit = v == "off" ? GridFitMode::Off : (v == "light" ? GridFitMode::Light : GridFitMode::Normal);
    } else texts.emplace_back(arg);
  }
  if (texts.empty()) {
    texts = {"文件编辑选择查看运行帮助", "概览组件数据控制通道已就绪刷新指标资源管理器打开文件设置"};
  }
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  TextRenderer renderer(*stack, static_cast<float>(std::lround(scale)));
  renderer.set_subpixel(true);
  renderer.set_grid_fit(GridFitMode::Off);
  renderer.set_coverage_gamma(gamma);

  std::vector<double> ratios;
  double band_total = 0.0;
  double band_base = 0.0;
  double mean_ratio = 0.0;
  std::size_t glyphs = 0;
  for (const std::string& text : texts) {
    for (const char32_t codepoint : st::utf8_decode(text)) {
      // 同一个字形量两次：先基准（不拟合），再目标档位。
      const auto reference = renderer.glyph_bitmap_of(codepoint, size);
      if (reference == nullptr || reference->coverage.empty()) continue;
      const double ink_reference = measure(*reference).ink;
      if (ink_reference <= 0.0) continue;
      TextRenderer fitted(*stack, static_cast<float>(std::lround(scale)));
      fitted.set_subpixel(true);
      fitted.set_grid_fit(fit);
      fitted.set_coverage_gamma(gamma);
      const auto bitmap = fitted.glyph_bitmap_of(codepoint, size);
      if (bitmap == nullptr || bitmap->coverage.empty()) continue;
      const GlyphStat stat = measure(*bitmap);
      if (stat.ink <= 0.0) continue;
      /// ⚠ 基准必须是**不拟合**（`GridFitMode::Off`）——本探针的建立前提是
      /// “拟合改变了多少”，所以基准固定为 off，与 `--fit` 的取值无关。
      ratios.push_back(stat.ink / ink_reference);
      mean_ratio += stat.ink / ink_reference;
      band_total += static_cast<double>(stat.band_pixels) / static_cast<double>(std::max(1, stat.ink_pixels));
      band_base += 1.0;
      ++glyphs;
    }
  }
  if (ratios.empty()) {
    st::print("没有可用字形\n");
    return 1;
  }
  const auto [min_it, max_it] = std::minmax_element(ratios.begin(), ratios.end());
  mean_ratio /= static_cast<double>(ratios.size());
  st::print("fit={:<6s} 字形={}  逐字变化率: min={:.3f} max={:.3f} 均值={:.3f}  "
            "**ratio_spread={:.1f}%**  过渡带={:.3f}\n",
            fit == GridFitMode::Off ? "off" : (fit == GridFitMode::Light ? "light" : "normal"),
            glyphs, *min_it, *max_it, mean_ratio,
            100.0 * (*max_it - *min_it),
            band_base > 0.0 ? band_total / band_base : 0.0);
  st::print("\n判读：**三个数一起看**——ratio_spread 小（拟合对每个字的改变一致）、\n"
            "      过渡带小（锐）、均值接近 1.0（没整体变轻）。\n"
            "      只看其中一个就会“顾此失彼”（例如为了均匀整体变糊）。\n");
  return 0;
}
