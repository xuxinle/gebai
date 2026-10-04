/// **界面文字三类样本探针**（仅验证用）：中文常规 / 中文粗体（合成加粗）/ 英文。
///
/// 存在理由（2026-10-04 用户反馈）：同一套参数在**中文常规体**上调好（字间均匀度追平
/// 不拟合），但用户看实际界面仍报「**中文粗体有点糊**」「**英文不够均匀和锐利**」——
/// 而这三类在渲染上确实不是一回事：
/// - 中文常规体：笔画多、结构复杂，是此前调参的目标；
/// - **中文粗体**：框架**没有独立字重的字体面**，粗体是**合成加粗**（沿水平采样格
///   平移后重复填充）——几何与常规体不同，糊的来源也不同（单向右扩 + 相邻笔画粘连）；
/// - **英文**：笔画少、字干细、大量曲线（圆/弧），与 CJK 的直线笔画结构完全不同。
///
/// 因此**必须分三类各量各的**，不能拿一类的结果代表全部（“顾此失彼”正是这么来的）。
///
/// 三个量（与 DESIGN 的二维口径同源）：
///   `spread`  逐字墨量变化率的极差（字间一致性），越小越好；
///   `band`    过渡带占比（0.5~0.9 覆盖率），越小越锐；
///   `edge/ink` 过渡像素 / 墨像素——边缘“软”的程度（糊的直接量）。
///
/// 用法：`build/probe/ui_text_probe.exe [--size N] [--scale N] [--fit off|light|normal]`

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

struct Sample {
  std::string title;
  std::string text;
  /// 合成加粗半径（物理像素）——与 app 的 `embolden_steps()` 同口径。
  float embolden_px{0.0f};
};

struct Stats {
  double ink{0.0};
  int ink_pixels{0};
  int band_pixels{0};
  int edge_pixels{0};
  /// **左/右边缘的“软硬”对比**（竖直笔画两侧的过渡量）。
  ///
  /// 存在理由：合成加粗是「沿**水平正方向**按采样格平移后重复填充」——**单向右扩**。
  /// 增量只落在右侧 ⇒ 同一根竖笔画的**右边被摊开、左边保持锐利**，字形右重、
  /// 观感发糊。本量直接测这件事：统计所有水平实心游程的**左端覆盖率**与**右端覆盖率**，
  /// 两端都该是过渡像素；若右边系统性更低（更软）就是单向右扩的实证。
  /// **彩边强度**：亚像素渲染下相邻子像素被不同强度点亮 → 边缘出现红/蓝偏色。
  /// R 与 B 通道差的最大值（0 = 完全灰度）——关掉 LCD 低通滤波的代价就长在这里，
  /// 必须与“变锐”一起看，否则又是一次“顾此失彼”。
  double fringe_max{0.0};
  double left_edge{0.0};
  double right_edge{0.0};
  int edge_runs{0};
};

