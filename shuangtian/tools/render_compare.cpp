// 三路渲染的量化对照：把参照图的统计量与本框架的输出比。
//
// ## 为何不能只看"看着像不像"
//
// 上面的对话里两次印证了同一件事：判据选松了，缺陷会藏。
// 所以对照必须给出**可断言的数值**，而且要分部看——全图平均会掩盖
// 「亮部太暗」或「暗部太亮」这类问题。
//
// ## 分哪些区
//
// | 区间 | 为什么单独看 |
// |---|---|
// | 暗部（<64） | 阴影与背面是否压得住 |
// | 中间调（64~192） | 主体材质的表现 |
// | 亮部（>192） | 高光是否够（金属的关键） |
//
// 再算**直方图重叠度**（Bhattacharyya 系数）：它对分布形状敏感，
// 比均值差更能反映"观感差多少"。
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "st/codec/png.hpp"

namespace {

struct Stats {
  double mean{0.0};
  double mean_r{0.0};
  double mean_g{0.0};
  double mean_b{0.0};
  double dark_ratio{0.0};      ///< 亮度 < 64
  double mid_ratio{0.0};       ///< 64..192
  double bright_ratio{0.0};    ///< > 192
  double max_luma{0.0};
  std::vector<double> hist;    ///< 64 档亮度直方图（归一化）
};

auto luma_of(int r, int g, int b) -> double {
  return 0.2126 * double(r) + 0.7152 * double(g) + 0.0722 * double(b);
}

auto analyze(const st::codec::PngImage& image) -> Stats {
  Stats out;
  out.hist.assign(64, 0.0);
  const std::size_t pixels = std::size_t(image.width) * image.height;
  if (pixels == 0) return out;
  double sum_r = 0.0;
  double sum_g = 0.0;
  double sum_b = 0.0;
  std::size_t dark = 0;
  std::size_t mid = 0;
  std::size_t bright = 0;
  for (std::size_t i = 0; i < pixels; ++i) {
    const int r = image.rgba[i * 4 + 0];
    const int g = image.rgba[i * 4 + 1];
    const int b = image.rgba[i * 4 + 2];
    sum_r += r;
    sum_g += g;
    sum_b += b;
    const double luma = luma_of(r, g, b);
    if (luma > out.max_luma) out.max_luma = luma;
    if (luma < 64.0) ++dark;
    else if (luma <= 192.0) ++mid;
    else ++bright;
    const std::size_t bin = static_cast<std::size_t>(luma) / 4U;
    out.hist[bin < 64 ? bin : 63] += 1.0;
  }
  out.mean_r = sum_r / double(pixels);
  out.mean_g = sum_g / double(pixels);
  out.mean_b = sum_b / double(pixels);
  out.mean = luma_of(int(out.mean_r), int(out.mean_g), int(out.mean_b));
  out.dark_ratio = double(dark) / double(pixels);
  out.mid_ratio = double(mid) / double(pixels);
  out.bright_ratio = double(bright) / double(pixels);
  for (auto& value : out.hist) value /= double(pixels);
  return out;
}

/// Bhattacharyya 系数：1.0 = 分布完全相同，0 = 完全不相交。
auto histogram_overlap(const Stats& a, const Stats& b) -> double {
  double sum = 0.0;
  for (std::size_t i = 0; i < a.hist.size() && i < b.hist.size(); ++i) {
    sum += std::sqrt(a.hist[i] * b.hist[i]);
  }
  return sum;
}

auto load(const std::string& path) -> st::codec::PngImage {
  auto result = st::codec::png_read_file(path);
  if (!result) {
    std::printf("读不到 %s：%s\n", path.c_str(), result.error().message.c_str());
    return st::codec::PngImage{};
  }
  return std::move(*result);
}

auto report(const char* label, const Stats& s) -> void {
  std::printf("%-14s 均值 %5.1f (R%.1f G%.1f B%.1f)  暗 %.1f%%  中 %.1f%%  亮 %.1f%%  最亮 %.0f\n",
              label, s.mean, s.mean_r, s.mean_g, s.mean_b, s.dark_ratio * 100.0,
              s.mid_ratio * 100.0, s.bright_ratio * 100.0, s.max_luma);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mine = argc > 1 ? argv[1] : "bpy_watch_render.png";
  const std::string ref = argc > 2 ? argv[2] : "reference_three_none.png";

  const auto a = load(mine);
  const auto b = load(ref);
  if (a.rgba.empty() || b.rgba.empty()) return 1;
  if (a.width != b.width || a.height != b.height) {
    std::printf("尺寸不同：%ux%u vs %ux%u（对照需要同尺寸）\n", a.width, a.height, b.width,
                b.height);
  }

  const auto sa = analyze(a);
  const auto sb = analyze(b);

  std::printf("=== 分区统计 ===\n\n");
  report("本框架(bpy)", sa);
  report("three.js", sb);
  std::printf("\n");
  std::printf("=== 差异 ===\n\n");
  std::printf("  均值差       %+6.1f / 255\n", sa.mean - sb.mean);
  std::printf("  暗部占比差   %+6.1f 个百分点\n", (sa.dark_ratio - sb.dark_ratio) * 100.0);
  std::printf("  中间调差     %+6.1f 个百分点\n", (sa.mid_ratio - sb.mid_ratio) * 100.0);
  std::printf("  亮部占比差   %+6.1f 个百分点\n", (sa.bright_ratio - sb.bright_ratio) * 100.0);
  std::printf("  最亮差       %+6.0f\n", sa.max_luma - sb.max_luma);
  std::printf("  直方图重叠度 %.3f（1.0 = 完全相同）\n", histogram_overlap(sa, sb));

  std::printf("\n=== 解读 ===\n\n");
  if (sa.bright_ratio < sb.bright_ratio * 0.5) {
    std::printf("  ⚠ 亮部明显不足：%.1f%% vs %.1f%%\n", sa.bright_ratio * 100.0,
                sb.bright_ratio * 100.0);
    std::printf("    → 这是**金属高光**的差别（three 的 GGX 高光更锐），不是色调映射\n");
  }
  if (sa.mid_ratio > sb.mid_ratio + 10.0) {
    std::printf("  ⚠ 中间调占比过多：%.1f%% vs %.1f%%\n", sa.mid_ratio * 100.0,
                sb.mid_ratio * 100.0);
    std::printf("    → 画面偏“平”：缺少明暗层次\n");
  }
  const double overlap = histogram_overlap(sa, sb);
  if (overlap > 0.95) {
    std::printf("  直方图高度重叠：整体明暗分布一致\n");
  } else if (overlap > 0.80) {
    std::printf("  直方图部分重叠：明暗分布接近但有可辨差异\n");
  } else {
    std::printf("  直方图重叠度低：明暗分布差别明显\n");
  }
  return 0;
}
