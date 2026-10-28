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
#include "st/probe/probe.hpp"
#include "st/core/string.hpp"
#include "st/core/log.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "**拟合生效性/拒载诊断**（仅验证用）：逐个字形报位图网格、**被拒笔画数**与",
      .criterion = "",  // 判据由人补（脚本不猜合格线）
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("fit_reject_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  const float device_scale = argc > 1 ? std::stof(argv[1]) : 1.5f;
  const float pixel_size = argc > 2 ? std::stof(argv[2]) : 20.25f;
  const std::vector<std::string> texts =
      argc > 3 ? std::vector<std::string>{argv[3]}
               : std::vector<std::string>{"概览组件数据控制通道已就绪刷新指标",
                                          "资源管理器打开文件设置", "霜天自绘渲染"};
  auto stack = FontStack::system_default();
  if (!stack) {
    // 「没测成」不是「不合格」：后者会让人以为量规有问题，实际只是本机缺字体。
    return st::probe::main_of(spec, opts, st::probe::skip("未找到可用字体\\n"));
  }
  const int supersample = std::max(1, static_cast<int>(std::lround(device_scale)));
  st::log::out("device_scale={} 物理字号={}（supersample={}，拟合的 grid 口径）\n", device_scale,
            pixel_size, supersample);

  TextRenderer renderer(*stack, device_scale);
  renderer.set_grid_fit(GridFitMode::Normal);
  std::size_t glyphs = 0;
  std::size_t rejected = 0;
  float worst = 0.0f;
  std::size_t with_reject = 0;
  for (const std::string& text : texts) {
    st::log::out("\n「{}」\n", text);
    st::log::out("  字形    位图网格       被拒笔画  最坏被拒位移\n");
    for (const char32_t codepoint : st::utf8_decode(text)) {
      const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
      if (bitmap == nullptr) continue;
      ++glyphs;
      rejected += static_cast<std::size_t>(bitmap->fit_rejected_stems);
      if (bitmap->fit_rejected_stems > 0) ++with_reject;
      worst = std::max(worst, bitmap->fit_worst_rejected_shift);
      st::log::out("  U+{:04X}  {:2d}x{:2d}@{:3d},{:3d}   {:4d}      {:.3f}px{}\n",
                static_cast<unsigned>(codepoint), bitmap->width, bitmap->height,
                bitmap->offset_x, bitmap->offset_y, bitmap->fit_rejected_stems,
                bitmap->fit_worst_rejected_shift,
                bitmap->fit_rejected_stems > 0 ? "   ← 没落网格（就是“看着更细”的那根）" : "");
    }
  }
  st::log::out("\n==== 汇总（{} 个字形）====\n", glyphs);
  st::log::out("被拒笔画合计 {} 条；含拒载的字形 {} 个（{:.0f}%）；最坏被拒位移 {:.3f}px\n",
            rejected, with_reject,
            glyphs > 0 ? 100.0 * static_cast<double>(with_reject) / static_cast<double>(glyphs)
                       : 0.0,
            worst);
  st::log::out("判读：被拒 ⇒ 该笔画的位移需求超过 `max_shift + grid/2`（小字号当前 {:.1f}+{:.1f}）；\n"
            "      “最坏被拒位移”就是 `max_shift` 要放到多大才收得下它。\n",
            1.0f, static_cast<float>(supersample) * 0.5f);
  // 结论退出码由外壳统一给出（0 合格 / 1 不合格 / 77 跳过）

  return st::probe::main_of(spec, opts, verdict);
}
