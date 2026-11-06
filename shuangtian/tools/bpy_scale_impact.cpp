// 标度改动的**影响面**预演：不改代码，先用覆盖参数模拟「去掉 /1000」后每个现有
// bpy 场景会变成什么样。
//
// ## 为什么要先预演
//
// `energy / 1000` 是一个 20 行注释在解释、代码与注释自相矛盾的常数。
// 去掉它是有依据的（Blender 的 SUN energy 单位就是 W/m² = 辐照度，同量纲）。
// 但**去掉之后所有存量 bpy 场景会变亮 1000 倍**——那可能是全场过曝，也可能是
// 终于对了。改之前必须知道是哪种。
//
// ## 判据
//
// · 主体均值落在 [40, 200] 且全白占比 < 5%  → 这个场景**修完更好**
// · 全白占比 > 50%                        → 该场景**需要重新调灯**（energy 写太大）
// · 主体均值 < 20                          → 该场景灯光方向/位置本身有问题
//
// 用法：bpy_scale_impact <目录或文件...>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

struct Stats {
  double mean{0.0};
  double saturated_pct{0.0};
  std::size_t subject{0};
};

auto stats_of(const st::raster::Canvas& canvas) -> Stats {
  const auto pixels = canvas.pixels();
  double sum = 0.0;
  std::size_t n = 0;
  std::size_t sat = 0;
  for (const auto& premul : pixels) {
    const auto c = st::math::unpremultiply(premul);
    const double luma = 0.2126 * double(c.r) + 0.7152 * double(c.g) + 0.0722 * double(c.b);
    // 主体：与背景（调用方传的 13,13,13）显著不同
    if (std::fabs(luma - 13.0) < 2.0) continue;
    ++n;
    sum += luma;
    if (luma >= 254.0) ++sat;
  }
  Stats s{};
  s.subject = n;
  s.mean = n ? sum / double(n) : 0.0;
  s.saturated_pct = n ? 100.0 * double(sat) / double(n) : 0.0;
  return s;
}

void render_and_report(const std::string& path, double scale, const char* tag) {
  const auto script = read_file(path);
  auto spec = st::raster::parse_scene_spec_text(script);
  if (!spec.has_value()) {
    std::printf("    %-14s 解析失败：%s\n", tag, spec.error().message.c_str());
    return;
  }
  spec->lighting.diffuse = static_cast<float>(double(spec->lighting.diffuse) * scale);
  for (auto& light : spec->lighting.extra_lights) {
    light.intensity = static_cast<float>(double(light.intensity) * scale);
  }
  st::raster::Canvas canvas{320, 240, 1.0f};
  const auto status = st::raster::render_scene(
      *spec, canvas, st::math::Color::rgba(13, 13, 13, 255));
  if (!status) {
    std::printf("    %-14s 渲染失败：%s\n", tag, status.error().message.c_str());
    return;
  }
  const auto s = stats_of(canvas);
  const char* verdict = "?";
  if (s.saturated_pct > 50.0) {
    verdict = "过曝，需重调 energy";
  } else if (s.mean >= 40.0 && s.mean <= 200.0) {
    verdict = "落在正常曝光";
  } else if (s.mean < 20.0) {
    verdict = "偏暗";
  }
  std::printf("    %-14s 主体 %6zu  均值 %6.1f  全白 %5.1f%%  → %s\n", tag, s.subject, s.mean,
              s.saturated_pct, verdict);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法：bpy_scale_impact <场景.py | 目录> ...\n");
    return 2;
  }
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path p(argv[i]);
    if (std::filesystem::is_directory(p)) {
      for (const auto& entry : std::filesystem::directory_iterator(p)) {
        if (entry.is_regular_file() && entry.path().extension() == ".py") {
          files.push_back(entry.path().string());
        }
      }
    } else {
      files.push_back(argv[i]);
    }
  }
  for (const auto& f : files) {
    std::printf("[%s]\n", f.c_str());
    render_and_report(f, 1.0, "当前(/1000)");
    render_and_report(f, 1000.0, "修正后(1:1)");
  }
  std::printf(
      "\n口径：主体 = 与背景 rgb(13,13,13) 相差 >2 的像素；全白 = luma>=254\n"
      "注意：本工具只改灯光标度，不动 ambient / IBL / 色调映射，\n"
      "      所以数字是「只修这一项」的效果，不是最终出片。\n");
  return 0;
}
