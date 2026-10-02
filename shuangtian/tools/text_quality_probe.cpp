/// 文字**笔画质量量尺**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 动机：用户反馈「霜天的字体渲染太差，远没有浏览器好」。要把这句观感变成可修的动作，
/// 两个抓手（2026-10-02 用户指定）：
///   ① **轮廓连续性**：边缘的过渡是否连续、平滑（有没有台阶/突兀跳变/断点）；
///   ② **宽度一致性**：同一条笔画的粗细是否沿长度稳定、同类笔画之间是否等宽。
///
/// 本工具从**渲染结果**（字形覆盖率位图 / 画布像素）反向量这两件事，逐字形输出：
///   - ASCII 覆盖率矩阵（肉眼看轮廓连续性；每个字符 = 10% 覆盖率档）；
///   - **竖笔画横截面**（逐行扫描窄游程）：每条笔画的重量（Σ覆盖率）、沿长度标准差、
///     过渡像素数——宽度一致性 = 同类笔画重量是否一致、单条笔画沿长度是否稳定；
///   - **横笔画横截面**（逐列扫描）：同上（对竖方向加权敏感的路径最有价值）；
///   - 覆盖率取值分布：若集中在 0.25/0.5/0.75 一类粗档位上，说明**纵向采样量化**在起效，
///     那就是轮廓「台阶感」的来源。
///
/// 浏览器侧对照：用 Chrome 无头截同文本同字号的 PNG（物理像素 = CSS px × 1，字号取物理值），
/// 再用 python 量同一组指标（见 `tmp/textprobe/`）。
///
/// 手工编译（Windows / MinGW，复用框架 dev 对象，剔除 platform/_test/main 等）：
/// ```powershell
/// cd shuangtian
/// $objs = Get-ChildItem build\dev\obj -Filter *.o |
///   Where-Object { $_.Name -match "shuangtian_src_(core|math|codec|raster|text)_" -and
///                  $_.Name -notmatch "platform" } | ForEach-Object { $_.FullName }
/// g++ -std=c++20 -O1 -Iinclude -Ithird_party tools/text_quality_probe.cpp @objs `
///   -o out/text_quality_probe.exe -lws2_32 -lwinpthread
/// out/text_quality_probe.exe <输出目录> [device_scale]
/// ```
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
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

constexpr int kWidth = 1000;
constexpr int kHeight = 120;
constexpr Color kBackground{0xFF, 0xFF, 0xFF, 0xFF};
constexpr Color kForeground{0x1A, 0x1A, 0x1E, 0xFF};

/// 笔画横截面（游程）。
struct Run {
  int row{0};           ///< 扫描行/列序号
  double center{0.0};   ///< 游程质心（横轴坐标）
  double weight{0.0};   ///< Σ覆盖率（该笔画的“墨量”= 宽度 × 覆盖率）
  double peak{0.0};     ///< 峰值覆盖率
  int transitions{0};   ///< 过渡像素数（0.03 < 覆盖率 < 0.97）
  int width{0};         ///< 游程像素宽度
};

/// 一条笔画（多条扫描行上同一位置的游程聚合）。
struct Stroke {
  double center{0.0};
  int rows{0};
  int first_row{0};
  int last_row{0};
  double weight_mean{0.0};
  double weight_sd{0.0};
  double transitions_mean{0.0};
  double peak_mean{0.0};
};

/// 扫描横截面：`vertical=true` 逐行扫（得到竖笔画），否则逐列扫（得到横笔画）。
[[nodiscard]] auto scan_runs(const std::vector<float>& coverage, int width, int height,
                             bool vertical, int max_width) -> std::vector<Run> {
  std::vector<Run> runs;
  const int outer = vertical ? height : width;
  const int inner = vertical ? width : height;
  const auto at = [&](int outer_index, int inner_index) -> float {
    return vertical
               ? coverage[static_cast<std::size_t>(outer_index) * static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(inner_index)]
               : coverage[static_cast<std::size_t>(inner_index) * static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(outer_index)];
  };
  for (int o = 0; o < outer; ++o) {
    int inner_index = 0;
    while (inner_index < inner) {
      if (at(o, inner_index) <= 0.03f) {
        ++inner_index;
        continue;
      }
      const int start = inner_index;
      while (inner_index < inner && at(o, inner_index) > 0.03f) ++inner_index;
      const int end = inner_index;  // [start, end)
      const int span = end - start;
      if (span > max_width) continue;  // 大块墨迹不是“笔画横截面”
      Run run;
      run.row = o;
      run.width = span;
      double weight = 0.0;
      double weighted_center = 0.0;
      for (int x = start; x < end; ++x) {
        const double value = static_cast<double>(at(o, x));
        weight += value;
        weighted_center += value * (static_cast<double>(x) + 0.5);
        run.peak = std::max(run.peak, value);
        if (value > 0.03 && value < 0.97) ++run.transitions;
      }
      if (weight <= 0.0) continue;
      run.weight = weight;
      run.center = weighted_center / weight;
      runs.push_back(run);
    }
  }
  return runs;
}

