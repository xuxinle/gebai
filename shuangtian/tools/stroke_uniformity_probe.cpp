/// **笔画均匀度量尺**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 要回答的问题（2026-10-04 用户反馈）：「中文整体可以，但**字体线条粗细不均匀**」。
///
/// 机理（本量尺存在的理由）：同一字里设计等宽的两条笔画，若**边缘相位**不同，
/// 渲染结果会完全不同——
///
/// ```
/// 相位落在像素中心：[1.00]                → 1 个满黑像素，看着“粗且实”
/// 相位落在像素边界：[0.37, 0.63]          → 2 个灰像素，看着“细且虚”
/// ```
///
/// 两者的**墨量完全一样**（weight = Σ覆盖率 = 1.00），所以任何只量墨量的尺子都看不见它。
/// 能看见它的是三个量：
///   - `trans`    过渡像素数（0.03 < 覆盖率 < 0.97）：0 = 边缘落在网格上（最锐）
///   - `peak`     峰值覆盖率：1.0 = 有满黑像素；0.5~0.7 = 摊成了灰
///   - `weight`   墨量：**同类笔画应当聚在整数上**（等宽设计 + 网格吸附的必然结果）
///
/// 本量尺逐字形扫描笔画横截面，聚合出：
///   `crisp`   过渡像素为 0 的笔画占比（越高越均匀/越锐）
///   `w_sd`    笔画墨量的标准差
///   `w_off`   墨量离最近整数的平均偏差（**均匀度的主判据**：等宽笔画该落在 1.0/2.0 上）
///   `w_hist`  墨量直方图（看有没有散落在 0.8/1.3/1.5 这些非整数量上）
///
/// 编译/运行：
/// ```powershell
/// pwsh tools/build_probes.ps1 -Only stroke_uniformity_probe
/// build/probe/stroke_uniformity_probe.exe [device_scale] [pixel_size] [--fit=off|light|normal] [--gamma=1.0]
/// ```
/// 不传文本时用真实界面里的中英混排样本。

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

struct Stroke {
  double weight{0.0};   ///< Σ覆盖率（= 笔画宽度 × 平均覆盖率）
  double peak{0.0};     ///< 峰值覆盖率
  int transitions{0};   ///< 过渡像素数
  int width{0};         ///< 像素跨度
};

/// 扫描一份灰度位图，抽出**窄**横截面（= 笔画）。`vertical=true` 逐行扫（得竖笔画）。
void scan(const st::text::TextRenderer::GlyphBitmap& bitmap, bool vertical,
          std::vector<Stroke>& out) {
  if (bitmap.format != st::raster::CoverageFormat::Grayscale) return;
  const int width = bitmap.width;
  const int height = bitmap.height;
  const int outer = vertical ? height : width;   // 扫描行数
  const int inner = vertical ? width : height;   // 每行的长度
  constexpr int kMaxStrokeWidth = 4;             // 更宽的游程不是“笔画横截面”
  for (int line = 0; line < outer; ++line) {
    int start = -1;
    for (int index = 0; index <= inner; ++index) {
      const double value =
          index < inner ? static_cast<double>(bitmap.coverage[static_cast<std::size_t>(
                              vertical ? line * width + index : index * width + line)])
                        : 0.0;
      const bool inked = value > 0.03;
      if (inked && start < 0) {
        start = index;
        continue;
      }
      if (inked) continue;
      if (start < 0) continue;
      const int span = index - start;
      if (span > kMaxStrokeWidth) {
        start = -1;
        continue;
      }
      Stroke stroke;
      stroke.width = span;
      for (int k = start; k < index; ++k) {
        const double v = static_cast<double>(bitmap.coverage[static_cast<std::size_t>(
            vertical ? line * width + k : k * width + line)]);
        stroke.weight += v;
        stroke.peak = std::max(stroke.peak, v);
        if (v > 0.03 && v < 0.97) ++stroke.transitions;
      }
      if (stroke.weight > 0.05) out.push_back(stroke);
      start = -1;
    }
  }
}

struct Summary {
  std::size_t count{0};
  std::size_t crisp{0};        ///< transitions == 0
  double weight_sd{0.0};
  double weight_off{0.0};      ///< 墨量离最近整数的平均偏差
  double peak_mean{0.0};
  std::map<int, std::size_t> histogram{};   ///< 墨量 ×10 取整 → 计数
  double crisp_ratio() const { return count ? static_cast<double>(crisp) / static_cast<double>(count) : 0.0; }
};

