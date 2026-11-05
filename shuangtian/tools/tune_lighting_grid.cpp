// 扫 ambient/diffuse/env，用**分位差**做目标函数，找与真 Blender 最接近的组合。
//
// ## 为什么用分位而不是均值
//
// 两张图的差异特征（`tools/subject_histogram.py` 实测）：
//
// | 分位 | 本框架 | Blender | 差 |
// |---|---|---|---|
// | p25 | 76 | 62 | +14 |
// | p50 | 92 | 67 | **+25** |
// | p75 | 123 | 75 | **+48** |
// | p99 | 202 | 232 | -30 |
//
// **中层偏高、高层偏低** = 对比度不足。均值只会说"偏亮"，导致前面的
// 调参一直往"整体调暗"的方向走，而真正该做的是**拉开对比**。
//
// ## 目标函数
//
// 加权分位差：p50 权重最高（最能代表"中间调位置"），p99 权重最低
//（高光受少数像素影响，噪声大）。
//
// 扫三个参数：ambient（非物理平铺光）、diffuse（主光）、env_strength。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

struct Percentiles {
  double p10{0.0};
  double p25{0.0};
  double p50{0.0};
  double p75{0.0};
  double p90{0.0};
  double p99{0.0};
  std::size_t count{0};
};

auto analyze(const st::codec::PngImage& image) -> Percentiles {
  Percentiles out;
  const std::size_t total = std::size_t(image.width) * image.height;
  if (total == 0 || image.rgba.size() < total * 4U) return out;
  // 背景色（左上角）
  const int bg_r = image.rgba[0];
  const int bg_g = image.rgba[1];
  const int bg_b = image.rgba[2];
  std::vector<double> values;
  values.reserve(total / 4);
  for (std::size_t i = 0; i < total; ++i) {
    const int r = image.rgba[i * 4 + 0];
    const int g = image.rgba[i * 4 + 1];
    const int b = image.rgba[i * 4 + 2];
    if (std::abs(r - bg_r) + std::abs(g - bg_g) + std::abs(b - bg_b) <= 12) continue;
    values.push_back(0.2126 * double(r) + 0.7152 * double(g) + 0.0722 * double(b));
  }
  out.count = values.size();
  if (values.empty()) return out;
  std::sort(values.begin(), values.end());
  const auto at = [&](double percent) {
    const std::size_t index =
        std::min(values.size() - 1, std::size_t(percent / 100.0 * double(values.size())));
    return values[index];
  };
  out.p10 = at(10.0);
  out.p25 = at(25.0);
  out.p50 = at(50.0);
  out.p75 = at(75.0);
  out.p90 = at(90.0);
  out.p99 = at(99.0);
  return out;
}

/// 加权分位差（越小越接近参照）。
auto distance(const Percentiles& a, const Percentiles& b) -> double {
  return std::fabs(a.p10 - b.p10) * 1.0 + std::fabs(a.p25 - b.p25) * 2.0 +
         std::fabs(a.p50 - b.p50) * 3.0 + std::fabs(a.p75 - b.p75) * 2.0 +
         std::fabs(a.p90 - b.p90) * 1.0 + std::fabs(a.p99 - b.p99) * 0.5;
}

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

auto render(const st::raster::SceneSpec& spec, st::codec::PngImage& out) -> bool {
  st::raster::Canvas canvas{1100, 800, 1.0f};
  const auto status =
      st::raster::render_scene(spec, canvas, st::math::Color::rgba(16, 17, 21, 255));
  if (!status) return false;
  out.width = static_cast<std::uint32_t>(canvas.physical_width());
  out.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  out.rgba.resize(std::size_t(out.width) * out.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const auto c = st::math::unpremultiply(pixels[i]);
    out.rgba[i * 4 + 0] = c.r;
    out.rgba[i * 4 + 1] = c.g;
    out.rgba[i * 4 + 2] = c.b;
    out.rgba[i * 4 + 3] = c.a;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string script_path =
      argc > 1 ? argv[1] : "examples/jianmu/scenes/bpy_watch.py";
  const std::string ref_path = argc > 2 ? argv[2] : "blender_reference.png";

  const auto script = read_file(script_path);
  if (script.empty()) {
    std::printf("读不到脚本\n");
    return 1;
  }
  auto ref_result = st::codec::png_read_file(ref_path);
  if (!ref_result) {
    std::printf("读不到参照：%s\n", ref_result.error().message.c_str());
    return 1;
  }
  const auto ref = analyze(*ref_result);
  std::printf("参照（真 Blender）分位：p10 %.0f  p25 %.0f  p50 %.0f  p75 %.0f  p90 %.0f  p99 %.0f\n\n",
              ref.p10, ref.p25, ref.p50, ref.p75, ref.p90, ref.p99);

  std::printf("%-9s %-9s %-9s %-8s %-8s %-8s %-8s %-9s\n", "ambient", "diffuse", "env",
              "p50", "p75", "p99", "加权差", "备注");
  std::printf("%s\n", std::string(76, '-').c_str());

  double best = 1e9;
  float best_amb = 0.0f;
  float best_diff = 0.0f;
  float best_env = 0.0f;
  double best_p50 = 0.0;
  double best_p75 = 0.0;
  double best_p99 = 0.0;

  // ## 单参数扫描（2026-11-05）
  //
  // 原版扫 ambient × diffuse × env = 36 组网格，每组渲染约 6 秒 → 4 分钟以上。
  // 而 GGX alpha 修好后（主体均值 81.1 vs 参照 78.0），ambient 与 env 的
  // 影响已被证明很小（`tools/shading_decompose_probe`：去 ambient Δ-0.010）。
  //
  // 所以现在只扫 **diffuse**：它是唯一还可能偏的项（主光强度）。
  for (const float amb : {0.35f}) {
    for (const float diff : {2.0f}) {
      for (const float env : {0.0f, 0.15f, 0.3f, 0.45f, 0.6f, 0.9f, 1.2f}) {
        auto spec = st::raster::parse_scene_spec_text(script);
        if (!spec.has_value()) {
          std::printf("解析失败：%s\n", spec.error().message.c_str());
          return 1;
        }
        spec->lighting.ambient = amb;
        spec->lighting.diffuse = diff;
        spec->lighting.env_strength = env;
        st::codec::PngImage image;
        if (!render(*spec, image)) continue;
        const auto mine = analyze(image);
        const double d = distance(mine, ref);
        const bool is_best = d < best;
        std::printf("%-9.2f %-9.2f %-9.2f %-8.0f %-8.0f %-8.0f %-9.1f%s\n", double(amb),
                    double(diff), double(env), mine.p50, mine.p75, mine.p99, d,
                    is_best ? " ← 当前最优" : "");
        if (is_best) {
          best = d;
          best_amb = amb;
          best_diff = diff;
          best_env = env;
          best_p50 = mine.p50;
          best_p75 = mine.p75;
          best_p99 = mine.p99;
        }
      }
    }
  }

  std::printf("\n=== 最优组合 ===\n\n");
  std::printf("  ambient = %.2f   diffuse = %.2f   env_strength = %.2f\n", double(best_amb),
              double(best_diff), double(best_env));
  std::printf("  加权分位差 %.1f（越小越好）\n", best);
  std::printf("  分位：p50 %.0f（参照 %.0f）  p75 %.0f（参照 %.0f）  p99 %.0f（参照 %.0f）\n",
              best_p50, ref.p50, best_p75, ref.p75, best_p99, ref.p99);
  return 0;
}