/// 把游程聚合成笔画（同一位置、扫描行连续）。
[[nodiscard]] auto cluster_runs(std::vector<Run> runs) -> std::vector<Stroke> {
  std::ranges::sort(runs, {}, &Run::row);
  struct Acc {
    double center_sum{0.0};
    int rows{0};
    int first_row{0};
    int last_row{0};
    std::vector<double> weights{};
    std::vector<double> transitions{};
    std::vector<double> peaks{};
  };
  std::vector<Acc> accs;
  for (const Run& run : runs) {
    Acc* best = nullptr;
    double best_distance = 1.0e9;
    for (auto& acc : accs) {
      if (run.row - acc.last_row > 2) continue;
      const double center = acc.center_sum / static_cast<double>(acc.rows);
      const double distance = std::abs(center - run.center);
      if (distance <= 1.25 && distance < best_distance) {
        best = &acc;
        best_distance = distance;
      }
    }
    if (best == nullptr) {
      accs.emplace_back();
      best = &accs.back();
      best->first_row = run.row;
    }
    best->center_sum += run.center;
    best->rows += 1;
    best->last_row = run.row;
    best->weights.push_back(run.weight);
    best->transitions.push_back(static_cast<double>(run.transitions));
    best->peaks.push_back(run.peak);
  }
  std::vector<Stroke> strokes;
  for (const auto& acc : accs) {
    if (acc.rows < 5) continue;
    Stroke stroke;
    stroke.center = acc.center_sum / static_cast<double>(acc.rows);
    stroke.rows = acc.rows;
    stroke.first_row = acc.first_row;
    stroke.last_row = acc.last_row;
    const auto mean = [](const std::vector<double>& values) {
      double sum = 0.0;
      for (const double value : values) sum += value;
      return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
    };
    stroke.weight_mean = mean(acc.weights);
    stroke.transitions_mean = mean(acc.transitions);
    stroke.peak_mean = mean(acc.peaks);
    double variance = 0.0;
    for (const double value : acc.weights) {
      const double delta = value - stroke.weight_mean;
      variance += delta * delta;
    }
    stroke.weight_sd = std::sqrt(variance / static_cast<double>(acc.weights.size()));
    strokes.push_back(stroke);
  }
  std::ranges::sort(strokes, {}, &Stroke::center);
  return strokes;
}

/// 覆盖率取值分布（1/16 档），只列 0.03..0.97 的非空档。
void print_coverage_levels(const std::vector<float>& coverage) {
  int buckets[17] = {};
  for (const float value : coverage) {
    if (value <= 0.03f || value >= 0.97f) continue;
    const int bucket = static_cast<int>(std::lround(static_cast<double>(value) * 16.0));
    buckets[std::clamp(bucket, 0, 16)] += 1;
  }
  st::print("    覆盖率档位（1/16，中间调）:");
  for (int index = 1; index < 16; ++index) {
    if (buckets[index] > 0) {
      st::print(" {:.4}={}", static_cast<double>(index) / 16.0, buckets[index]);
    }
  }
  st::print("\n");
}

constexpr std::string_view kRamp = " .:-=+*#%@";

void dump_bitmap(const std::vector<float>& coverage, int width, int height) {
  for (int y = 0; y < height; ++y) {
    std::string line;
    for (int x = 0; x < width; ++x) {
      const float value =
          coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(x)];
      int index = static_cast<int>(static_cast<double>(value) * 10.0);
      index = std::clamp(index, 0, 9);
      line.push_back(kRamp[static_cast<std::size_t>(index)]);
    }
    st::print("    |{}|\n", line);
  }
}

