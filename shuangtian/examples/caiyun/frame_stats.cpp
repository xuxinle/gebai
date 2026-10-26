#include "frame_stats.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/ext/json.hpp"

namespace caiyun {
namespace {

/// Rec.601 亮度权重（整数版，与 `st::raster` 的灰度约定一致）。
constexpr int kLumaR = 77;   // 0.299 * 256
constexpr int kLumaG = 150;  // 0.587 * 256
constexpr int kLumaB = 29;   // 0.114 * 256

/// 两张同尺寸降采样灰度图的平均绍对差（0–255）；尺寸不同返回 -1（不可比）。
[[nodiscard]] auto mean_abs_diff(const std::vector<std::uint8_t>& a,
                                 const std::vector<std::uint8_t>& b) -> double {
  if (a.empty() || a.size() != b.size()) return -1.0;
  double acc = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    acc += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
  }
  return acc / static_cast<double>(a.size());
}

}  // namespace

/// ## 从**已回读的 RGBA 字节**降采样（2026-10-25 GPU 接入）
///
/// 原来直接读 `canvas.pixels()`——那是软件画布的内存直通，零成本。
/// 但 GPU 画布没有可直读的内存，每次 `pixels()` 都要 staging 回读一次。
///
/// 而导出路径**本来就要** `to_rgba8()` 把像素送给 ffmpeg。
/// 所以改成让调用方把那份字节传进来：**一次回读，两处使用**。
/// 这不是优化取舍，而是避免同一帧回读两遍（1080p 每遍约 5~17ms）。
auto StatsAccumulator::downscale_luma(std::span<const std::uint8_t> rgba, int width, int height,
                                      int target_width) -> std::vector<std::uint8_t> {
  const int w = width;
  const int h = height;
  if (w <= 0 || h <= 0) return {};

  // 目标高度按比例取偶（避免奇数行让块均值偏移）；至少 2 行。
  const int out_w = std::clamp(target_width, 2, w);
  const int ratio = std::max(1, w / out_w);
  const int out_h = std::max(2, ((h / ratio) / 2) * 2);

  // 入参是 **RGBA 字节序**（`to_rgba8()` 的输出，每像素 4 字节）。
  // 校验要按 4 字节算，否则末尾会读越界。
  const std::span<const std::uint8_t> px = rgba;
  if (px.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U) return {};

  std::vector<std::uint8_t> out(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h), 0);
  for (int oy = 0; oy < out_h; ++oy) {
    const int y0 = oy * h / out_h;
    const int y1 = std::max(y0 + 1, (oy + 1) * h / out_h);
    for (int ox = 0; ox < out_w; ++ox) {
      const int x0 = ox * w / out_w;
      const int x1 = std::max(x0 + 1, (ox + 1) * w / out_w);
      // 块均值（不是抽样）：抽样会让"只移动一像素的细线"整块丢失或整块突变，
      // 静止判据因此抖动。块均值让变化按覆盖面积平滑计入。
      std::uint64_t sum = 0;
      std::uint32_t count = 0;
      for (int y = y0; y < y1; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = x0; x < x1; ++x) {
          // ## `to_rgba8()` 的字节序是 **A,B,G,R**，不是 R,G,B,A（2026-10-25 实测）
          //
          // 探针 `tools/frame_stats_diff_probe` 打出来的实证：
          // 写 `(255<<24)|(b<<16)|(g<<8)|r`（0xAABBGGRR）后，`to_rgba8()[0..3]`
          // 是 `(255, 255, 255, 16)` —— 而期望的 r=16 在**最后一个**字节。
          // 所以顺序是 A,B,G,R（`Canvas` 内部是 0xAABBGGRR 的预乘值，直接按字节吐出来）。
          //
          // 之前按 R,G,B,A 读，读到的"红"其实是 **alpha（恒 255）**：
          // 每帧亮度都是 255 → 帧间差恒 0 → `still_ratio` 恒 100%、
          // `change_density` 恒 0。症状是"两帧明明不同却判成静止"。
          const std::size_t base = (row + static_cast<std::size_t>(x)) * 4U;
          const std::uint32_t argb = (static_cast<std::uint32_t>(px[base + 3U]) << 24U) |
                                     (static_cast<std::uint32_t>(px[base + 2U]) << 16U) |
                                     (static_cast<std::uint32_t>(px[base + 1U]) << 8U) |
                                     static_cast<std::uint32_t>(px[base]);
          // **Canvas 的像素是 0xRRGGBBAA**（实测：写 R=0x11 G=0x22 B=0x33 A=0xFF
          // 后 raw=0x112233FF，即 R 在最高字节、A 在最低字节）。
          //
          // 这一句曾写反（按 0xAARRGGBB 读），后果是**把 alpha 当成红**：全不透明
          // 画面被算成"纯红"，灰度恒为 76 左右；而静止占比仍能算对（两边同样错），
          // 所以只有亮度相关的判据会偏——数值看起来完全"正常"，最难发现。
          // 布局由 `tests/caiyun_frame_stats_test.cpp` 的
          // frame_stats_luma_reads_the_right_channels 钉住。
          const int r = static_cast<int>((argb >> 24) & 0xFFU);
          const int g = static_cast<int>((argb >> 16) & 0xFFU);
          const int b = static_cast<int>((argb >> 8) & 0xFFU);
          const int a = static_cast<int>(argb & 0xFFU);
          // 透明像素按"画布底色"处理：直接算 RGB 会把预乘前的黑当成暗场，
          // 让空白区域虚报为黑（本项目全帧不透明，但局部渲染/遮罩会碰到）。
          const int lum = a == 0 ? 0 : (kLumaR * r + kLumaG * g + kLumaB * b) >> 8;
          sum += static_cast<std::uint64_t>(lum);
          ++count;
        }
      }
      out[static_cast<std::size_t>(oy) * static_cast<std::size_t>(out_w) + static_cast<std::size_t>(ox)] =
          count > 0 ? static_cast<std::uint8_t>(sum / count) : 0;
    }
  }
  return out;
}

