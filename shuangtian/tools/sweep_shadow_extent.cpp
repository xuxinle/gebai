// 扫描 `shadow_map.extent`：用「暗部质心与 Cycles 的距离」做判据。
//
// ## 为什么要扫，而不是继续读代码
//
// 实测霜天的影子**偏了**（暗部质心 x=381，Cycles 是 530），但光方向大致
// 对得上（把光 x 取反后质心跑到 762，两个极端的中点 571 与 530 接近）。
// 所以问题不在方向，而在**投影覆盖范围**：`extent` 越大，同一个 1024²
// 贴图摊到越大的世界范围，纹素越粗 → 影子边缘越糊、暗部重心越容易偏。
//
// 与其猜代码，不如把 extent 当参数扫一遍，让数据说话。
//
// ## 判据
//
// 对每张图算「地面区域（y>460）的暗部质心」。Cycles 的质心是基准，
// 取距离最小的 extent。同时报告暗部像素数（太少的说明影子被打散了、
// 或根本没了——那种"距离小"是假优）。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

constexpr int kWidth = 1100;
constexpr int kHeight = 800;

/// 读脚本文件全文
auto read_text(const char* path) -> std::string {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) return {};
  std::string out;
  char buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, got);
  std::fclose(file);
  return out;
}

/// 把脚本里的 `"extent": <old>` 换成新值。返回是否替换成功。
auto with_extent(const std::string& source, double extent, std::string* out) -> bool {
  const std::string key = "\"extent\": ";
  const auto pos = source.find(key);
  if (pos == std::string::npos) return false;
  const auto value_begin = pos + key.size();
  auto value_end = value_begin;
  while (value_end < source.size() &&
         (std::isdigit(static_cast<unsigned char>(source[value_end])) ||
          source[value_end] == '.')) {
    ++value_end;
  }
  char number[64];
  std::snprintf(number, sizeof(number), "%g", extent);
  *out = source.substr(0, value_begin) + number + source.substr(value_end);
  return true;
}

struct Measure {
  double centroid_x{0.0};
  double centroid_y{0.0};
  double dark_pixels{0.0};
  double mean{0.0};
};

