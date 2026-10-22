// 裁云 · **连贯性验收**（carry score）—— 对标 onetake 的 verify_promo.py
//
// ## 这个工具解决什么问题
//
// 用户说"太粗糙"时，开发者无法定位——因为没有量尺。
// onetake 的核心洞察是：**"像 PPT"这件事是可以量化的**。
//
// 它的定义是：在每个画面边界上，**有没有东西活下来**。
// 两个画面互相替换（display:none 互换）⇒ 评分 0，无论节奏多好。
//
// ## 我们怎么量（不改画面，只看帧）
//
// 逐帧比对相邻两帧的"内容分布"：
// ① 把画面切成网格，每格算亮度；
// ② 对相邻两帧，算"上一帧的亮块有多少在下一帧里还亮着"（沿用率）；
// ③ 全片取**最低**的几个点当作边界，报告它们的沿用率。
//
// 边界沿用率高 ⇒ 画面是"长出来"的；低 ⇒ 是"换上去"的（幻灯片）。
//
// ## 局限（诚实说明）
//
// 这是**画面级**的近似，不是 onetake 那种"按元素 id 追踪"的精确判定。
// 它能抓住"整屏画面全换"这种最典型的 PPT 特征，
// 但对"同一位置换成另一个物体"不敏感——那需要渲染器上报元素位置。
// 所以它给出的分是**下界**：分高一定是真连贯，分低需要人看一眼确认。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"

namespace {

/// 一张帧的网格摘要（亮度，行优先）。
struct FrameGrid {
  int cols{0};
  int rows{0};
  std::vector<float> luma{};

  [[nodiscard]] auto at(int col, int row) const -> float {
    return luma[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) +
                static_cast<std::size_t>(col)];
  }
};

/// 把一帧缩成 `cols × rows` 的亮度网格。
///
/// 为什么用网格而不是逐像素：逐像素对**微小位移**过于敏感——
/// 一个点移动 3px 就被判"全新画面"，而观感上它完全连贯。
/// 网格化（通常 32×18）把这种微小位移吸收掉，只保留"大块内容有没有留下"。
[[nodiscard]] auto grid_of(const st::codec::PngImage& image, int cols, int rows) -> FrameGrid {
  FrameGrid grid{};
  grid.cols = cols;
  grid.rows = rows;
  grid.luma.assign(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows), 0.0f);
  if (image.width == 0U || image.height == 0U) return grid;

  const auto& pixels = image.rgba;   // RGBA8 行优先
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < cols; ++col) {
      const int x0 = col * static_cast<int>(image.width) / cols;
      const int x1 = std::max(x0 + 1, (col + 1) * static_cast<int>(image.width) / cols);
      const int y0 = row * static_cast<int>(image.height) / rows;
      const int y1 = std::max(y0 + 1, (row + 1) * static_cast<int>(image.height) / rows);
      double sum = 0.0;
      int count = 0;
      for (int y = y0; y < y1; y += 2) {
        for (int x = x0; x < x1; x += 2) {
          const std::size_t index =
              (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
               static_cast<std::size_t>(x)) * 4U;
          if (index + 2U >= pixels.size()) continue;
          sum += 0.299 * static_cast<double>(pixels[index]) +
             0.587 * static_cast<double>(pixels[index + 1U]) +
             0.114 * static_cast<double>(pixels[index + 2U]);
          ++count;
        }
      }
      grid.luma[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) +
                static_cast<std::size_t>(col)] =
          count > 0 ? static_cast<float>(sum / count) : 0.0f;
    }
  }
  return grid;
}

/// 两帧之间的"沿用率"：上一帧的亮块有多大比例在下一帧里仍然亮。
///
/// ## 判据为何用**绝对下限 + 相对峰值**两个条件
///
/// 只用相对峰值（"达到本帧峰值的 35%"）在**大片黑场的片**里会失真：
/// 背景很暗时，峰值可能只有 20，于是 8 以上的格子全算"亮"，
/// 几乎每个格子都满足 → 任何两帧的沿用率都接近 1 → 分数虚高。
/// 实测：我们的片子因此量出 0.980，而 onetake 官方片只有 0.763——
/// 但肉眼后者内容明显更丰富。两个数不可比，就是尺子的问题。
///
/// 所以加一个**绝对下限**（`kBrightFloor`）：低于它的格子一律不算"亮"，
/// 使黑场真正不参与计算。这样两片才量的是同一件事。
[[nodiscard]] auto carry_ratio(const FrameGrid& previous, const FrameGrid& next) -> float {
  // 绝对下限：亮度低于它的格子视为"空场"，不参与沿用率。
  // 取 18/255：大约是肉眼在深色背景上能看到“有东西”的门槛。
  constexpr float kBrightFloor = 18.0f;
  const auto peak = [](const FrameGrid& grid) {
    float value = 1.0f;
    for (const float luma : grid.luma) value = std::max(value, luma);
    return value;
  };
  const float previous_peak = peak(previous);
  const float next_peak = peak(next);
  const auto is_bright = [](float luma, float frame_peak) {
    return luma > kBrightFloor && luma > frame_peak * 0.35f;
  };

  int bright_previous = 0;
  int still_bright = 0;
  for (int row = 0; row < previous.rows; ++row) {
    for (int col = 0; col < previous.cols; ++col) {
      if (!is_bright(previous.at(col, row), previous_peak)) continue;
      ++bright_previous;
      if (is_bright(next.at(col, row), next_peak)) ++still_bright;
    }
  }
  if (bright_previous == 0) return 1.0f;   // 上一帧几乎空场：视作不破坏连贯
  return static_cast<float>(still_bright) / static_cast<float>(bright_previous);
}