/// 单字形报告：ASCII 矩阵 + 笔画统计 + 覆盖率档位。
void report_glyph(const FontStack& fonts, char32_t codepoint, float pixel_size, GridFitMode fit,
                  float supersample) {
  TextRenderer renderer(fonts, supersample);
  renderer.set_subpixel(false);
  renderer.set_grid_fit(fit);
  const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
  std::string utf8;
  st::encode_utf8(codepoint, utf8);
  const char* fit_name =
      fit == GridFitMode::Off ? "off" : (fit == GridFitMode::Light ? "light" : "normal");
  if (bitmap == nullptr) {
    st::print("[{}] size={} fit={} ss={} —— 无字形\n", utf8, pixel_size, fit_name, supersample);
    return;
  }
  st::print("[{}] size={} fit={} ss={} bitmap={}x{} offset=({},{})\n", utf8, pixel_size, fit_name,
            supersample, bitmap->width, bitmap->height, bitmap->offset_x, bitmap->offset_y);
  dump_bitmap(bitmap->coverage, bitmap->width, bitmap->height);
  const auto vertical = cluster_runs(scan_runs(bitmap->coverage, bitmap->width, bitmap->height,
                                               true, 6));
  const auto horizontal = cluster_runs(scan_runs(bitmap->coverage, bitmap->width, bitmap->height,
                                                 false, 6));
  st::print("    竖笔画（{} 条，≥5 行）:\n", vertical.size());
  for (const auto& stroke : vertical) {
    st::print("      x≈{:.1f} rows={:>3} weight={:.2f} sd={:.2f} trans={:.1f} peak={:.2f}\n",
              stroke.center, stroke.rows, stroke.weight_mean, stroke.weight_sd,
              stroke.transitions_mean, stroke.peak_mean);
  }
  st::print("    横笔画（{} 条，≥5 列）:\n", horizontal.size());
  for (const auto& stroke : horizontal) {
    st::print("      y≈{:.1f} cols={:>3} weight={:.2f} sd={:.2f} trans={:.1f} peak={:.2f}\n",
              stroke.center, stroke.rows, stroke.weight_mean, stroke.weight_sd,
              stroke.transitions_mean, stroke.peak_mean);
  }
  print_coverage_levels(bitmap->coverage);
}

// —— 字符串 → PNG（浏览器侧对照用同一段文字） ——

struct Sample {
  std::string_view label;
  std::string_view text;
  float size;  ///< 逻辑 px
};

const Sample kSamples[] = {
    {"title", "霜天 · 组件画廊", 20.0f},
    {"body", "霜天 · 组件画廊 概览 组件 数据 控制通道", 13.5f},
    {"latin", "Handgloves Illegible 0123456789 abcdef", 13.5f},
};

void render_sample(const FontStack& fonts, const Sample& sample, bool lcd, GridFitMode fit,
                   float device_scale, const std::string& out_dir) {
  const int width = static_cast<int>(static_cast<float>(kWidth) * device_scale);
  const int height = static_cast<int>(static_cast<float>(kHeight) * device_scale);
  Canvas canvas{width, height};
  canvas.clear(kBackground);
  canvas.set_device_scale(device_scale);
  TextRenderer renderer(fonts, device_scale);
  renderer.set_subpixel(lcd);
  renderer.set_grid_fit(fit);
  renderer.draw(canvas, sample.text, Point{6.0f, 6.0f}, sample.size, kForeground);
  const std::string fit_name =
      fit == GridFitMode::Off ? "off" : (fit == GridFitMode::Light ? "light" : "normal");
  const std::string name = std::string(sample.label) + "-" + (lcd ? "lcd" : "gray") + "-" + fit_name;
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba = canvas.to_rgba8();
  const auto status = st::codec::png_write_file(out_dir + "/" + name + ".png", image);
  if (!status.has_value()) {
    st::print("  写 PNG 失败：{}\n", status.error().message);
  } else {
    st::print("  → {}.png\n", name);
  }
}

