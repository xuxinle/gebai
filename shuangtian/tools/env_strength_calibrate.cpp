// 标定 env_strength：扫几档，找直方图最接近 three.js 参照的值。
//
// ## 为什么必须扫而不是"看着调"
//
// 「与 three 对齐」有两个可调旋钮（`env_strength`、`ambient`），而它们
// **都能让画面变亮**——随便调大一个都会"看起来更接近"，但可能让亮度分布
// 形状更差。只有拿**分布指标**（直方图重叠度）扫一遍才知道哪个值最好。
//
// ## 为什么用直方图重叠度而不是均值差
//
// 均值差只看"整体亮暗"。实测发现本框架中间调占 99.9%、three 只占 89.3%
// ——两者**均值可以接近，而分布形状完全不同**（一个平、一个有层次）。
// 直方图重叠度对这种差别敏感（Bhattacharyya 系数）。
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
  std::vector<double> bins;
  double mean{0.0};
  double dark{0.0};
  double bright{0.0};
  double max_luma{0.0};
};

auto analyze(const st::codec::PngImage& image) -> Hist {
  Hist out;
  out.bins.assign(64, 0.0);
  const std::size_t pixels = std::size_t(image.width) * image.height;
  if (pixels == 0) return out;
  double sum = 0.0;
  std::size_t dark = 0;
  std::size_t bright = 0;
  for (std::size_t i = 0; i < pixels; ++i) {
    const double r = image.rgba[i * 4 + 0];
    const double g = image.rgba[i * 4 + 1];
    const double b = image.rgba[i * 4 + 2];
    const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    sum += luma;
    if (luma > out.max_luma) out.max_luma = luma;
    if (luma < 64.0) ++dark;
    if (luma > 192.0) ++bright;
    const std::size_t bin = static_cast<std::size_t>(luma) / 4U;
    out.bins[bin < 64 ? bin : 63] += 1.0;
  }
  out.mean = sum / double(pixels);
  out.dark = double(dark) / double(pixels);
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

/// 把场景渲成 PNG 的字节（走框架自己的编码路径）。
auto render_to_png(const std::string& script, const st::raster::SceneSpec& spec,
                   st::codec::PngImage& out) -> bool {
  (void)script;
  st::raster::Canvas canvas{1100, 800, 1.0f};
  const auto background = st::math::Color::rgba(18, 20, 24, 255);
  const auto status = st::raster::render_scene(spec, canvas, background);
  if (!status) {
    std::printf("  渲染失败：%s\n", status.error().message.c_str());
    return false;
  }
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

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string script_path =
      argc > 1 ? argv[1] : "examples/jianmu/scenes/bpy_watch.py";
  const std::string reference_path = argc > 2 ? argv[2] : "reference_three_none.png";

  const auto script = read_file(script_path);
  if (script.empty()) {
    std::printf("读不到脚本：%s\n", script_path.c_str());
    return 1;
  }
  auto ref_result = st::codec::png_read_file(reference_path);
  if (!ref_result) {
    std::printf("读不到参照图：%s\n", ref_result.error().message.c_str());
    return 1;
  }
  const Hist ref = analyze(*ref_result);
  std::printf("参照（three.js）：均值 %.1f  暗 %.1f%%  亮 %.1f%%  最亮 %.0f\n\n", ref.mean,
              ref.dark * 100.0, ref.bright * 100.0, ref.max_luma);

  std::printf("%-14s %-8s %-8s %-8s %-8s %-8s\n", "env_strength", "均值", "暗部%", "亮部%",
              "最亮", "重叠度");
  std::printf("%s\n", std::string(64, '-').c_str());

  double best_overlap = -1.0;
  float best_env = 0.0f;
  for (const float env : {0.0f, 0.3f, 0.6f, 0.9f, 1.2f, 1.6f}) {
    auto spec = st::raster::parse_scene_spec_text(script);
    if (!spec.has_value()) {
      std::printf("解析失败：%s\n", spec.error().message.c_str());
      return 1;
    }
    spec->lighting.env_strength = env;
    st::codec::PngImage image;
    if (!render_to_png(script, *spec, image)) continue;
    const Hist mine = analyze(image);
    const double ov = overlap(mine, ref);
    std::printf("%-14.2f %-8.1f %-8.1f %-8.1f %-8.0f %-8.3f\n", double(env), mine.mean,
                mine.dark * 100.0, mine.bright * 100.0, mine.max_luma, ov);
    if (ov > best_overlap) {
      best_overlap = ov;
      best_env = env;
    }
  }

  std::printf("\n最佳 env_strength = %.2f（重叠度 %.3f）\n", double(best_env), best_overlap);
  std::printf("注：重叠度 1.0 = 分布完全相同；实际受光照积分差异限制，能达到的上限见上表。\n");
  return 0;
}
