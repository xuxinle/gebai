#pragma once

/// @file dialog.hpp
/// @brief `Dialog`：模态对话框：遮罩 + 居中卡片 + 标题/正文 + 底部按钮行。。
///
/// 从 `overlay.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `overlay.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <functional>
#include <string>
#include <string_view>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 模态对话框：遮罩 + 居中卡片 + 标题/正文 + 底部按钮行。
/// 交互：Esc 与点击卡片外区域触发 `on_dismiss`；按钮点击触发 `on_action(index)`。
class Dialog : public Element {
 public:
  static constexpr float kMinWidth{420.0f};
  static constexpr float kMaxWidth{520.0f};
  static constexpr float kPadding{20.0f};

  explicit Dialog(std::string title = {}, std::string body = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Dialog"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Dialog; }
  /// 模态对话框只有在场时才拦截输入：`set_visible(false)` 后不再截住下层内容
  /// （不可见浮层的命中穿透；见 `Element::intercepts_input` 契约）。
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return visible(); }

  void set_title(std::string title);
  [[nodiscard]] auto title() const noexcept -> const std::string& { return title_; }
  void set_body(std::string body);
  [[nodiscard]] auto body() const noexcept -> const std::string& { return body_; }
  /// 设定底部按钮行（顺序即 `on_action` 的序号；末项为主按钮）。
  ///
  /// 每个按钮会拿到**可预测的 id**：`<dialog-id>-action-<序号>`（末项另有
  /// `<dialog-id>-action-primary`）。没有 id 时，控制通道**定位不到这些按钮**，
  /// 于是“危险操作必须确认”这条安全路径就无法被自动化验证——而它恰恰是最该被
  /// 验证的一条（实测：确认浮层出现得了、点不到，e2e 只能卡在那里）。
  void set_actions(std::vector<std::string> actions);
  [[nodiscard]] auto actions() const noexcept -> const std::vector<std::string>& { return actions_; }

  /// 视口矩形兼容注入（旧代码路径）：设置后 `measure`/`arrange` 以它为准。
  /// 新代码用 `add_overlay(dialog, OverlayLayout::FillViewport)`——遮罩直接铺满分到的
  /// 矩形，无需注入。
  void set_viewport_rect(math::Rect rect);
  [[nodiscard]] auto viewport_rect() const noexcept -> math::Rect { return viewport_; }
  /// 居中卡片矩形（arrange 后有效）。
  [[nodiscard]] auto card_rect() const noexcept -> math::Rect { return card_; }
  /// 第 `index` 个按钮的矩形（arrange 后有效；越界返回空矩形）。
  [[nodiscard]] auto action_rect(std::size_t index) const noexcept -> math::Rect;
  [[nodiscard]] auto action_count() const noexcept -> std::size_t { return actions_.size(); }

  /// 触发关闭（调用 `on_dismiss`；是否真正移除由调用方决定）。
  void dismiss();

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return title_; }
  [[nodiscard]] auto semantics_value() const -> std::string override { return body_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 按钮点击回调（参数为按钮序号）。
  std::function<void(std::size_t)> on_action{};
  /// 关闭请求回调（Esc / 点击遮罩）。
  std::function<void()> on_dismiss{};

 private:
  [[nodiscard]] auto body_lines(const RenderContext& context, float width) const
      -> std::vector<std::string>;

  std::string title_{};
  std::string body_{};
  std::vector<std::string> actions_{};
  std::vector<Element*> buttons_{};  ///< 非拥有（生命周期随子节点）
  math::Rect viewport_{};
  math::Rect card_{};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Dialog, "Dialog")
