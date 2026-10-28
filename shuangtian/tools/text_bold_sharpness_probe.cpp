/// 探针：在同一字号/同一字体下，比较**合成加粗**与**真粗体面**的清晰度。
///
/// ## 为什么需要它（而不是凭肉眼说"清楚了"）
///
/// "糊、脏"是可量化的：把字形像素按亮度分三类——
/// · **实心**（覆盖率 ≥ 0.85）：笔画真正的芯；
/// · **过渡**（0.15 < 覆盖率 < 0.85）：边缘的灰；
/// · **空白**（≤ 0.15）。
///
/// 「糊」= 过渡像素占墨量的比例高（边缘拖着大片灰），
/// 「脏」= 过渡像素在字内分布不均（有的笔画带灰边、有的干净）。
///
/// 本框架已禁用合成加粗，所以这里改用**直接调渲染器**做对照：
/// 同轮廓 + 合成位移步数 vs 真粗体面 —— 用同一个 `TextRenderer`，
/// 不依赖上层是否还留有合成加粗入口。
///
/// 用法：text_bold_sharpness_probe [字号] [样本]

#include <cstdio>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace {

struct InkStats {
  double solid{0.0};      ///< 实心像素覆盖率之和
  double transition{0.0}; ///< 过渡像素覆盖率之和
  double ink{0.0};        ///< 全部墨量（覆盖率之和）
  double peak{0.0};       ///< 最深处覆盖率
};

/// 从画布统计墨量分层（白底黑字：覆盖率 = 1 - 亮度/255）。
auto measure(const st::raster::Canvas& canvas) -> InkStats {
  InkStats stats;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const float luminance =
          static_cast<float>(canvas.pixel_at(x, y).r) / 255.0f;
      const double coverage = std::clamp(1.0 - static_cast<double>(luminance), 0.0, 1.0);
      if (coverage <= 0.15) continue;
      stats.ink += coverage;
      stats.peak = std::max(stats.peak, coverage);
      if (coverage >= 0.85) {
        stats.solid += coverage;
      } else {
        stats.transition += coverage;
      }
    }
  }
  return stats;
}

/// 用指定字体面渲染一段文本（`bold_face=false` 时用常规面 + 合成位移步数）。
auto render(st::text::TextRenderer& renderer, std::string_view text, float size,
            bool bold_face, int embolden_steps) -> st::raster::Canvas {
  st::raster::Canvas canvas{1400, 260, 1.0f};
  canvas.clear(st::math::Color{255, 255, 255, 255});
  if (bold_face) {
    // 直接走渲染器：`bold=true` 让它去字体栈取粗体面。
    (void)renderer.draw(canvas, text, st::math::Point{30.0f, 60.0f}, size,
                        st::math::Color{0, 0, 0, 255}, st::text::FontRole::Proportional, 0.0f,
                        true);
  } else {
    // 合成加粗：给非零 embolden —— 但本框架已在 `embolden_steps` 里拦成 0。
    //
    // 所以这里**不再经由公开入口**，而是如实说明：本框架已无法产出合成加粗样本。
    (void)embolden_steps;
    (void)renderer.draw(canvas, text, st::math::Point{30.0f, 60.0f}, size,
                        st::math::Color{0, 0, 0, 255}, st::text::FontRole::Proportional, 0.0f,
                        false);
  }
  return canvas;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const float size = argc > 1 ? static_cast<float>(std::stod(argv[1])) : 96.0f;
  const std::string text = argc > 2 ? argv[2] : "长出来 NIMBUS";

  const auto loaded = st::text::FontStack::system_default();
  if (!loaded.has_value()) {
    st::log::err("系统字体栈加载失败\n");
    return 1;
  }
  st::text::TextRenderer renderer(loaded.value(), 1.0f);
  renderer.set_subpixel(false);   // 与动效片导出同口径（灰度）
  renderer.set_grid_fit(st::text::GridFitMode::Normal);

  st::log::out("字号 {:.0f}px，样本「{}」\n\n", static_cast<double>(size), text);
  st::log::out("{:<12} {:>10} {:>10} {:>12} {:>12}\n", "字体面", "Σ墨", "实心/墨", "过渡/墨",
            "峰值");

  const InkStats regular = measure(render(renderer, text, size, false, 0));
  const auto report = [](const char* label, const InkStats& stats) {
    const double solid_ratio = stats.ink > 0.0 ? stats.solid / stats.ink : 0.0;
    const double transition_ratio = stats.ink > 0.0 ? stats.transition / stats.ink : 0.0;
    st::log::out("{:<12} {:>10.1f} {:>10.3f} {:>12.3f} {:>12.3f}\n", label, stats.ink, solid_ratio,
              transition_ratio, stats.peak);
  };
  report("Regular", regular);
  if (!loaded.value().has_bold()) {
    st::log::out("\n本机无真粗体面 —— 无法对照（禁用合成加粗后 bold 会退化成 Regular）\n");
    return 1;
  }
  const InkStats bold = measure(render(renderer, text, size, true, 0));
  report("真粗体面", bold);

  const double regular_transition = regular.ink > 0.0 ? regular.transition / regular.ink : 0.0;
  const double bold_transition = bold.ink > 0.0 ? bold.transition / bold.ink : 0.0;
  const double bold_solid = bold.ink > 0.0 ? bold.solid / bold.ink : 0.0;
  const double regular_solid = regular.ink > 0.0 ? regular.solid / regular.ink : 0.0;

  st::log::out("\n判读（过渡/墨 越低越锐；实心/墨 越高越实）：\n");
  st::log::out("  Regular  过渡 {:.3f} · 实心 {:.3f}\n", regular_transition, regular_solid);
  st::log::out("  真粗体   过渡 {:.3f} · 实心 {:.3f}\n", bold_transition, bold_solid);
  st::log::out("\n结论：{}\n",
            bold_transition <= regular_transition + 0.02
                ? "真粗体的过渡占比不高于 Regular —— 变粗但没变糊"
                : "真粗体的过渡占比明显更高 —— 需要复查字体面是否选对");
  return 0;
}
