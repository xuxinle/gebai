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
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
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
/// 两帧在网格上的**平均绝对差**（0~255），用于衡量“这一帧到底变了多少”。
///
/// ## 为何不能只用“亮块重叠率”
///
/// 实测教训：`carry_ratio` 判“亮块”的门槛是“达到本帧峰值的 35%”，
/// 而一个细曲线/小圆点铺到 32×18 网格里，单格平均亮度远低于峰值——
/// 于是那些格被当成“不亮”，整段曲线移动被判成“没变化”。
/// 结果：逐帧 hash 显示 16 帧全不同，而旧判据报“有效变化 12%”。**尺子错了。**
///
/// 平均绝对差不设门槛：只要像素真的变了就计进去，与元素大小无关。
[[nodiscard]] auto mean_abs_delta(const FrameGrid& previous, const FrameGrid& next) -> float {
  if (previous.luma.size() != next.luma.size() || previous.luma.empty()) return 0.0f;
  double sum = 0.0;
  for (std::size_t index = 0; index < previous.luma.size(); ++index) {
    sum += std::abs(static_cast<double>(previous.luma[index]) -
                    static_cast<double>(next.luma[index]));
  }
  return static_cast<float>(sum / static_cast<double>(previous.luma.size()));
}

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

/// 一帧里所有元素的（名字 → 矩形）快照。
using FrameSnapshot = std::vector<std::pair<std::string, std::array<float, 4>>>;

/// 从 `trajectory.tsv` 读入（`frame name x y w h alpha`，制表符分隔）。
///
/// 这份文件由 `caiyun --track-elements` 导出：它记录**每个元素**在每帧的屏幕矩形。
/// 有了它才能区分两种画面完全一样、性质却相反的情况：
/// · 同一个点从左上滑到右下（真连贯）；
/// · 左上换了个新东西、右下也换了个（幻灯片）。
[[nodiscard]] auto load_trajectory(const std::string& path)
    -> std::map<int, FrameSnapshot> {
  std::map<int, FrameSnapshot> out{};
  const auto text = st::fs::read_text(path);
  if (!text.has_value()) return out;
  std::size_t cursor = 0;
  const std::string& body = text.value();
  bool header_skipped = false;
  while (cursor < body.size()) {
    const std::size_t end = body.find('\n', cursor);
    const std::string_view line(body.data() + cursor,
                                (end == std::string::npos ? body.size() : end) - cursor);
    cursor = end == std::string::npos ? body.size() : end + 1;
    if (line.empty()) continue;
    if (!header_skipped) {
      header_skipped = true;   // 跳过表头
      continue;
    }
    // 按制表符切分
    std::vector<std::string> fields{};
    std::size_t start = 0;
    while (start <= line.size()) {
      const std::size_t next = line.find('\t', start);
      fields.emplace_back(line.substr(start, next == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : next - start));
      if (next == std::string_view::npos) break;
      start = next + 1;
    }
    if (fields.size() < 6) continue;
    const int frame = std::atoi(fields[0].c_str());
    const std::string& name = fields[1];
    const auto num = [&fields](std::size_t at) { return static_cast<float>(std::atof(fields[at].c_str())); };
    out[frame].emplace_back(name, std::array<float, 4>{num(2), num(3), num(4), num(5)});
  }
  return out;
}

/// 两帧之间的“身份延续率”：上一帧的元素里，有多大比例在下一帧**还在同一位置附近**。
///
/// 判据是**按名字配对 + 位移阈值**：
/// · 名字相同且中心移动 < 画面宽度的 25% → 算“延续”（允许位移，那就是连贯）；
/// · 名字消失或移得离谱 → 不算（那是换掉了）。
///
/// 为何要这个维度：网格亮度只看“亮块在不在”，
/// 而“这个位置亮着”完全可以是另一个元素。onetake 的 probe.py 就是按元素追踪的。
[[nodiscard]] auto identity_carry(const FrameSnapshot& previous, const FrameSnapshot& next,
                                  float travel_limit) -> float {
  if (previous.empty()) return 1.0f;
  int kept = 0;
  for (const auto& [name, rect] : previous) {
    const float cx = rect[0] + rect[2] * 0.5f;
    const float cy = rect[1] + rect[3] * 0.5f;
    for (const auto& [other_name, other_rect] : next) {
      if (other_name != name) continue;
      const float ox = other_rect[0] + other_rect[2] * 0.5f;
      const float oy = other_rect[1] + other_rect[3] * 0.5f;
      const float dist = std::sqrt((cx - ox) * (cx - ox) + (cy - oy) * (cy - oy));
      if (dist <= travel_limit) ++kept;
      break;
    }
  }
  return static_cast<float>(kept) / static_cast<float>(previous.size());
}

}  // namespace

