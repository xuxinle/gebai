#pragma once

/// @file form_row.hpp
/// @brief `FormRow`：表单行（左标签 + 右控件 + 可选说明/错误），三档标签宽度策略。
///
/// ## 为什么表单行要是一个组件
///
/// 一行表单看起来是 `Row(Text, Control)`，手搭时会立刻撞到四件事：
///
/// 1. **标签列宽**：每个行各自 `width` → 竖向看过去标签参差不齐。真正的表单要求
///    同组行的标签列**等宽对齐**——所以本组件提供 `LabelWidth::Fixed(w)` 与
///    `LabelWidth::Auto`（同行内按最宽的标签撑）两种策略，且**上下行可共享同一宽度**
///    （`set_label_width` 显式给值，典型用法：父容器量一次再传给每行）。
/// 2. **标签垂直对齐**：控件高 30、标签高 20 时，标签该居中对齐**控件的首行基线**
///    而不是整行盒（多行文本域上这个差别有十几像素）。
/// 3. **必填星号**：`*` 要贴标签右缘、用 danger 色，且不占标签文本的排版宽。
/// 4. **错误文案出现时会不会把下面的行推下去**：会。所以错误行必须**参与布局**
///    （而不是浮在上面盖住下一行）——这是本组件用真实子节点而不是绘制的原因。
///
/// ## 几何
///
/// ```
/// 标签********   ┌──────────────────────────┐
/// （required）   │ 控件（占满剩余宽）        │
///                └──────────────────────────┘
///                说明 / 错误（控件下方、小字）
/// ```
///
/// ## 属性面
///
/// `label`、`hint`、`error`（有值即错误态）、`required`、`label_width`。
/// 控件本身通过 `add_child()` 挂进来（**只能有一个**，第二个会被拒绝并记录）。

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

/// 表单行。
class FormRow : public Element {
 public:
  static constexpr float kLabelGap{12.0f};      ///< 标签与控件之间的间距（px）
  static constexpr float kHintGap{4.0f};        ///< 控件与说明之间的间距（px）
  static constexpr float kDefaultLabelWidth{88.0f};  ///< 默认标签列宽（px）

  FormRow();
  explicit FormRow(std::string label);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "FormRow"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  void set_label(std::string label);
  [[nodiscard]] auto label() const noexcept -> const std::string& { return label_; }
  /// 说明文字（控件下方、`text_faint`）。
  void set_hint(std::string hint);
  [[nodiscard]] auto hint() const noexcept -> const std::string& { return hint_; }
  /// 错误文案（`text_faint` 换 `danger`；有值即错误态，同时把控件描边染红）。
  void set_error(std::string error);
  [[nodiscard]] auto error() const noexcept -> const std::string& { return error_; }
  void set_required(bool required);
  [[nodiscard]] auto required() const noexcept -> bool { return required_; }
  /// 标签列宽（`<= 0` = 自动：按自身标签宽）。
  void set_label_width(float width);
  [[nodiscard]] auto label_width() const noexcept -> float { return label_width_; }
  /// 标签在上、控件在下（窄屏/长控件用；默认左右）。
  void set_stacked(bool stacked);
  [[nodiscard]] auto stacked() const noexcept -> bool { return stacked_; }

  /// 挂控件（**只接受一个**；已有则返回 `nullptr` 并保持原控件不动）。
  auto add_child(std::unique_ptr<Element> child) -> Element* override;
  /// 当前控件（未挂时 `nullptr`）。
  [[nodiscard]] auto control() const noexcept -> Element*;

  // —— 几何（arrange 后有效）——
  [[nodiscard]] auto label_rect() const noexcept -> math::Rect { return label_box_; }
  [[nodiscard]] auto control_rect() const noexcept -> math::Rect { return control_box_; }
  [[nodiscard]] auto message_rect() const noexcept -> math::Rect { return message_box_; }
  /// 实际生效的标签列宽（`Auto` 时是量出来的值）。
  [[nodiscard]] auto effective_label_width() const noexcept -> float { return effective_label_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto semantics_value() const -> std::string override { return error_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;

 private:
  [[nodiscard]] auto message_text() const -> const std::string&;
  void relayout(const RenderContext& context, math::Rect rect) const;

  std::string label_{};
  std::string hint_{};
  std::string error_{};
  bool required_{false};
  bool stacked_{false};
  float label_width_{0.0f};
  float label_font_{14.0f};
  float message_font_{12.0f};

  mutable math::Rect label_box_{};
  mutable math::Rect control_box_{};
  mutable math::Rect message_box_{};
  mutable float effective_label_{kDefaultLabelWidth};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::FormRow, "FormRow")
