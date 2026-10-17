#pragma once

/// 裁云（Nimbus）视频剪辑器 — 工程模型与时间轴编辑。
///
/// ## 现在的真值源是**导演脚本**（见 `director.hpp`）
///
/// 「AI 控制生成高质量视频」的入口是一份 JSON 分镜脚本：`Project::apply_script`
/// 把它投影成时间轴片段（**一个场景 = 一个片段**），`Project::render_frame` 按片段
/// 与场景数据合成画面。人工编辑（修剪/移动/分割/波纹删除/不透明度/速度）作用在
/// **片段**上，与脚本投影共用同一份数据——不存在"脚本一套、时间轴一套"的双源。
///
/// 保留早期的**程序化生成器**（色板/字幕/图形/测试图）：它们是"素材箱"里的素材，
/// 与场景片段走同一条合成路径（`Clip::scene_index >= 0` 走场景渲染，否则走生成器）。
///
/// ## 为什么片段渲染仍是纯函数
///
/// `render_frame(canvas, time)` 只依赖工程数据与 `time`——同一时刻渲染两次**逐像素
/// 相同**。这是 e2e 的确定性判据，也是逐帧导出可复现的前提。
///
/// ## 诚实边界（设计文档 §4.1）
///
/// 框架内置的 `st::ext::VideoDecoder`（ffmpeg 子进程抽帧）定位是**素材查看/预处理**，
/// 不是逐帧播放（每抽一帧起一个进程 ~30ms）。裁云的"素材"因此仍是**程序化场景**；
/// 真实视频素材导入作为后续大单记在 BACKLOG。**不假占位、不冒充能播。**
/// 成片编码走外部 ffmpeg（`render.hpp`）：探测不到时如实报"未找到 ffmpeg"，
/// 仍会产出 PNG 帧序列——不静默失败。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "director.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/text_port.hpp"

namespace caiyun {

using st::math::Color;
using st::math::Rect;
using st::raster::BlendMode;
using st::raster::Canvas;

/// 素材箱里的程序化生成器（非场景片段）。
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

/// 缓动（生成器入出场的运动感来源）。场景模板用 `motion.hpp` 里那一套。
[[nodiscard]] inline auto ease_in_out(float t) -> float {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  return clamped < 0.5f ? 2.0f * clamped * clamped
                        : 1.0f - 2.0f * (1.0f - clamped) * (1.0f - clamped);
}

/// 一个片段：时间轴上的区间 + 内容（场景 or 生成器）。
struct Clip {
  int id{0};
  int track{0};                 ///< 0 = V1（下），1 = V2（上）
  double t_in{0.0};             ///< 时间轴入点（秒）
  double t_out{1.0};            ///< 时间轴出点（秒）
  GeneratorKind generator{GeneratorKind::ColorBars};
  /// `>= 0` = 该片段是导演脚本的第 N 个场景（渲染走 `motion.cpp`）。
  /// `-1` = 程序化生成器片段（素材箱拖入的素材）。
  int scene_index{-1};
  /// 素材自身的入点（修剪左边缘时改它，`t_in` 与它同步变化）。
  double source_in{0.0};
  float opacity{1.0f};
  float speed{1.0f};
  /// 基色（生成器片段用到；场景片段取场景的强调色，便于时间轴辨认）。
  Color tint{90, 140, 220, 255};

  [[nodiscard]] auto duration() const noexcept -> double { return t_out - t_in; }
  /// 把时间轴时刻换算成**片段内时间**（含素材入点与速度）。
  [[nodiscard]] auto local_time(double timeline_time) const noexcept -> double {
    return source_in + (timeline_time - t_in) * static_cast<double>(speed);
  }
  [[nodiscard]] auto covers(double timeline_time) const noexcept -> bool {
    return timeline_time >= t_in && timeline_time < t_out;
  }
  [[nodiscard]] auto is_scene() const noexcept -> bool { return scene_index >= 0; }
};

/// 时间轴工程。
struct Project {
  double fps{30.0};
  double duration{10.0};       ///< 工程总时长（秒）
  double playhead{0.0};
  bool playing{false};
  bool loop{true};
  /// 吸附开关（吸附逻辑已回流 `st::ui::timeline::Model::snap`，经
  /// `TimelineView::snap_time` 调用；阈值换算见该处）。
  bool snapping{true};
  /// 吸附阈值（秒）：拖拽时落点靠近片段边缘/播放头就吸附到它。
  double snap_threshold{0.12};

  /// 导演脚本（场景真值）。`clips` 是它的**投影** + 人工编辑的结果：
  /// 应用脚本会按脚本重建片段；人工编辑只改片段（脚本保留，可另存）。
  Script script{builtin_gebai_promo()};

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
  /// 片段对应的场景（非场景片段返回 `nullptr`）。
  [[nodiscard]] auto scene_of(const Clip& clip) const -> const Scene* {
    if (clip.scene_index < 0 || static_cast<std::size_t>(clip.scene_index) >= script.scenes.size()) {
      return nullptr;
    }
    return &script.scenes[static_cast<std::size_t>(clip.scene_index)];
  }
  [[nodiscard]] auto scene_of(const Clip& clip) -> Scene* {
    return const_cast<Scene*>(static_cast<const Project*>(this)->scene_of(clip));
  }

  /// **应用导演脚本**：清空片段、按脚本重建（一段场景 = 一个 V1 片段），
  /// 工程时长/fps/分辨率随脚本更新。返回是否成功（脚本无场景则拒绝并保持原状）。
  auto apply_script(Script next) -> bool;
  /// 重新把**当前脚本**投影成片段（保留 `playhead`/`selected` 尽量不变）。
  void rebuild_from_script();

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

  /// 合成一帧到 `target`（`target` 尺寸决定输出分辨率）。
  ///
  /// `text_port` 为空时文字类内容退化为几何占位块（见 `render_generator`）。
  /// 逐帧导出时传 nullptr 是有意的：导出路径不依赖 UI 线程的字体状态，
  /// 而“少一段文字”比“整帧黑”诊断起来容易得多。
  void render_frame(Canvas& target, double time,
                    const st::ui::TextPort* text_port = nullptr) const;
};

/// 一段程序化画面的渲染（纯函数：只依赖 `local_time` 与 `size`）。
void render_generator(Canvas& target, GeneratorKind kind, double local_time, Color tint,
                      const std::string& caption, const st::ui::TextPort* text_port);

/// 时间码格式化（`hh:mm:ss.mmm`）——设计文档 §8 列的"小件"。
[[nodiscard]] auto format_timecode(double seconds) -> std::string;
/// 简短时间码（`mm:ss`）——时间标尺用。
[[nodiscard]] auto format_short(double seconds) -> std::string;

}  // namespace caiyun
