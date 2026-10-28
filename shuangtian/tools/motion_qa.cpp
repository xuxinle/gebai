/// 霜天 · **动效验收器**（motion QA）。
///
/// ## 这个工具解决什么问题
///
/// 一条动效片「闷不闷、像不像 PPT」是观感判断，不可直接测量。
/// 但可以拆成几个**可测的代理量**，并与一条**已知好的片**做同口径对照——
/// 这样「更差/更好」才有依据，而不是凭感觉。
///
/// 三个量（互相正交，缺一不可）：
///
/// 1. **画面沿用率**（carry）：相邻两帧的亮块重叠比例。
///    低 = 画面在"换掉"而不是"长出来"。
/// 2. **变化强度**（delta）：相邻两帧的网格平均绝对差（0~255）。
///    它是**唯一能把『静止』暴露出来**的量——静止片的沿用率接近 1、
///    身份延续率 1.0，前两个量都满分，只有 delta 会掉到 0。
/// 3. **元素身份延续率**（identity）：按元素名字配对，看它是否还在原处附近。
///    区分「同一个东西滑过去」与「同一位置换了个新东西」。
///
/// ## 为什么必须支持「带参考片」
///
/// 绝对阈值不可迁移：不同风格（大片留白 / 密集信息流）的合理 delta 差一个数量级。
/// 唯一可靠的判法是**同口径对照**——同一个工具、同一套网格、同一采样间隔，
/// 量两条片，比相对关系。这也是本项目反复踩坑后得到的教训（见文档）。
///
/// ## 两个必须遵守的口径纪律（都是实测踩出来的）
///
/// · **采样间隔必须一致**。比较 10 fps 抽的参考片与 30 fps 抽的自制片，
///   间隔差 3 倍，运动量本来就该差 3 倍——据此得出的「差 5.3 倍」是假的。
///   所以本工具**强制打印采样间隔**，并在对照时校验两者一致。
/// · **均值必须与中位数、峰值一起看**。均值会被少数尖峰支配：
///   一条「大部分静止 + 两次剧烈运动」的片子，均值可以和有持续微动的片子一样高，
///   但两者的节奏完全不同。只报均值等于撒谎。
///
/// ## 尺度事实（决定了能验收什么）
///
/// 网格默认 32×18，1080p 下每格 60×60 px。**1~2 px 的位移会被格内平均抹掉**，
/// 逐帧差值为 0。要验收细微观感（呼吸、微颤），必须调细网格（`--cols/--rows`）
/// 或走元素追踪，而不是靠这个量。
///
/// 用法见 `--help`。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"

namespace {

/// 一张帧的网格摘要（亮度，行优先）。
struct FrameGrid {
  int cols{0};
  int rows{0};
  std::vector<float> luma{};

  /// 取格子亮度。
  ///
  /// 用下标而不是 `.at()`：本框架禁用 `.at()`（L12——键/索引越界时抛异常，
  /// 而框架没有通用异常边界）。索引由调用方保证在界内（行/列都来自循环上界）。
  [[nodiscard]] auto luma_at(int col, int row) const -> float {
    return luma[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) +
                static_cast<std::size_t>(col)];
  }
};

/// 把一帧缩成 `cols × rows` 的亮度网格。
///
/// 为什么用网格而不是逐像素：逐像素对**微小位移**过于敏感——
/// 一个点移动 3px 就被判「全新画面」，而观感上它完全连贯。
/// 网格化把这种微小位移吸收掉，只保留「大块内容有没有留下」。
///
/// 代价：太细的位移也会被吸收（见文件头「尺度事实」）。
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

