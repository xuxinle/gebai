// 对齐基准：把同一份场景用本框架渲出来（三路之一）。
//
// ## 用法
//
//     align_bench_probe <场景文件> [输出.png]
//
// 自动识别 .js（three_script）与 .py（bpy_script）。
//
// ## 为什么单独一个探针
//
// 现有的 `bpy_watch_probe` / `bpy_script_probe` 都绑定了特定场景与判据。
// 对齐验证需要一个**中性的出图工具**：给它任何场景，出一张同参数的 PNG，
// 好与真 three.js / 真 Blender 的图并排看。
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

auto save_png(const st::raster::Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const st::math::Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法：align_bench_probe <场景.js|场景.py> [输出.png]\n");
    return 2;
  }
  const std::string scene_path = argv[1];
  const std::string out_path = argc > 2 ? argv[2] : "align_bench_framework.png";

  const auto script = read_file(scene_path);
  if (script.empty()) {
    std::printf("读不到场景：%s\n", scene_path.c_str());
    return 1;
  }

  const auto spec = st::raster::parse_scene_spec_text(script);
  if (!spec.has_value()) {
    std::printf("解析失败：%s\n", spec.error().message.c_str());
    return 1;
  }

  // 与参照图同尺寸（1100×800）
  st::raster::Canvas canvas{1100, 800, 1.0f};
  // 背景必须是**线性值**（`begin_frame` 的契约）。
  // 场景里的 `0.05 中性灰` 是线性值，经 AgX+sRGB 后约 rgb(71,76,84)——
  // 与三路一致。
  const auto background = st::math::Color::rgba(13, 13, 13, 255);
  const auto status = st::raster::render_scene(*spec, canvas, background);
  if (!status) {
    std::printf("渲染失败：%s\n", status.error().message.c_str());
    return 1;
  }

  if (!save_png(canvas, out_path)) {
    std::printf("写图失败：%s\n", out_path.c_str());
    return 1;
  }

  // 简短统计（便于与参照图对照）
  const auto pixels = canvas.pixels();
  const auto bg = st::math::unpremultiply(pixels[0]);
  std::size_t subject = 0;
  double sum = 0.0;
  for (const auto& premul : pixels) {
    const auto c = st::math::unpremultiply(premul);
    const int diff = std::abs(int(c.r) - int(bg.r)) + std::abs(int(c.g) - int(bg.g)) +
                     std::abs(int(c.b) - int(bg.b));
    if (diff <= 12) continue;
    ++subject;
    sum += 0.2126 * double(c.r) + 0.7152 * double(c.g) + 0.0722 * double(c.b);
  }
  const double mean = subject > 0 ? sum / double(subject) : 0.0;
  std::printf("已出图 %s\n", out_path.c_str());
  std::printf("  主体像素 %zu（%.1f%%）  主体均值 %.1f\n", subject,
              100.0 * double(subject) / double(pixels.size()), mean);
  std::printf("  物体 %zu 个\n", spec->objects.size());
  return 0;
}
