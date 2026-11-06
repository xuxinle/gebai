// BRDF 层对照：把「非 BRDF 的偏差」全部对齐之后，逐像素量残留。
//
// ## 为什么要先对齐四件事
//
// 与真 Blender 的差可以分成两层：**参数/管线差**（改设置就能消掉）与
// **BRDF 差**（要改公式）。混在一起量，得到的数没法指导行动——
// 你不知道该改设置还是该改公式。
//
// 本工具把下面四件「非 BRDF」的事按真 Blender 的口径强制对齐：
//
// | # | 项 | 真 Blender | 本框架默认 | 对齐动作 |
// |---|---|---|---|---|
// | ① | world | 脚本没设 = 无环境光 | 默认 world 线性 0.05（IBL 1.0） | ibl_strength = 0 |
// | ② | ambient 保底 | 0 | 0.35 | ambient = 0 |
// | ③ | SUN energy 标度 | 1 W/m² = irradiance 1.0 | `energy/1000 × 1.0` | diffuse × 1000 |
// | ④ | 色调映射 | Standard = 纯 sRGB 编码 | AgX | tone_map = None |
//
// 对齐之后剩下的差**只可能**来自 BRDF / 光栅化（采样、法线、精度）。
//
// ## 两层口径
//
// · **软件腿**（`ST_SCENE3D_BACKEND=software`）：1 样本/像素，可与 Blender
//   的单样本几何边缘逐像素比
// · **GPU 腿**：走 MSAA，边缘必然不同（与 three.js 对标时同一个结论）
//
// 用法：bpy_brdf_compare <场景.py> <真值.png> <输出前缀> [W] [H]
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

/// 真值 PNG 的灰度（只比亮度：本阶段的目的是看 BRDF 的整体形状，
/// 而不是通道级色偏；色偏由受控平面单色场景单独验）。
struct GrayImage {
  int width{0};
  int height{0};
  std::vector<float> luma{};
};

auto load_gray(const std::string& path) -> GrayImage {
  GrayImage out{};
  auto decoded = st::codec::png_read_file(path);
  if (!decoded.has_value()) return out;
  const auto& png = *decoded;
  out.width = static_cast<int>(png.width);
  out.height = static_cast<int>(png.height);
  if (out.width <= 0 || out.height <= 0) return out;
  out.luma.resize(static_cast<std::size_t>(out.width) * out.height);
  // `PngImage::rgba` 恒为非预乘 RGBA8，长度 width*height*4
  for (std::size_t i = 0; i < out.luma.size(); ++i) {
    const float r = static_cast<float>(png.rgba[i * 4 + 0]);
    const float g = static_cast<float>(png.rgba[i * 4 + 1]);
    const float b = static_cast<float>(png.rgba[i * 4 + 2]);
    out.luma[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  }
  return out;
}

auto percentile(std::vector<float> values, double t) -> float {
  if (values.empty()) return 0.0f;
  const auto idx = static_cast<std::size_t>(t * double(values.size() - 1));
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(idx),
                   values.end());
  return values[idx];
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::printf("用法：bpy_brdf_compare <场景.py> <真值.png> <输出前缀> [W] [H]\n");
    return 2;
  }
  const std::string scene_path = argv[1];
  const std::string truth_path = argv[2];
  const std::string prefix = argv[3];
  const int width = argc > 4 ? std::atoi(argv[4]) : 400;
  const int height = argc > 5 ? std::atoi(argv[5]) : 400;

  const auto script = read_file(scene_path);
  auto spec = st::raster::parse_scene_spec_text(script);
  if (!spec.has_value()) {
    std::printf("解析失败：%s\n", spec.error().message.c_str());
    return 1;
  }

  // ── 三项按真 Cycles 口径对齐 ─────────────────────────────────────
  //
  // ⚠ `diffuse` **不再 ×1000**：2026-11-06 修掉了 `bpy_runtime.cpp` 里多余的
  // `/1000`，标度现在是量纲 1:1。再乘就是多乘——实测会直接到 255 全白。
  spec->lighting.ibl_strength = 0.0f;         // ① 脚本没设 world = 无环境光
  spec->lighting.ambient = 0.0f;              // ② 无 ambient 保底
  spec->lighting.tone_map = st::raster::ToneMap::None;  // ③ Standard

  st::raster::Canvas canvas{width, height, 1.0f};
  const auto background = st::math::Color::rgba(0, 0, 0, 255);
  const auto status = st::raster::render_scene(*spec, canvas, background);
  if (!status) {
    std::printf("渲染失败：%s\n", status.error().message.c_str());
    return 1;
  }

  // 落图
  {
    const auto pixels = canvas.pixels();
    st::codec::PngImage image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      const auto c = st::math::unpremultiply(pixels[i]);
      image.rgba[i * 4 + 0] = c.r;
      image.rgba[i * 4 + 1] = c.g;
      image.rgba[i * 4 + 2] = c.b;
      image.rgba[i * 4 + 3] = c.a;
    }
    (void)st::codec::png_write_file(prefix + "_aligned.png", image);
  }

  const auto truth = load_gray(truth_path);
  if (truth.width != width || truth.height != height) {
    std::printf("尺寸不符：真值 %dx%d，本框架 %dx%d\n", truth.width, truth.height, width,
                height);
    return 1;
  }

  const auto pixels = canvas.pixels();
  std::vector<float> diffs;
  diffs.reserve(pixels.size());
  double sum_truth = 0.0;
  double sum_fw = 0.0;
  std::size_t subject = 0;
  std::size_t over8 = 0;
  std::size_t over16 = 0;
  std::size_t over32 = 0;
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const auto c = st::math::unpremultiply(pixels[i]);
    const float fw = 0.2126f * float(c.r) + 0.7152f * float(c.g) + 0.0722f * float(c.b);
    const float tv = truth.luma[i];
    if (tv <= 1.0f) continue;   // 只比真值里的主体像素
    ++subject;
    sum_truth += double(tv);
    sum_fw += double(fw);
    const float d = std::fabs(fw - tv);
    diffs.push_back(d);
    if (d > 8.0f) ++over8;
    if (d > 16.0f) ++over16;
    if (d > 32.0f) ++over32;
  }
  if (subject == 0) {
    std::printf("真值里没有主体像素（全黑？）\n");
    return 1;
  }
  const double denom = double(subject);
  std::printf("主体 %zu px\n", subject);
  std::printf("  真值均值 %.2f   本框架均值 %.2f   差 %+.2f\n", sum_truth / denom,
              sum_fw / denom, (sum_fw - sum_truth) / denom);
  std::printf("  |Δ| p50=%.1f p90=%.1f p99=%.1f max=%.1f\n", double(percentile(diffs, 0.50)),
              double(percentile(diffs, 0.90)), double(percentile(diffs, 0.99)),
              double(percentile(diffs, 1.0)));
  std::printf("  >8: %.1f%%   >16: %.1f%%   >32: %.1f%%\n",
              100.0 * double(over8) / denom, 100.0 * double(over16) / denom,
              100.0 * double(over32) / denom);
  return 0;
}
