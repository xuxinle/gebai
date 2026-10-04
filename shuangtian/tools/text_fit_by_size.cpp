/// 网格拟合在 **UI 字号区间**（11~16px）的收益/代价量尺。
///
/// 动机：拟合当初是在 **20~21px** 上调参并验收的（`tools/stem_phase_probe.cpp`）。
/// 但用户看到的「UI 字体差点意思」发生在 12~14px。量化宽度取 `round(width_px)`，
/// 而 UI 字号下拉丁主干普遍只有 1.2~1.5 物理像素——舍入的相对误差比 21px 时大得多。
/// 若小字上拟合的收益消失甚至转负，那就是**参数没跟着字号走**的真缺陷。
///
/// 判据（灰度、真实应用口径）：
///   `peak`      峰值覆盖率（能否满黑）
///   `mid`       中间调像素占比（越低越锐）
///   `ink`       Σ覆盖率（与真值比，看有没有被抽瘦）
///   `sharp`     相邻像素 |Δ覆盖率| 的均值（边缘陡度，越高越锐）
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

constexpr int kW = 460;
constexpr int kH = 60;

struct Stat {
  float peak{0.0f};
  double ink{0.0};
  int solid{0};
  int mid{0};
  double sharp{0.0};
  std::size_t on{0};
};

[[nodiscard]] auto measure(const TextRenderer& renderer, std::string_view text, float logical_size,
                           float device_scale, FontRole role) -> Stat {
  const int width = static_cast<int>(kW * device_scale);
  const int height = static_cast<int>(kH * device_scale);
  Canvas canvas{width, height, device_scale};
  canvas.clear(Color::rgb(255, 255, 255));
  (void)renderer.draw(canvas, text, Point{2.0f, 2.0f}, logical_size, Color::rgb(0, 0, 0), role);
  const auto rgba = canvas.to_rgba8();
  std::vector<float> cov(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0f);
  double min_lum = 255.0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t base =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) * 4U;
      const double lum =
          (static_cast<double>(rgba[base]) + rgba[base + 1] + rgba[base + 2]) / 3.0;
      min_lum = std::min(min_lum, lum);
      cov[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x)] =
          static_cast<float>(std::clamp((255.0 - lum) / 255.0, 0.0, 1.0));
    }
  }
  Stat stat;
  stat.peak = static_cast<float>(std::clamp((255.0 - min_lum) / 255.0, 0.0, 1.0));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float v = cov[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(x)];
      stat.ink += v;
      if (v > 0.85) ++stat.solid;
      else if (v > 0.15) ++stat.mid;
      if (x + 1 < width) {
        const float r = cov[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(x + 1)];
        if (v > 0.02f || r > 0.02f) {
          stat.sharp += std::abs(r - v);
          ++stat.on;
        }
      }
    }
  }
  stat.sharp = stat.on > 0 ? stat.sharp / static_cast<double>(stat.on) : 0.0;
  return stat;
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
      {"UI 中文", "资源管理器设置概览", FontRole::Proportional},
      {"UI 拉丁混排", "Open File 24 text", FontRole::Proportional},
      {"UI 路径", "src/raster/renderer.cpp", FontRole::Proportional},
      {"编辑器 等宽", "const auto polylines", FontRole::Monospace},
  };
  st::print("口径：device_scale={}、灰度（把相位定位与彩边分开）、拟合 Off vs Normal\n",
            device_scale);
  for (const Sample& sample : samples) {
    st::print("\n=== {} 「{}」 ===\n", sample.tag, sample.text);
    st::print("  {:>5s} {:>9s} {:>9s} {:>9s} {:>10s} {:>9s}\n", "逻辑px", "峰值", "solid",
              "mid", "mid/solid", "边缘陡度");
    for (const float size : {11.0f, 12.0f, 12.5f, 13.0f, 13.5f, 14.0f, 16.0f, 21.0f}) {
      for (const bool fit : {false, true}) {
        TextRenderer renderer(fonts, device_scale);
        renderer.set_subpixel(false);
        renderer.set_grid_fit(fit ? GridFitMode::Normal : GridFitMode::Off);
        const Stat stat = measure(renderer, sample.text, size, device_scale, sample.role);
        st::print("  {:>5.1f} {:>9.3f} {:>9d} {:>9d} {:>10.3f} {:>9.4f}   {}\n", size, stat.peak,
                  stat.solid, stat.mid,
                  stat.solid > 0 ? static_cast<double>(stat.mid) / stat.solid : -1.0, stat.sharp,
                  fit ? "拟合" : "原样");
      }
    }
  }
  return 0;
}