auto measure(const st::raster::Canvas& canvas) -> Measure {
  const auto pixels = canvas.pixels();
  const int width = canvas.width();
  const int height = canvas.height();
  const auto luma_at = [&pixels, width](int x, int y) -> double {
    const auto c = st::math::unpremultiply(
        pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
               static_cast<std::size_t>(x)]);
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
  };

  // 只在地面区域统计（y > height*0.575 ≈ 460），避开物体本身
  const int y_begin = static_cast<int>(static_cast<double>(height) * 0.575);

  // 全图亮度范围 → 动态阈值
  double min_luma = 255.0;
  double max_luma = 0.0;
  double sum = 0.0;
  double count = 0.0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const double v = luma_at(x, y);
      if (v < min_luma) min_luma = v;
      if (v > max_luma) max_luma = v;
      sum += v;
      count += 1.0;
    }
  }
  const double threshold = min_luma + (max_luma - min_luma) * 0.22;

  double sx = 0.0;
  double sy = 0.0;
  double dark = 0.0;
  for (int y = y_begin; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (luma_at(x, y) <= threshold) {
        sx += x;
        sy += y;
        dark += 1.0;
      }
    }
  }
  Measure m;
  m.mean = count > 0.0 ? sum / count : 0.0;
  m.dark_pixels = dark;
  if (dark > 0.0) {
    m.centroid_x = sx / dark;
    m.centroid_y = sy / dark;
  }
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法: sweep_shadow_extent <脚本路径> [基准图]\n");
    return 1;
  }
  const std::string script = read_text(argv[1]);
  if (script.empty()) {
    std::printf("读不到脚本：%s\n", argv[1]);
    return 1;
  }

  // 基准（Cycles）的暗部质心
  double base_x = 530.0;
  double base_y = 608.0;
  bool have_base = false;
  if (argc > 2) {
    const auto png = st::codec::png_read_file(argv[2]);
    if (png.has_value()) {
      // 直接用基准图的像素算质心（不经过 Canvas，少一层假设）
      const int width = static_cast<int>(png->width);
      const int height = static_cast<int>(png->height);
      // `PngImage::rgba` 恒为 RGBA8（非预乘），所以通道数固定 4
      constexpr int kChannels = 4;
      const auto luma_at = [&](int x, int y) -> double {
        const auto base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x)) *
                          kChannels;
        const double r = png->rgba[base];
        const double g = png->rgba[base + 1];
        const double b = png->rgba[base + 2];
        return 0.2126 * r + 0.7152 * g + 0.0722 * b;
      };
      double min_luma = 255.0;
      double max_luma = 0.0;
      double sum = 0.0;
      double count = 0.0;
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const double v = luma_at(x, y);
          if (v < min_luma) min_luma = v;
          if (v > max_luma) max_luma = v;
          sum += v;
          count += 1.0;
        }
      }
      const double threshold = min_luma + (max_luma - min_luma) * 0.22;
      const int y_begin = static_cast<int>(static_cast<double>(height) * 0.575);
      double sx = 0.0;
      double sy = 0.0;
      double dark = 0.0;
      for (int y = y_begin; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          if (luma_at(x, y) <= threshold) {
            sx += x;
            sy += y;
            dark += 1.0;
          }
        }
      }
      if (dark > 0.0) {
        base_x = sx / dark;
        base_y = sy / dark;
      }
      have_base = true;
      std::printf("基准(%s): 质心=(%.1f, %.1f) 暗部=%.0f 均值=%.1f\n", argv[2], base_x, base_y,
                  dark, count > 0.0 ? sum / count : 0.0);
    } else {
      std::printf("基准图读不到（用缺省 530,608）：%s\n", argv[2]);
    }
  }
  if (!have_base) {
    std::printf("基准: 质心=(%.1f, %.1f)（硬编码）\n", base_x, base_y);
  }

  std::printf("\n%8s %10s %10s %10s %8s %8s\n", "extent", "质心x", "质心y", "暗部像素", "均值",
              "距基准");

  const std::vector<double> extents = {3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 11.0, 13.0};
  double best_extent = 0.0;
  double best_distance = 1e18;
  for (const double extent : extents) {
    std::string patched;
    if (!with_extent(script, extent, &patched)) {
      std::printf("脚本里找不到 \"extent\": ——无法替换\n");
      return 1;
    }
    const auto spec = st::raster::parse_scene_spec_text(patched);
    if (!spec.has_value()) {
      std::printf("%8g 解析失败：%s\n", extent, spec.error().message.c_str());
      continue;
    }
    // ## 必须与 `bpy_render.cpp` 用**完全相同的口径**
    //
    // 第一版漏了 `tone_map = Agx`，扫描出来的暗部像素只有 6500~9400，
    // 而 Cycles 基准是 117595——差 13 倍。因为一个做了 AgX、另一个没做，
    // 亮度分布完全不同，「动态阈值」选中的“暗部”根本不是一个东西。
    //
    // 这类错误比代码 bug 更危险：数字看起来很有规律（质心 x 随 extent
    // 单调递增），很容易被误读成“找到了规律”。
    st::raster::SceneSpec tuned = *spec;
    tuned.lighting.tone_map = st::raster::ToneMap::Agx;
    st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
    const auto status =
        st::raster::render_scene(tuned, canvas, st::math::Color::rgba(40, 40, 40, 255));
    if (!status.has_value()) {
      std::printf("%8g 渲染失败：%s\n", extent, status.error().message.c_str());
      continue;
    }
    const auto m = measure(canvas);
    const double distance = std::hypot(m.centroid_x - base_x, m.centroid_y - base_y);
    if (m.dark_pixels > 2000.0 && distance < best_distance) {
      best_distance = distance;
      best_extent = extent;
    }
    std::printf("%8g %10.1f %10.1f %10.0f %8.1f %8.1f\n", extent, m.centroid_x, m.centroid_y,
                m.dark_pixels, m.mean, distance);
  }
  std::printf("\n最优 extent = %g（距基准 %.1f 像素）\n", best_extent, best_distance);
  return 0;
}