auto StatsAccumulator::push(std::span<const std::uint8_t> rgba, int width, int height, int index,
                            double time_s) -> FrameStats {
  std::vector<std::uint8_t> luma = downscale_luma(rgba, width, height, kStatsWidth);
  std::vector<std::uint8_t> luma_hi = downscale_luma(rgba, width, height, kHiStatsWidth);
  FrameStats stats{};
  stats.index = index;
  stats.time_s = time_s;
  if (luma.empty()) return stats;

  double sum = 0.0;
  for (const std::uint8_t v : luma) sum += static_cast<double>(v);
  stats.mean_luma = sum / static_cast<double>(luma.size());

  // 返回值里的帧差只给「本路相邻帧」——并行时这几乎总是非相邻帧，
  // 因而**不作为任何判据的依据**（判据在 report() 里按归并后的帧号算）。
  // 保留它是因为 worker 侧日志/诊断会用，且单线程场景下它就是真值。
  if (has_prev_ && index == prev_.index + 1) {
    stats.diff_prev = mean_abs_diff(prev_luma_, luma);
  } else {
    stats.diff_prev = -1.0;  // -1 = 无相邻前帧
  }
  prev_luma_ = luma;
  prev_ = stats;
  has_prev_ = true;

  Sample sample{};
  sample.index = index;
  sample.time_s = time_s;
  sample.mean_luma = stats.mean_luma;
  sample.luma = std::move(luma);
  sample.luma_hi = std::move(luma_hi);
  samples_.push_back(std::move(sample));
  return stats;
}

void StatsAccumulator::merge(const StatsAccumulator& other) {
  samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
  // prev_ 不并（归并后帧序未必连续，帧差一律在 report() 里重算）
}

void StatsAccumulator::attach_spans(std::vector<ShotSpan> spans) { spans_ = std::move(spans); }

