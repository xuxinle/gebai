/// 文字**锐度量尺**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 要回答的问题：小字「发糊」到底有多少来自哪一环？本工具把**同一段真实界面文本**
/// 在同一 DPI 下按 (亚像素 on/off) × (LCD 滤波 on/off) × (网格拟合 off/light/normal)
/// 全组合渲染到画布，逐组合统计**像素级**指标：
///
///   - `bg`        背景亮度（行内众数）
///   - `dark`      最黑像素覆盖率 = 1 - min(lum)/bg ——「笔画有没有到满黑」
///   - `solid`     实心像素数（覆盖率 > 0.85）
///   - `mid`       中间调像素数（0.15 < 覆盖率 < 0.85）——过渡带的宽度
///   - `mid/solid` **中间调占比**（越低越锐；这是主判据）
///   - `ink`       Σ覆盖率（墨量；用来发现「更锐」实为「更细」的假改善）
///
/// 与浏览器对照：`--edge <exe>` 会调 Chrome/Edge 无头渲染同文本同字号同 DPI 的 PNG
/// 并打印同一组指标（口径一致才可比）。
///
/// 手工编译（Windows/MinGW，复用框架 dev 对象；**必须排除 platform_gl / _test / 各 target 的 main**）：
/// ```powershell
/// cd shuangtian
/// $objs = Get-ChildItem build\dev\obj -Filter *.o |
///   Where-Object { $_.Name -match "_src_(core|math|codec|raster|text)_" -and
///                  $_.Name -notmatch "platform_gl|_test|test_runner" } | ForEach-Object { $_.FullName }
/// g++ -std=c++20 -O1 -Iinclude -Ithird_party tools/text_sharpness_probe.cpp @objs `
///   -o out/text_sharpness_probe.exe -lws2_32 -lwinpthread
/// out/text_sharpness_probe.exe <输出目录> [device_scale]
/// ```
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

/// 一段**真实界面文本**（取自 gallery：13.5px 正文 / 12.5px 卡片标题 / 11px 脚注）。
struct Sample {
  const char* label;
  std::string_view text;
  float size;  ///< 逻辑像素
};

const Sample kSamples[] = {
    {"body13.5", "自绘 UI · 跨平台 · 软硬件渲染兼容 · 无头可控 · DPI 感知", 13.5f},
    {"body12.5", "进度与状态（示例自绘组件）", 12.5f},
    {"foot11", "霜天 v0.1.0 · C++20", 11.0f},
};

constexpr int kWidth = 760;
constexpr int kHeight = 64;
constexpr Color kBackground{0xF5, 0xF6, 0xF8, 0xFF};
constexpr Color kForeground{0x1A, 0x1A, 0x1E, 0xFF};

/// 一次渲染的像素统计。
struct Stats {
  double bg{0.0};
  double darkest{0.0};   ///< 最黑像素的覆盖率
  long long solid{0};
  long long mid{0};
  double ink{0.0};
  double mid_ratio{0.0};  ///< mid / solid
};

/// 背景亮度：**行内众数**（取整到 1 灰阶后统计）——比最小值稳，比均值更抗墨迹干扰。
[[nodiscard]] auto background_luminance(const std::vector<std::uint8_t>& rgba, int width,
                                        int height) -> double {
  std::map<int, int> histogram;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t base =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4U;
      const double lum = (static_cast<double>(rgba[base]) + rgba[base + 1] + rgba[base + 2]) / 3.0;
      ++histogram[static_cast<int>(lum + 0.5)];
    }
  }
  int best = 0;
  int best_count = -1;
  for (const auto& [value, count] : histogram) {
    if (count > best_count) {
      best_count = count;
      best = value;
    }
  }
  return static_cast<double>(best);
}

[[nodiscard]] auto measure(const std::vector<std::uint8_t>& rgba, int width, int height) -> Stats {
  Stats stats;
  stats.bg = background_luminance(rgba, width, height);
  const double bg = stats.bg <= 1.0 ? 1.0 : stats.bg;
  double min_lum = 255.0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t base =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4U;
      const double lum = (static_cast<double>(rgba[base]) + rgba[base + 1] + rgba[base + 2]) / 3.0;
      min_lum = std::min(min_lum, lum);
      const double coverage = std::clamp((bg - lum) / bg, 0.0, 1.0);
      stats.ink += coverage;
      if (coverage > 0.85) ++stats.solid;
      else if (coverage > 0.15) ++stats.mid;
    }
  }
  stats.darkest = std::clamp((bg - min_lum) / bg, 0.0, 1.0);
  stats.mid_ratio = stats.solid > 0 ? static_cast<double>(stats.mid) / stats.solid : -1.0;
  return stats;
}

