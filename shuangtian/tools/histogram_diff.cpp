// 分布对比：本框架与真渲染器的亮度直方图**形状**差异在哪。
//
// ## 为什么单点分析到头了
//
// 前面测了"黄铜正面（n·v=1）"的亮度拆解，得到 0.942。但那个数字对定位
// 帮助有限——因为主体是个立体物体，各朝向的面亮度差异很大，**平均值
// 才是要对的量**。
//
// ## 要看什么
//
// 逐档比对两个图的**主体亮度直方图**，找出：
//
// 1. 哪一档差异最大（是暗部不足？中间调过密？高光缺失？）
// 2. 本框架的分布是否"太窄"（说明层次不足）
//
// ## 为什么这比均值差有用
//
// 均值差只说明"整体偏亮"。而直方图逐档差能说明"**哪一段**偏亮"——
// 比如若暗部档差得最多，就要压暗部；若中间调档太密，就要增加对比。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/codec/png.hpp"

namespace {

constexpr int kBins = 32;

struct Histogram {
  std::vector<double> bins{kBins, 0.0};
  std::size_t subject{0};
  double mean{0.0};
  double stddev{0.0};
};

auto analyze(const st::codec::PngImage& image) -> Histogram {
  Histogram out;
  const int w = static_cast<int>(image.width);
  const int h = static_cast<int>(image.height);
  if (w <= 0 || h <= 0) return out;
  // ## 必须验 rgba 长度（这里踩过坑）
  //
  // `image.rgba` 若比 w*h*4 短，下面的下标会越界读。越界读到的垃圾值经
  // double 运算后变成天文数字（打印出 1e300 那种），而**不会崩**——
  // 症状就是"某些档的百分比离谱到 3.6e160"。
  //
  // 第一版没这个检查。判据：这类"数值离谱但不报错"的缺陷，
  // 先怀疑边界读，而不是算法。
  const std::size_t required = std::size_t(w) * std::size_t(h) * 4U;
  if (image.rgba.size() < required) {
    std::printf("  ! rgba 长度 %zu 不足 %zu，跳过\n", image.rgba.size(), required);
    return out;
  }

  // 背景色（四角）
  int bg[3] = {0, 0, 0};
  {
    std::vector<int> values[3];
    const int corners[4][2] = {{0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}};
    for (int c = 0; c < 3; ++c) {
      for (const auto& corner : corners) {
        const std::size_t at =
            (std::size_t(corner[1]) * image.width + std::size_t(corner[0])) * 4;
        values[c].push_back(image.rgba[at + std::size_t(c)]);
      }
      std::sort(values[c].begin(), values[c].end());
      bg[c] = values[c][values[c].size() / 2];
    }
  }

  double sum = 0.0;
  double sum_sq = 0.0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t at = (std::size_t(y) * image.width + std::size_t(x)) * 4;
      const int diff = std::abs(int(image.rgba[at]) - bg[0]) +
                       std::abs(int(image.rgba[at + 1]) - bg[1]) +
                       std::abs(int(image.rgba[at + 2]) - bg[2]);
      if (diff <= 12) continue;
      const double luma = 0.2126 * image.rgba[at] + 0.7152 * image.rgba[at + 1] +
                          0.0722 * image.rgba[at + 2];
      ++out.subject;
      sum += luma;
      sum_sq += luma * luma;
      int bin = static_cast<int>(luma * double(kBins) / 256.0);
      if (bin < 0) bin = 0;
      if (bin >= kBins) bin = kBins - 1;
      out.bins[std::size_t(bin)] += 1.0;   // 先计数
    }
  }
  if (out.subject > 0) {
    out.mean = sum / double(out.subject);
    const double variance = sum_sq / double(out.subject) - out.mean * out.mean;
    out.stddev = variance > 0.0 ? std::sqrt(variance) : 0.0;
    // 归一化成百分比（0~100）。**只做一次**——之前误在循环里重复调用
    for (auto& value : out.bins) value = value / double(out.subject) * 100.0;
  }
  return out;
}

auto load(const std::string& path) -> st::codec::PngImage {
  auto result = st::codec::png_read_file(path);
  if (!result) {
    std::printf("读不到 %s\n", path.c_str());
    return st::codec::PngImage{};
  }
  return std::move(*result);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("用法：histogram_diff <本框架.png> <参照.png>\n");
    return 2;
  }
  const auto mine = analyze(load(argv[1]));
  const auto ref = analyze(load(argv[2]));
  if (mine.subject == 0 || ref.subject == 0) return 1;

  std::printf("=== 主体亮度直方图逐档对比 ===\n\n");
  std::printf("本框架：均值 %.1f  标准差 %.1f  主体像素 %zu\n", mine.mean, mine.stddev,
              mine.subject);
  std::printf("参照：  均值 %.1f  标准差 %.1f  主体像素 %zu\n\n", ref.mean, ref.stddev,
              ref.subject);

  std::printf("%-12s %-9s %-9s %-9s\n", "亮度区间", "本框架%", "参照%", "差(百分点)");
  std::printf("%s\n", std::string(46, '-').c_str());

  double max_abs_diff = 0.0;
  int worst_bin = 0;
  for (int i = 0; i < kBins; ++i) {
    const int low = i * 256 / kBins;
    const int high = (i + 1) * 256 / kBins;
    const double a = mine.bins[std::size_t(i)];
    const double b = ref.bins[std::size_t(i)];
    const double d = a - b;
    if (std::fabs(d) > max_abs_diff) {
      max_abs_diff = std::fabs(d);
      worst_bin = i;
    }
    // 只打印有内容的档，避免 32 行噪音
    if (a < 0.3 && b < 0.3) continue;
    std::printf("%3d~%-8d %-9.1f %-9.1f %+.1f\n", low, high, a, b, d);
  }

  std::printf("\n差异最大的一档：%d~%d（差 %.1f 个百分点）\n", worst_bin * 256 / kBins,
              (worst_bin + 1) * 256 / kBins, max_abs_diff);

  std::printf("\n=== 判读 ===\n\n");
  std::printf("标准差：本框架 %.1f vs 参照 %.1f\n", mine.stddev, ref.stddev);
  if (mine.stddev < ref.stddev * 0.7) {
    std::printf("→ 本框架**分布更窄**：明暗对比不足（需要的不是整体调暗，而是拉开层次）\n");
  } else if (mine.stddev > ref.stddev * 1.3) {
    std::printf("→ 本框架**分布更宽**：对比过强\n");
  } else {
    std::printf("→ 两者分布宽度接近\n");
  }
  std::printf("均值差 %+.1f；标准差差 %+.1f\n", mine.mean - ref.mean,
              mine.stddev - ref.stddev);
  return 0;
}