auto StatsAccumulator::report(double duration_s, bool partial) const -> QualityReport {
  QualityReport out{};
  out.partial = partial;
  out.frames = static_cast<int>(samples_.size());
  if (samples_.empty()) {
    out.summary = "无帧（未渲染）";
    return out;
  }

  // 按 index 排序后统计：并行归并进来的顺序是"按 worker 拼接"的，不是时间序。
  std::vector<const Sample*> ordered{};
  ordered.reserve(samples_.size());
  for (const auto& f : samples_) ordered.push_back(&f);
  std::sort(ordered.begin(), ordered.end(),
            [](const Sample* a, const Sample* b) { return a->index < b->index; });

  // 帧差：排序后按**帧号相邻**算（不从喂入时的 diff_prev 取——并行时那是 0/16/32…）。
  // `diff` 与 `ordered` 对齐：`diff[i]` = 帧 `ordered[i]` 与其前一张（帧号恰为 -1）的差；
  // -1 = 无相邻前帧（首帧或前面丢了一帧），不计入分母。
  std::vector<double> diff(ordered.size(), -1.0);
  // 主口径的两个量（见 header 的说明：它们不被元素尺寸稀释）
  std::vector<double> changed_pct(ordered.size(), -1.0);
  std::vector<double> max_delta(ordered.size(), -1.0);
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    if (ordered[i]->index != ordered[i - 1]->index + 1) continue;
    diff[i] = mean_abs_diff(ordered[i - 1]->luma, ordered[i]->luma);
    const auto& a = ordered[i - 1]->luma_hi;
    const auto& b = ordered[i]->luma_hi;
    if (a.size() != b.size() || a.empty()) continue;
    std::size_t changed = 0;
    int peak = 0;
    for (std::size_t k = 0; k < a.size(); ++k) {
      const int d = std::abs(static_cast<int>(a[k]) - static_cast<int>(b[k]));
      if (d > peak) peak = d;
      if (d > kChangePixelDelta) ++changed;
    }
    changed_pct[i] = 100.0 * static_cast<double>(changed) / static_cast<double>(a.size());
    max_delta[i] = static_cast<double>(peak);
  }

  double luma_sum = 0.0;
  int pairs = 0;
  int still = 0;
  int moving = 0;
  int bright = 0;
  double changed_sum = 0.0;
  int changed_pairs = 0;
  int moving_by_pixels = 0;
  std::vector<double> max_deltas{};
  max_deltas.reserve(ordered.size());
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    luma_sum += ordered[i]->mean_luma;
    if (ordered[i]->mean_luma > out.bright_luma) ++bright;
    if (diff[i] >= 0.0) {
      ++pairs;
      if (diff[i] < out.static_threshold) {
        ++still;
      } else {
        ++moving;
      }
    }
    if (changed_pct[i] >= 0.0) {
      ++changed_pairs;
      changed_sum += changed_pct[i];
      // “在动”的像素级判据：变化像素 > 1%。为何 1%：一个 200×200 元素
      // 占 1080p 的 4%，移动它自身尺寸量级的距离就会让 >1% 的格子变化——
      // 而纯噪点在 192 宽降采样+阈值 6 下几乎不会超过 0.5%。
      if (changed_pct[i] > 1.0) ++moving_by_pixels;
    }
    if (max_delta[i] >= 0.0) max_deltas.push_back(max_delta[i]);
  }
  out.mean_luma = luma_sum / static_cast<double>(ordered.size());
  out.still_frames = still;
  out.bright_frames = bright;
  out.still_ratio = pairs > 0 ? 100.0 * static_cast<double>(still) / static_cast<double>(pairs) : 0.0;
  out.bright_ratio = 100.0 * static_cast<double>(bright) / static_cast<double>(ordered.size());
  out.change_density = duration_s > 0.0 ? static_cast<double>(moving) / duration_s : 0.0;
  out.frame_pairs = pairs;
  if (changed_pairs > 0) {
    out.changed_pixels_pct = changed_sum / static_cast<double>(changed_pairs);
    out.moving_pair_ratio = 100.0 * static_cast<double>(moving_by_pixels) / static_cast<double>(changed_pairs);
  }
  if (!max_deltas.empty()) {
    std::sort(max_deltas.begin(), max_deltas.end());
    // 中位数而不是均值：单帧一个像素的尖尖（切换、过渡闪白）会把均值拉高，
    // 而我们要的是“通常有多大差异”。
    const std::size_t mid = max_deltas.size() / 2;
    out.max_pixel_delta = max_deltas.size() % 2 == 0
                              ? 0.5 * (max_deltas[mid - 1] + max_deltas[mid])
                              : max_deltas[mid];
  }

  // —— 逐镜 ——
  out.shots.clear();
  for (const ShotSpan& span : spans_) {
    ShotStats shot{};
    shot.clip_id = span.clip_id;
    shot.scene_index = span.scene_index;
    shot.label = span.label;
    shot.t_in = span.t_in;
    shot.t_out = span.t_out;
    double shot_luma = 0.0;
    int shot_pairs = 0;
    double shot_changed = 0.0;
    int shot_changed_pairs = 0;
    std::vector<double> shot_max{};
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      const Sample* f = ordered[i];
      if (f->time_s < span.t_in || f->time_s >= span.t_out) continue;
      ++shot.frames;
      shot_luma += f->mean_luma;
      if (diff[i] >= 0.0) {
        ++shot_pairs;
        if (diff[i] < out.static_threshold) ++shot.still_frames;
      }
      if (changed_pct[i] >= 0.0) {
        ++shot_changed_pairs;
        shot_changed += changed_pct[i];
        shot_max.push_back(max_delta[i]);
      }
    }
    shot.frame_pairs = shot_pairs;
    shot.mean_luma = shot.frames > 0 ? shot_luma / static_cast<double>(shot.frames) : 0.0;
    if (shot_changed_pairs > 0) {
      shot.changed_pixels_pct = shot_changed / static_cast<double>(shot_changed_pairs);
    }
    if (!shot_max.empty()) {
      std::sort(shot_max.begin(), shot_max.end());
      const std::size_t mid = shot_max.size() / 2;
      shot.max_pixel_delta = shot_max.size() % 2 == 0 ? 0.5 * (shot_max[mid - 1] + shot_max[mid])
                                                      : shot_max[mid];
    }
    out.shots.push_back(shot);
  }
  // 静止占比最高的一镜：这就是「慢在哪」的答案
  double worst = -1.0;
  for (const ShotStats& s : out.shots) {
    if (s.frames > 1 && s.still_ratio() > worst) {
      worst = s.still_ratio();
      out.worst_still_clip_id = s.clip_id;
    }
  }

  // —— 可读摘要 ——
  std::string worst_line{};
  for (const ShotStats& s : out.shots) {
    if (s.clip_id == out.worst_still_clip_id && s.frames > 1) {
      worst_line = std::format("；最静一镜「{}」{}%（{:.2f}–{:.2f}s，变化像素 {:.2f}%）",
                               s.label, std::format("{:.1f}", s.still_ratio()), s.t_in,
                               s.t_out, s.changed_pixels_pct);
      break;
    }
  }
  out.summary = std::format(
      "{} 帧｜参考量 变化像素 {:.2f}% / 格均差峰值 {:.0f}（无及格线，仅供定位）"
      "｜静止 {:.1f}%（帧差 <{:.1f}，{} 对）｜过亮 {:.1f}%（>{:.0f}）"
      "｜平均亮度 {:.1f}｜变化 {:.2f}/s{}",
      out.frames, out.changed_pixels_pct, out.max_pixel_delta,
      out.still_ratio, out.static_threshold, pairs, out.bright_ratio, out.bright_luma,
      out.mean_luma, out.change_density, worst_line);
  // 分母为 0 时必须明说：否则「0 对 / 0.0%」会被读成一个有效的「不静」结论。
  if (pairs == 0) out.summary += "｜⚠ 无有效相邻帧对（只渲了 1 帧或帧号不连续）：静止占比不可信";
  if (partial) out.summary += "（局部渲染，结论只覆盖该区间）";
  return out;
}

