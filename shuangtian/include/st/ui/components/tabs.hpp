#pragma once

/// 组件库 A · 标签页（`DESIGN.md` §4.5）：`Tabs`（横向标签条）。
///
/// 依赖纪律：文本经 `context.text`（空则退化 `NullTextPort`）；颜色/间距/字号一律取
/// `context.theme` 的 token（`apply_theme` 时落进 `style_`），组件内不硬编码颜色。
/// 标签项由本组件自绘（不产生子 Element），命中区与指示条锚点经 `tab_rect` 对外暴露。
///
/// 视觉：选中项 `text` 色 + 下方 2px `primary` 指示条（按 `metrics.motion_normal` 滑动），
/// 未选中 `text_muted`，hover 背景 `surface_alt`，disabled 统一降到 `text_faint`。
///
/// 交互：点击标签切换；`ArrowLeft`/`ArrowRight` 循环切换、`Home`/`End` 到首/末；
/// `Enter`/`Space` 重新确认当前项；切换经 `on_change(active_index)` 通知。
///
/// 控制通道属性面：`active`（序号或标签名，可写）、`options`（`|` 分隔的标签清单，可写）、
/// `label`（当前标签名，可写=按名选中）。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class Tabs : public Element {
 public:
  Tabs();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Tabs"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Tab; }

  /// 整表替换（活动项夹取到合法范围，越界时回到首项）。
  void set_tabs(std::vector<std::string> labels);
  void add_tab(std::string label);
  void clear_tabs();

  [[nodiscard]] auto tab_count() const noexcept -> std::size_t { return labels_.size(); }
  /// 越界返回空视图。
  [[nodiscard]] auto tab_label(std::size_t index) const -> std::string_view;

  /// 设置活动标签（越界自动夹取；`notify` 为 true 时触发 `on_change`）。
  void set_active(std::size_t index, bool notify = false);
  [[nodiscard]] auto active_index() const noexcept -> std::size_t { return active_; }
  [[nodiscard]] auto active_label() const -> std::string_view;

  /// 标签项矩形（命中/指示条锚定/测试用；未布局或越界时为空矩形）。
  [[nodiscard]] auto tab_rect(std::size_t index) const -> math::Rect;
  /// 全部标签所需宽度（布局期缓存；未布局时为 0）。
  [[nodiscard]] auto tabs_width() const noexcept -> float { return total_width_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 活动标签变化回调。
  std::function<void(std::size_t)> on_change{};

 private:
  /// 重算标签项宽度缓存（布局期调用；文本度量依赖 `context.text`）。
  void rebuild_widths(const RenderContext& context) const;
  [[nodiscard]] auto tab_index_at(math::Point point) const -> std::optional<std::size_t>;
  /// 指示条当前位置（180ms 滑动；静态帧直接到位）。
  [[nodiscard]] auto resolve_indicator(const RenderContext& context, math::Rect target) const
      -> math::Rect;
  /// 按标签名定位（不存在返回 nullopt）。
  [[nodiscard]] auto index_of_label(std::string_view label) const -> std::optional<std::size_t>;

  std::vector<std::string> labels_{};
  mutable std::vector<float> widths_{};  ///< 每个标签项宽（含内边距），布局期缓存
  mutable float total_width_{0.0f};      ///< 标签条总宽（含项间距）
  std::size_t active_{0};
  float gap_{0.0f};         ///< 标签项间距（`metrics.space_xs`）
  int hover_index_{-1};     ///< 指针悬停项（-1 无）

  // 指示条动画状态（绘制期推进）
  mutable float indicator_x_{0.0f};
  mutable float indicator_width_{0.0f};
  mutable float indicator_from_x_{0.0f};
  mutable float indicator_from_width_{0.0f};
  mutable double indicator_start_{-1.0};
  mutable double last_time_{0.0};
};

}  // namespace st::ui
