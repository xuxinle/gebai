#include "snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <map>

#include "director.hpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"

namespace caiyun {
namespace {

using st::ErrorCode;

[[nodiscard]] auto now_iso8601() -> std::string {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z", tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/// 抽帧位置：入场后 / 中段 / 落定后（**不是** 0%/50%/100%）。
///
/// 边界帧会拍到淡入淡出的半透明画面与转场中间态——那些帧两版之间
/// "永远不同"（因为转场曲线一改就变），但它们不是要看的东西。
constexpr double kSamplePositions[] = {0.30, 0.50, 0.85};
constexpr const char* kSampleRoles[] = {"in", "mid", "out"};

/// `frame_00012.png` / `frame_0012.png` → 12；不匹配返回 -1。
///
/// 位数不固定：渲染器的 `--export-dir` 产物用 4 位（`frame_0001.png`），
/// 而早期帧序列用的是 5 位。**两者都收**——固定位数会让另一半静默抽不到帧。
[[nodiscard]] auto parse_frame_index(std::string_view name) -> int {
  constexpr std::string_view kPrefix = "frame_";
  constexpr std::string_view kSuffix = ".png";
  if (name.size() <= kPrefix.size() + kSuffix.size()) return -1;
  if (name.substr(0, kPrefix.size()) != kPrefix) return -1;
  if (name.substr(name.size() - kSuffix.size()) != kSuffix) return -1;
  const std::string digits{name.substr(kPrefix.size(), name.size() - kPrefix.size() -
                                                           kSuffix.size())};
  if (digits.empty()) return -1;
  int value = 0;
  for (const char ch : digits) {
    if (ch < '0' || ch > '9') return -1;
    value = value * 10 + (ch - '0');
  }
  // 文件名是 **1 起**（`frame_0001.png` 是第一帧），而场景边界按 0 起算。
  // 这里统一减一，使返回的 index 与 `总帧号 = floor(t*fps)` 同口径——
  // 否则每一帧都会错一格，抽帧位置全部偏一帧。
  return value - 1;
}

}  // namespace

auto SnapshotDiff::verdict() const -> std::string {
  if (compared == 0) return "无可比帧（两版没有同号的帧）";
  if (only_left != 0 || only_right != 0) {
    return std::format("帧数不一致（{} 对可比，左独有 {}，右独有 {}）——剧本时长变了",
                       compared, only_left, only_right);
  }
  if (mean_diff_pct <= 0.0 && worst_diff_pct <= 0.0) return "逐像素完全一致";
  // 0.05% 是"肉眼看不到"与"看得到"之间的经验界：1080p 上约 1000 个像素。
  if (mean_diff_pct < 0.05) return std::format("几乎一致（均差 {:.4f}%）", mean_diff_pct);
  if (mean_diff_pct < 2.0) return std::format("局部改动（均差 {:.3f}%）", mean_diff_pct);
  return std::format("显著变化（均差 {:.3f}%）", mean_diff_pct);
}

auto read_snapshot(std::string_view dir) -> Result<Snapshot> {
  const std::string path = st::fs::join(std::string(dir), std::string(kSnapshotMetaFile));
  const auto text = st::fs::read_text(path);
  if (!text) {
    return st::unexpected(ErrorCode::NotFound,
                          std::format("快照缺少元信息文件：{}", path));
  }
  const auto parsed = st::json_parse(*text);
  if (!parsed || !parsed->is_object()) {
    return st::unexpected(ErrorCode::Parse, std::format("快照元信息不是合法 JSON：{}", path));
  }
  Snapshot snap{};
  snap.name = st::json_get_string(*parsed, "name");
  snap.created_at = st::json_get_string(*parsed, "created_at");
  snap.script_sha256 = st::json_get_string(*parsed, "script_sha256");
  snap.width = static_cast<int>(st::json_get_i64(*parsed, "width", 0));
  snap.height = static_cast<int>(st::json_get_i64(*parsed, "height", 0));
  snap.fps = st::json_get_double(*parsed, "fps", 30.0);
  snap.total_frames = static_cast<int>(st::json_get_i64(*parsed, "total_frames", 0));
  if (const auto frames = parsed->find("frames"); frames != parsed->end() && frames->is_array()) {
    for (const auto& item : *frames) {
      if (!item.is_object()) continue;
      SnapshotFrame frame{};
      frame.index = static_cast<int>(st::json_get_i64(item, "index", -1));
      frame.time_s = st::json_get_double(item, "time_s", 0.0);
      frame.scene_index = static_cast<int>(st::json_get_i64(item, "scene", -1));
      frame.role = st::json_get_string(item, "role");
      frame.path = st::json_get_string(item, "path");
      if (frame.index >= 0) snap.frames.push_back(std::move(frame));
    }
  }
  return snap;
}

auto write_snapshot(std::string_view dir, const Snapshot& snapshot) -> st::Status {
  st::Json root = st::Json::object();
  root["name"] = snapshot.name;
  root["created_at"] = snapshot.created_at;
  root["script_sha256"] = snapshot.script_sha256;
  root["width"] = snapshot.width;
  root["height"] = snapshot.height;
  root["fps"] = snapshot.fps;
  root["total_frames"] = snapshot.total_frames;
  st::Json frames = st::Json::array();
  for (const SnapshotFrame& frame : snapshot.frames) {
    st::Json node = st::Json::object();
    node["index"] = frame.index;
    node["time_s"] = frame.time_s;
    node["scene"] = frame.scene_index;
    node["role"] = frame.role;
    node["path"] = frame.path;
    frames.push_back(std::move(node));
  }
  root["frames"] = std::move(frames);
  return st::fs::write_text(
      st::fs::join(std::string(dir), std::string(kSnapshotMetaFile)),
      st::json_dump(root, true, 2));
}

auto diff_png(std::string_view path_a, std::string_view path_b) -> Result<FrameDiff> {
  const auto a = st::codec::png_read_file(path_a);
  if (!a) {
    return st::unexpected(a.error().code,
                          std::format("读左图失败：{}", a.error().message));
  }
  const auto b = st::codec::png_read_file(path_b);
  if (!b) {
    return st::unexpected(b.error().code,
                          std::format("读右图失败：{}", b.error().message));
  }
  if (a->width != b->width || a->height != b->height) {
    return st::unexpected(
        ErrorCode::Invalid,
        std::format("尺寸不同：{}x{} vs {}x{}（画幅变了就不该逐像素比）", a->width, a->height,
                    b->width, b->height));
  }

  FrameDiff out{};
  const std::size_t pixels = static_cast<std::size_t>(a->width) * a->height;
  std::size_t differing = 0;
  int min_x = static_cast<int>(a->width);
  int min_y = static_cast<int>(a->height);
  int max_x = -1;
  int max_y = -1;

  for (std::size_t i = 0; i < pixels; ++i) {
    const std::size_t at = i * 4;
    int worst = 0;
    // 只比 RGB：alpha 通道在裁云的成片里恒为 255（合成后不透明），
    // 比它只会让"编码差异"混进"画面差异"。
    for (std::size_t c = 0; c < 3; ++c) {
      const int diff = std::abs(static_cast<int>(a->rgba[at + c]) -
                                static_cast<int>(b->rgba[at + c]));
      if (diff > worst) worst = diff;
    }
    if (worst == 0) continue;
    ++differing;
    if (worst > out.max_channel_delta) out.max_channel_delta = worst;
    const int x = static_cast<int>(i % a->width);
    const int y = static_cast<int>(i / a->width);
    if (x < min_x) min_x = x;
    if (y < min_y) min_y = y;
    if (x > max_x) max_x = x;
    if (y > max_y) max_y = y;
  }

  out.diff_pct = 100.0 * static_cast<double>(differing) / static_cast<double>(pixels);
  if (max_x >= 0) {
    out.bounds_x = min_x;
    out.bounds_y = min_y;
    out.bounds_w = max_x - min_x + 1;
    out.bounds_h = max_y - min_y + 1;
  }
  return out;
}

auto build_snapshot(std::string_view frames_dir, std::string_view out_dir, std::string_view name,
                    const std::vector<std::pair<int, int>>& scene_bounds, int total_frames,
                    double fps, int width, int height) -> Result<Snapshot> {
  if (!st::fs::is_directory(frames_dir)) {
    return st::unexpected(ErrorCode::NotFound,
                          std::format("帧目录不存在：{}", std::string(frames_dir)));
  }
  if (const auto made = st::fs::create_directories(out_dir); !made) {
    return st::forward_error(made.error());
  }

  // 收集磁盘上真实存在的帧号（渲染可能没跑满 total_frames）。
  std::map<int, std::string> on_disk;
  const auto entries = st::fs::list_dir(frames_dir);
  if (!entries) return st::forward_error(entries.error());
  for (const auto& entry : *entries) {
    if (entry.is_dir) continue;
    const int index = parse_frame_index(entry.name);
    if (index >= 0) on_disk[index] = entry.path;
  }
  if (on_disk.empty()) {
    return st::unexpected(ErrorCode::NotFound,
                          std::format("帧目录里没有 frame_XXXXX.png：{}", std::string(frames_dir)));
  }

  Snapshot snapshot{};
  snapshot.name = std::string{name};
  snapshot.created_at = now_iso8601();
  snapshot.width = width;
  snapshot.height = height;
  snapshot.fps = fps;
  snapshot.total_frames = total_frames > 0 ? total_frames : static_cast<int>(on_disk.size());

  const auto take = [&](int index, int scene_index, const char* role) {
    const auto found = on_disk.find(index);
    if (found == on_disk.end()) return;
    const std::string leaf = std::format("f{:05d}.png", index);
    const std::string dest = st::fs::join(std::string(out_dir), leaf);
    if (const auto copied = st::fs::copy_file(found->second, dest); !copied) {
      ST_LOG_WARN("快照复制失败 {}：{}", found->second, copied.error().message);
      return;
    }
    SnapshotFrame frame{};
    frame.index = index;
    frame.time_s = fps > 0.0 ? static_cast<double>(index) / fps : 0.0;
    frame.scene_index = scene_index;
    frame.role = role;
    frame.path = leaf;
    snapshot.frames.push_back(std::move(frame));
  };

  for (std::size_t s = 0; s < scene_bounds.size(); ++s) {
    const auto [begin, end] = scene_bounds[s];
    const int span = end - begin;
    if (span <= 0) continue;
    for (std::size_t k = 0; k < std::size(kSamplePositions); ++k) {
      const int offset = static_cast<int>(std::lround(
          static_cast<double>(span) * kSamplePositions[k]));
      // 夹在 [begin, end-1]：85% 处四舍五入可能落到 end。
      const int index = std::min(begin + offset, end - 1);
      take(index, static_cast<int>(s), kSampleRoles[k]);
    }
  }

  // 没有场景信息时（调用方给空 bounds）退化为均匀抽三帧：
  // 总比"什么都没抽到"有用，但要在名字里区分，免得被当成按镜抽的。
  if (snapshot.frames.empty() && !on_disk.empty()) {
    const int max_index = on_disk.rbegin()->first;
    for (std::size_t k = 0; k < std::size(kSamplePositions); ++k) {
      const int index = static_cast<int>(
          std::lround(static_cast<double>(max_index) * kSamplePositions[k]));
      take(index, -1, kSampleRoles[k]);
    }
  }

  if (const auto wrote = write_snapshot(out_dir, snapshot); !wrote) {
    return st::forward_error(wrote.error());
  }
  return snapshot;
}

auto diff_snapshots(std::string_view path_a, std::string_view path_b) -> Result<SnapshotDiff> {
  const auto a = read_snapshot(path_a);
  if (!a) return st::forward_error(a.error());
  const auto b = read_snapshot(path_b);
  if (!b) return st::forward_error(b.error());
  if (a->width != b->width || a->height != b->height) {
    return st::unexpected(ErrorCode::Invalid,
                          std::format("两版画幅不同（{}x{} vs {}x{}），无法逐像素比较",
                                      a->width, a->height, b->width, b->height));
  }

  std::map<int, SnapshotFrame> left;
  for (const SnapshotFrame& frame : a->frames) left[frame.index] = frame;
  std::map<int, SnapshotFrame> right;
  for (const SnapshotFrame& frame : b->frames) right[frame.index] = frame;

  SnapshotDiff out{};
  double sum = 0.0;
  for (const auto& [index, left_frame] : left) {
    const auto it = right.find(index);
    if (it == right.end()) {
      ++out.only_left;
      continue;
    }
    const std::string left_path = st::fs::join(std::string(path_a), left_frame.path);
    const std::string right_path = st::fs::join(std::string(path_b), it->second.path);
    if (!st::fs::exists(left_path) || !st::fs::exists(right_path)) {
      ++out.only_left;
      continue;
    }
    const auto diff = diff_png(left_path, right_path);
    if (!diff) return st::forward_error(diff.error());
    FrameDiff frame_diff = *diff;
    frame_diff.index = index;
    out.frames.push_back(frame_diff);
    sum += frame_diff.diff_pct;
    ++out.compared;
    if (frame_diff.diff_pct > out.worst_diff_pct) {
      out.worst_diff_pct = frame_diff.diff_pct;
      out.worst_frame_index = index;
    }
  }
  for (const auto& [index, right_frame] : right) {
    (void)right_frame;
    if (left.find(index) == left.end()) ++out.only_right;
  }
  out.mean_diff_pct = out.compared > 0 ? sum / static_cast<double>(out.compared) : 0.0;
  return out;
}

}  // namespace caiyun