auto quality_to_json(const QualityReport& report) -> std::string {
  st::Json root = st::Json::object();
  root["static_threshold"] = report.static_threshold;
  root["bright_luma"] = report.bright_luma;
  root["probe_width"] = report.probe_width;
  root["frames"] = report.frames;
  root["frame_pairs"] = report.frame_pairs;
  root["still_frames"] = report.still_frames;
  root["bright_frames"] = report.bright_frames;
  root["still_ratio"] = report.still_ratio;
  root["bright_ratio"] = report.bright_ratio;
  root["mean_luma"] = report.mean_luma;
  root["change_density"] = report.change_density;
  root["changed_pixels_pct"] = report.changed_pixels_pct;
  root["max_pixel_delta"] = report.max_pixel_delta;
  root["moving_pair_ratio"] = report.moving_pair_ratio;
  root["worst_still_clip_id"] = report.worst_still_clip_id;
  root["partial"] = report.partial;
  root["summary"] = report.summary;
  st::Json shots = st::Json::array();
  for (const ShotStats& shot : report.shots) {
    st::Json item = st::Json::object();
    item["clip_id"] = shot.clip_id;
    item["scene_index"] = shot.scene_index;
    item["label"] = shot.label;
    item["t_in"] = shot.t_in;
    item["t_out"] = shot.t_out;
    item["frames"] = shot.frames;
    item["frame_pairs"] = shot.frame_pairs;
    item["still_frames"] = shot.still_frames;
    item["still_ratio"] = shot.still_ratio();
    item["changed_pixels_pct"] = shot.changed_pixels_pct;
    item["max_pixel_delta"] = shot.max_pixel_delta;
    item["mean_luma"] = shot.mean_luma;
    shots.push_back(std::move(item));
  }
  root["shots"] = std::move(shots);
  return root.dump();
}

}  // namespace caiyun