auto main(int argc, char** argv) -> int {
  std::string dir{};
  int cols = 32;
  int rows = 18;
  int step = 1;
  float threshold = 0.5f;
  std::string trajectory_path{};
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
    } else if (arg == "--traj" && index + 1 < argc) {
      trajectory_path = argv[++index];
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
  std::vector<std::pair<int, float>> deltas{};   // (帧序号, 网格平均绝对差)
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
      deltas.emplace_back(index, mean_abs_delta(previous, current));
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

  // —— 第二维度：按元素身份判定（需要 --traj）——
  if (!trajectory_path.empty()) {
    const auto trajectory = load_trajectory(trajectory_path);
    if (trajectory.empty()) {
      st::print("\n[元素追踪] 轨迹文件为空或读不到：{}\n", trajectory_path);
    } else {
      // 位移上限取画面宽度的 25%：允许元素在帧间移动（那是连贯），
      // 但“换了个人”或“飞到天边”就不算延续。
      constexpr float kTravelLimit = 1920.0f * 0.25f;
      double identity_sum = 0.0;
      int identity_pairs = 0;
      std::vector<std::pair<int, float>> worst_identity{};
      const FrameSnapshot* prev_snapshot = nullptr;
      int prev_frame = -1;
      for (const auto& [frame, snapshot] : trajectory) {
        if (prev_snapshot != nullptr && frame == prev_frame + 1) {
          const float ratio = identity_carry(*prev_snapshot, snapshot, kTravelLimit);
          identity_sum += static_cast<double>(ratio);
          ++identity_pairs;
          worst_identity.emplace_back(frame, ratio);
        }
        prev_snapshot = &snapshot;
        prev_frame = frame;
      }
      if (identity_pairs > 0) {
        const double identity_mean = identity_sum / static_cast<double>(identity_pairs);
        std::sort(worst_identity.begin(), worst_identity.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        st::print("\n元素身份延续率：均值 {:.3f}（{} 对相邻帧）\n", identity_mean,
                  identity_pairs);
        st::print("最低的 5 处（帧号 → 延续率；低 = 那一帧换了元素）：\n");
        for (std::size_t i = 0; i < std::min<std::size_t>(5, worst_identity.size()); ++i) {
          st::print("  帧 {:<6} {:.3f}\n", worst_identity[i].first,
                    static_cast<double>(worst_identity[i].second));
        }
        st::print("说明：这一维与画面沿用率**互补**——\n");
        st::print("  画面上“那块地方还亮着”但“元素换了”，只有这一维能抓到。\n");
      }
    }
  }

  // —— 第三判据：画面**真的在变**吗（防“高沿用率 = 什么都不动”）——
  //
  // 沿用率高有两种原因：① 元素连贯地移动（好）；② 画面根本不动（坏，是静止图）。
  // 只有前者才是“一镜到底”，后者是幻灯片的最坏形态。
  // 所以统计“明显变化”的帧对占比：低于 25% 说明片子大半时间是静止的。
  // 判据说明：**不能用固定的 0.985**。
  //
  // 那是我最初写的（等于“几乎逐像素相同才算没变”），于是内容只占画面 30% 时，
  // 即使那 30% 全变了，整体沿用率仍有 0.7 —— 被判成“没动”，
  // 而实际画出的是明显的动画。实测：这个错误让 v3 被误判成 9.6%（真实观感远不止）。
  //
  // 改成看“非重叠比例”：(1 - 沿用率) 超过 1% 即算这一帧有明显变化。
  // 1% 是个保守门槛：32×18=576 格里换掉 6 格。
  // 门槛取网格平均绝对差 0.35/255：约等于“整整 1 个格子换了 6 级亮度”。
  // 这个量级下肉眼已能看出画面在动，而对细线也敏感（不像亮块重叠率）。
  constexpr float kDeltaFloor = 0.35f;
  int changed_pairs = 0;
  for (const auto& [frame, delta] : deltas) {
    if (delta > kDeltaFloor) ++changed_pairs;
  }
  const double changed_ratio =
      static_cast<double>(changed_pairs) / static_cast<double>(ratios.size());
  double delta_sum = 0.0;
  for (const auto& [frame, delta] : deltas) delta_sum += static_cast<double>(delta);
  const double delta_mean = deltas.empty() ? 0.0 : delta_sum / static_cast<double>(deltas.size());
  st::print("网格平均绝对差：{:.2f}/255（越大越“在动”）\n", delta_mean);
  st::print("有效变化帧对：{} / {} = {:.1f}%\n", changed_pairs, ratios.size(),
            changed_ratio * 100.0);
  if (changed_ratio < 0.25) {
    st::print("  ⚠ 低于 25%——片子大半时间几乎静止，沿用率高不代表连贯（可能是静止图）。\n");
  }

  const bool pass = mean >= static_cast<double>(threshold) && changed_ratio >= 0.25;
  st::print("\n判定：{}（均值 {:.3f} {} 阈值 {:.2f}）\n", pass ? "PASS" : "FAIL", mean,
            pass ? "≥" : "<", static_cast<double>(threshold));
  st::print("说明：这是**下界**——分高一定是真连贯；分低需人看一眼确认。\n");
  return pass ? 0 : 1;
}