auto measure(const TextRenderer& renderer, char32_t codepoint, float pixel_size, float embolden_px,
             int width, int height, bool real_bold = false,
             st::text::FontRole role = st::text::FontRole::Proportional) -> Stats {
  const int steps = renderer.embolden_steps(embolden_px);
  const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size, st::text::FontRole::Proportional,
                                               steps, real_bold);
  Stats stats;
  if (bitmap == nullptr) return stats;
  for (const float value : bitmap->coverage) {
    if (value <= 0.03f) continue;
    stats.ink += static_cast<double>(value);
    ++stats.ink_pixels;
    if (value >= 0.5f && value < 0.9f) ++stats.band_pixels;
    if (value < 0.95f) ++stats.edge_pixels;
  }
  if (width <= 0) return stats;
  // 逐行找**长度 ≥ 2 的水平实心游程**，取其左右端像素的覆盖率：
  // 若是“按原始轮廓正常渲染”，两端大致对称；若是“单向右扩”，右端系统性更软。
  const int channels = bitmap->format == st::raster::CoverageFormat::Lcd ? 3 : 1;
  if (channels == 3) {
    for (std::size_t base = 0; base + 2U < bitmap->coverage.size(); base += 3U) {
      const double fring = std::abs(static_cast<double>(bitmap->coverage[base]) -
                                    static_cast<double>(bitmap->coverage[base + 2U]));
      stats.fringe_max = std::max(stats.fringe_max, fring);
    }
  }
  const auto value_at = [&](int x, int y) {
    const std::size_t base =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(bitmap->width) +
         static_cast<std::size_t>(x)) *
        static_cast<std::size_t>(channels);
    float mean = 0.0f;
    for (int ch = 0; ch < channels; ++ch) {
      mean += bitmap->coverage[base + static_cast<std::size_t>(ch)];
    }
    return mean / static_cast<float>(channels);
  };
  for (int y = 0; y < bitmap->height; ++y) {
    int start = -1;
    for (int x = 0; x <= bitmap->width; ++x) {
      const bool solid = x < bitmap->width && value_at(x, y) > 0.75f;
      if (solid && start < 0) { start = x; continue; }
      if (solid) continue;
      if (start >= 0 && x - start >= 2) {
        // 端点的**外侧**像素才是过渡像素；取游程两端自身的覆盖率也可以，
        // 但更敏感的是紧邻外側——这里用两端自身（更稳，不受相邻笔画影响）。
        stats.left_edge += static_cast<double>(value_at(start, y));
        stats.right_edge += static_cast<double>(value_at(x - 1, y));
        ++stats.edge_runs;
      }
      start = -1;
    }
  }
  return stats;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  float size = 20.25f;
  float scale = 1.5f;
  GridFitMode fit = GridFitMode::Light;
  /// 合成加粗半径覆盖（物理像素）。默认 -1 = 用样本自带值。
  /// 用途：把 `ST_FONT_CJK` 指向**真粗体面**（如 msyhbd.ttc）时用 `--bold=0`，
  /// 这样比较的是「真粗体」而不是「常规体 + 合成加粗」。
  /// 用**等宽**角色量（代码编辑器的路径）：比例字体调好后等宽可能完全不同——
  /// 等宽字形更窄、笔画更细，且同一字串里每字宽度相同（笔画落相位更规律）。
  bool mono = false;
  float bold_override = -1.0f;
  /// 用**真粗体字体面**（而不是合成加粗）——粗体档的正解，见 `prefers_real_bold`。
  bool real_bold = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    if (arg == "--size" && index + 1 < argc) size = std::stof(argv[++index]);
    else if (arg == "--scale" && index + 1 < argc) scale = std::stof(argv[++index]);
    else if (arg == "--bold" && index + 1 < argc) bold_override = std::stof(argv[++index]);
    else if (arg == "--realbold") real_bold = true;
    else if (arg == "--mono") mono = true;
    else if (arg.starts_with("--fit=")) {
      const std::string_view v = arg.substr(6);
      fit = v == "off" ? GridFitMode::Off : (v == "light" ? GridFitMode::Light : GridFitMode::Normal);
    }
  }
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  // 与 app 一致：亚像素 + LCD 滤波 + 拟合 + 墨量补偿（gamma/Fit 档位按参数）。
  const std::vector<Sample> samples =
      mono ? std::vector<Sample>{
                 {"等宽代码", "namespace st::raster { return fill_path_aa(x); }", 0.0f},
                 {"等宽中文", "霜天光栅器扫描线覆盖率抗锯齿", 0.0f},
                 {"等宽数字", "0123456789 ff(){}[];", 0.0f},
             }
           : std::vector<Sample>{
                 {"中文常规", "概览组件数据控制通道已就绪刷新指标", 0.0f},
                 {"中文粗体", "概览组件数据控制通道已就绪刷新指标", real_bold ? 0.0f : 0.75f},
                 {"英文常规", "Renderer Pipeline Overview Settings Ggpq", 0.0f},
                 {"英文粗体", "Renderer Pipeline Overview Settings Ggpq", real_bold ? 0.0f : 0.75f},
             };
  st::print("逻辑字号={} 缩放={} fit={}\n\n", size, scale,
            fit == GridFitMode::Off ? "off" : (fit == GridFitMode::Light ? "light" : "normal"));
  st::print("{:<10s} {:>8s} {:>10s} {:>10s} {:>10s} {:>11s} {:>10s} {:>10s} {:>10s}\n", "样本",
            "字形数", "墨量均值", "过渡带占比", "过渡/墨像素", "字间离散", "左端覆盖", "右端覆盖",
            "右-左", "彩边");
  for (const Sample& sample : samples) {
    TextRenderer renderer(*stack, static_cast<int>(std::lround(scale)));
    renderer.set_subpixel(true);
    renderer.set_grid_fit(fit);
    renderer.set_ink_compensation(true);
    renderer.set_coverage_gamma(0.6f);
    // 基准 = 不拟合（同字号/同加粗）——与 DESIGN 的口径一致：比“拟合改变了多少”。
    TextRenderer reference(*stack, static_cast<int>(std::lround(scale)));
    reference.set_subpixel(true);
    reference.set_grid_fit(GridFitMode::Off);
    reference.set_coverage_gamma(0.6f);

    std::vector<double> ratios;
    double band_ratio = 0.0;
    double edge_ratio = 0.0;
    double ink_mean = 0.0;
    double fringe_max = 0.0;
    double left_edge = 0.0;
    double right_edge = 0.0;
    int edge_runs = 0;
    std::size_t count = 0;
    const float embolden_px = bold_override >= 0.0f ? bold_override : sample.embolden_px;
    for (const char32_t codepoint : st::utf8_decode(sample.text)) {
      if (codepoint == U' ') continue;
      const auto role =
          mono ? st::text::FontRole::Monospace : st::text::FontRole::Proportional;
      const Stats fitted = measure(renderer, codepoint, size * scale, embolden_px, 1, 1, real_bold, role);
      // 基准必须用**同一个字体面**（真粗体档就用真粗体）——否则 ratio 比的是两个不同设计
      // 的字体，字间离散会被字体设计差异淹没（本探针第二版就踩到：离散虚高到 21.8%/46.9%）。
      const Stats base = measure(reference, codepoint, size * scale, embolden_px, 1, 1, real_bold, role);
      if (fitted.ink_pixels == 0 || base.ink <= 0.0) continue;
      ratios.push_back(fitted.ink / base.ink);
      band_ratio += static_cast<double>(fitted.band_pixels) /
                    static_cast<double>(std::max(1, fitted.ink_pixels));
      edge_ratio += static_cast<double>(fitted.edge_pixels) /
                    static_cast<double>(std::max(1, fitted.ink_pixels));
      ink_mean += fitted.ink;
      fringe_max = std::max(fringe_max, fitted.fringe_max);
      left_edge += fitted.left_edge;
      right_edge += fitted.right_edge;
      edge_runs += fitted.edge_runs;
      ++count;
    }
    if (ratios.empty()) continue;
    const auto [low, high] = std::minmax_element(ratios.begin(), ratios.end());
    const double l_mean = edge_runs ? left_edge / static_cast<double>(edge_runs) : 0.0;
    const double r_mean = edge_runs ? right_edge / static_cast<double>(edge_runs) : 0.0;
    st::print("{:<10s} {:>8d} {:>10.1f} {:>10.3f} {:>10.3f} {:>10.1f}% {:>10.3f} {:>10.3f} "
              "{:>10.3f} {:>8.3f}\n",
              sample.title, count, ink_mean / static_cast<double>(count),
              count ? band_ratio / static_cast<double>(count) : 0.0,
              count ? edge_ratio / static_cast<double>(count) : 0.0, 100.0 * (*high - *low), l_mean,
              r_mean, r_mean - l_mean, fringe_max);
  }
  st::print("\n判读：**分三类各看各的**——中文常规 / 中文粗体 / 英文的几何不同，\n"
            "      拿一类的结果代表全部正是“顾此失彼”的来源。\n"
            "      过渡带占比高 = 边缘摊得开（糊）；字间离散大 = 有的字实、有的字灰。\n");
  return 0;
}
