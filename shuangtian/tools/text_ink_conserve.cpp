/// **笔画重量守恒**量尺：量化后的笔画，墨重量是否还等得住原设计？
///
/// 动机：用户反馈「UI 字体还差点意思」。`grid_fit` 会把笔画宽度**量化到整数物理像素**
/// （`max(1, round(width_px))`），这在 sharp 度上是必需的（宽度不跳档），
/// 但它会改变墨量：1.4px 的设计宽度被四舍五入成 1px 就丢掉 29% 的墨，
/// 成 2px 则多 43%。**UI 正文（12~14px）的拉丁主干普遍落在 1.2~1.5px**，
/// 于是要么被抽瘦（发虚）、要么被加胖（发糊），取决于这一档的舍入方向。
///
/// 做法：把同一段文本在**极高超采样**下渲染（近似解析覆盖率 = 设计真值），
/// 再与真实口径（supersample=2 + 拟合）的墨量比较，差值就是量化带来的偏差。
/// 顺带逐档给出「拟合前 / 拟合后」的对比，确认拟合本身有没有额外吃墨。
///
/// 判据：`ink / ideal` 偏离 1 超过约 3% 就说明量化在系统性地改字重。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

constexpr int kW = 420;
constexpr int kH = 60;

/// 覆盖率口径的墨量（按前景/背景归一，与颜色无关）。
[[nodiscard]] auto ink_of(const TextRenderer& renderer, std::string_view text, float logical_size,
                          float device_scale, FontRole role) -> double {
  Canvas canvas{static_cast<int>(kW * device_scale), static_cast<int>(kH * device_scale),
                device_scale};
  canvas.clear(Color::rgb(255, 255, 255));
  (void)renderer.draw(canvas, text, Point{2.0f, 2.0f}, logical_size, Color::rgb(0, 0, 0), role);
  const auto rgba = canvas.to_rgba8();
  double ink = 0.0;
  for (std::size_t index = 0; index < rgba.size(); index += 4U) {
    const double lum =
        (static_cast<double>(rgba[index]) + rgba[index + 1] + rgba[index + 2]) / 3.0;
    ink += std::clamp((255.0 - lum) / 255.0, 0.0, 1.0);
  }
  return ink;
}

}  // namespace

auto main() -> int {
  auto stack = FontStack::system_default();
  if (!stack) return 1;
  const FontStack& fonts = *stack;
  const float device_scale = 1.5f;

  struct Sample {
    const char* tag;
    std::string_view text;
    FontRole role;
  };
  const Sample samples[] = {
      {"UI 拉丁", "Segoe UI abcdefgh", FontRole::Proportional},
      {"UI 拉丁小写", "abcdefghijklmnop", FontRole::Proportional},
      {"UI 中文", "资源管理器设置", FontRole::Proportional},
      {"UI 混排", "打开文件 Open File 24", FontRole::Proportional},
      {"编辑器", "const auto polylines = path", FontRole::Monospace},
  };

  st::print("口径：device_scale={}；「真值」= 超采样 8 档（近似解析覆盖率）\n", device_scale);
  st::print("  {:<14s} {:>5s} {:>10s} {:>10s} {:>10s} {:>9s} {:>9s}\n", "样本", "逻辑px", "真值(ss8)",
            "ss2 无拟合", "ss2 拟合", "拟合/真值", "无拟合/真值");
  for (const Sample& sample : samples) {
    for (const float size : {11.0f, 12.0f, 13.0f, 13.5f, 14.0f, 16.0f}) {
      TextRenderer ideal(fonts, 8.0f);
      ideal.set_subpixel(false);
      ideal.set_grid_fit(GridFitMode::Off);
      const double truth = ink_of(ideal, sample.text, size, device_scale, sample.role);

      TextRenderer raw(fonts, device_scale);
      raw.set_subpixel(false);
      raw.set_grid_fit(GridFitMode::Off);
      const double no_fit = ink_of(raw, sample.text, size, device_scale, sample.role);

      TextRenderer fitted(fonts, device_scale);
      fitted.set_subpixel(false);
      fitted.set_grid_fit(GridFitMode::Normal);
      const double with_fit = ink_of(fitted, sample.text, size, device_scale, sample.role);

      st::print("  {:<14s} {:>5.1f} {:>10.1f} {:>10.1f} {:>10.1f} {:>9.3f} {:>9.3f}\n",
                sample.tag, size, truth, no_fit, with_fit, with_fit / truth, no_fit / truth);
    }
    st::print("\n");
  }
  return 0;
}
