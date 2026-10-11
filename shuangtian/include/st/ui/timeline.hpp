#pragma once

/// **时间轴数据模型**（`st::ui::timeline`）：多轨道 + 片段 + 时间↔像素换算 + 吸附。
///
/// ## 为什么把模型与绘制分开
///
/// caiyun 原先把「轨道/片段数据」与「命中判定/拖拽/吸附/标尺刻度」全写在一个
/// 400 行的 `TimelineView` 里。拆开的好处不是"好看"，而是**可测**：
/// 时间↔像素的换算、吸附到哪些候选、命中落在播放头/修剪边缘/片段体上——
/// 这些都能用纯数据用例覆盖，而混在自绘件里只能靠截图比对（慢且不稳）。
///
/// 组件（`TimelineView`）只负责：调这里的换算、画、把鼠标事件翻成这里的操作。
///
/// ## 时间与像素
///
/// 时间单位是**毫秒**（`std::int64_t`）：与帧率无关，也不会有浮点累积误差。
/// `time_to_x()` / `x_to_time()` 是唯一的换算入口——**不要**让调用方自己乘除
/// （那正是"标尺刻度与片段位置差一像素"这类问题的来源）。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui::timeline {

/// 时间戳（毫秒）。
using Millis = std::int64_t;

/// 一个片段（视频/音频/字幕条目…）。
struct Clip {
  /// 业务身份（轨道内唯一；与 `key` 一样用于"重排后仍指向同一条"）。
  std::string id{};
  /// 显示名。
  std::string label{};
  /// 在**时间轴**上的起点（毫秒）。
  Millis start{0};
  /// 时长（毫秒）。`<= 0` 的片段不参与命中与绘制。
  Millis duration{0};
  /// 源素材内的入点（毫秒）——修剪左边缘时它跟着动，这才是"修剪"而不是"移动"。
  Millis source_in{0};
  /// 是否选中（绘制高亮与批量操作都看它）。
  bool selected{false};

  [[nodiscard]] auto end() const noexcept -> Millis { return start + duration; }
  /// 是否与 `[from, to)` 有交集（半开区间）。
  [[nodiscard]] auto intersects(Millis from, Millis to) const noexcept -> bool {
    return duration > 0 && start < to && from < end();
  }
};

/// 一条轨道。
struct Track {
  std::string id{};
  std::string label{};
  std::vector<Clip> clips{};
  bool muted{false};
  bool locked{false};   ///< 锁定轨道不接受编辑（拖拽/修剪），但仍可选中查看
  float height{44.0f};  ///< 逻辑像素
};

/// 命中到片段上的**部位**：决定拖拽语义（移动 vs 修剪）。
enum class ClipPart : std::uint8_t {
  None,
  Body,        ///< 片体：拖动 = 移动
  LeftEdge,    ///< 左边缘：拖动 = 修剪入点
  RightEdge,   ///< 右边缘：拖动 = 修剪出点
};

/// 一次命中结果。
struct Hit {
  /// 命中的轨道下标（`kNone` = 没命中）。
  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  std::size_t track{kNone};
  std::size_t clip{kNone};
  ClipPart part{ClipPart::None};
  /// 是否命中播放头（**优先级最高**：播放头是一根 1~2px 的线，被片段盖住就没法拖了）。
  bool playhead{false};

  [[nodiscard]] auto valid() const noexcept -> bool { return playhead || clip != kNone; }
};

/// 吸附候选的来源（开关型：时间轴的吸附是**多源**的——片段边缘、播放头、零点、标记）。
struct SnapOptions {
  bool to_clip_edges{true};   ///< 吸附到其他片段的起止点
  bool to_playhead{true};
  bool to_zero{true};
  /// 额外标记点（毫秒；如章节标记、节拍点）。
  std::vector<Millis> markers{};
  /// 吸附阈值（**像素**，不是毫秒）：缩放后阈值应保持"手感一致"，
  /// 用毫秒做阈值会导致"放大后吸附变得极难触发"。
  float threshold_px{8.0f};
};

/// 时间轴模型：轨道集合 + 视口（横向缩放与滚动）。
///
/// 它**不是** `Element`：数据模型与绘制分开（见文件头）。组件持有一个本对象。
class Model {
 public:
  /// 无轨道的下标哨兵。
  static constexpr std::size_t kNone = Hit::kNone;

  // —— 视口（横向）——
  /// 每秒占多少逻辑像素（缩放）。
  void set_pixels_per_second(float value);
  [[nodiscard]] auto pixels_per_second() const noexcept -> float { return pixels_per_second_; }
  /// 左边缘对应的时间（滚动）。
  void set_scroll(Millis time);
  [[nodiscard]] auto scroll() const noexcept -> Millis { return scroll_; }
  /// 视口宽度（逻辑像素；由组件在 `arrange` 时同步进来）。
  void set_viewport_width(float width) noexcept { viewport_width_ = width; }
  [[nodiscard]] auto viewport_width() const noexcept -> float { return viewport_width_; }

