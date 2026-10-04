/// **字形落点相位量尺**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 要回答的问题（对应《字体高清渲染全链路优化指导书》§1.1）：
/// **霜天有没有子像素定位？** 如果没有，字距会不会「随相位跳」？
///
/// 机理：`TextRenderer::draw` 里每字形落点是
/// `x_begin = lround((origin.x + run.x) * device_scale) + bitmap->offset_x`——
/// 即**笔位（浮点）被取整到整数物理像素**。而 run.x = 前面所有字形的 advance(+kern) 之和，
/// 是任意小数；于是每个字形的**真实相位**（笔位的小数部分）被丢掉。
/// 后果两条：
///   ① **字距误差**：相邻两字形的实际间距 = round(x2)−round(x1)，而设计间距 = x2−x1；
///      误差在 [−1, +1] 物理像素之间随相位变化 =「字距看起来不齐」；
///   ② **同字形在不同位置渲染不同**：位图按整数落点采样，相位被量化到 0。
///
/// 做法：对真实 UI 串整形，逐字形算出**理想笔位**与**实际落点**，报：
///   - 相位分布（笔位小数部分落在 8 个桶里的比例；理想应均匀，量化后恒为 0）
///   - 字距误差分布（实际间距 − 设计间距），以及其标准差
///   - 与「1/4 像素相位」方案的对比估计：若保留相位，间距误差上界降到 ±0.125px
///
/// 编译：`pwsh tools/build_probes.ps1 -Only glyph_phase_probe`（复用 build/dev/obj）
/// 用法：`build/probe/glyph_phase_probe.exe [device_scale] [逻辑字号] [文本...]`

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/text.hpp"

using st::text::FontRole;
using st::text::FontStack;
using st::text::TextRenderer;

auto main(int argc, char** argv) -> int {
  const float device_scale = argc > 1 ? std::stof(argv[1]) : 1.5f;
  const float size = argc > 2 ? std::stof(argv[2]) : 13.5f;
  const std::vector<std::string> texts =
      argc > 3 ? std::vector<std::string>{argv[3]}
               : std::vector<std::string>{
                     "自绘 UI · 跨平台 · 软硬件渲染兼容 · 无头可控 · DPI 感知",
                     "Settings Open File 24 text",
                     "资源管理器 打开文件 设置",
                     "const auto polylines = p",
                 };
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  TextRenderer renderer(*stack, device_scale);
  st::print("device_scale={} 逻辑字号={} 物理字号={}\n", device_scale, size, size * device_scale);

  // 相位分桶：把 [0,1) 分成 8 份。
  constexpr int kBuckets = 8;
  std::vector<std::size_t> phase_hist(kBuckets, 0);
  std::size_t total = 0;
  double worst_spacing_err = 0.0;
  double sum_spacing_err = 0.0;
  double sum_spacing_err_sq = 0.0;
  std::size_t spacing_count = 0;
  double sum_abs_shift = 0.0;
  std::size_t shift_count = 0;

  for (const std::string& text : texts) {
    for (const FontRole role : {FontRole::Proportional, FontRole::Monospace}) {
      const auto shaped = renderer.shape_cached(text, size, role);
      if (shaped->runs.size() < 2) continue;
      st::print("\n── [{}] 「{}」 字形 {} ──\n", role == FontRole::Monospace ? "等宽" : "正文",
                text, shaped->runs.size());
      std::vector<double> ideal;
      std::vector<double> actual;
      for (const auto& run : shaped->runs) {
        if (run.face == nullptr) continue;
        const double physical_x = static_cast<double>(run.x) * device_scale;
        ideal.push_back(physical_x);
        // 复刻 draw 的落点：lround(笔位)。offset_x 是字形自身的固定偏移，两边都带、相减消去。
        actual.push_back(std::round(physical_x));
        const double phase = physical_x - std::floor(physical_x);
        phase_hist[std::min(kBuckets - 1, static_cast<int>(phase * kBuckets))] += 1;
        ++total;
        // 单个字形的“被挪动量” = 理想位置与取整后位置之差（0..0.5）。
        sum_abs_shift += std::abs(std::round(physical_x) - physical_x);
        ++shift_count;
      }
      for (std::size_t index = 1; index < ideal.size(); ++index) {
        const double ideal_gap = ideal[index] - ideal[index - 1];
        const double actual_gap = actual[index] - actual[index - 1];
        const double error = actual_gap - ideal_gap;
        worst_spacing_err = std::max(worst_spacing_err, std::abs(error));
        sum_spacing_err += error;
        sum_spacing_err_sq += error * error;
        ++spacing_count;
        if (spacing_count <= 400) {
          st::print("    字距{:2}: 设计 {:7.3f}px → 实际 {:6.0f}px  误差 {:+.3f}px\n", index,
                    ideal_gap, actual_gap, error);
        }
      }
    }
  }

  const double mean_spacing =
      spacing_count > 0 ? sum_spacing_err / static_cast<double>(spacing_count) : 0.0;
  const double var_spacing = spacing_count > 0
                                 ? sum_spacing_err_sq / static_cast<double>(spacing_count) -
                                       mean_spacing * mean_spacing
                                 : 0.0;
  st::print("\n==== 汇总（{} 个字形 / {} 对相邻字距）====\n", total, spacing_count);
  st::print("笔位相位分布（8 桶，理想均匀 = 12.5%/桶）：\n  ");
  for (int index = 0; index < kBuckets; ++index) {
    st::print("[{:.2f}~{:.2f}) {:5.1f}%  ", static_cast<double>(index) / kBuckets,
              static_cast<double>(index + 1) / kBuckets,
              total > 0 ? 100.0 * static_cast<double>(phase_hist[index]) / static_cast<double>(total)
                        : 0.0);
  }
  st::print("\n");
  st::print("字形被取整挪动的平均量：{:.3f}px（理论均匀相位 0.250px）\n",
            shift_count > 0 ? sum_abs_shift / static_cast<double>(shift_count) : 0.0);
  st::print("字距误差：均值 {:+.4f}px  标准差 {:.4f}px  最坏 {:.4f}px\n", mean_spacing,
            std::sqrt(std::max(0.0, var_spacing)), worst_spacing_err);
  st::print("（对照：保留 1/4 像素相位 → 误差上界 ±0.125px；1/64 像素相位 → ±0.008px）\n");
  return 0;
}
