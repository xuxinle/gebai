/// **「粗细不均」二分探针**（仅验证用）：把「笔画看起来不均匀」拆成两个**互不相干**的量，
/// 分别统计，回答「用户看到的到底是哪一个」。
///
/// 动机（2026-10-04，多次走偏后）：
/// 此前的量尺把 `weight` 定义为「Σ覆盖率 ÷ 像素跨度」＝**平均覆盖率**，
/// 于是一并吞掉了两种完全不同的现象：
///
/// ```
/// 渐变横截面 [0.3, 0.7]   → weight=0.5, peak=0.7  看着“细且灰”
/// 实心横截面 [1.0]        → weight=1.0, peak=1.0  看着“粗且黑”
/// ```
///
/// 前者是**几何**（墨摊在 2 个像素上），后者是**墨色**（有满黑像素）。
/// 用户说的「线条粗细不均匀」既可能是几何上的宽窄，也可能是墨色上的深浅，
/// **修法完全不同**（几何 → 轮廓/相位；墨色 → 覆盖率映射/加墨）。
///
/// 本探针把每条笔画横截面记成三个正交量：
///   `ink`    Σ覆盖率（真实墨量，与几何宽度和墨色都有关）
///   `peak`   峰值覆盖率（**墨色**：有没有满黑像素；1.0 = 有）
///   `span`   覆盖像素跨度（**几何**：墨摊在几个像素上）
///
/// 再按**字内**统计它们的离散（同一字形内部，所以字形之间的设计差异不计入）：
///   `ink_sd`  字内墨量标准差
///   `peak_sd` 字内峰值标准差 ← **这个才是“有的笔画黑、有的发灰”**
///   `span_sd` 字内跨度标准差 ← 这个才是“有的笔画宽、有的窄”
///
/// 用法：`build/probe/stroke_two_axis_probe.exe [scale] [pixel_size] [--fit=...] [--gamma=...] [文本...]`
///
/// 判读：哪个 `_sd` 大，用户看到的「不均匀」就主要是那一路。
///   `span_sd` 大 ⇒ 几何：笔画真的宽窄不一
///   `peak_sd` 大 ⇒ 墨色：笔画有的满黑、有的发灰
/// 若两个都小而用户仍说不均匀，说明问题不在**字内**，而在**字间**（字形之间的相位差）。

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

struct Cross {
  double ink{0.0};    ///< Σ覆盖率
  double peak{0.0};   ///< 峰值覆盖率（墨色）
  int span{0};        ///< 覆盖像素跨度（几何）
  double center{0.0}; ///< 质心（用于识别「长笔画」）
};

/// 逐行扫（得竖笔画横截面）；`vertical=false` 逐列扫（横笔画）。
///
/// 两种位图格式都要支持：
/// - `Grayscale`：1 项/像素；
/// - `Lcd`：3 项/像素。**水平方向是 3 倍分辨率**（这正是 ClearType 的机理）——
///   扫行时按子像素展开（能看出“一个子像素的覆盖率”），扫列时按像素取三通道均值
///   （横边落在三个子像素上是同一件事）。
void scan(const st::text::TextRenderer::GlyphBitmap& bitmap, bool vertical,
          std::vector<Cross>& out, std::vector<int>& lines, int line_base) {
  const bool lcd = bitmap.format == st::raster::CoverageFormat::Lcd;
  if (!lcd && bitmap.format != st::raster::CoverageFormat::Grayscale) return;
  const int width = bitmap.width;
  const int height = bitmap.height;
  const int channels = lcd ? 3 : 1;
  // 扫行时（竖笔画）亚像素提供 3 倍横向分辨率；扫列时（横笔画）按像素。
  const int inner = vertical ? width * (lcd ? 3 : 1) : height;
  const int outer = vertical ? height : width;
  const auto sample = [&](int line, int index) -> double {
    if (vertical) {
      const int x = index / channels;
      const int channel = index % channels;
      return static_cast<double>(
          bitmap.coverage[(static_cast<std::size_t>(line) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x)) *
                              static_cast<std::size_t>(channels) +
                          static_cast<std::size_t>(channel)]);
    }
    const std::size_t base =
        (static_cast<std::size_t>(index) * static_cast<std::size_t>(width) +
         static_cast<std::size_t>(line)) *
        static_cast<std::size_t>(channels);
    double sum = 0.0;
    for (int c = 0; c < channels; ++c) sum += static_cast<double>(bitmap.coverage[base + c]);
    return sum / static_cast<double>(channels);
  };
  constexpr int kMaxSpan = 12;   // 亚像素跨度是像素的 3 倍
  const int max_span = vertical && lcd ? kMaxSpan : 4;
  for (int line = 0; line < outer; ++line) {
    int start = -1;
    for (int index = 0; index <= inner; ++index) {
      const double value = index < inner ? sample(line, index) : 0.0;
      const bool inked = value > 0.03;
      if (inked && start < 0) { start = index; continue; }
      if (inked) continue;
      if (start < 0) continue;
      const int span = index - start;
      if (span > max_span) { start = -1; continue; }
      Cross cross;
      cross.span = span;
      double weighted = 0.0;
      for (int k = start; k < index; ++k) {
        const double v = sample(line, k);
        cross.ink += v;
        cross.peak = std::max(cross.peak, v);
        weighted += v * (static_cast<double>(k) + 0.5);
      }
      if (cross.ink > 0.0) cross.center = weighted / cross.ink;
      out.push_back(cross);
      lines.push_back(line_base + line);
      start = -1;
    }
  }
}

struct Stats {
  double ink_sd{0.0};
  double peak_sd{0.0};
  double span_sd{0.0};
  int samples{0};
};

