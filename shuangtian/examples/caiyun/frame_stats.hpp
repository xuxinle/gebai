#pragma once

/// 裁云（Nimbus）· **成片的画面质量度量**：把「慢了 / 卡着不动 / 这段太白」变成可复算的数。
///
/// ## 为什么在渲染循环里量，而不是事后量成片
///
/// 渲染循环里每一帧刚渲进 `Canvas`，**像素就在手边**。在这里统计比事后用 ffmpeg
/// 解码 mp4 再抽帧好在三点：
///
/// 1. **精确**：不经过 h264 有损压缩，不吃码率噪声（降码率会让「静止」变得"在动"）。
/// 2. **免费**：只是每帧一次降采样遍历（1080p 约 1~2ms，相对渲染 15ms/帧可忽略），
///    不需要额外解码一遍全片。
/// 3. **天然逐镜**：循环里拿得到 `Clip` 与 `scene_index`，所以能给**每一镜**的静止占比，
///    而不是只有全片平均。这一点是「慢」类问题的关键——实测里全片静止 48% 掩盖了
///    引导页 81% 的真相，改时长改了半天没用，因为问题在镜内。
///
/// ## 口径（写死在这里，report 里也带）
///
/// - **降采样**：帧按块均值降到宽 `kStatsWidth`（默认 64）的灰度图。降到这么小是
///   有意的：判据是「画面整体在不在动」，全分辨率会把编码噪声、亚像素抖动都算成运动，
///   于是每一帧都"在动"、静止占比恒为 0。
/// - **帧差**：相邻两帧降采样灰度图的**平均绝对差**（0–255 尺度）。
/// - **静止帧**：帧差 `< static_threshold`（默认 1.2）。这个阈值必须随数一起报——
///   阈值一动，48% 能变 25% 也能变 60%，不给阈值的百分比无法复核。
/// - **静止占比**：静止帧对数 / 总帧对数 ×100。
/// - **过亮帧**：平均亮度 > `bright_luma`（默认 180）的帧占比。
///   暗场片里嵌一段白底录屏会把它顶上去——整屏发白不需要靠眼睛发现。
/// - **逐镜**：按片段区间聚合，每镜单独给静止占比与平均亮度。
///
/// ## 边界
///
/// - 只统计**渲染到的帧**；`first_frame/last_frame` 局部渲染时结论只覆盖那段区间
///   （报告里带 `partial` 标记）。
/// - 不做主观判断（好不好看）。它只回答「有没有在动、亮了还是暗了、哪一镜最静」。
///
/// ## 帧差在**归并后**按帧号算，不在喂入时算
///
/// 导出是 16 路并行，每路用 `fetch_add` 抢帧号，于是同一路拿到的是 0、16、32…——
/// **相邻帧永远在不同 worker 上**。若帧差只在「本路上一帧」之间算，相邻对照永远凑不出来：
/// 实测一次 16 秒的导出报「静止 0.0%（帧差 <1.2，**0 对**）」——分母是 0，指标恒为 0，
/// 而它看起来完全正常（百分比有值、摘要没报错）。这正是本类工具最坏的失效方式：
/// **静默且被信任**。
///
/// 所以每帧的降采样灰度图**留到归并之后**：按帧号排序，再对每对相邻帧号算差。
/// 代价是每帧多存 `kStatsWidth × (h/ratio)` 字节（1080p 约 2.2 KB，900 帧约 2 MB），
/// 换来的是「并行不影响口径」。`push()` 的返回值仍给本路相邻帧的差（供调用方诊断），
/// 但**报告不读它**。

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"

namespace caiyun {

/// 降采样后的灰度图宽度（高度按比例取偶）；整帧 64×N 字节。
inline constexpr int kStatsWidth = 64;

/// 一帧的度量。
struct FrameStats {
  int index{0};          ///< 帧序号（0 起）
  double time_s{0.0};    ///< 该帧在时间轴上的时刻
  double mean_luma{0.0}; ///< 降采样灰度均值（0–255）
  double diff_prev{0.0}; ///< 与上一帧的平均绝对差；首帧为 0
};

/// 一镜（一片段区间）的度量。
struct ShotStats {
  int clip_id{0};
  int scene_index{-1};       ///< `>= 0` = 导演脚本的第 N 个场景；`-1` = 生成器片段
  std::string label{};       ///< 场景标题或「片段 #id」，给日志看
  double t_in{0.0};
  double t_out{0.0};
  int frames{0};
  int still_frames{0};       ///< 与上一帧相比判为静止的帧数
  /// 该镜内有效的相邻帧对数（分母）；与 `frames-1` 不等说明区间边界丢帧。
  int frame_pairs{0};
  double mean_luma{0.0};
  /// 静止占比（%）；无有效帧对时为 0（无法判动静，不该报 100%）
  [[nodiscard]] auto still_ratio() const -> double {
    return frame_pairs > 0 ? 100.0 * static_cast<double>(still_frames) / static_cast<double>(frame_pairs) : 0.0;
  }
};

/// 整次渲染的画面度量汇总。
struct QualityReport {
  /// 判据参数（必须与数一起给：换阈值就换答案）。
  double static_threshold{1.2};
  double bright_luma{180.0};
  int probe_width{kStatsWidth};

