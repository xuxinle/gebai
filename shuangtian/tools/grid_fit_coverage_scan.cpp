/// **网格拟合覆盖率门槛扫描**（仅验证用）：把 `min_stem_coverage` 逐档扫过，
/// 报「应用率」与「双峰度」的取舍曲线。
///
/// 动机（2026-10-04）：「中文线条粗细不均」的直接形态是**双峰**——同一字里一部分笔画
/// 被吸成满黑、另一部分留在原相位（摊成灰边）。根因是**笔画抽取召回有限**
/// （漏斗实测每字形 43 条候选只成 8 对），“吸一半”比“都不吸”更刺眼。
/// 于是加了护栏③：覆盖率不足就整个字形放弃拟合（`applied=false`）。
///
/// 本量尺回答「门槛取多少」：**应用率**（还有多少字形真被拟合 = 锐度收益）
/// 对**双峰率**（同组笔画里实心与发灰共存 = 不均匀）。
/// 这是一次明确的取舍，不是“越多越好”——两个数都要看。
///
/// 运行：`build/probe/grid_fit_coverage_scan.exe [scale] [pixel_size] [文本...]`

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

/// 一条笔画横截面（与 `stroke_two_axis_probe` 同口径：几何跨度 + 墨色峰值）。
struct Cross {
  double ink{0.0};
  double peak{0.0};
  int span{0};
  double center{0.0};
};

void scan(const st::text::TextRenderer::GlyphBitmap& bitmap, bool vertical,
          std::vector<Cross>& out, std::vector<int>& lines, int line_base) {
  const bool lcd = bitmap.format == st::raster::CoverageFormat::Lcd;
  if (!lcd && bitmap.format != st::raster::CoverageFormat::Grayscale) return;
  const int width = bitmap.width;
  const int height = bitmap.height;
  const int channels = lcd ? 3 : 1;
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
  const int max_span = (vertical && lcd) ? 12 : 4;
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

}  // namespace