/// 两帧的**网格平均绝对差**（0~255）：这一帧到底变了多少。
///
/// 不设亮度门槛——只要像素真的变了就计进去，因此对细线、小点同样敏感。
/// （对比：`carry_ratio` 判「亮块」要过峰值 35% 的门槛，
///  一条细曲线铺到网格里单格亮度远低于峰值，会被判成「不亮」而漏掉。）
[[nodiscard]] auto mean_abs_delta(const FrameGrid& previous, const FrameGrid& next) -> float {
  if (previous.luma.size() != next.luma.size() || previous.luma.empty()) return 0.0f;
  double sum = 0.0;
  for (std::size_t index = 0; index < previous.luma.size(); ++index) {
    sum += std::abs(static_cast<double>(previous.luma[index]) -
                    static_cast<double>(next.luma[index]));
  }
  return static_cast<float>(sum / static_cast<double>(previous.luma.size()));
}

/// 两帧的**画面沿用率**：上一帧的亮块有多大比例在下一帧里仍然亮。
///
/// 判据用**绝对下限 + 相对峰值**两个条件：
/// · 绝对下限（18/255）让真正的黑场不参与计算；
/// · 相对峰值（35%）排除暗部噪声。
///
/// 只用相对峰值会在大片黑场的片子里失真：背景很暗时峰值可能只有 20，
/// 于是 8 以上的格子全算「亮」，几乎每格都满足 → 任何两帧沿用率都接近 1。
[[nodiscard]] auto carry_ratio(const FrameGrid& previous, const FrameGrid& next) -> float {
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
      if (!is_bright(previous.luma_at(col, row), previous_peak)) continue;
      ++bright_previous;
      if (is_bright(next.luma_at(col, row), next_peak)) ++still_bright;
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

/// 从元素轨迹文件读入（制表符分隔：`frame name x y w h alpha`）。
///
/// 这份文件由渲染器导出（裁云：`--track-elements`）。
/// 有了它才能区分两种「画面完全一样、性质却相反」的情况：
/// 同一个元素滑过去（连贯） vs 同一位置换了个新元素（幻灯片）。
[[nodiscard]] auto load_trajectory(const std::string& path) -> std::map<int, FrameSnapshot> {
  std::map<int, FrameSnapshot> out{};
  const auto text = st::fs::read_text(path);
  if (!text.has_value()) return out;
  const std::string& body = text.value();
  std::size_t cursor = 0;
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
    const auto num = [&fields](std::size_t at) {
      return static_cast<float>(std::atof(fields[at].c_str()));
    };
    out[std::atoi(fields[0].c_str())].emplace_back(
        fields[1], std::array<float, 4>{num(2), num(3), num(4), num(5)});
  }
  return out;
}

/// 两帧的**元素身份延续率**：上一帧的元素有多大比例在下一帧还在附近。
///
/// 判据：名字相同 + 中心位移 < `travel_limit`（按画面宽度比例给的阈值）。
/// 名字消失或移得离谱都不算——那就是「换掉了」。
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

/// 一个帧目录的完整测量结果。
struct Measurement {
  std::string label{};
  int frame_count{0};
  std::vector<std::pair<int, float>> carries{};   // (帧序号, 沿用率)
  std::vector<std::pair<int, float>> deltas{};    // (帧序号, 网格平均绝对差)
  double carry_mean{0.0};
  double delta_mean{0.0};
  double delta_median{0.0};
  double delta_p95{0.0};
  float delta_max{0.0f};
  int changed_pairs{0};
  double changed_ratio{0.0};
  double identity_mean{-1.0};   // <0 表示没有轨迹文件
  int identity_pairs{0};
  bool ok{false};
  std::string error{};
};

[[nodiscard]] auto median_of(std::vector<float> values) -> double {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  return static_cast<double>(values[values.size() / 2]);
}

