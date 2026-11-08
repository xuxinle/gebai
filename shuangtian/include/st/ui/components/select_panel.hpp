#pragma once

/// @file select_panel.hpp
/// @brief `SelectPanel`：下拉面板：`surface` + `shadow_md` + `radius_md`，选项行高 32（hover `surface_alt` / 选中 `primary_soft`）。。
///
/// 从 `select.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `select.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 下拉选项：`value` 为表单/控制通道值，`label` 为显示文本（空则显示 `value`）。
///
/// 为何放 `select_panel.hpp` 而不是 `select.hpp`：两者都要它，而 `Select` 已依赖
/// `SelectPanel`（面板由它创建并交出）。共享定义放**被依赖的那一侧**——依赖方向单向，
/// 不会出现“两个头互相要对方”。
struct SelectOption {
  std::string value{};
  std::string label{};
};

/// 下拉面板：`surface` + `shadow_md` + `radius_md`，选项行高 32（hover `surface_alt` / 选中 `primary_soft`）。
///
/// 由 `Select` 创建并经 `overlay_host` 交出，调用方负责其生命周期。面板按 `anchor`
/// （绝对坐标 = 控件左下角）自行定位：叠加层容器给它的布局槽位只用于尺寸，原点由锚点决定。
class SelectPanel : public Element {
 public:
  SelectPanel(std::vector<SelectOption> options, std::optional<std::size_t> selected);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "SelectPanel"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::List; }

  /// 锚点（绝对坐标：控件左下角）。
  void set_anchor(math::Point anchor);
  /// 面板宽度（通常与控件等宽）。
  void set_panel_width(float width);
  void set_selected(std::optional<std::size_t> index);

  /// 选项行矩形（命中/测试用；越界返回空矩形）。
  [[nodiscard]] auto option_rect(std::size_t index) const -> math::Rect;
  [[nodiscard]] auto option_count() const noexcept -> std::size_t { return options_.size(); }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

  /// 选项被选中（点击）。回调返回后本对象可能已被摘除，实现内已先 `set_visible(false)`。
  std::function<void(std::size_t)> on_pick{};
  /// 请求收起（`Esc`）。
  std::function<void()> on_dismiss{};

 private:
  /// 指针命中的选项行（-1 无）。
  [[nodiscard]] auto index_at(math::Point point) const -> int;

  std::vector<SelectOption> options_{};
  std::optional<std::size_t> selected_{};
  math::Point anchor_{};
  float panel_width_{0.0f};
  int hover_index_{-1};
};

}  // namespace st::ui

// `SelectPanel(options, selected)`：空选项、无选中是合理默认（实际由 `Select` 填入）。
ST_UI_COMPONENT_WITH(::st::ui::SelectPanel, "SelectPanel",
                     std::make_unique<::st::ui::SelectPanel>(std::vector<::st::ui::SelectOption>{},
                                                             std::nullopt))
