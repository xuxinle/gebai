/// **拟合生效性/拒载诊断**（仅验证用）：逐个字形报位图网格、**被拒笔画数**与
/// **最坏被拒位移**。
///
/// 动机（2026-10-04，用户反馈「中文字体线条粗细不均匀」）：同一字里若一部分笔画被吸附、
/// 另一部分**因预算不足被拒**，画面上就是「粗细不一」——被拒的那根没落网格，摊成灰边。
/// 旧诊断只报“生效数”，分不清「找不到笔画」与「找到了但推不动」，两者调参方向相反。
///
/// 运行：`build/probe/fit_reject_probe.exe [device_scale] [pixel_size] [文本...]`

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

auto main(int argc, char** argv) -> int {
  const float device_scale = argc > 1 ? std::stof(argv[1]) : 1.5f;
  const float pixel_size = argc > 2 ? std::stof(argv[2]) : 20.25f;
  const std::vector<std::string> texts =
      argc > 3 ? std::vector<std::string>{argv[3]}
               : std::vector<std::string>{"概览组件数据控制通道已就绪刷新指标",
                                          "资源管理器打开文件设置", "霜天自绘渲染"};
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const int supersample = std::max(1, static_cast<int>(std::lround(device_scale)));
  st::print("device_scale={} 物理字号={}（supersample={}，拟合的 grid 口径）\n", device_scale,
            pixel_size, supersample);

  TextRenderer renderer(*stack, device_scale);
  renderer.set_grid_fit(GridFitMode::Normal);
  std::size_t glyphs = 0;
  std::size_t rejected = 0;
  float worst = 0.0f;
  std::size_t with_reject = 0;
  for (const std::string& text : texts) {
    st::print("\n「{}」\n", text);
    st::print("  字形    位图网格       被拒笔画  最坏被拒位移\n");
    for (const char32_t codepoint : st::utf8_decode(text)) {
      const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
      if (bitmap == nullptr) continue;
      ++glyphs;
      rejected += static_cast<std::size_t>(bitmap->fit_rejected_stems);
      if (bitmap->fit_rejected_stems > 0) ++with_reject;
      worst = std::max(worst, bitmap->fit_worst_rejected_shift);
      st::print("  U+{:04X}  {:2d}x{:2d}@{:3d},{:3d}   {:4d}      {:.3f}px{}\n",
                static_cast<unsigned>(codepoint), bitmap->width, bitmap->height,
                bitmap->offset_x, bitmap->offset_y, bitmap->fit_rejected_stems,
                bitmap->fit_worst_rejected_shift,
                bitmap->fit_rejected_stems > 0 ? "   ← 没落网格（就是“看着更细”的那根）" : "");
    }
  }
  st::print("\n==== 汇总（{} 个字形）====\n", glyphs);
  st::print("被拒笔画合计 {} 条；含拒载的字形 {} 个（{:.0f}%）；最坏被拒位移 {:.3f}px\n",
            rejected, with_reject,
            glyphs > 0 ? 100.0 * static_cast<double>(with_reject) / static_cast<double>(glyphs)
                       : 0.0,
            worst);
  st::print("判读：被拒 ⇒ 该笔画的位移需求超过 `max_shift + grid/2`（小字号当前 {:.1f}+{:.1f}）；\n"
            "      “最坏被拒位移”就是 `max_shift` 要放到多大才收得下它。\n",
            1.0f, static_cast<float>(supersample) * 0.5f);
  return 0;
}
