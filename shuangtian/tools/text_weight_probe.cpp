/// 合成加粗**幅度**校核（仅验证用）。
///
/// 目的：确认 `embolden_radius` 的三档比例落在「可辨认的字重差」而不「糊成一团」。
/// 判据（都是像素口径，不靠肉眼）：
///   ① **墨量增幅**：合成加粗相对 Regular 的 Σ覆盖率增幅——真 SemiBold 字体面
///      对 CJK 通常只加 10~20%，超过 35% 就是「太粗」；
///   ② **中间调占比**：加粗不该让边缘变糊（占比不应显著上升）；
///   ③ **峰值覆盖率**：必须仍然到满黑（否则是「糊」不是「粗」）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/core/string.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/text_port.hpp"

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;
using st::ui::FontWeight;

namespace {

struct Stat {
  double ink{0.0};
  int solid{0};
  int mid{0};
  float peak{0.0f};
};

constexpr int kWidth = 300;
constexpr int kHeight = 60;

[[nodiscard]] auto render(const TextRenderer& renderer, std::string_view text, float size,
                          float embolden) -> Stat {
  Canvas canvas{kWidth, kHeight, 1.5f};
  canvas.clear(Color::rgb(255, 255, 255));
  (void)renderer.draw(canvas, text, Point{4.0f, 4.0f}, size, Color::rgb(0x0F, 0x17, 0x2A),
                      FontRole::Proportional, embolden);
  const auto rgba = canvas.to_rgba8();
  Stat stat;
  double min_lum = 255.0;
  for (std::size_t index = 0; index < rgba.size(); index += 4U) {
    const double lum =
        (static_cast<double>(rgba[index]) + rgba[index + 1] + rgba[index + 2]) / 3.0;
    min_lum = std::min(min_lum, lum);
    const double coverage = std::clamp((255.0 - lum) / 255.0, 0.0, 1.0);
    stat.ink += coverage;
    if (coverage > 0.85) ++stat.solid;
    else if (coverage > 0.15) ++stat.mid;
  }
  stat.peak = static_cast<float>(std::clamp((255.0 - min_lum) / 255.0, 0.0, 1.0));
  return stat;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "合成加粗**幅度**校核（仅验证用）。",
      .criterion = "",  // 判据由人补（脚本不猜合格线）
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("text_weight_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  auto stack = FontStack::system_default();
  if (!stack) {
    // 「没测成」不是「不合格」：后者会让人以为量规有问题，实际只是本机缺字体。
    return st::probe::main_of(spec, opts, st::probe::skip("未找到可用字体\\n"));
  }
  const FontStack& fonts = *stack;
  TextRenderer renderer(fonts, 1.5f);
  renderer.set_subpixel(true);
  renderer.set_grid_fit(GridFitMode::Normal);

  struct Sample {
    const char* tag;
    std::string_view text;
    float size;
  };
  const Sample samples[] = {
      {"21px 标题 概览", "概览", 21.0f},
      {"26px 大数字 91", "91", 26.0f},
      {"13.5px 按钮 提交", "提交", 13.5f},
      {"11px 脚注 霜天", "霜天 v0.1.0", 11.0f},
  };
  for (const Sample& sample : samples) {
    const float physical = sample.size * 1.5f;
    st::log::out("\n=== {} 「{}」（物理 {}px）===\n", sample.tag, sample.text, physical);
    const Stat base = render(renderer, sample.text, sample.size, 0.0f);
    st::log::out("  {:<10s} ink={:8.1f} solid={:4d} mid={:4d} peak={:.3f}\n", "Regular",
              base.ink, base.solid, base.mid, base.peak);
    for (const FontWeight weight : {FontWeight::Medium, FontWeight::SemiBold, FontWeight::Bold}) {
      const float radius = st::ui::embolden_radius(physical, weight);
      // 与 TextRenderer 内部同一口径：步长 1/3 物理像素，向上取整（外扩只加不减）。
      const int steps = std::max(1, static_cast<int>(std::lround(radius / (1.0f / 3.0f))));
      for (const int use_steps : {steps}) {
        const float embolden = static_cast<float>(use_steps) / 3.0f;
        const Stat stat = render(renderer, sample.text, sample.size, embolden);
        const char* name = weight == FontWeight::Medium ? "Medium"
                           : (weight == FontWeight::SemiBold ? "SemiBold" : "Bold");
        st::log::out("  {:<10s} ink={:8.1f} solid={:4d} mid={:4d} peak={:.3f}  "
                  "墨量 {:+.1f}%  中间调 {:+.1f}%\n",
                  name, stat.ink, stat.solid, stat.mid, stat.peak,
                  (stat.ink / base.ink - 1.0) * 100.0,
                  base.mid > 0 ? (static_cast<double>(stat.mid) / base.mid - 1.0) * 100.0 : 0.0);
      }
    }
  }
  // 结论退出码由外壳统一给出（0 合格 / 1 不合格 / 77 跳过）

  return st::probe::main_of(spec, opts, verdict);
}