auto main(int argc, char** argv) -> int {
  float scale = 1.5f;
  float size = 20.25f;
  std::vector<std::string> texts;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    if (arg == "--scale" && index + 1 < argc) scale = std::stof(argv[++index]);
    else if (arg == "--size" && index + 1 < argc) size = std::stof(argv[++index]);
    else texts.emplace_back(arg);
  }
  if (texts.empty()) {
    texts = {"概览组件数据控制通道已就绪刷新指标资源管理器打开文件设置",
             "霜天自绘渲染无头可控"};
  }
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  st::print("scale={} 物理字号={}  覆盖率门槛扫描（汉字，γ=0.6，LCD）\n", scale, size);
  st::print("\n{:<10s} {:>8s} {:>12s} {:>10s}\n", "门槛", "应用率", "双峰笔划占比", "墨色 sd");
  for (const float coverage : {0.0f, 0.5f, 0.75f, 0.9f, 1.01f}) {
    std::size_t glyphs = 0;
    std::size_t applied = 0;
    int groups = 0;
    int bimodal = 0;
    int scan_total = 0;
    int glyph_index = 0;
    double peak_sd_sum = 0.0;
    int sd_groups = 0;
    for (const std::string& text : texts) {
      for (const char32_t codepoint : st::utf8_decode(text)) {
        ++glyph_index;
        TextRenderer renderer(*stack, static_cast<int>(std::lround(scale)));
        renderer.set_subpixel(true);
        renderer.set_coverage_gamma(0.6f);
        // 按档位设置覆盖率门槛（通过 TextRenderer 的网格拟合选项注入）。
        renderer.set_grid_fit(GridFitMode::Normal);
        renderer.set_min_stem_coverage(coverage);
        const auto bitmap = renderer.glyph_bitmap_of(codepoint, size);
        if (bitmap == nullptr || bitmap->coverage.empty()) continue;
        ++glyphs;
        if (bitmap->fit_applied) ++applied;
        std::vector<Cross> vertical;
        std::vector<int> v_lines;
        scan(*bitmap, true, vertical, v_lines, static_cast<int>(glyph_index) * 1000);
        scan_total += static_cast<int>(vertical.size());
        // **按位置跟踪聚合成长笔画**（不能按扫描顺序比较：LCD 下同一行有多条游程，
        // 行边界必然打断链——这会让“组数”永远只有 1~2，量出来的占比没有意义）。
        // 口径：同一笔画 = 跨相邻行且质心漂移 ≤1px。
        struct Track {
          int last_line{-1};
          double last_center{0.0};
          std::vector<Cross> members;
        };
        std::vector<Track> tracks;
        for (std::size_t k = 0; k < vertical.size(); ++k) {
          Track* best = nullptr;
          for (Track& track : tracks) {
            if (track.last_line != v_lines[k] - 1) continue;
            if (std::abs(track.last_center - vertical[k].center) > 1.0) continue;
            if (best == nullptr || std::abs(track.last_center - vertical[k].center) <
                                       std::abs(best->last_center - vertical[k].center)) {
              best = &track;
            }
          }
          if (best == nullptr) {
            tracks.push_back(Track{v_lines[k], vertical[k].center, {vertical[k]}});
            continue;
          }
          best->last_line = v_lines[k];
          best->last_center = vertical[k].center;
          best->members.push_back(vertical[k]);
        }
        for (const Track& track : tracks) {
          if (track.members.size() < 5U) continue;
          ++groups;
          int solid = 0;
          int soft = 0;
          double sum = 0.0;
          for (const Cross& cross : track.members) {
            if (cross.peak >= 0.9) ++solid;
            else if (cross.peak <= 0.7) ++soft;
            sum += cross.peak;
          }
          if (solid > 0 && soft > 0) ++bimodal;
          const double mean = sum / static_cast<double>(track.members.size());
          double variance = 0.0;
          for (const Cross& cross : track.members) {
            const double d = cross.peak - mean;
            variance += d * d;
          }
          peak_sd_sum += std::sqrt(variance / static_cast<double>(track.members.size()));
          ++sd_groups;
        }
        // 分组（连续行、质心漂移 ≤1px、≥5 行）——与 two_axis 探针同口径。
        std::size_t index = 0;
        while (index < vertical.size()) {
          std::size_t end = index;
          while (end + 1U < vertical.size() && v_lines[end + 1U] == v_lines[end] + 1 &&
                 std::abs(vertical[end + 1U].center - vertical[end].center) <= 1.0) {
            ++end;
          }
          if (end - index + 1U >= 5U) {
            ++groups;
            int solid = 0;
            int soft = 0;
            double sum = 0.0;
            for (std::size_t k = index; k <= end; ++k) {
              if (vertical[k].peak >= 0.9) ++solid;
              else if (vertical[k].peak <= 0.7) ++soft;
              sum += vertical[k].peak;
            }
            if (solid > 0 && soft > 0) ++bimodal;
            const double mean = sum / static_cast<double>(end - index + 1U);
            double variance = 0.0;
            for (std::size_t k = index; k <= end; ++k) {
              const double d = vertical[k].peak - mean;
              variance += d * d;
            }
            peak_sd_sum += std::sqrt(variance / static_cast<double>(end - index + 1U));
            ++sd_groups;
          }
          index = end + 1U;
        }
      }
    }
    st::print("{:<10.2f} {:>7.1f}% {:>9.1f}% {:>10.4f}   [字形 {} / 横截面 {} / 组 {}]\n", coverage,
              glyphs ? 100.0 * static_cast<double>(applied) / static_cast<double>(glyphs) : 0.0,
              groups ? 100.0 * static_cast<double>(bimodal) / static_cast<double>(groups) : 0.0,
              sd_groups ? peak_sd_sum / static_cast<double>(sd_groups) : 0.0, glyphs, scan_total,
              groups);
  }
  st::print("\n判读：**两个数一起看**——应用率（还有多少字形真被拟合 = 锐度收益）\n"
            "      对双峰组占比（同组内实心与发灰共存 = 观感上的“粗细不均”）。\n"
            "      门槛抬高会让更多字形整体放弃：这是“看得见的代价”（应用率下降），\n"
            "      换来的是不再产出**带低覆盖率却声称已拟合**的混合字形（看不见的那种）。\n");
  return 0;
}
