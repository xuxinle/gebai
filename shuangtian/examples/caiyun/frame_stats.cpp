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

}  // namespace

auto StatsAccumulator::downscale_luma(const st::raster::Canvas& canvas) -> std::vector<std::uint8_t> {
  const int w = canvas.physical_width();
  const int h = canvas.physical_height();
  if (w <= 0 || h <= 0) return {};

  // 目标高度按比例取偶（避免奇数行让块均值偏移）；至少 2 行。
  const int out_w = kStatsWidth;
  const int ratio = std::max(1, w / out_w);
  const int out_h = std::max(2, ((h / ratio) / 2) * 2);

  const std::span<const std::uint32_t> px = canvas.pixels();
  if (px.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) return {};

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
          const std::uint32_t argb = px[row + static_cast<std::size_t>(x)];
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

auto StatsAccumulator::push(const st::raster::Canvas& canvas, int index, double time_s) -> FrameStats {
  const std::vector<std::uint8_t> luma = downscale_luma(canvas);
  FrameStats stats{};
  stats.index = index;
  stats.time_s = time_s;
  if (luma.empty()) return stats;

  double sum = 0.0;
  for (const std::uint8_t v : luma) sum += static_cast<double>(v);
  stats.mean_luma = sum / static_cast<double>(luma.size());

  // 帧差只在**同一 worker 的相邻帧**之间有意义。并行渲染时 index 不连续，
  // 若拿"上一个 push 的帧"去比，会把两个不相邻的帧相减得到伪运动。
  // 因此这里要求调用方按 index 顺序喂（render.cpp 里每个 worker 严格递增），
  // 且只接受 index 恰为 prev+1 的情况；否则标记为"无相邻前帧"。
  //
  // **首帧也必须标记为 -1**：`FrameStats::diff_prev` 的默认值是 0.0，而 0.0 在
  // report() 里会通过 `>= 0` 检查并被判成「静止」。于是任何序列的第一帧都被算作
  // 静止帧——10 帧运动序列会得到 1/9 ≈ 11% 的静止占比（实测就是这么错的）。
  // 第一帧没有前一帧可比，它**不是**静止。
  if (has_prev_ && index == prev_.index + 1 && prev_luma_.size() == luma.size()) {
    double acc = 0.0;
    for (std::size_t i = 0; i < luma.size(); ++i) {
      acc += std::abs(static_cast<double>(luma[i]) - static_cast<double>(prev_luma_[i]));
    }
    stats.diff_prev = acc / static_cast<double>(luma.size());
  } else {
    stats.diff_prev = -1.0;  // -1 = 无相邻前帧（报告据此不计入静止判据）
  }

  prev_luma_ = luma;
  prev_ = stats;
  has_prev_ = true;
  frames_.push_back(stats);
  return stats;
}

void StatsAccumulator::merge(const StatsAccumulator& other) {
  frames_.insert(frames_.end(), other.frames_.begin(), other.frames_.end());
  // prev_ 不并（归并后帧序未必连续，差分已各自算好）
}

void StatsAccumulator::attach_spans(std::vector<ShotSpan> spans) { spans_ = std::move(spans); }

auto StatsAccumulator::report(double duration_s, bool partial) const -> QualityReport {
  QualityReport out{};
  out.partial = partial;
  out.frames = static_cast<int>(frames_.size());
  if (frames_.empty()) {
    out.summary = "无帧（未渲染）";
    return out;
  }

  // 按 index 排序后统计：并行归并进来的顺序是"按 worker 拼接"的，不是时间序。
  std::vector<const FrameStats*> ordered{};
  ordered.reserve(frames_.size());
  for (const auto& f : frames_) ordered.push_back(&f);
  std::sort(ordered.begin(), ordered.end(),
            [](const FrameStats* a, const FrameStats* b) { return a->index < b->index; });

  double luma_sum = 0.0;
  int pairs = 0;
  int still = 0;
  int moving = 0;
  int bright = 0;
  for (const FrameStats* f : ordered) {
    luma_sum += f->mean_luma;
    if (f->mean_luma > out.bright_luma) ++bright;
    if (f->diff_prev >= 0.0) {
      ++pairs;
      if (f->diff_prev < out.static_threshold) {
        ++still;
      } else {
        ++moving;
      }
    }
  }
  out.mean_luma = luma_sum / static_cast<double>(ordered.size());
  out.still_frames = still;
  out.bright_frames = bright;
  out.still_ratio = pairs > 0 ? 100.0 * static_cast<double>(still) / static_cast<double>(pairs) : 0.0;
  out.bright_ratio = 100.0 * static_cast<double>(bright) / static_cast<double>(ordered.size());
  out.change_density = duration_s > 0.0 ? static_cast<double>(moving) / duration_s : 0.0;

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
    for (const FrameStats* f : ordered) {
      if (f->time_s < span.t_in || f->time_s >= span.t_out) continue;
      ++shot.frames;
      shot_luma += f->mean_luma;
      if (f->diff_prev >= 0.0 && f->diff_prev < out.static_threshold) ++shot.still_frames;
    }
    shot.mean_luma = shot.frames > 0 ? shot_luma / static_cast<double>(shot.frames) : 0.0;
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
      worst_line = std::format("；最静一镜「{}」{}%（{:.2f}–{:.2f}s）", s.label,
                               std::format("{:.1f}", s.still_ratio()), s.t_in, s.t_out);
      break;
    }
  }
  out.summary = std::format(
      "{} 帧｜静止 {:.1f}%（帧差 <{:.1f}，{} 对）｜过亮 {:.1f}%（>{:.0f}）"
      "｜平均亮度 {:.1f}｜变化 {:.2f}/s{}",
      out.frames, out.still_ratio, out.static_threshold, pairs, out.bright_ratio,
      out.bright_luma, out.mean_luma, out.change_density, worst_line);
  if (partial) out.summary += "（局部渲染，结论只覆盖该区间）";
  return out;
}

auto quality_to_json(const QualityReport& report) -> std::string {
  st::Json root = st::Json::object();
  root["static_threshold"] = report.static_threshold;
  root["bright_luma"] = report.bright_luma;
  root["probe_width"] = report.probe_width;
  root["frames"] = report.frames;
  root["still_frames"] = report.still_frames;
  root["bright_frames"] = report.bright_frames;
  root["still_ratio"] = report.still_ratio;
  root["bright_ratio"] = report.bright_ratio;
  root["mean_luma"] = report.mean_luma;
  root["change_density"] = report.change_density;
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
    item["still_frames"] = shot.still_frames;
    item["still_ratio"] = shot.still_ratio();
    item["mean_luma"] = shot.mean_luma;
    shots.push_back(std::move(item));
  }
  root["shots"] = std::move(shots);
  return root.dump();
}

}  // namespace caiyun
