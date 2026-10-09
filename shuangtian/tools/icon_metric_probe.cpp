// 量「图标实际渲染多大」——按真实光栅化结果，不靠几何算式推断。
//
// 为什么必须量像素：`Icon::path` 的光学归一化按**几何墨迹包围盒**（`view_bounds`）
// 缩放到 `kOpticalTargetPx`。而描边式图标是用 `stroke_path` 画的——线宽**跨在路径上**，
// 于是它实际涂出的墨迹比几何包围盒**每边多半个线宽**；实心（filled）图标用
// `fill_path`，涂出的就是几何包围盒本身。
//
// 两者的差在 `view_bounds` 里看不见（它是纯几何量），只有渲染像素才能测到。
// 本探针把每个图标渲染到已知盒径的画布上、量出墨迹包围盒，输出：
//   ① 各图标墨迹最长边（逻辑 px）——"看着一样大"是否成立
//   ② 描边式 vs 实心式的系统性差
//
// 用法： icon_metric_probe [box_px]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Rect;

/// 在 `size`×`size` 画布上画 `name`，返回**实际墨迹**的包围盒（像素）。
/// 判据：与底色差异 > 阈值（覆盖率 > 8%）——比"非底色"严，避开抗锯齿尾巴。
struct Ink {
  float x0{0.0f};
  float y0{0.0f};
  float x1{0.0f};
  float y1{0.0f};
  [[nodiscard]] auto width() const -> float { return x1 - x0 + 1.0f; }
  [[nodiscard]] auto height() const -> float { return y1 - y0 + 1.0f; }
  [[nodiscard]] auto extent() const -> float { return std::max(width(), height()); }
};

[[nodiscard]] auto measure(std::string_view name, int size) -> Ink {
  st::raster::Canvas canvas{size, size, 1.0f};
  const Color background = Color::rgb(0xFF, 0xFF, 0xFF);
  const Color ink = Color::rgb(0x00, 0x00, 0x00);
  canvas.clear(background);
  st::ui::Icon::draw(canvas, name, Rect{0.0f, 0.0f, static_cast<float>(size),
                                        static_cast<float>(size)},
                     ink);  // 与 Button::paint_content 同调用形态（stroke_width 走默认 2.0）
  Ink result{-1.0f, -1.0f, -1.0f, -1.0f};
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      // 覆盖率 ≈ 1 - r/255；取 > 0.08（明显有墨）
      if (static_cast<int>(pixel.r) < 235) {
        if (result.x0 < 0.0f) {
          result = Ink{static_cast<float>(x), static_cast<float>(y),
                       static_cast<float>(x), static_cast<float>(y)};
        } else {
          result.x0 = std::min(result.x0, static_cast<float>(x));
          result.y0 = std::min(result.y0, static_cast<float>(y));
          result.x1 = std::max(result.x1, static_cast<float>(x));
          result.y1 = std::max(result.y1, static_cast<float>(y));
        }
      }
    }
  }
  return result;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int size = argc > 1 ? std::atoi(argv[1]) : 64;
  st::print("渲染盒 {}×{} 逻辑 px（与 Button::paint_content 同调用形态）\n", size, size);
  st::print("理论：几何墨迹最长边被归一到 kOpticalTargetPx=17 单位 → "
            "在 {}px 盒里 = {:.2f}px\n\n",
            size, 17.0 * (static_cast<double>(size) / 24.0));

  // 活动栏用到的图标 + 对照组（描边式 / 实心式各取几个）
  const std::vector<std::string_view> names = {
      "folder", "search", "diff", "play", "package", "git-branch", "terminal",
      "check",  "star",   "cpu",  "heart", "gear",    "sun",        "list"};

  st::print("{:<14} {:>8} {:>8} {:>10} {:>6} {:>9}\n", "name", "ink_w", "ink_h", "extent",
            "filled", "vs目标");
  double sum_stroked = 0.0;
  int n_stroked = 0;
  double sum_filled = 0.0;
  int n_filled = 0;
  for (const auto name : names) {
    const Ink ink = measure(name, size);
    if (ink.x0 < 0.0f) {
      st::print("{:<14} （空）\n", name);
      continue;
    }
    const auto path = st::ui::Icon::path(name, Rect{0.0f, 0.0f, 24.0f, 24.0f}, 2.0f);
    (void)path;
    const double target = 17.0 * (static_cast<double>(size) / 24.0);
    // 判断来源：内置表里 stroke==0 且 filled 即实心
    const bool likely_filled = name == "diff" || name == "play";
    st::print("{:<14} {:>8.1f} {:>8.1f} {:>10.1f} {:>6} {:>8.2f}%\n", name,
              static_cast<double>(ink.width()), static_cast<double>(ink.height()),
              static_cast<double>(ink.extent()), likely_filled ? "?" : "-",
              (static_cast<double>(ink.extent()) / target - 1.0) * 100.0);
    if (likely_filled) {
      sum_filled += static_cast<double>(ink.extent());
      ++n_filled;
    } else {
      sum_stroked += static_cast<double>(ink.extent());
      ++n_stroked;
    }
  }
  st::print("\n描边式均值 {:.2f}px（n={}）· 实心式均值 {:.2f}px（n={}）\n",
            n_stroked > 0 ? sum_stroked / n_stroked : 0.0, n_stroked,
            n_filled > 0 ? sum_filled / n_filled : 0.0, n_filled);
  return 0;
}
