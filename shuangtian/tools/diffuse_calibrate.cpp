// 标定 bpy 的 energy → diffuse 换算系数，用三路参照图做基准。
//
// ## 为什么需要标定而不是"按物理算"
//
// `Lighting.diffuse` 是**相对权重**，没有物理单位；而 Blender 的 SUN
// `energy` 是 W/m²。两者之间没有可从公式推出的关系——只能**实测标定**。
//
// 验证标定是否合理的方法：拿真 Blender / 真 three.js 的渲染统计做基准，
// 看哪一档最接近。
//
// ## 实测发现的偏差
//
// 原换算 `diffuse = energy / 1000 × 0.75`：脚本写 `energy=2600` → `1.95`。
// 三路对照结果：
//
// | | 均值 | 暗部% |
// |---|---|---|
// | 本框架（diffuse=1.95） | **91.6** | **0.0** |
// | 真 Blender（energy=3.0） | 75.7 | 7.0 |
// | 真 three.js | 76.9 | 9.8 |
//
// 两个真渲染器彼此只差 1.2，说明「76 左右 + 7~10% 暗部」是这个场景的正确答案。
// 所以原系数**偏大约 2 倍**。
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

struct Hist {
  double mean{0.0};
  double dark{0.0};
  double mid{0.0};
  double bright{0.0};
  double max_luma{0.0};
  std::vector<double> bins;
};

auto analyze(const st::codec::PngImage& image) -> Hist {
  Hist out;
  out.bins.assign(64, 0.0);
  const std::size_t pixels = std::size_t(image.width) * image.height;
  if (pixels == 0) return out;
  double sum = 0.0;
  std::size_t dark = 0;
  std::size_t mid = 0;
  std::size_t bright = 0;
  for (std::size_t i = 0; i < pixels; ++i) {
    const double r = image.rgba[i * 4 + 0];
    const double g = image.rgba[i * 4 + 1];
    const double b = image.rgba[i * 4 + 2];
    const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    sum += luma;
    if (luma > out.max_luma) out.max_luma = luma;
    if (luma < 64.0) ++dark;
    else if (luma <= 192.0) ++mid;
    else ++bright;
    const std::size_t bin = static_cast<std::size_t>(luma) / 4U;
    out.bins[bin < 64 ? bin : 63] += 1.0;
  }
  out.mean = sum / double(pixels);
  out.dark = double(dark) / double(pixels);
  out.mid = double(mid) / double(pixels);
  out.bright = double(bright) / double(pixels);
  for (auto& value : out.bins) value /= double(pixels);
  return out;
}

auto overlap(const Hist& a, const Hist& b) -> double {
  double sum = 0.0;
  for (std::size_t i = 0; i < a.bins.size() && i < b.bins.size(); ++i) {
    sum += std::sqrt(a.bins[i] * b.bins[i]);
  }
  return sum;
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
  const st::math::Color background = st::math::Color::rgba(16, 17, 21, 255);
  const auto status = st::raster::render_scene(spec, canvas, background);
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
  const std::string script_path = argc > 1 ? argv[1] : "examples/threelab/scenes/bpy_watch.py";
  const std::string blender_path = argc > 2 ? argv[2] : "blender_reference.png";

  const auto script = read_file(script_path);
  if (script.empty()) {
    std::printf("读不到脚本：%s\n", script_path.c_str());
    return 1;
  }
  auto ref_result = st::codec::png_read_file(blender_path);
  if (!ref_result) {
    std::printf("读不到 Blender 参照：%s\n", ref_result.error().message.c_str());
    return 1;
  }
  const Hist ref = analyze(*ref_result);
  std::printf("参照（真 Blender）：均值 %.1f  暗 %.1f%%  中 %.1f%%  亮 %.1f%%  最亮 %.0f\n\n",
              ref.mean, ref.dark * 100.0, ref.mid * 100.0, ref.bright * 100.0,
              ref.max_luma);

  std::printf("%-10s %-8s %-8s %-8s %-8s %-8s %-8s\n", "diffuse", "均值", "暗部%", "中%",
              "亮%", "最亮", "重叠度");
  std::printf("%s\n", std::string(70, '-').c_str());

  double best_overlap = -1.0;
  float best_diffuse = 0.0f;
  double best_mean_error = 1e9;
  float best_by_mean = 0.0f;

  // 从 0.4 扫到 2.0：覆盖"原系数 1.95"到"明显偏暗"
  for (int step = 4; step <= 20; ++step) {
    const float diffuse = static_cast<float>(step) * 0.1f;
    auto spec = st::raster::parse_scene_spec_text(script);
    if (!spec.has_value()) {
      std::printf("解析失败：%s\n", spec.error().message.c_str());
      return 1;
    }
    spec->lighting.diffuse = diffuse;
    st::codec::PngImage image;
    if (!render(*spec, image)) continue;
    const Hist mine = analyze(image);
    const double ov = overlap(mine, ref);
    std::printf("%-10.2f %-8.1f %-8.1f %-8.1f %-8.1f %-8.0f %-8.3f\n", double(diffuse),
                mine.mean, mine.dark * 100.0, mine.mid * 100.0, mine.bright * 100.0,
                mine.max_luma, ov);
    if (ov > best_overlap) {
      best_overlap = ov;
      best_diffuse = diffuse;
    }
    const double mean_error = std::fabs(mine.mean - ref.mean);
    if (mean_error < best_mean_error) {
      best_mean_error = mean_error;
      best_by_mean = diffuse;
    }
  }

  std::printf("\n直方图最优：diffuse = %.2f（重叠度 %.3f）\n", double(best_diffuse), best_overlap);
  std::printf("均值最优：  diffuse = %.2f（均值差 %.1f）\n", double(best_by_mean),
              best_mean_error);
  std::printf("\n两个判据不一致时以**直方图**为准：它同时对亮度和形状敏感，\n");
  std::printf("而均值只看整体亮暗（可能用“更平的分布”凑出接近的均值）。\n");
  return 0;
}