/// 量一个帧目录。
///
/// @param dir        帧目录（PNG）
/// @param step       采样步长（每 step 帧取一帧）
/// @param cols/rows  验收网格
/// @param fps        帧率（用于报告采样间隔）
/// @param trajectory 可选的元素轨迹文件
/// @param change_floor 「有效变化」门槛（网格平均绝对差，默认 0.35）
[[nodiscard]] auto measure(const std::string& label, const std::string& dir, int step, int cols,
                           int rows, float /*fps*/, const std::string& trajectory,
                           float change_floor) -> Measurement {
  Measurement out{};
  out.label = label;

  std::vector<std::string> files{};
  if (const auto listed = st::fs::list_dir(dir); listed.has_value()) {
    for (const auto& entry : listed.value()) {
      if (entry.name.ends_with(".png")) files.push_back(st::fs::join(dir, entry.name));
    }
  }
  std::sort(files.begin(), files.end());
  if (files.size() < 3) {
    out.error = std::format("帧数不足（{}）", files.size());
    return out;
  }
  out.frame_count = static_cast<int>(files.size());

  FrameGrid previous{};
  int index = 0;
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
      out.carries.emplace_back(index, carry_ratio(previous, current));
      out.deltas.emplace_back(index, mean_abs_delta(previous, current));
    }
    previous = std::move(current);
    ++index;
  }
  if (out.deltas.empty()) {
    out.error = "没有可比对的帧对";
    return out;
  }

  double carry_sum = 0.0;
  for (const auto& [frame, ratio] : out.carries) carry_sum += static_cast<double>(ratio);
  out.carry_mean = carry_sum / static_cast<double>(out.carries.size());

  std::vector<float> delta_values{};
  delta_values.reserve(out.deltas.size());
  double delta_sum = 0.0;
  for (const auto& [frame, delta] : out.deltas) {
    delta_sum += static_cast<double>(delta);
    delta_values.push_back(delta);
    out.delta_max = std::max(out.delta_max, delta);
    if (delta > change_floor) ++out.changed_pairs;
  }
  out.delta_mean = delta_sum / static_cast<double>(out.deltas.size());
  out.delta_median = median_of(delta_values);
  {
    std::vector<float> sorted = delta_values;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t p95 = static_cast<std::size_t>(0.95 * static_cast<double>(sorted.size() - 1));
    out.delta_p95 = static_cast<double>(sorted[p95]);
  }
  out.changed_ratio =
      static_cast<double>(out.changed_pairs) / static_cast<double>(out.deltas.size());

  // 元素身份（可选）
  if (!trajectory.empty()) {
    const auto tracks = load_trajectory(trajectory);
    if (!tracks.empty()) {
      // 允许元素移动：阈值取画面宽度的 25%。
      // 太小会把"连贯的滑行"判成"换掉了"，太大则什么都判成延续。
      constexpr float kTravelLimit = 1920.0f * 0.25f;
      double identity_sum = 0.0;
      const FrameSnapshot* prev_snapshot = nullptr;
      int prev_frame = -1;
      for (const auto& [frame, snapshot] : tracks) {
        if (prev_snapshot != nullptr && frame == prev_frame + 1) {
          identity_sum += static_cast<double>(identity_carry(*prev_snapshot, snapshot, kTravelLimit));
          ++out.identity_pairs;
        }
        prev_snapshot = &snapshot;
        prev_frame = frame;
      }
      if (out.identity_pairs > 0) {
        out.identity_mean = identity_sum / static_cast<double>(out.identity_pairs);
      }
    }
  }

  out.ok = true;
  return out;
}

void print_report(const Measurement& m, int cols, int rows, int step, float fps,
                  float change_floor) {
  if (!m.ok) {
    st::log::out("[{}] 无法测量：{}\n", m.label, m.error);
    return;
  }
  const float interval_ms = 1000.0f / fps * static_cast<float>(step);
  st::log::out("[{}] {} 帧，网格 {}×{}，步长 {}（采样间隔 {:.1f} ms）\n", m.label, m.frame_count,
            cols, rows, step, static_cast<double>(interval_ms));
  st::log::out("  画面沿用率   均值 {:.3f}\n", m.carry_mean);
  st::log::out("  变化强度     均值 {:.3f} · 中位数 {:.3f} · p95 {:.3f} · 峰值 {:.2f}\n",
            m.delta_mean, m.delta_median, m.delta_p95, static_cast<double>(m.delta_max));
  st::log::out("  有效变化     {} / {} = {:.1f}%（门槛 {:.2f}）\n", m.changed_pairs,
            static_cast<int>(m.deltas.size()), m.changed_ratio * 100.0,
            static_cast<double>(change_floor));
  if (m.identity_mean >= 0.0) {
    st::log::out("  元素身份延续 均值 {:.3f}（{} 对相邻帧）\n", m.identity_mean, m.identity_pairs);
  }
  st::log::out("  判读：均值被尖峰支配，**必须与中位数/峰值一起看**；\n");
  st::log::out("        均值高 + 中位数低 = 「大部分静止 + 少数剧烈运动」。\n");
}

}  // namespace

