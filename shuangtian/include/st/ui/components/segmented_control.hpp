#pragma once

/// @file segmented_control.hpp
/// @brief `SegmentedControl`：分段控件（`[ 日 | 周 | 月 ]`；选中段有滑块指示）。
///
/// ## 与 `Tabs` / `RadioGroup` 的分工
///
/// 三者视觉上都像「一排可选项」，语义不同：
///
/// | 组件 | 语义 | 选择是否必须 | 典型场景 |
/// |---|---|---|---|
/// | `Tabs` | 切换**视图**（内容区跟着换） | 是 | 文件标签页、设置分区 |
/// | `SegmentedControl` | 切换**同一个视图的参数** | 是 | 日/周/月、列表/网格、源码/预览 |
/// | `RadioGroup` | 一组**可空**选项（表单字段） | 否（可都不选） | 单选题 |
///
/// 关键差别在**选择是否可空**与**是否自带指示滑块**：分段控件的选中段有一块随动滑块
/// （`metrics.motion_normal` 内滑动），它表达「这是当前的一个状态」，而不是「你填了哪项」。
///
/// ## 属性面
///
/// `options`（`|` 分隔的段标签，可写）、`index`（当前段，可写；`-1` = 无选中）、
/// `count`（段数，只读）。动作面：`select`（参数 = 序号或标签名）、
/// `step_forward`、`step_backward`。
///
/// ## 键盘
///
/// `ArrowLeft`/`ArrowRight`（横向）或 `ArrowUp`/`ArrowDown`（纵向）切换，
/// `Home`/`End` 到首/末；切换即派发 `on_change`。等宽分段用 `set_stretch(true)`。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 分段控件。
class SegmentedControl : public Element {
 public:
  static constexpr float kHeight{30.0f};       ///< 控件高（px）
  static constexpr float kInset{2.0f};         ///< 滑块与外壳的内缩（px）
  static constexpr float kRadius{7.0f};        ///< 外圆角（px）

  SegmentedControl();

  [[nodiscard]] auto type() const noexcept -> std::string_view override {
    return "SegmentedControl";
  }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Tab; }

  // —— 数据 ——
  void set_options(std::vector<std::string> labels);
  void add_option(std::string label);
  void clear_options();
  [[nodiscard]] auto options() const noexcept -> const std::vector<std::string>& { return labels_; }
  [[nodiscard]] auto option_count() const noexcept -> std::size_t { return labels_.size(); }
  [[nodiscard]] auto option_label(std::size_t index) const -> std::string_view;

  /// 选中某段（越界夹取到 `[0, count-1]`；`notify` 为真时派发 `on_change`）。
  ///
  /// 空表时无选中（`index()` 返回 `-1`）。
  void set_index(int index, bool notify = false);
  /// 当前段（-1 = 无选中，如空表）。
  [[nodiscard]] auto index() const noexcept -> int { return index_; }
  /// 当前段标签（无选中为空）。
  [[nodiscard]] auto selected_label() const -> std::string_view;
  /// 按标签名选中（不存在返回 `false`）。
  auto select_label(std::string_view label, bool notify = false) -> bool;

  /// 每段等宽（默认 `false` = 按文本宽；`true` 时均分总宽——一排等大方块更好扫）。
  void set_stretch(bool value);
  [[nodiscard]] auto stretch() const noexcept -> bool { return stretch_; }
  void set_orientation(FlexDirection orientation);
  [[nodiscard]] auto orientation() const noexcept -> FlexDirection { return orientation_; }

  // —— 几何（arrange 后有效）——
  /// 第 `index` 段的命中矩形（越界为空）。
  [[nodiscard]] auto segment_rect(std::size_t index) const -> math::Rect;
  /// 选中滑块矩形（无选中时为空）。
  [[nodiscard]] auto indicator_rect() const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 选中段变化回调（实参为新序号）。
  std::function<void(int)> on_change{};

 private:
  /// 重算各段宽度缓存（依赖文本端口）。
  void rebuild_widths(const RenderContext& context) const;
  /// 重算落位（绘制/命中/属性的几何唯一来源）。
  void relayout() const;
  [[nodiscard]] auto segment_at(math::Point point) const -> std::optional<std::size_t>;
  void nudge(int delta, bool notify);

  std::vector<std::string> labels_{};
  int index_{0};
  bool stretch_{false};
  FlexDirection orientation_{FlexDirection::Row};

  mutable std::vector<float> widths_{};
  mutable std::vector<math::Rect> rects_{};
  mutable int hover_index_{-1};

  // 滑块动画（绘制期推进；静态帧直接到位）。
  //
  // 三态：`kAnimIdle`（已完成/未启动，直接画目标位置）、
  // `kAnimPending`（已请求但还没拿到时间轴，下一帧起跑）、正数（起跑时刻）。
  // 需要一个待启态的理由：`set_index` 时不知道下一帧的时刻，而把“起跑”推迟到
  // 绘制里判断会让重排/主题重入也重置过渡（实测：动画永远停在半程）。
  static constexpr double kAnimIdle = -1.0;
  static constexpr double kAnimPending = -2.0;
  mutable math::Rect indicator_{};
  mutable math::Rect indicator_from_{};
  mutable double indicator_start_{kAnimIdle};
  mutable double last_time_{0.0};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::SegmentedControl, "SegmentedControl")
