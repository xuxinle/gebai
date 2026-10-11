#pragma once

/// **时间轴视图**（`TimelineView`）：把 `st::ui::timeline::Model` 画出来并允许编辑。
///
/// ## 为什么数据与绘制分开（这个组件存在的意义）
///
/// caiyun 原先把「轨道/片段数据」与「命中判定/拖拽/修剪/吸附/标尺刻度」全写在一个
/// 400 行的自绘件里，那些逻辑只能靠截图验证。数据层（`timeline.hpp`）抽走之后
/// 有 7 例 79 断言的纯逻辑测试；本组件只做三件事：
/// **调换算 → 画 → 把鼠标事件翻译成模型操作**。
///
/// ## 三条交互约定（都是"错了会让人以为功能坏了"的地方）
///
/// 1. **拖拽期间不吸附到自己**：`Model::snap` 的 `exclude_*` 参数必须传**被拖的那个
///    片段**——不传的话它会永远吸在自己原来的位置，表现是"拖不动"。
/// 2. **修剪要动 `source_in`**：左边缘往右拖时，源入点要同步 +Δ（否则"修剪"变成了
///    "把内容往后挪"，而画面看起来只是"片段变短了"——素材内容错位在那个尺度上看不出来）。
/// 3. **锁定轨道不接受编辑**（但仍可选中查看）：锁定的语义是"别动它"，
///    不是"看不见它"。
///
/// ## 空白处拖动 = 平移（而不是框选）
///
/// 两个都合理，选平移的理由是**它不需要额外状态**：框选要维护"框"、要定义
/// "被框中的片段"、还要与单击（取消选择）区分开——而那三件事都要有可见反馈才不困惑。
/// 时间轴的高频动作是"看别处"，平移正好；框选留给将来的多选需求。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/timeline.hpp"

namespace st::ui {

class TimelineView : public Element {
 public:
  /// 标尺带高度（逻辑像素）。
  static constexpr float kRulerHeight{24.0f};
  /// 轨道名前缀栏宽度（`名称` 显示在轨道左端）。
  static constexpr float kTrackHeaderWidth{88.0f};
  /// 修剪边缘的命中带宽度（与 `Model::hit_test` 的默认一致）。
  static constexpr float kEdgePx{6.0f};

  TimelineView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "TimelineView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  /// 数据模型（宿主填充轨道与片段；组件只读它、改它）。
  [[nodiscard]] auto model() noexcept -> timeline::Model& { return model_; }
  [[nodiscard]] auto model() const noexcept -> const timeline::Model& { return model_; }

  /// 显示选项。
  void set_show_ruler(bool value) { show_ruler_ = value; mark_layout_dirty(); }
  [[nodiscard]] auto show_ruler() const noexcept -> bool { return show_ruler_; }
  /// 吸附开关（关掉时拖拽完全跟手，不再被候选拉走）。
  void set_snap_enabled(bool value) noexcept { snap_enabled_ = value; }
  [[nodiscard]] auto snap_enabled() const noexcept -> bool { return snap_enabled_; }
  /// 吸附候选来源（透传给 `Model::snap`）。
  void set_snap_options(timeline::SnapOptions options) { snap_options_ = std::move(options); }
  [[nodiscard]] auto snap_options() const noexcept -> const timeline::SnapOptions& {
    return snap_options_;
  }

  // —— 回调（宿主据此更新"当前时间"显示、落盘撤销栈等）——
  std::function<void(timeline::Millis)> on_playhead_changed{};
  std::function<void(std::size_t track, std::size_t clip)> on_clip_moved{};
  std::function<void(std::size_t track, std::size_t clip)> on_clip_trimmed{};

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action,
                                   std::string_view argument) -> bool override;

 private:
  /// 拖拽目标（一次只可能拖一样）。
  struct Drag {
    enum class Kind : std::uint8_t { None, Playhead, ClipBody, ClipLeft, ClipRight, Pan };
    Kind kind{Kind::None};
    std::size_t track{0};
    std::size_t clip{0};
    /// 开始拖动时的指针位置（屏幕）——用来算增量，而不是"跟着指针跳变"。
    ///
    /// 用增量而非绝对位置的理由：修剪的"最小长度"与"不能越过相邻片段"这类约束
    /// 一旦生效，绝对定位会让片段在约束边界上"粘住后跳走"；增量式则自然停住。
    math::Point origin{};
    timeline::Millis clip_start{0};
    timeline::Millis clip_duration{0};
    timeline::Millis clip_source_in{0};
    timeline::Millis scroll{0};
  };

  /// 时间轴的绘图区（排除轨道名前缀栏）。
  [[nodiscard]] auto lane_rect() const -> math::Rect;
  /// 标尺带矩形（`show_ruler_` 为假时为空）。
  [[nodiscard]] auto ruler_rect() const -> math::Rect;
  /// 轨道带矩形（内容区坐标，含横向滚动）。
  [[nodiscard]] auto track_band(std::size_t index) const -> math::Rect;
  /// 片段矩形（为空 = 该片段不可见/不存在）。
  [[nodiscard]] auto clip_rect(std::size_t track, std::size_t clip) const -> math::Rect;
  /// 屏幕 x → 时间轴时间（把 lane 内的相对 x 转成模型坐标）。
  [[nodiscard]] auto x_to_time(float screen_x) const -> timeline::Millis;
  [[nodiscard]] auto time_to_x(timeline::Millis time) const -> float;

  timeline::Model model_{};
  bool show_ruler_{true};
  bool snap_enabled_{true};
  timeline::SnapOptions snap_options_{};
  Drag drag_{};
  /// 播放头是否正在被拖（绘制时把柄画实心，提示"你抓着它"）。
  bool dragging_playhead_{false};

  math::Color surface_{};
  math::Color surface_alt_{};
  math::Color surface_sunken_{};
  math::Color border_{};
  math::Color text_color_{};
  math::Color text_muted_{};
  math::Color accent_{};
  math::Color clip_fill_{};
  math::Color clip_selected_{};
  math::Color playhead_color_{};
  math::Color muted_track_{};
  float font_size_{12.0f};
};

}  // namespace st::ui
