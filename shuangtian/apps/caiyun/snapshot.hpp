/// 视觉快照与版本比对：回答「改了这一版，画面到底哪里变了」。
///
/// ## 为什么两种 diff 都要有
///
/// - **文本 diff**（`git diff scene.json`）说"字段变了"：精确，但读者要自己在
///   脑内把它渲染成画面。改 `cam_zoom_to: 1.06 → 1.09` 在文本里是一行，
///   在画面上是整片的位移。
/// - **视觉 diff**（本层）说"哪一帧的哪些区域变了"：直观，但说不出为什么。
///
/// 两个都要，且**不能互相替代**。这也是"不做 zip 打包"的真正理由——
/// 压缩包会让文本 diff 失效，于是只剩一半的信息。
///
/// ## 快照是什么、不是什么
///
/// 快照 = 一组**稀疏关键帧**（默认每镜首/中/尾三张）+ 元信息（脚本哈希、
/// 尺寸、帧号）。它回答"这一版长这样"，用来与另一版比。
///
/// 它**不是**视频备份：不做全帧序列（一个 10 秒片全帧就是 300 张 1080p PNG），
/// 也不存 mp4（体积大且不便逐帧比）。要全片就重新渲染。
///
/// ## 选帧策略：为什么是"每镜首中尾"
///
/// 实测过的三种都不合适：
/// - **等间隔全片采样**：镜头边界会错位——剧本改了时长，第 N 帧就落在别的镜里，
///   两版比出来的差异全是"错位"而不是"改动"。
/// - **只比首帧**：错过所有入场动画与落定状态。
/// - **比全部帧**：成本高，且相邻帧本来就该不同（那是运动不是改动）。
///
/// 每镜首/中/尾三帧是折中：按**场景边界**定位，所以剧本时长变了也不会错位；
/// 三个时间点分别覆盖"刚入场""动到中段""落定后"。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace caiyun {

using st::Result;

/// 一个快照里的单帧。
struct SnapshotFrame {
  /// 帧序号（0 起，绝对帧号）。
  int index{0};
  /// 该帧在时间轴上的时刻（秒）。
  double time_s{0.0};
  /// 属于哪个场景（-1 = 未知）。
  int scene_index{-1};
  /// 在一镜内的角色：`in`（入场后）/ `mid`（中段）/ `out`（落定后）。
  std::string role{};
  /// PNG 相对包根的路径（`snapshots/<快照名>/f000012.png`）。
  std::string path{};
};

/// 一次快照（某一版画面的稀疏留影）。
struct Snapshot {
  /// 快照名（`v3` / `before-camera-tweak` 这类人起的名字）。
  std::string name{};
  /// 建立时间（ISO-8601 UTC）。
  std::string created_at{};
  /// 建立时的脚本内容哈希（用来发现"快照与当前脚本不是同一版"）。
  std::string script_sha256{};
  int width{0};
  int height{0};
  double fps{30.0};
  /// 总帧数（该版渲染出来的）。
  int total_frames{0};
  /// 逐帧度量（复用 `frame_stats` 的口径；供"哪一镜最静"这类查询）。
  std::vector<SnapshotFrame> frames{};
};

/// 单帧比对的统计结果。
struct FrameDiff {
  int index{0};
  /// 差异像素占比（%）。0 = 逐像素完全相同。
  double diff_pct{0.0};
  /// 最大通道差（0–255）。
  int max_channel_delta{0};
  /// 差异区域（像素坐标；无差异时全 0）。
  int bounds_x{0};
  int bounds_y{0};
  int bounds_w{0};
  int bounds_h{0};

  [[nodiscard]] auto identical() const -> bool { return diff_pct <= 0.0 && max_channel_delta == 0; }
};

/// 两个快照的比对结果。
struct SnapshotDiff {
  /// 两版共有的帧对数。
  int compared{0};
  /// 只在左版 / 右版出现的帧（剧本时长变了会出现）。
  int only_left{0};
  int only_right{0};
  /// 逐帧差异。
  std::vector<FrameDiff> frames{};
  /// 全片差异占比的均值（%）。
  double mean_diff_pct{0.0};
  /// 差异最大的一帧（人应当先看它）。
  int worst_frame_index{-1};
  double worst_diff_pct{0.0};

  /// 一句判词：完全一致 / 仅有轻微差异 / 显著变化。
  ///
  /// 阈值写在这里而不是让调用方自己判：同一个数字在不同片子上的含义不同，
  /// 但"逐像素完全一致"这个判据是绝对的（编码无损时）。
  [[nodiscard]] auto verdict() const -> std::string;
};

/// 从一版渲染的 PNG 帧目录抽快照。
///
/// `frames_dir` 里的文件按 `frame_XXXXX.png` 命名（渲染器的 `--keep-frames` 产物）。
/// `scene_bounds` 是各场景的 [起, 止) 帧区间（按 `duration * fps` 累加得到）。
///
/// 抽帧规则：每镜取 入场后 30% / 50% / 85% 三个位置
/// （而不是 0/50/100%——正好落在边界上会拍到淡入淡出的半透明帧）。
[[nodiscard]] auto build_snapshot(std::string_view frames_dir, std::string_view out_dir,
                                  std::string_view name,
                                  const std::vector<std::pair<int, int>>& scene_bounds,
                                  int total_frames, double fps, int width, int height)
    -> Result<Snapshot>;

/// 快照的元信息文件名（与 PNG 同目录）。
inline constexpr std::string_view kSnapshotMetaFile = "snapshot.json";

/// 读回快照元信息。
[[nodiscard]] auto read_snapshot(std::string_view dir) -> Result<Snapshot>;

/// 把快照元信息写盘。
[[nodiscard]] auto write_snapshot(std::string_view dir, const Snapshot& snapshot)
    -> st::Status;

/// 比对两个快照目录（按帧号配对共有的帧）。
///
/// `path_a` / `path_b` 是快照目录（含 `snapshot.json` 与 PNG）。
/// 只比**两边都有**的帧；只在一边的计入 `only_left`/`only_right`。
[[nodiscard]] auto diff_snapshots(std::string_view path_a, std::string_view path_b)
    -> Result<SnapshotDiff>;

/// 单对 PNG 的逐像素比对（供上面复用，也供工具直接调）。
[[nodiscard]] auto diff_png(std::string_view path_a, std::string_view path_b)
    -> Result<FrameDiff>;

}  // namespace caiyun
