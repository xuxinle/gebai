/// 网格拟合（hinting）**收益量尺**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 三个指标一起看，缺一个都可能误判：
/// 1. **竖笔画边缘落在整数网格的比例**——`tools/stem_phase_probe.cpp` 的口径，
///    直接对应"笔画边缘有没有缓坡"；这是拟合的**目标本身**。
/// 2. **中间调像素占比**（覆盖度 ∈ (0.15, 0.85) / 有墨像素）——最接近"看着糊不糊"的量化；
///    边缘落在分数相位上时笔画两侧各产生一个中间调像素。
/// 3. **墨量**（覆盖率和）——**形变护栏**：拟合不该显著改变字的粗细。
///
/// 参考基准（FreeType auto-hinter，`tools/hinting_gain_probe.cpp` 实测）：
/// 拉丁 49.0% / 中文 20.6%。自研 `grid_fit` 实测：拉丁 50.9% / 中文 63.4%
/// （见 `DESIGN.md §4.3.2`）。
///
/// 手工编译：
/// ```bash
/// cd shuangtian && ./build/bin/st build st --profile dev
/// OBJS=$(ls build/dev/obj/*.o | grep -v -E "(main\.cpp\.o|_test\.cpp\.o|test_runner\.cpp\.o|_vendor_|examples_)")
/// g++ -std=c++20 -O1 -Iinclude -Ithird_party tools/grid_fit_report.cpp $OBJS -o /tmp/gridfit_report -lpthread -ldl -lm
/// /tmp/gridfit_report
/// ```
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/text.hpp"

using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

constexpr float kSize = 13.5f;        // 逻辑 px
constexpr float kDpi = 1.25f;          // 与反馈现场一致
constexpr std::string_view kCjk =
    "霜天自绘概览组件数据控制通道关于按钮表单密码提交重置进度状态窗口字体渲染"
    "一二三四五六七八九十日月田目国回口品晶磊赢疆餐囊藏";
constexpr std::string_view kLatin =
    "Handgloves Illegible ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789";

struct Stats {
  std::size_t edges{0};
  std::size_t on_grid{0};
  std::size_t ink{0};
  std::size_t mid{0};
  double ink_sum{0.0};
};

[[nodiscard]] auto measure(const FontStack& stack, std::string_view text, GridFitMode mode)
    -> Stats {
  Stats stats;
  TextRenderer renderer(stack, 1.0f);
  renderer.set_grid_fit(mode);
  const float pixel_size = kSize * kDpi;
  // 相位口径：直接从**位图**数"每行的上升沿过渡带"太粗（受合成影响），
  // 这里用位图覆盖率的极值分布近似——真正的相位口径见 stem_phase_probe。
  for (const char32_t codepoint : st::utf8_decode(text)) {
    const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
    if (bitmap == nullptr || bitmap->coverage.empty()) continue;
    for (const float value : bitmap->coverage) {
      stats.ink_sum += static_cast<double>(value);
      if (value <= 0.02f) continue;
      ++stats.ink;
      if (value > 0.15f && value < 0.85f) ++stats.mid;
      if (value >= 0.97f) ++stats.on_grid;  // 满墨像素（锐笔画的内部）
    }
  }
  return stats;
}

void report(std::string_view label, const Stats& stats) {
  const double mid = stats.ink > 0 ? 100.0 * static_cast<double>(stats.mid) /
                                         static_cast<double>(stats.ink)
                                   : 0.0;
  st::print("  {:<14} 有墨 {:6}  中间调占比 {:5.1f}%  墨量 {:9.0f}\n", label, stats.ink, mid,
            stats.ink_sum);
}

}  // namespace

int main() {
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("无可用字体\n");
    return 1;
  }
  st::print("\n字号 {} 逻辑 px @{} DPI → 物理 {:.2f}px\n", kSize, kDpi, kSize * kDpi);
  for (const auto& [name, text] : {std::pair{"中文", kCjk}, std::pair{"拉丁", kLatin}}) {
    st::print("── {} ──\n", name);
    const Stats off = measure(*stack, text, GridFitMode::Off);
    const Stats light = measure(*stack, text, GridFitMode::Light);
    const Stats normal = measure(*stack, text, GridFitMode::Normal);
    report("无拟合", off);
    report("Light", light);
    report("Normal", normal);
    const double ink_delta = off.ink_sum > 0.0 ? 100.0 * (light.ink_sum / off.ink_sum - 1.0) : 0.0;
    st::print("  → 中间调占比降低 {:.1f} 个百分点，墨量变化 {:+.2f}%\n",
              (off.ink > 0 ? 100.0 * static_cast<double>(off.mid) / static_cast<double>(off.ink) : 0.0) -
                  (light.ink > 0 ? 100.0 * static_cast<double>(light.mid) / static_cast<double>(light.ink) : 0.0),
              ink_delta);
  }
  return 0;
}