  int frames{0};
  int still_frames{0};
  int bright_frames{0};
  /// 有效的**相邻帧对**数（分母）。为 0 时静止占比不可信（渲染只出了 1 帧、或帧号不连续），
  /// 必须随数一起报——否则「0 对 / 0.0%」会被当成「一点也不静」。
  int frame_pairs{0};
  double mean_luma{0.0};
  /// 全片静止占比（%）：静止帧对数 / 帧对数。
  double still_ratio{0.0};
  /// 过亮帧占比（%）。
  double bright_ratio{0.0};
  /// 变化密度：判为"在动"的帧对数 / 时长（每秒）。与静止占比**正交**——
  /// 多而碎（密集但幅度小）不等于不快，单看一个量会把「更碎」判成「更好」。
  double change_density{0.0};

  std::vector<ShotStats> shots{};
  /// 静止占比最高的那一镜（`shots` 为空时无值）；用来一眼指出「慢在哪」。
  int worst_still_clip_id{-1};

  /// 局部渲染（`first_frame/last_frame`）时结论只覆盖那段区间。
  bool partial{false};
  /// 可读摘要（一行，日志与 `get` 都用它）。
  std::string summary{};
};

/// 报告 → JSON（给控制通道 `get quality_json` 用）。
///
/// 为什么在 C++ 侧序列化而不是让调用方拼字符串：字段名与结构只在这里定义一次，
/// 智能体/e2e 拿到的形状才有唯一来源（否则每多一个消费者就多一份易漂移的拼装）。
[[nodiscard]] auto quality_to_json(const QualityReport& report) -> std::string;

/// 一个帧的采样（归并后算帧差要用的都留着）。
///
/// **为什么把灰度图存到归并后**：见文件头「帧差在归并后按帧号算」。
struct Sample {
  int index{0};
  double time_s{0.0};
  double mean_luma{0.0};
  std::vector<std::uint8_t> luma{};
};

/// 增量累加器（渲染循环里逐帧喂；并行时**每线程一份**，最后归并）。
///
/// 为什么每线程一份而不是加锁共享：每帧都要写，锁会成为热点；而这里的状态
/// 只有几十字节，每线程一份再归并的代价可以忽略。这也是 `render.cpp` 里
/// 现有 render/encode 计时用的同一套做法（`worker_render_ns` 等）。
class StatsAccumulator {
 public:
  /// 喂一帧。`frame` 必须带好 index/time_s；`mean_luma`/`diff_prev` 由本方法算。
  /// `prev` 由调用方持有（并行时同一 worker 的上一帧）。
  /// 返回本帧的度量（供 worker 保存"自己的上一帧"）。
  ///
  /// ⚠ 返回的 `diff_prev` 只是**本路相邻帧**的差（并行时这是 0/16/32… 的间隔），
  /// 仅用于诊断；判定与时序口径的差一律来自 `report()`（它按归并后的帧号算）。
  /// 推入一帧的画面统计。
  ///
  /// `rgba` 是这一帧的像素（`to_rgba8()` 的输出）。**由调用方传入而不是自己回读**：
  /// 导出路径本来就要把像素送给 ffmpeg，重用同一份字节能省掉一次 GPU staging 回读
  /// （1080p 每遍 5~17ms）。
  auto push(std::span<const std::uint8_t> rgba, int width, int height, int index,
            double time_s) -> FrameStats;

  /// 把**另**一个累加器的内容并入自己（并行归并）。
  void merge(const StatsAccumulator& other);

  /// 按片段时间区间把已收帧聚合成逐镜统计。`spans` 由调用方从 `Project::clips` 生成。
  struct ShotSpan {
    int clip_id{0};
    int scene_index{-1};
    std::string label{};
    double t_in{0.0};
    double t_out{0.0};
  };
  void attach_spans(std::vector<ShotSpan> spans);

  /// 出报告。`duration_s` 用于变化密度；`partial` 标记局部渲染。
  [[nodiscard]] auto report(double duration_s, bool partial) const -> QualityReport;

  [[nodiscard]] auto empty() const -> bool { return samples_.empty(); }

 private:
  std::vector<Sample> samples_{};
  std::vector<ShotSpan> spans_{};
  /// 本路上一帧（只用于 `push()` 的返回值；判据不用它）。
  bool has_prev_{false};
  FrameStats prev_{};
  /// 本路上一帧的降采样灰度图（与 `prev_` 配套，同原因：只给 push 的返回值用）。
  std::vector<std::uint8_t> prev_luma_{};

  static auto downscale_luma(std::span<const std::uint8_t> rgba, int width, int height)
      -> std::vector<std::uint8_t>;
};

}  // namespace caiyun
