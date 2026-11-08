#pragma once

/// @file tag_input.hpp
/// @brief `TagInput`：标签输入（已输入的标签做成 Chip + 末尾一个可输入的空位）。
///
/// ## 它解决的具体问题
///
/// 「输入多个标签」的界面在实现上必然碰到**标签与输入框共用一行、且折行**：
///
/// - 标签要能**换行**（十几个标签时不能横向溢出），而输入位必须**跟着最后一个标签**
///   落到同一行末尾；
/// - 空位宽度要随已输入文本增长（否则看不到自己在打什么），但增长到行尾要能**折行**，
///   把空位带到下一行；
/// - 一个字符都还没敲时，空位要显示占位提示；
/// - 退格键在**空输入时**要删掉最后一个标签（这是标签输入的肌肉记忆，漏了就很难用）。
///
/// 这四件事任何一件手搭都容易做成「看起来像、用起来别扭」。本组件把它们一次写对。
///
/// ## 几何
///
/// ```
/// ┌──────────────────────────────────────────────┐
/// │ (标签A ×)(标签B ×)(标签C ×) [输入中…      ] │
/// │ (标签D ×)(标签E ×) [                      ] │
/// └──────────────────────────────────────────────┘
/// ```
///
/// ## 属性面
///
/// `tags`（`|` 分隔，可写）、`draft`（正在输入、尚未提交的文本）、`placeholder`、
/// `max`（上限，`0` = 不限）、`addable`（是否允许新增）。
/// 动作面：`add`（参数为标签文本）、`remove`（参数为文本或索引）、`clear`。

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

/// 标签输入。
class TagInput : public Element {
 public:
  static constexpr float kPadding{6.0f};        ///< 内边距（px）
  static constexpr float kTagGap{6.0f};         ///< 标签间距（px）
  static constexpr float kRowGap{6.0f};         ///< 折行行距（px）
  static constexpr float kTagHeight{22.0f};     ///< 标签高（px）
  static constexpr float kTagPaddingX{7.0f};    ///< 标签内横向留白（px）
  static constexpr float kCloseWidth{14.0f};    ///< 标签关闭 × 的宽（px）
  static constexpr float kRadius{6.0f};         ///< 外框圆角（px）
  static constexpr float kMinDraftWidth{48.0f}; ///< 输入位最小宽（px）

  TagInput();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "TagInput"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::TextInput; }

  // —— 标签 ——
  [[nodiscard]] auto tags() const noexcept -> const std::vector<std::string>& { return tags_; }
  /// 新增（去重、去空白、受 `max` 限制）。返回是否真的加了。
  auto add_tag(std::string tag) -> bool;
  /// 按下标删除。返回是否删了。
  auto remove_tag(std::size_t index) -> bool;
  /// 按文本删除（删除**所有**同值项——重复项本就不该存在，防御式全清）。
  auto remove_tag_value(std::string_view tag) -> bool;
  void clear_tags();
  void set_tags(std::vector<std::string> tags);

  void set_max_tags(std::size_t max_tags);
  [[nodiscard]] auto max_tags() const noexcept -> std::size_t { return max_tags_; }
  void set_addable(bool addable);
  [[nodiscard]] auto addable() const noexcept -> bool { return addable_; }
  void set_placeholder(std::string placeholder);
  [[nodiscard]] auto placeholder() const noexcept -> const std::string& { return placeholder_; }
  /// 正在输入、尚未提交的文本。
  [[nodiscard]] auto draft() const noexcept -> const std::string& { return draft_; }
  void set_draft(std::string draft);
  /// 提交当前 `draft`（空/重复/超限时不动），成功返回 `true`。
  auto commit_draft() -> bool;

  // —— 几何（arrange 后有效）——
  /// 第 `index` 个标签的矩形（越界为空）。
  [[nodiscard]] auto tag_rect(std::size_t index) const -> math::Rect;
  /// 输入位矩形（含占位提示的绘制区）。
  [[nodiscard]] auto draft_rect() const noexcept -> math::Rect { return draft_box_; }
  /// 第 `index` 个标签的关闭 × 命中区（越界为空）。
  [[nodiscard]] auto close_rect(std::size_t index) const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 标签集合变化（新增/删除/清空都会派发）。
  std::function<void(const std::vector<std::string>&)> on_change{};
  /// 输入了重复标签（界面给个轻提示用）。
  std::function<void(std::string_view)> on_duplicate{};

 private:
  /// 标签的绘制宽（文本 + 左右留白 + 关闭位）。
  [[nodiscard]] auto tag_width(const RenderContext& context, std::string_view tag) const -> float;
  void relayout(const RenderContext& context, math::Rect rect) const;
  void notify_change();
  auto handle_key(const Event& event) -> bool;
  void insert_text(std::string_view inserted);

  std::vector<std::string> tags_{};
  std::string draft_{};
  std::string placeholder_{"输入后按回车添加"};
  std::size_t max_tags_{0};
  bool addable_{true};
  float font_size_{13.0f};

  mutable std::vector<math::Rect> tag_boxes_{};
  mutable std::vector<math::Rect> close_boxes_{};
  mutable math::Rect draft_box_{};
  mutable int hover_tag_{-1};
  mutable int hover_close_{-1};
  mutable int last_blink_visible_{-1};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::TagInput, "TagInput")