  [[nodiscard]] auto time_to_x(Millis time) const -> float;
  [[nodiscard]] auto x_to_time(float x) const -> Millis;
  /// 视口可见的时间区间 `[from, to)`。
  [[nodiscard]] auto visible_range() const -> std::pair<Millis, Millis>;

  // —— 轨道与片段 ——
  void set_tracks(std::vector<Track> tracks);
  [[nodiscard]] auto tracks() const noexcept -> const std::vector<Track>& { return tracks_; }
  [[nodiscard]] auto track_count() const noexcept -> std::size_t { return tracks_.size(); }
  /// 找片段的可变引用（`nullptr` = 不存在）。
  [[nodiscard]] auto find_clip(std::size_t track, std::size_t clip) -> Clip*;
  [[nodiscard]] auto find_clip(std::size_t track, std::size_t clip) const -> const Clip*;
  /// 按 id 找（跨轨道；返回 `{track, clip}`）。
  [[nodiscard]] auto locate(std::string_view clip_id) const
      -> std::pair<std::size_t, std::size_t>;
  /// 全部片段的最晚结束时间（内容总长；`0` = 空）。
  [[nodiscard]] auto content_duration() const noexcept -> Millis;

  // —— 时间↔像素（纵向）——
  /// 轨道顶边相对内容区的 y（未计纵向滚动）。
  [[nodiscard]] auto track_top(std::size_t index) const -> float;
  /// 轨道带的 y 范围（未计纵向滚动）。
  [[nodiscard]] auto track_rect(std::size_t index) const -> std::pair<float, float>;
  /// 片段在屏幕上的横向范围（逻辑像素，含滚动与缩放）。
  [[nodiscard]] auto clip_x_range(std::size_t track, std::size_t clip) const
      -> std::pair<float, float>;
  /// 由 y 反查轨道下标（`kNone` = 落在轨道区之外）。
  [[nodiscard]] auto track_at_y(float y) const -> std::size_t;

  // —— 命中 ——
  /// 命中判定。`x`/`y` 是相对时间轴内容区左上角的逻辑坐标。
  ///
  /// 优先级（**顺序很重要，它决定"叠在一起时拖到谁"**）：
  /// ① 播放头（最细、最难重新抓住，且它总在最上层）
  /// ② 片段修剪边缘（`edge_px` 宽的带；它在片体之上）
  /// ③ 片体（同一 x 上有多条时取**上面**那条：视觉上压在上面的应该先被抓住）
  [[nodiscard]] auto hit_test(float x, float y, float edge_px = 5.0f) const -> Hit;
  /// 播放头的屏幕 x（`nullopt` = 不在可见范围内）。
  [[nodiscard]] auto playhead_x() const -> std::optional<float>;
  void set_playhead(Millis time) noexcept { playhead_ = time; }
  [[nodiscard]] auto playhead() const noexcept -> Millis { return playhead_; }

  // —— 吸附 ——
  /// 把 `time` 吸附到最近的候选（在阈值内）。
  ///
  /// `exclude_*` 用于排除"正在被拖的那个片段自己的边缘"——不自排的话它会**永远**
  /// 吸在自己原来的位置上，表现为"拖不动"（这是吸附实现最经典的错法）。
  [[nodiscard]] auto snap(Millis time, const SnapOptions& options, std::size_t exclude_track,
                          std::size_t exclude_clip) const -> Millis;

  /// 选中的片段（批量操作用）。
  [[nodiscard]] auto selected_clips() const -> std::vector<std::pair<std::size_t, std::size_t>>;
  void clear_selection();

 private:
  std::vector<Track> tracks_{};
  float pixels_per_second_{64.0f};
  Millis scroll_{0};
  float viewport_width_{0.0f};
  Millis playhead_{0};
};

/// **时间码格式化**（`HH:MM:SS.mmm`；`compact` 时省略小时）。
///
/// 放在模型层而不是组件里：它被标尺、片段标签、状态栏共用，各写一份必然不一致
///（而"三处显示的时间不一样"是那种让人怀疑数据本身的错）。
[[nodiscard]] auto format_timecode(Millis time, bool compact = false) -> std::string;

/// 标尺刻度：给一个时间跨度，选出**好看的**步长（1/2/5 × 10^n 毫秒），
/// 使得屏幕上两条刻线间距不小于 `min_px`。
///
/// 为什么步长要"跳档"而不是等分：等分出来的刻度标签是 `0:03.333` 这种数，
/// 读起来要算——而 1/2/5 档出来的都是整秒/整分。
[[nodiscard]] auto ruler_step(Millis span, float pixels_per_second, float min_px = 72.0f)
    -> Millis;

}  // namespace st::ui::timeline
