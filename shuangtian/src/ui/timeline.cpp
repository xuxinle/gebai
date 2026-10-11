#include "st/ui/timeline.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace st::ui::timeline {

namespace {

/// `hit_test` 的默认修剪边缘带宽（`edge_px` 缺省值用）。
/// 定义在这里而不是函数签名默认值里：`Model::hit_test` 是公开接口，
/// 而"默认多宽"是**实现口径**（调用方可覆盖）。
constexpr float kDefaultEdgePx = 6.0f;

/// 毫秒 → 秒（浮点）。换算里唯一的除法点。
[[nodiscard]] auto to_seconds(Millis value) -> double {
  return static_cast<double>(value) / 1000.0;
}

}  // namespace

void Model::set_pixels_per_second(float value) {
  // 下限保护：0 或负数会让 `time_to_x` 除零/反向，而那种输入在缩放滑杆拉到尽头时
  // 很容易出现。上限只是防止极端值下的浮点溢出（不是"不能更大"）。
  pixels_per_second_ = std::clamp(value, 0.25f, 100'000.0f);
}

void Model::set_scroll(Millis time) {
  scroll_ = std::max<Millis>(0, time);
}

auto Model::time_to_x(Millis time) const -> float {
  return static_cast<float>(to_seconds(time - scroll_)) * pixels_per_second_;
}

auto Model::x_to_time(float x) const -> Millis {
  const auto seconds = static_cast<double>(x) / static_cast<double>(pixels_per_second_);
  return scroll_ + static_cast<Millis>(std::llround(seconds * 1000.0));
}

auto Model::visible_range() const -> std::pair<Millis, Millis> {
  const double pps = static_cast<double>(std::max(0.001f, pixels_per_second_));
  const double seconds = static_cast<double>(viewport_width_) / pps;
  return {scroll_, scroll_ + static_cast<Millis>(std::llround(seconds * 1000.0))};
}

void Model::set_tracks(std::vector<Track> tracks) {
  tracks_ = std::move(tracks);
}

auto Model::find_clip(std::size_t track, std::size_t clip) -> Clip* {
  if (track >= tracks_.size() || clip >= tracks_[track].clips.size()) return nullptr;
  return &tracks_[track].clips[clip];
}

auto Model::find_clip(std::size_t track, std::size_t clip) const -> const Clip* {
  if (track >= tracks_.size() || clip >= tracks_[track].clips.size()) return nullptr;
  return &tracks_[track].clips[clip];
}

auto Model::locate(std::string_view clip_id) const -> std::pair<std::size_t, std::size_t> {
  for (std::size_t track = 0; track < tracks_.size(); ++track) {
    for (std::size_t clip = 0; clip < tracks_[track].clips.size(); ++clip) {
      if (tracks_[track].clips[clip].id == clip_id) return {track, clip};
    }
  }
  return {kNone, kNone};
}

auto Model::content_duration() const noexcept -> Millis {
  Millis latest = 0;
  for (const auto& track : tracks_) {
    for (const auto& clip : track.clips) {
      latest = std::max(latest, clip.end());
    }
  }
  return latest;
}

auto Model::track_top(std::size_t index) const -> float {
  float top = 0.0f;
  for (std::size_t cursor = 0; cursor < index && cursor < tracks_.size(); ++cursor) {
    top += tracks_[cursor].height;
  }
  return top;
}

auto Model::track_rect(std::size_t index) const -> std::pair<float, float> {
  if (index >= tracks_.size()) return {0.0f, 0.0f};
  const float top = track_top(index);
  return {top, top + tracks_[index].height};
}

auto Model::clip_x_range(std::size_t track, std::size_t clip) const
    -> std::pair<float, float> {
  const Clip* item = find_clip(track, clip);
  if (item == nullptr) return {0.0f, 0.0f};
  return {time_to_x(item->start), time_to_x(item->end())};
}

auto Model::track_at_y(float y) const -> std::size_t {
  if (y < 0.0f) return kNone;
  float cursor = 0.0f;
  for (std::size_t index = 0; index < tracks_.size(); ++index) {
    const float bottom = cursor + tracks_[index].height;
    if (y < bottom) return index;
    cursor = bottom;
  }
  return kNone;
}

auto Model::playhead_x() const -> std::optional<float> {
  const float x = time_to_x(playhead_);
  if (x < -1.0f || x > viewport_width_ + 1.0f) return std::nullopt;
  return x;
}

auto Model::hit_test(float x, float y, float edge_px) const -> Hit {
  Hit result;
  const std::size_t track = track_at_y(y);
  if (track == kNone) return result;
  result.track = track;

  // ② **修剪边缘优先于播放头**（但播放头刚已判过）——这里先看有没有边缘命中：
  // 若这一 x 落在某个片段的修剪边缘上，它比播放头更该被抓住（播放头随时可以
  // 点标尺移走，而边缘是"只能在这里抓"的操作）。
  //
  // 顺序设计：**标尺带里播放头优先**（那里没有片段），**轨道区里边缘优先**。
  // 两者共存的情形（播放头停在 0ms、第一个片段也从 0 开始）曾让左缘完全抓不住
  //（实测：修剪左边缘没反应）。所以这里先扫一遍边缘，命中就返回；
  // 没有再回头看播放头。
  const auto& clips = tracks_[track].clips;
  for (std::size_t offset = clips.size(); offset > 0; --offset) {
    const std::size_t index = offset - 1;
    const Clip& clip = clips[index];
    if (clip.duration <= 0) continue;
    const auto [left, right] = clip_x_range(track, index);
    const float edge = std::min(kDefaultEdgePx, std::max(0.5f, (right - left) / 3.0f));
    const bool on_left = x >= left && x < left + edge;
    const bool on_right = x > right - edge && x <= right;
    if (!on_left && !on_right) continue;
    result.track = track;
    result.clip = index;
    result.part = on_left ? ClipPart::LeftEdge : ClipPart::RightEdge;
    return result;
  }
  // ③ 没有边缘命中：这时才让播放头抢（它可能压在片体上）。
  if (const auto head = playhead_x(); head.has_value()) {
    if (std::fabs(x - *head) <= 2.0f) {
      result.playhead = true;
      return result;
    }
  }
  // ④ 片体（同一 x 上从右往左取"视觉上压在上面"的那条）。
  for (std::size_t offset = clips.size(); offset > 0; --offset) {
    const std::size_t index = offset - 1;
    const Clip& clip = clips[index];
    if (clip.duration <= 0) continue;
    const auto [left, right] = clip_x_range(track, index);
    if (x < left || x > right) continue;
    result.clip = index;
    // 修剪边缘：宽度取 `edge_px`，但**不能超过片宽的 1/3**——很窄的片段若两端
    // 各占 `edge_px`，就再也点不到"片体"（无法移动它）。这是"小片段拖不动"的根因。
    // 边缘带宽度：不超过片宽的 1/3，且**严格小于半宽**——很窄的片段若两端各占
    // 1px，中点就会同时"<= 左缘带"与">= 右缘带"，取谁都不对（实测：2px 宽的片段
    // 点正中被判成左缘，于是"想移动它却变成修剪"）。
    const float edge = std::min(edge_px, std::max(0.5f, (right - left) / 3.0f));
    // 判据用**严格小于**：落在 `left + edge` 上的点已经在片体里（边缘带是
    // `[left, left+edge)` 半开区间）。
    if (x < left + edge) {
      result.part = ClipPart::LeftEdge;
    } else if (x > right - edge) {
      result.part = ClipPart::RightEdge;
    } else {
      result.part = ClipPart::Body;
    }
    return result;
  }
  return result;
}

auto Model::snap(Millis time, const SnapOptions& options, std::size_t exclude_track,
                 std::size_t exclude_clip) const -> Millis {
  // 阈值：像素 → 毫秒。**必须**走这个换算：用固定的毫秒阈值会让"放大后吸附极难触发"
  //（放大 10 倍时，同样的 8px 只对应 1/10 的毫秒数）。
  const double snap_pps = static_cast<double>(std::max(0.001f, pixels_per_second_));
  const Millis tolerance = static_cast<Millis>(
      std::llround(static_cast<double>(options.threshold_px) / snap_pps * 1000.0));
  if (tolerance <= 0) return time;

  Millis best = time;
  Millis best_distance = tolerance + 1;
  const auto consider = [&](Millis candidate) {
    const Millis distance = std::llabs(candidate - time);
    if (distance <= tolerance && distance < best_distance) {
      best = candidate;
      best_distance = distance;
    }
  };

  if (options.to_zero) consider(0);
  if (options.to_playhead) consider(playhead_);
  for (const Millis marker : options.markers) consider(marker);
  if (options.to_clip_edges) {
    for (std::size_t track = 0; track < tracks_.size(); ++track) {
      for (std::size_t index = 0; index < tracks_[track].clips.size(); ++index) {
        // ⚠ 排除被拖的那个片段自身：不排除的话它**永远**吸在自己原来的位置上——
        // 表现是"拖不动"（吸附实现最经典的错法）。
        if (track == exclude_track && index == exclude_clip) continue;
        consider(tracks_[track].clips[index].start);
        consider(tracks_[track].clips[index].end());
      }
    }
  }
  return best;
}

auto Model::selected_clips() const -> std::vector<std::pair<std::size_t, std::size_t>> {
  std::vector<std::pair<std::size_t, std::size_t>> out;
  for (std::size_t track = 0; track < tracks_.size(); ++track) {
    for (std::size_t clip = 0; clip < tracks_[track].clips.size(); ++clip) {
      if (tracks_[track].clips[clip].selected) out.emplace_back(track, clip);
    }
  }
  return out;
}

void Model::clear_selection() {
  for (auto& track : tracks_) {
    for (auto& clip : track.clips) clip.selected = false;
  }
}

auto format_timecode(Millis time, bool compact) -> std::string {
  // 负数（拖到 0 之前）按 0 显示：时间轴上没有"负时间"，
  // 而显示 `-00:00:01` 会让人以为数据坏了。
  const Millis clamped = std::max<Millis>(0, time);
  const Millis total_seconds = clamped / 1000;
  const Millis millis = clamped % 1000;
  const Millis seconds = total_seconds % 60;
  const Millis minutes = (total_seconds / 60) % 60;
  const Millis hours = total_seconds / 3600;
  if (compact && hours == 0) {
    return std::format("{:02}:{:02}.{:03}", minutes, seconds, millis);
  }
  return std::format("{:02}:{:02}:{:02}.{:03}", hours, minutes, seconds, millis);
}

auto ruler_step(Millis span, float pixels_per_second, float min_px) -> Millis {
  if (span <= 0 || pixels_per_second <= 0.0f) return 1000;
  // 先用"最小像素间距"反推一个下界步长，再向上跳到 1/2/5 × 10^n 档。
  const double min_ms = static_cast<double>(min_px) / static_cast<double>(pixels_per_second) *
                        1000.0;
  double step = 1.0;   // 毫秒
  while (step < min_ms) {
    // 1 → 2 → 5 → 10 → 20 → 50 …
    const double magnitude = std::pow(10.0, std::floor(std::log10(step)));
    const double mantissa = step / magnitude;
    if (mantissa < 1.5) {
      step = 2.0 * magnitude;
    } else if (mantissa < 3.5) {
      step = 5.0 * magnitude;
    } else {
      step = 10.0 * magnitude;
    }
  }
  (void)span;
  return static_cast<Millis>(std::llround(step));
}

}  // namespace st::ui::timeline