auto spread(const std::vector<Cross>& values, double Cross::*field) -> double {
  if (values.size() < 2) return 0.0;
  double sum = 0.0;
  for (const Cross& cross : values) sum += cross.*field;
  const double mean = sum / static_cast<double>(values.size());
  double variance = 0.0;
  for (const Cross& cross : values) variance += (cross.*field - mean) * (cross.*field - mean);
  return std::sqrt(variance / static_cast<double>(values.size()));
}

/// 把同一字形内部的横截面分组统计（排除端点/斜笔：用「长笔画」口径，连续 ≥5 行、质心漂移 ≤1px）。
[[nodiscard]] auto within_glyph_stats(const std::vector<Cross>& all, const std::vector<int>& lines)
    -> Stats {
  Stats stats;
  std::vector<Cross> group;
  int group_end_line = -1;
  const auto flush = [&]() {
    if (group.size() >= 5) {
      stats.ink_sd += spread(group, &Cross::ink);
      stats.peak_sd += spread(group, &Cross::peak);
      // span 是整数，用 double 成员不可得——单独算一次。
      double sum = 0.0;
      for (const Cross& cross : group) sum += static_cast<double>(cross.span);
      const double mean = sum / static_cast<double>(group.size());
      double variance = 0.0;
      for (const Cross& cross : group) {
        const double d = static_cast<double>(cross.span) - mean;
        variance += d * d;
      }
      stats.span_sd += std::sqrt(variance / static_cast<double>(group.size()));
      ++stats.samples;
    }
    group.clear();
  };
  for (std::size_t index = 0; index < all.size(); ++index) {
    const bool continues =
        !group.empty() && lines[index] == group_end_line + 1 &&
        std::abs(all[index].center - group.back().center) <= 1.0;
    if (!continues) flush();
    group.push_back(all[index]);
    group_end_line = lines[index];
  }
  flush();
  return stats;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  float scale = 1.5f;
  float size = 20.25f;
  GridFitMode fit = GridFitMode::Normal;
  float gamma = 0.6f;
  // **默认与 app 一致（LCD 亚像素）**——第一版没设这个，于是量的是灰度位图，
  // 而实际渲染是亚像素（三通道）。�K 一个子像素的覆盖率与“一个物理像素的覆盖率”不是同一个量。
  bool lcd = true;
  std::vector<std::string> texts;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    if (arg.starts_with("--fit=")) {
      const std::string_view v = arg.substr(6);
      fit = v == "off" ? GridFitMode::Off : (v == "light" ? GridFitMode::Light : GridFitMode::Normal);
    } else if (arg.starts_with("--gamma=")) {
      gamma = std::stof(std::string(arg.substr(8)));
    } else if (arg == "--lcd=off") {
      lcd = false;
    } else if (arg == "--lcd=on") {
      lcd = true;
    } else if (arg == "--scale" && index + 1 < argc) {
      scale = std::stof(argv[++index]);
    } else if (arg == "--size" && index + 1 < argc) {
      size = std::stof(argv[++index]);
    } else {
      texts.emplace_back(arg);
    }
  }
  if (texts.empty()) {
    texts = {"概览组件数据控制通道已就绪刷新指标", "资源管理器打开文件设置"};
  }
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  st::print("scale={} 物理字号={} fit={} gamma={} lcd={}\n", scale, size,
            fit == GridFitMode::Off ? "off" : (fit == GridFitMode::Light ? "light" : "normal"),
            gamma, lcd ? "on" : "off");

  for (const std::string& text : texts) {
    std::vector<Cross> vertical;
    std::vector<int> v_lines;
    std::vector<Cross> horizontal;
    std::vector<int> h_lines;
    for (const char32_t codepoint : st::utf8_decode(text)) {
      TextRenderer renderer(*stack, static_cast<int>(std::lround(scale)));
      renderer.set_subpixel(lcd);
      renderer.set_grid_fit(fit);
      renderer.set_coverage_gamma(gamma);
      const auto bitmap = renderer.glyph_bitmap_of(codepoint, size);
      if (bitmap == nullptr || bitmap->coverage.empty()) continue;
      const int base = static_cast<int>(vertical.size()) * 1000;
      scan(*bitmap, true, vertical, v_lines, base);
      scan(*bitmap, false, horizontal, h_lines, base);
    }
    const Stats v = within_glyph_stats(vertical, v_lines);
    const Stats h = within_glyph_stats(horizontal, h_lines);
    const auto avg = [](double a, double b) { return (a + b) / 2.0; };
    st::print("\n「{}」每字内部离散（{} 组长笔画）：\n", text, v.samples + h.samples);
    st::print("  墨量 ink_sd  = {:.4f}   （总墨量波动）\n", avg(v.ink_sd, h.ink_sd));
    st::print("  墨色 peak_sd = {:.4f}   ← **同一字里“有的笔画黑、有的发灰”**\n",
              avg(v.peak_sd, h.peak_sd));
    st::print("  几何 span_sd = {:.4f}   ← **同一字里“有的笔画宽、有的窄”**\n",
              avg(v.span_sd, h.span_sd));
  }
  st::print("\n判读：哪个 _sd 大，用户看到的“不均匀”就主要是哪一路。\n"
            "  span_sd 大 ⇒ 几何（轮廓相位）——该动的是字形/落点；\n"
            "  peak_sd 大 ⇒ 墨色（覆盖率达不满）——该动的是覆盖率映射。\n"
            "  两者都小而观感仍不均匀 ⇒ 问题在**字间**（不同字形的相位差），不在字内。\n");
  return 0;
}