auto summarize(const std::vector<Stroke>& strokes) -> Summary {
  Summary summary;
  summary.count = strokes.size();
  if (strokes.empty()) return summary;
  double sum = 0.0;
  for (const Stroke& stroke : strokes) {
    if (stroke.transitions == 0) ++summary.crisp;
    sum += stroke.weight;
    summary.peak_mean += stroke.peak;
    summary.weight_off += std::abs(stroke.weight - std::round(stroke.weight));
    summary.histogram[static_cast<int>(std::lround(stroke.weight * 10.0))] += 1;
  }
  const double mean = sum / static_cast<double>(strokes.size());
  double variance = 0.0;
  for (const Stroke& stroke : strokes) variance += (stroke.weight - mean) * (stroke.weight - mean);
  summary.weight_sd = std::sqrt(variance / static_cast<double>(strokes.size()));
  summary.weight_off /= static_cast<double>(strokes.size());
  summary.peak_mean /= static_cast<double>(strokes.size());
  return summary;
}

void report(std::string_view label, const Summary& summary) {
  st::print("  {:<10s} n={:5d}  crisp={:5.1f}%  peak={:.3f}  w_sd={:.3f}  **w_off={:.3f}**\n",
            label, summary.count, summary.crisp_ratio() * 100.0, summary.peak_mean,
            summary.weight_sd, summary.weight_off);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  float device_scale = 1.5f;
  float pixel_size = 20.25f;
  GridFitMode fit = GridFitMode::Normal;
  float gamma = 1.0f;
  std::vector<std::string> texts;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    if (arg.starts_with("--fit=")) {
      const std::string_view value = arg.substr(6);
      fit = value == "off" ? GridFitMode::Off
                           : (value == "light" ? GridFitMode::Light : GridFitMode::Normal);
    } else if (arg.starts_with("--gamma=")) {
      gamma = std::stof(std::string(arg.substr(8)));
    } else if (arg == "--scale" && index + 1 < argc) {
      device_scale = std::stof(argv[++index]);
    } else if (arg == "--size" && index + 1 < argc) {
      pixel_size = std::stof(argv[++index]);
    } else {
      texts.emplace_back(arg);
    }
  }
  if (texts.empty()) {
    texts = {"概览组件数据控制通道已就绪刷新指标", "资源管理器打开文件设置", "霜天自绘渲染无头可控"};
  }
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  st::print("device_scale={} 逻辑字号={} fit={} gamma={}\n", device_scale, pixel_size,
            fit == GridFitMode::Off ? "off" : (fit == GridFitMode::Light ? "light" : "normal"),
            gamma);

  for (const std::string& text : texts) {
    std::vector<Stroke> vertical;
    std::vector<Stroke> horizontal;
    for (const char32_t codepoint : st::utf8_decode(text)) {
      // 逐字形取位图（灰度）：取值域干净，且与“字里哪条笔画不齐”一一对应。
      TextRenderer renderer(*stack, static_cast<int>(std::lround(device_scale)));
      renderer.set_grid_fit(fit);
      renderer.set_coverage_gamma(gamma);
      const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
      if (bitmap == nullptr || bitmap->coverage.empty()) continue;
      scan(*bitmap, /*vertical=*/true, vertical);
      scan(*bitmap, /*vertical=*/false, horizontal);
    }
    st::print("\n「{}」（{} 个字形）\n", text, st::utf8_decode(text).size());
    const Summary v = summarize(vertical);
    const Summary h = summarize(horizontal);
    report("竖笔画", v);
    report("横笔画", h);
    const Summary all{[&] {
                        Summary merged = v;
                        merged.count = v.count + h.count;
                        merged.crisp = v.crisp + h.crisp;
                        merged.weight_sd = (v.weight_sd + h.weight_sd) / 2.0;
                        merged.weight_off = (v.weight_off + h.weight_off) / 2.0;
                        merged.peak_mean = (v.peak_mean + h.peak_mean) / 2.0;
                        return merged;
                      }()};
    report("合计", all);
    st::print("    墨量直方图（×10）：");
    std::map<int, std::size_t> merged_hist = v.histogram;
    for (const auto& [key, value] : h.histogram) merged_hist[key] += value;
    for (const auto& [key, value] : merged_hist) {
      if (key % 10 == 0) continue;   // 只列非整数量（整数是期望）
      st::print(" {:.1f}×{}", static_cast<double>(key) / 10.0, value);
    }
    st::print("   ← 非整数墨量的笔画（越少越均匀）\n");
  }
  return 0;
}
