// 量：屏幕空间 AO 能把主体亮度拉近多少。
//
// ## 为什么先量再做
//
// 剩余差异（主体均值 99.9 vs 真 Blender 78.1）我判断主要来自 AO——
// 但这是**猜**。AO 在 CPU 上逐像素做邻域采样，代价不低（1100×800 × N 次采样）。
//
// 所以先用一个**离线原型**量效果：拿已渲染好的图的深度信息做 AO，
// 看主体均值能降到多少。若降不到 80 附近，说明 AO 不是主因，不该做。
//
// ## 怎么拿到深度
//
// 这里不接渲染器内部——用**亮度梯度**做代理：AO 的观感特征是
// 「几何交界处变暗」，而亮度的高频梯度正好标出那些交界。
//
// 这是粗糙代理，但它足以回答一个关键问题：**「让交界处变暗」这个方向
// 能带来多少均值下降**。若答案是"很少"，那就别做 AO。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/codec/png.hpp"

namespace {

struct Stats {
  double mean{0.0};
  double dark{0.0};
  double mid{0.0};
  double bright{0.0};
};

auto analyze_subject(const st::codec::PngImage& image, int radius) -> Stats {
  Stats out;
  const int w = static_cast<int>(image.width);
  const int h = static_cast<int>(image.height);
  if (w <= 0 || h <= 0) return out;

  // 背景色（四角中位数）
  int bg[3] = {0, 0, 0};
  {
    std::vector<int> values[3];
    for (int c = 0; c < 3; ++c) {
      const int corners[4][2] = {{0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}};
      for (const auto& corner : corners) {
        const std::size_t at =
            (std::size_t(corner[1]) * image.width + std::size_t(corner[0])) * 4;
        values[c].push_back(image.rgba[at + std::size_t(c)]);
      }
      std::sort(values[c].begin(), values[c].end());
      bg[c] = values[c][values[c].size() / 2];
    }
  }

  const auto luma_at = [&](int x, int y) {
    const std::size_t at = (std::size_t(y) * image.width + std::size_t(x)) * 4;
    return 0.2126 * image.rgba[at] + 0.7152 * image.rgba[at + 1] + 0.0722 * image.rgba[at + 2];
  };
  const auto is_subject = [&](int x, int y) {
    const std::size_t at = (std::size_t(y) * image.width + std::size_t(x)) * 4;
    const int diff = std::abs(int(image.rgba[at]) - bg[0]) +
                     std::abs(int(image.rgba[at + 1]) - bg[1]) +
                     std::abs(int(image.rgba[at + 2]) - bg[2]);
    return diff > 12;
  };

  double sum = 0.0;
  std::size_t subject = 0;
  std::size_t dark = 0;
  std::size_t mid = 0;
  std::size_t bright = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (!is_subject(x, y)) continue;
      ++subject;
      double luma = luma_at(x, y);
      if (radius > 0) {
        // ## AO 代理：邻域内的「更亮」程度
        //
        // 真实 AO 是两个深度值的比较；这里没有深度，用亮度当代理——
        // 在几何交界处，一侧亮一侧暗，暗的那侧会被邻居"遮蔽"。
        //
        // 取 4 个方向的环形采样，算「邻域最亮 - 自己」，归一化后当遮蔽量。
        double neighbour_max = 0.0;
        for (int k = 0; k < 8; ++k) {
          const double angle = double(k) * 3.14159265 / 4.0;
          const int sx = x + int(std::cos(angle) * double(radius));
          const int sy = y + int(std::sin(angle) * double(radius));
          if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
          if (!is_subject(sx, sy)) continue;
          const double nl = luma_at(sx, sy);
          if (nl > neighbour_max) neighbour_max = nl;
        }
        const double occlusion = std::clamp((neighbour_max - luma) / 255.0 * 0.6, 0.0, 0.7);
        luma *= (1.0 - occlusion);
      }
      sum += luma;
      if (luma < 64.0) ++dark;
      else if (luma <= 192.0) ++mid;
      else ++bright;
    }
  }
  if (subject > 0) {
    out.mean = sum / double(subject);
    out.dark = double(dark) / double(subject);
    out.mid = double(mid) / double(subject);
    out.bright = double(bright) / double(subject);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "bpy_watch_render.png";
  auto result = st::codec::png_read_file(path);
  if (!result) {
    std::printf("读不到 %s：%s\n", path.c_str(), result.error().message.c_str());
    return 1;
  }

  std::printf("=== AO 代理效果（当前图：%s）===\n\n", path.c_str());
  std::printf("%-22s %-10s %-9s %-9s %-9s\n", "配置", "主体均值", "暗部%", "中间调%", "亮部%");
  std::printf("%s\n", std::string(64, '-').c_str());
  for (const int radius : {0, 2, 4, 8, 16}) {
    const auto s = analyze_subject(*result, radius);
    char label[64];
    if (radius == 0) {
      std::snprintf(label, sizeof(label), "无 AO（基准）");
    } else {
      std::snprintf(label, sizeof(label), "AO 半径 %d px", radius);
    }
    std::printf("%-22s %-10.1f %-9.1f %-9.1f %-9.1f\n", label, s.mean, s.dark * 100.0,
                s.mid * 100.0, s.bright * 100.0);
  }

  std::printf("\n参照：真 Blender 主体均值 78.1 / 暗部 34.6%%；真 three.js 74.0 / 50.2%%\n");
  std::printf("\n=== 判读 ===\n\n");
  std::printf("若 AO 半径增大后主体均值明显下降（朝 78 靠近），说明这个方向有效，\n");
  std::printf("值得在渲染器里做真正的深度 AO。\n");
  std::printf("若几乎不动，说明剩余差异**不是** AO 造成的，不要白做。\n");
  return 0;
}