/// 单字形 PNG：在**物理像素**坐标下渲染一个字形（画布 scale=1），
/// 与浏览器截图（CSS px = 物理 px）逐像素可比。
/// `supersample` 传应用实际使用的档位（`round(device_scale)`）。
void render_glyph_png(const FontStack& fonts, char32_t codepoint, float pixel_size,
                      GridFitMode fit, float supersample, bool lcd, const std::string& out_dir) {
  TextRenderer renderer(fonts, supersample);
  renderer.set_subpixel(lcd);
  renderer.set_grid_fit(fit);
  const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
  std::string utf8;
  st::encode_utf8(codepoint, utf8);
  if (bitmap == nullptr) return;
  const char* fit_name =
      fit == GridFitMode::Off ? "off" : (fit == GridFitMode::Light ? "light" : "normal");
  const bool is_lcd = bitmap->format == st::raster::CoverageFormat::Lcd;
  const int channels = is_lcd ? 3 : 1;
  const int width = bitmap->width;
  const int height = bitmap->height;
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U,
                    255U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t base =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          static_cast<std::size_t>(channels);
      const float alpha =
          is_lcd ? (bitmap->coverage[base] + bitmap->coverage[base + 1] + bitmap->coverage[base + 2]) /
                       3.0f
                 : bitmap->coverage[base];
      // 以**覆盖率**直出灰度（不做颜色合成）：这样像素值本身就是覆盖率
      const auto value = static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
      const std::size_t pixel =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4U;
      image.rgba[pixel + 0] = value;
      image.rgba[pixel + 1] = value;
      image.rgba[pixel + 2] = value;
      image.rgba[pixel + 3] = 255U;
    }
  }
  // 文件名用**码点号**（U+XXXX）：非 ASCII 文件名在 Windows 上受编码链路影响，
  // 量尺工具不该因此假失败（那是量尺自身的缺陷，不是被量对象的）。
  char name_buffer[96];
  const std::string name = std::format("glyph-U{:04X}-{:.2f}px-{}-ss{}",
                                       static_cast<unsigned>(codepoint), pixel_size, fit_name,
                                       static_cast<int>(supersample));
  (void)name_buffer;
  const auto status = st::codec::png_write_file(out_dir + "/" + name + ".png", image);
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
  st::print("device_scale={}  应用口径：supersample=round(scale)={}\n\n", scale,
            std::lround(scale));

  st::print("=== 字形报告（物理像素 20.25 = 13.5 逻辑 @1.5）===\n");
  for (const char32_t codepoint : {U'霜', U'画'}) {
    for (const GridFitMode fit :
         {GridFitMode::Off, GridFitMode::Light, GridFitMode::Normal}) {
      report_glyph(fonts, codepoint, 20.25f, fit, 2.0f);
      st::print("\n");
    }
  }
  st::print("=== 量化对照：三（横画）与川（竖画），ss=1 vs ss=2 ===\n");
  for (const char32_t codepoint : {U'三', U'川'}) {
    report_glyph(fonts, codepoint, 21.0f, GridFitMode::Off, 1.0f);
    st::print("\n");
    report_glyph(fonts, codepoint, 21.0f, GridFitMode::Off, 2.0f);
    st::print("\n");
    report_glyph(fonts, codepoint, 21.0f, GridFitMode::Normal, 2.0f);
    st::print("\n");
  }

  st::print("=== 单字形 PNG（与浏览器逐像素可比）===\n");
  for (const char32_t codepoint : {U'霜', U'川', U'三'}) {
    for (const auto [fit, sup] : {std::pair{GridFitMode::Off, 1.0f}, {GridFitMode::Off, 2.0f},
                                  {GridFitMode::Normal, 2.0f}}) {
      render_glyph_png(fonts, codepoint, 20.25f, fit, sup, true, out_dir);
    }
  }

  st::print("=== 字符串渲染（浏览器对照）===\n");
  for (const Sample& sample : kSamples) {
    st::print("[{}] {}\n", sample.label, sample.text);
    render_sample(fonts, sample, false, GridFitMode::Off, scale, out_dir);
    render_sample(fonts, sample, true, GridFitMode::Off, scale, out_dir);
    render_sample(fonts, sample, true, GridFitMode::Normal, scale, out_dir);
  }
  return 0;
}