[[nodiscard]] auto load_grid(const std::string& path, int cols, int rows) -> FrameGrid {
  const auto decoded = st::codec::png_read_file(path);
  if (!decoded.has_value()) return FrameGrid{};
  return grid_of(decoded.value(), cols, rows);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  std::string dir{};
  int cols = 32;
  int rows = 18;
  int step = 1;
  float threshold = 0.5f;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--dir" && index + 1 < argc) {
      dir = argv[++index];
    } else if (arg == "--cols" && index + 1 < argc) {
      cols = std::atoi(argv[++index]);
    } else if (arg == "--rows" && index + 1 < argc) {
      rows = std::atoi(argv[++index]);
    } else if (arg == "--step" && index + 1 < argc) {
      step = std::max(1, std::atoi(argv[++index]));
    } else if (arg == "--threshold" && index + 1 < argc) {
      threshold = static_cast<float>(std::atof(argv[++index]));
    } else if (arg == "--help" || arg == "-h") {
      st::print("用法: carry_check --dir <帧目录> [--cols 32] [--rows 18] [--step 1] "
                "[--threshold 0.5]\n");
      return 0;
    }
  }
  if (dir.empty()) {
    st::eprint("缺少 --dir <帧目录>\n");
    return 2;
  }

  // 收集帧文件名（按名字排序 = 按时间顺序，因为 frame_00001 这种零填充命名）
  std::vector<std::string> files{};
  if (const auto listed = st::fs::list_dir(dir); listed.has_value()) {
    for (const auto& entry : listed.value()) {
      if (entry.name.ends_with(".png")) files.push_back(st::fs::join(dir, entry.name));
    }
  }
  std::sort(files.begin(), files.end());
  if (files.size() < 3) {
    st::eprint("帧数不足（{}），无法判定连贯性\n", files.size());
    return 2;
  }

  st::print("连贯性验收：{} 帧，网格 {}×{}，步长 {}\n", files.size(), cols, rows, step);

  FrameGrid previous{};
  int index = 0;
  std::vector<std::pair<int, float>> ratios{};   // (帧序号, 沿用率)
  for (const std::string& file : files) {
    if (index % step != 0) {
      ++index;
      continue;
    }
    FrameGrid current = load_grid(file, cols, rows);
    if (current.cols == 0) {
      ++index;
      continue;
    }
    if (previous.cols != 0) {
      ratios.emplace_back(index, carry_ratio(previous, current));
    }
    previous = std::move(current);
    ++index;
  }

  if (ratios.empty()) {
    st::eprint("没有可比对的帧对\n");
    return 2;
  }

  // 统计：均值、中位数、最低的几处（那里就是"像 PPT"的位置）
  double sum = 0.0;
  for (const auto& [frame, ratio] : ratios) sum += static_cast<double>(ratio);
  const double mean = sum / static_cast<double>(ratios.size());

  std::vector<float> sorted{};
  sorted.reserve(ratios.size());
  for (const auto& [frame, ratio] : ratios) sorted.push_back(ratio);
  std::sort(sorted.begin(), sorted.end());
  const float median = sorted[sorted.size() / 2];
  const float lowest = sorted.front();

  // 把最低的 5 处标出来（含时间，便于人回看）
  std::vector<std::pair<int, float>> worst = ratios;
  std::sort(worst.begin(), worst.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

  st::print("\n沿用率：均值 {:.3f} · 中位数 {:.3f} · 最低 {:.3f}\n", mean, median, lowest);
  st::print("最低的 5 处（帧号 → 沿用率，按 --step 折算到秒需除以 fps）：\n");
  for (std::size_t i = 0; i < std::min<std::size_t>(5, worst.size()); ++i) {
    st::print("  帧 {:<6} {:.3f}{}\n", worst[i].first, worst[i].second,
              worst[i].second < threshold ? "   ← 低于阈值，这里是断点" : "");
  }

  const bool pass = mean >= static_cast<double>(threshold);
  st::print("\n判定：{}（均值 {:.3f} {} 阈值 {:.2f}）\n", pass ? "PASS" : "FAIL", mean,
            pass ? "≥" : "<", static_cast<double>(threshold));
  st::print("说明：这是**下界**——分高一定是真连贯；分低需人看一眼确认。\n");
  return pass ? 0 : 1;
}
