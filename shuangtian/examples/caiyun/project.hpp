#pragma once

/// 裁云（Nimbus）视频剪辑器 — 工程模型与时间轴编辑。
///
/// ## 诚实边界（设计文档 §4.1）
///
/// **框架没有视频解码器**。v1 的"素材"全部是**程序化生成片段**（走本地光栅
/// 原语实时渲染）；真实视频解码是显式记入 BACKLOG 的框架缺口，**不做假占位、
/// 不冒充能播**。
///
/// ## 为什么片段是"纯函数式生成器"
///
/// `Generator::render(canvas, local_time)` 只依赖片段内时间，**不持有播放状态**。
/// 于是：
///   - **确定性**：同一 `seek` 两次渲染的结果逐像素相同——这正是 e2e 的硬判据
///     （设计文档 §7.1），也把"预览跟着播放头走"与"预览是静帧"区分开；
///   - 可并行/可导出：逐帧导出不需要任何播放上下文。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/text_port.hpp"

namespace caiyun {

using st::math::Color;
using st::math::Rect;
using st::raster::BlendMode;
using st::raster::Canvas;

/// 程序化片段的四类（设计文档 §4.2）。
enum class GeneratorKind : std::uint8_t {
  ColorBars,   ///< 色板（纯色 + 柔和渐变动画）
  Subtitle,    ///< 字幕滚动（文本渲染 + 缓动入场）
  Shapes,      ///< 图形运动（圆/矩形 + easing 轨迹）
  TestChart,   ///< 测试图（色条/网格/时码刻度，风格自定——不抄 SMPTE 布局）
};

inline constexpr GeneratorKind kGenerators[] = {GeneratorKind::ColorBars, GeneratorKind::Subtitle,
                                                GeneratorKind::Shapes, GeneratorKind::TestChart};
inline constexpr std::size_t kGeneratorCount = std::size(kGenerators);

[[nodiscard]] inline auto to_string(GeneratorKind kind) -> const char* {
  switch (kind) {
    case GeneratorKind::ColorBars: return "色板";
    case GeneratorKind::Subtitle: return "字幕";
    case GeneratorKind::Shapes: return "图形";
    case GeneratorKind::TestChart: return "测试图";
  }
  return "色板";
}

/// 缓动（片段入出场的运动感来源）。
///
/// 为什么手写而不引 `<cmath>` 之外的依赖：这几个曲线是**观感决策**，
/// 集中在一处才好整体调（改动会同时影响字幕入场与图形轨迹）。
[[nodiscard]] inline auto ease_in_out(float t) -> float {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  return clamped < 0.5f ? 2.0f * clamped * clamped
                        : 1.0f - 2.0f * (1.0f - clamped) * (1.0f - clamped);
}

/// 一个片段：时间轴上的区间 + 生成器 + 参数。
struct Clip {
  int id{0};
  int track{0};                 ///< 0 = V1（下），1 = V2（上）
  double t_in{0.0};             ///< 时间轴入点（秒）
  double t_out{1.0};            ///< 时间轴出点（秒）
  GeneratorKind generator{GeneratorKind::ColorBars};
  /// 素材自身的入点（修剪左边缘时改它，`t_in` 与它同步变化）。
  double source_in{0.0};
  float opacity{1.0f};
  float speed{1.0f};
  /// 基色（各类生成器都用到；让同一生成器的不同片段可区分）。
  Color tint{90, 140, 220, 255};

  [[nodiscard]] auto duration() const noexcept -> double { return t_out - t_in; }
  /// 把时间轴时刻换算成**片段内时间**（含素材入点与速度）。
  [[nodiscard]] auto local_time(double timeline_time) const noexcept -> double {
    return source_in + (timeline_time - t_in) * static_cast<double>(speed);
  }
  [[nodiscard]] auto covers(double timeline_time) const noexcept -> bool {
    return timeline_time >= t_in && timeline_time < t_out;
  }
};

/// 时间轴工程。
struct Project {
  double fps{30.0};
  double duration{10.0};       ///< 工程总时长（秒）
  double playhead{0.0};
  bool playing{false};
  bool loop{true};
  bool snapping{true};
  /// 吸附阈值（秒）：拖拽时落点靠近片段边缘/播放头就吸附到它。
  double snap_threshold{0.12};

  std::vector<Clip> clips{};
  int next_id{1};
  int selected{-1};            ///< 选中的片段 id（-1 = 无）

  [[nodiscard]] auto find_clip(int id) -> Clip* {
    for (auto& clip : clips) {
      if (clip.id == id) return &clip;
    }
    return nullptr;
  }
  [[nodiscard]] auto find_clip(int id) const -> const Clip* {
    for (const auto& clip : clips) {
      if (clip.id == id) return &clip;
    }
    return nullptr;
  }

  /// 新增片段（返回新 id；区间非法时返回 -1）。
  auto add_clip(GeneratorKind kind, int track, double t_in, double t_out) -> int;
  /// 删除片段（返回是否删掉）。
  auto remove_clip(int id) -> bool;
  /// **波纹删除**：删掉并把同轨道上它之后的片段整体前移它的时长。
  auto ripple_delete(int id) -> bool;
  /// 在 `time` 处**分割**选中/命中的片段（返回是否切开）。
  auto split_at(double time) -> bool;
  /// 修剪边缘（`edge` = 0 左 / 1 右），带最小时长护栏。
  auto trim(int id, int edge, double time) -> bool;
  /// 移动片段到新入点（保持时长）。
  auto move_clip(int id, double new_t_in) -> bool;
  /// 把 `time` 吸附到最近的片段边缘或播放头（阈值内）；吸附关闭时原样返回。
  [[nodiscard]] auto snap(double time) const -> double;

  /// 合成一帧到 `target`（`target` 尺寸决定输出分辨率）。
  ///
  /// `text_port` 为空时字幕类片段退化为几何占位块（见 `render_generator`）。
  /// 逐帧导出时传 nullptr 是有意的：导出路径不依赖 UI 线程的字体状态，
  /// 而“少一段文字”比“整帧黑”诊断起来容易得多。
  void render_frame(Canvas& target, double time,
                    const st::ui::TextPort* text_port = nullptr) const;
};

/// 一段程序化画面的渲染（纯函数：只依赖 `local_time` 与 `size`）。
///
/// `text_port` 可为空——为空时字幕类片段退化为**几何占位块**（而不是崩溃或
/// 什么都不画）：导出/自检路径上不一定有文本端口，而"少一段文字"比"整帧黑"
/// 诊断起来容易得多。
void render_generator(Canvas& target, GeneratorKind kind, double local_time, Color tint,
                      const std::string& caption, const st::ui::TextPort* text_port);

/// 时间码格式化（`hh:mm:ss.mmm`）——设计文档 §8 列的"小件"。
[[nodiscard]] auto format_timecode(double seconds) -> std::string;
/// 简短时间码（`mm:ss`）——时间标尺用。
[[nodiscard]] auto format_short(double seconds) -> std::string;

}  // namespace caiyun