void print_stats(const std::string& tag, const Stats& stats) {
  st::print("  {:<26s} dark={:.3f} solid={:6d} mid={:6d} mid/solid={:6.3f} ink={:9.0f}\n", tag,
            stats.darkest, stats.solid, stats.mid, stats.mid_ratio, stats.ink);
}

/// 渲染一个组合，返回像素。
///
/// `canvas_scale` 是**画布 DPI 口径**：取 1 得到「逻辑单位 = 物理像素」的基准，
/// 取 `device_scale` 得到与真实界面（高 DPI）逐像素一致的口径。
/// 两者必须分开量——2026-10-04 实测就是「画布 DPI 口径」下的差异被漏掉了：
/// 同一段 13.5px 文本在 `canvas_scale = 1` 下满黑、在 `canvas_scale = 1.5` 下
/// 最黑像素只有 58% 覆盖率（笔画摊在像素缝里）。
[[nodiscard]] auto render(const FontStack& stack, const Sample& sample, float canvas_scale,
                          float supersample, bool lcd, bool filter, GridFitMode fit)
    -> std::vector<std::uint8_t> {
  Canvas canvas{static_cast<int>(kWidth * canvas_scale),
                static_cast<int>(kHeight * canvas_scale), canvas_scale};
  canvas.clear(kBackground);
  TextRenderer renderer(stack, supersample);
  renderer.set_subpixel(lcd);
  renderer.set_subpixel_filter(filter);
  renderer.set_grid_fit(fit);
  const float size = sample.size;  // 逻辑单位（与界面一致）
  (void)renderer.draw(canvas, sample.text, Point{4.0f, 8.0f}, size, kForeground);
  return canvas.to_rgba8();
}

void write_png(const std::vector<std::uint8_t>& rgba, const std::string& path, int canvas_scale,
               int zoom) {
  st::codec::PngImage image;
  const int src_width = static_cast<int>(kWidth * canvas_scale);
  const int width = src_width * zoom;
  const int height = static_cast<int>(kHeight * canvas_scale) * zoom;
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t src =
          (static_cast<std::size_t>(y / zoom) * static_cast<std::size_t>(src_width) +
           static_cast<std::size_t>(x / zoom)) *
          4U;
      const std::size_t dst =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4U;
      image.rgba[dst + 0] = rgba[src + 0];
      image.rgba[dst + 1] = rgba[src + 1];
      image.rgba[dst + 2] = rgba[src + 2];
      image.rgba[dst + 3] = 255U;
    }
  }
  const auto status = st::codec::png_write_file(path, image);
  if (!status.has_value()) st::print("  写 PNG 失败：{}\n", status.error().message);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const float scale = argc > 2 ? std::stof(argv[2]) : 1.5f;
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体（ST_FONT_LATIN/ST_FONT_CJK 可显式指定）\n");
    return 1;
  }
  const FontStack& fonts = *stack;

  struct Mode {
    const char* name;
    bool lcd;
    bool filter;
    GridFitMode fit;
  };
  const Mode modes[] = {
      {"gray/nofit      ", false, true, GridFitMode::Off},
      {"gray/fit=light  ", false, true, GridFitMode::Light},
      {"gray/fit=normal ", false, true, GridFitMode::Normal},
      {"lcd+filt/nofit  ", true, true, GridFitMode::Off},
      {"lcd+filt/light  ", true, true, GridFitMode::Light},
      {"lcd+filt/normal ", true, true, GridFitMode::Normal},
      {"lcd-raw/nofit   ", true, false, GridFitMode::Off},
      {"lcd-raw/light   ", true, false, GridFitMode::Light},
      {"lcd-raw/normal  ", true, false, GridFitMode::Normal},
  };

  for (const Sample& sample : kSamples) {
    st::print("\n=== {} \"{}\" @{}px ===\n", sample.label, sample.text, sample.size);
    for (const float supersample : {1.0f, 2.0f}) {
      st::print("  — 画布 DPI 口径 canvas_scale={} / supersample={} —\n", scale, supersample);
      for (const Mode& mode : modes) {
        const auto pixels =
            render(fonts, sample, scale, supersample, mode.lcd, mode.filter, mode.fit);
        print_stats(mode.name,
                    measure(pixels, static_cast<int>(kWidth * scale),
                            static_cast<int>(kHeight * scale)));
      }
    }
    const auto pixels =
        render(fonts, sample, 1.0f, 1.0f, true, true, GridFitMode::Normal);
    st::print("  — 画布 DPI 口径 canvas_scale=1 / supersample=1 —\n");
    print_stats("lcd+filt/normal (基准)",
                measure(pixels, kWidth, kHeight));
    std::string name = std::string("sharp-") + sample.label + "-lcd-f-normal";
    write_png(pixels, out_dir + "/" + name + ".png", 1, 4);
  }
  return 0;
}
