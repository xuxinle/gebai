#pragma once

/// 裁云（Nimbus）的**自绘时间轴**与**预览监视器**。
///
/// 这两个都是 `Element` 子类、走 `dsl::custom<T>` 逃生舱（同 louyue 的
/// `CanvasView`）：时间轴是连续拖拽交互面，监视器是每帧重合成的自绘面，
/// 都不适合进声明式重组树。
///
/// ## 为什么时间轴要自绘而不是用现成组件拼
///
/// 设计文档 §8 把 `Timeline` 列为"最大件"的框架缺口候选，路径是
/// **先在示例里自绘验证形态、稳定后再内置化**（§8.1.1 的演进策略，
/// `SplitView`/`CommandPalette` 当年走的同一条路）。所以这里刻意不抽通用
/// 组件：形态还没定，抽早了就是把未定的东西固化成接口。

#include <functional>
#include <string>

#include "project.hpp"
#include "st/ui/element.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

namespace caiyun {

/// 时间轴的命中区（拖拽开始时判定"抓到什么"）。
enum class TimelineGrab : std::uint8_t { None, Playhead, ClipBody, ClipLeftEdge, ClipRightEdge };

/// 自绘时间轴：标尺 + 轨道 + 片段块 + 修剪手柄 + 播放头。
class TimelineView final : public st::ui::Element {
 public:
  TimelineView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "TimelineView"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Panel; }

  void set_project(Project* project);
  /// 工程内容变化后的回调（宿主用来刷新检查器/时间码）。
  std::function<void()> on_project_changed{};
  /// 播放头变化（拖动中）的回调——监视器据此重画。
  std::function<void()> on_seek{};

  [[nodiscard]] auto selected_clip() const noexcept -> int { return selected_; }

  void apply_theme(const st::ui::Theme& theme) override;
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 时间 → 视口 x（秒 → 像素）。
  [[nodiscard]] auto time_to_x(double time) const -> float;
  [[nodiscard]] auto x_to_time(float x) const -> double;
  /// 某轨道的行矩形（逻辑坐标）。
  [[nodiscard]] auto track_rect(int track) const -> st::math::Rect;
  /// 片段块的矩形。
  [[nodiscard]] auto clip_rect(const Clip& clip) const -> st::math::Rect;
  /// 命中的片段与抓取部位。
  ///
  /// ⚠ 名字**不能叫 `hit_test`**：`Element` 已有一个返回 `bool` 的虚函数叫那名
  ///（“这个点是否命中本元素”），同名不同返回类型会编译报 conflicting return type；
  /// 而且即使返回类型相同，覆写基类的命中语义也会把点派发弄坏。
  [[nodiscard]] auto grab_at(st::math::Point point) const -> std::pair<int, TimelineGrab>;
  void notify_changed();
  void notify_seek();

  Project* project_{nullptr};
  int selected_{-1};
  int hover_clip_{-1};
  TimelineGrab grab_{TimelineGrab::None};
  int grab_clip_{-1};
  /// 拖拽片段时的抓取偏移（秒）：保持"抓到哪就跟着哪"，避免块跳到鼠标位置。
  double grab_offset_{0.0};
  bool dragging_playhead_{false};

  st::math::Color ruler_bg_{};
  st::math::Color track_bg_{};
  st::math::Color track_bg_alt_{};
  st::math::Color grid_{};
  st::math::Color clip_fill_{};
  st::math::Color clip_selected_{};
  st::math::Color clip_border_{};
  st::math::Color playhead_{};
  st::math::Color text_{};
};

/// 自绘预览监视器：显示播放头处当前帧的合成结果。
///
/// 与时间轴分离的理由：监视器的重绘频率（播放中每帧）远高于时间轴
///（只有播放头动），合成到一起会让整条时间轴跟着每帧重画。
class MonitorView final : public st::ui::Element {
 public:
  MonitorView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "MonitorView"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Panel; }

  void set_project(Project* project);
  /// 强制按当前播放头重合成（seek/播放推进后调用）。
  void refresh();
  /// 取最近一帧的 **FNV-1a 64 哈希**（e2e 用它断言"同帧两次截图逐像素相同"）。
  ///
  /// 为什么在组件里算而不是让 e2e 截屏后比：截屏含**整个窗口**（含状态栏的
  /// 时间码文字、播放头位置等），那些量本来就该随播放头变——拿它们当判据会把
  /// "预览真的跟着走"与"UI 文字变了"混在一起。这里哈希的是**监视器画布本身**。
  [[nodiscard]] auto frame_hash() const noexcept -> std::uint64_t { return frame_hash_; }
  [[nodiscard]] auto frame_time() const noexcept -> double { return frame_time_; }

  void apply_theme(const st::ui::Theme& theme) override;
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  Project* project_{nullptr};
  /// 最近一次合成的帧（离屏）；`paint_content` 直接贴它。
  mutable st::raster::Canvas frame_;
  mutable double frame_time_{-1.0e9};
  mutable std::uint64_t frame_hash_{0};
  mutable bool frame_valid_{false};
  st::math::Color background_{};
  st::math::Color border_{};
};

}  // namespace caiyun
