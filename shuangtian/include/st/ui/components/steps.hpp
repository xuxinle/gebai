#pragma once

/// @file steps.hpp
/// @brief `Steps`：步骤条（`① 已完成 ── ② 进行中 ── ③ 待办`；横/纵两向）。
///
/// ## 三态与几何约定
///
/// 每一步是「编号圆（或对勾/图标）+ 标题 + 可选说明 + 与下一步之间的连线」：
///
/// | 状态 | 圆 | 标题 | 连线（本步右侧那段） |
/// |---|---|---|---|
/// | 已完成 `Done` | `primary` 底 + 白对勾 | `text` | `primary` |
/// | 进行中 `Active` | `primary` 底 + 白编号 + 外圈 `primary_soft` 光环 | `text` + `Medium` | `border` |
/// | 待办 `Todo` | `surface_sunken` 底 + `border` 描边 + `text_muted` 编号 | `text_muted` | `border` |
///
/// 连线颜色取**左端那一步**的状态：已完成步右侧的线是通行色，这才是进度条的正确读法。
///
/// ## 为什么状态由调用方给、不由 `current` 推
///
/// 「第 3 步正在进行，但第 2 步被跳过了」是真实流程（向导里跳步很常见）。只给一个
/// `current` 就没法表达「跳过的步骤不该是对勾」。所以主接口是 `set_steps` 带显式状态，
/// `set_current(index)` 只是**便捷入口**：按 `index` 推出「前面全 Done、自己 Active、
/// 后面 Todo」。
///
/// ## 属性面
///
/// `current`（当前步，可写）、`steps`（`|` 分隔的标题，可写）、`orientation`（可写）、
/// `count`（步数，只读）。动作面：`next`、`prev`、`goto`（参数 = 序号）。

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 步骤条。
class Steps : public Element {
 public:
  /// 单步状态。
  enum class State : std::uint8_t { Todo, Active, Done };

  /// 一步。
  struct Step {
    std::string title{};
    std::string description{};
    State state{State::Todo};
    /// 可选图标名（非空时**替代**编号：如已完成用 `check`、某步是上传用 `upload`）。
    std::string icon{};
  };

  static constexpr float kCircleSize{24.0f};      ///< 编号圆直径（px）
  static constexpr float kCircleGap{10.0f};       ///< 圆与文字的间距（px）
  static constexpr float kLineGap{8.0f};          ///< 连线两端与相邻内容的间距（px）
  static constexpr float kLineThickness{2.0f};    ///< 连线厚度（px）
  static constexpr float kActiveHalo{4.0f};       ///< 进行中的光环外扩（px）

  Steps();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Steps"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::List; }

  // —— 数据 ——
  void set_steps(std::vector<Step> steps);
  /// 按标题建一组「纯文本步骤」，状态由 `set_current` 推。
  void set_titles(std::vector<std::string> titles);
  [[nodiscard]] auto steps() const noexcept -> const std::vector<Step>& { return steps_; }
  [[nodiscard]] auto step_count() const noexcept -> std::size_t { return steps_.size(); }

  /// 当前步：前面全部 `Done`、该步 `Active`、后面全部 `Todo`（越界忽略）。
  ///
  /// `notify` 为真时派发 `on_change`。控制通道写 `current` 属性走这条路径。
  void set_current(std::size_t index, bool notify = false);
  /// 当前步序号（无 `Active` 步时为 0；全 `Done` 时返回末步——「已完成到末尾」）。
  [[nodiscard]] auto current() const -> std::size_t;
  /// 是否全部已完成。
  [[nodiscard]] auto all_done() const noexcept -> bool;

  void set_orientation(FlexDirection orientation);
  [[nodiscard]] auto orientation() const noexcept -> FlexDirection { return orientation_; }

  // —— 几何（arrange 后有效）——
  /// 第 `index` 步的编号圆矩形（越界为空）。
  [[nodiscard]] auto circle_rect(std::size_t index) const -> math::Rect;
  /// 第 `index` 步与下一步之间的连线矩形（末步为空）。
  [[nodiscard]] auto connector_rect(std::size_t index) const -> math::Rect;
  /// 第 `index` 步的可点区（圆 + 文字整块；越界为空）。
  [[nodiscard]] auto step_rect(std::size_t index) const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 点击某一步（实参为步序号；是否允许跳步由调用方决定——组件不删/不改状态）。
  std::function<void(std::size_t)> on_click{};
  /// 当前步变化（`set_current` 且 `notify` 为真时）。
  std::function<void(std::size_t)> on_change{};

 private:
  /// 逐步的落位结果（绘制与命中共用；`measure` 也会填它——宽度度量要同一份几何）。
  struct Placed {
    math::Rect step{};      ///< 整块可点区（圆 + 文字）
    math::Rect circle{};    ///< 编号圆
    math::Rect connector{}; ///< 与下一步之间的连线（末步为空）
  };

  /// 计算落位（横向按文本宽逐段推进；纵向每步一行）。
  void relayout(const RenderContext& context) const;
  [[nodiscard]] auto step_at(math::Point point) const -> std::optional<std::size_t>;
  /// 标题字号（主题 `font_sm`，进行中不改字号只改字重）。
  [[nodiscard]] auto title_size() const noexcept -> float { return title_size_; }

  std::vector<Step> steps_{};
  FlexDirection orientation_{FlexDirection::Row};
  mutable std::vector<Placed> placed_{};
  mutable float title_size_{13.0f};
  mutable int hover_index_{-1};
  /// 进入 `arrange` 后置真：`paint`/命中只在有几何时才工作。
  mutable bool placed_valid_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Steps, "Steps")