auto main(int argc, char** argv) -> int {
  std::string dir{};
  std::string reference{};
  std::string trajectory{};
  std::string reference_trajectory{};
  int cols = 32;
  int rows = 18;
  int step = 1;
  float fps = 30.0f;
  float change_floor = 0.35f;
  std::string dump_csv{};

  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    const auto next = [&index, argc, argv]() -> std::string {
      return index + 1 < argc ? std::string(argv[++index]) : std::string{};
    };
    if (arg == "--dir") dir = next();
    else if (arg == "--ref") reference = next();
    else if (arg == "--traj") trajectory = next();
    else if (arg == "--ref-traj") reference_trajectory = next();
    else if (arg == "--cols") cols = std::max(1, std::atoi(next().c_str()));
    else if (arg == "--rows") rows = std::max(1, std::atoi(next().c_str()));
    else if (arg == "--step") step = std::max(1, std::atoi(next().c_str()));
    else if (arg == "--fps") {
      fps = static_cast<float>(std::atof(next().c_str()));
      if (fps < 1.0f) fps = 30.0f;
    } else if (arg == "--change-floor") {
      change_floor = static_cast<float>(std::atof(next().c_str()));
    } else if (arg == "--csv") dump_csv = next();
    else if (arg == "--help" || arg == "-h") {
      st::log::out("动效验收（motion QA）\n\n");
      st::log::out("用法:\n");
      st::log::out("  motion_qa --dir <帧目录> [--ref <参考帧目录>] [选项]\n\n");
      st::log::out("选项:\n");
      st::log::out("  --traj <tsv>        元素轨迹（渲染时 --track-elements 导出）\n");
      st::log::out("  --ref-traj <tsv>    参考片的元素轨迹\n");
      st::log::out("  --fps <n>           帧率，默认 30（用于报告采样间隔）\n");
      st::log::out("  --step <n>          采样步长，默认 1\n");
      st::log::out("  --cols/--rows <n>   验收网格，默认 32×18\n");
      st::log::out("  --change-floor <v>  有效变化门槛（默认 0.35/255）\n");
      st::log::out("  --csv <path>        导出逐帧数据（帧号,沿用率,变化强度）\n\n");
      st::log::out("⚠ 与参考片对比时必须**同采样间隔**（同 fps 同 step），否则强度不可比。\n");
      st::log::out("⚠ 均值会被尖峰支配，请同时看中位数与峰值。\n");
      return 0;
    } else {
      st::log::err("未知参数：{}（用 --help 看用法）\n", arg);
      return 2;
    }
  }
  if (dir.empty()) {
    st::log::err("缺少 --dir <帧目录>\n");
    return 2;
  }

  const Measurement subject = measure("片子", dir, step, cols, rows, fps, trajectory, change_floor);
  print_report(subject, cols, rows, step, fps, change_floor);
  if (!subject.ok) return 2;

  // 逐帧数据（便于自己画图 / 定位是哪一秒闷）
  if (!dump_csv.empty()) {
    std::string csv = "frame,carry,delta\n";
    for (std::size_t i = 0; i < subject.deltas.size() && i < subject.carries.size(); ++i) {
      csv += std::format("{},{:.4f},{:.4f}\n", subject.deltas[i].first,
                         static_cast<double>(subject.carries[i].second),
                         static_cast<double>(subject.deltas[i].second));
    }
    if (const auto written = st::fs::write_text(dump_csv, csv); written.has_value()) {
      st::log::out("逐帧数据 → {}\n", dump_csv);
    }
  }

  if (reference.empty()) return 0;

  // ── 与参考片同口径对照 ──
  const Measurement ref =
      measure("参考", reference, step, cols, rows, fps, reference_trajectory, change_floor);
  print_report(ref, cols, rows, step, fps, change_floor);
  if (!ref.ok) return 2;

  st::log::out("\n=== 同口径对照 ===\n");
  const auto relative = [](double mine, double theirs) {
    if (theirs <= 1e-9) return std::string{"—"};
    return std::format("{:.2f}×", mine / theirs);
  };
  st::log::out("  变化强度均值   {:.3f} vs {:.3f}  → {}\n", subject.delta_mean, ref.delta_mean,
            relative(subject.delta_mean, ref.delta_mean));
  st::log::out("  变化强度中位数 {:.3f} vs {:.3f}  → {}\n", subject.delta_median, ref.delta_median,
            relative(subject.delta_median, ref.delta_median));
  st::log::out("  强度峰值       {:.2f} vs {:.2f}  → {}\n", static_cast<double>(subject.delta_max),
            static_cast<double>(ref.delta_max),
            relative(static_cast<double>(subject.delta_max), static_cast<double>(ref.delta_max)));
  st::log::out("  有效变化率     {:.1f}% vs {:.1f}%\n", subject.changed_ratio * 100.0,
            ref.changed_ratio * 100.0);
  st::log::out("  画面沿用率     {:.3f} vs {:.3f}\n", subject.carry_mean, ref.carry_mean);

  // 诊断：把「均值 / 中位数 / 峰值」三者的关系讲成人话。
  st::log::out("\n  诊断：\n");
  const auto diagnose = [](const Measurement& m, const char* who) {
    const double mean_to_median = m.delta_median > 1e-9 ? m.delta_mean / m.delta_median : 0.0;
    // 分级阈值（12 / 4）是实测标定的：
    //   参照片（健康的「静止为主 + 偶尔爆发」）是 5.6；
    //   真正「几乎全静止」的片是 43.2。
    // 最初用 2.0，结果把两者归成同一类，诊断失去区分力。
    if (mean_to_median > 12.0) {
      st::log::out("    · {}：均值/中位数 = {:.1f} → 大多数帧**几乎完全冻住**（尖峰集中在极少数帧）\n",
                who, mean_to_median);
    } else if (mean_to_median > 4.0) {
      st::log::out("    · {}：均值/中位数 = {:.1f} → 静止为主 + 少数强运动段（常见的推进感节奏）\n",
                who, mean_to_median);
    } else {
      st::log::out("    · {}：均值/中位数 = {:.1f} → 运动分布较均匀\n", who, mean_to_median);
    }
    if (m.changed_ratio < 0.10) {
      st::log::out("    · {}：有效变化率 {:.1f}% 极低 —— 大半时间完全冻住\n", who,
                m.changed_ratio * 100.0);
    } else if (m.changed_ratio < 0.25) {
      st::log::out("    · {}：有效变化率 {:.1f}% 偏低 —— 大半时间几乎静止\n", who,
                m.changed_ratio * 100.0);
    }
  };
  diagnose(subject, "片子");
  diagnose(ref, "参考");

  const double strength = ref.delta_median > 1e-9 ? subject.delta_median / ref.delta_median : 0.0;
  st::log::out("\n  结论：按**中位数**衡量的变化强度是参考片的 {:.2f}×", strength);
  if (strength < 0.6) {
    st::log::out("（明显偏弱：内容量/运动幅度不够）\n");
  } else if (strength > 1.6) {
    st::log::out("（明显偏强：可能过于躁动）\n");
  } else {
    st::log::out("（与参考接近）\n");
  }
  return 0;
}
