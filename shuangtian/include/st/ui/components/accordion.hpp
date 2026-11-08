#pragma once

/// @file accordion.hpp
/// @brief `Accordion`：折叠面板组（标题行 + 展开内容，单选/多选、动画、受控展开）。
///
/// ## 为什么需要它
///
/// 折叠面板看似只是「点标题显示隐藏内容」，但真实需求一定会撞到这几件：
///
/// 1. **单选 vs 多选**：`Single`（同时只开一个，如 FAQ）与 `Multiple`（各自独立）。
/// 2. **`Single` 的边界语义**：点开当前已展开的那一项时，是「收起」还是「什么都不做」？
///    两种做法都有人要（`allow_collapse` 开关）。
/// 3. **展开动画与高度**：内容高度要**逐帧插值**，而插值期间命中区必须与视觉一致
///    （否则点"看得见但还不在命中区"的地方没反应）。
/// 4. **标题行的角色**：整行可点（不是只有箭头可点），且箭头要旋转。
///
/// 本组件把面板项作为**数据**管理（标题 + 内容 Element），上述四件事一次写对。
///
/// ## 几何
///
/// ```
/// ┌──────────────────────────────────────────┐
/// │ ▸ 标题一                                 │  ← 标题行（整行可点）
/// ├──────────────────────────────────────────┤
/// │ ▾ 标题二                                 │
/// │   ┌────────────────────────────────────┐ │
/// │   │ 内容（Element）                    │ │
/// │   └────────────────────────────────────┘ │
/// └──────────────────────────────────────────┘
/// ```
///
/// ## 属性面
///
/// `items`（`|` 分隔的标题，只读）、`open`（展开项的索引，`|` 分隔，可写）、
/// `mode`（single/multiple，可写）、`allow_collapse`（可写）、`separated`（可写）。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 折叠面板组。
class Accordion : public Element {
 public:
  static constexpr float kHeaderHeight{38.0f};   ///< 标题行高（px）
  static constexpr float kPaddingX{10.0f};       ///< 标题行横向内边距（px）
  static constexpr float kChevronSize{14.0f};    ///< 箭头边长（px）
  static constexpr float kGap{8.0f};             ///< 箭头与标题文本间距（px）
  static constexpr float kContentPadding{10.0f}; ///< 内容区内边距（px）
  static constexpr float kAnimSeconds{0.16f};    ///< 展开/收起动画时长（秒）

  /// 展开模式。
  enum class Mode : std::uint8_t { Single, Multiple };

  Accordion();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Accordion"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  // —— 面板项 ——
  /// 追加一个面板（`content` 可为空：先只有标题，之后再 `set_content`）。
  void add_panel(std::string title, std::unique_ptr<Element> content = nullptr);
  void clear_panels();
  [[nodiscard]] auto panel_count() const noexcept -> std::size_t { return panels_.size(); }
  [[nodiscard]] auto title_at(std::size_t index) const -> std::string;
  void set_title(std::size_t index, std::string title);
  /// 第 `index` 个面板的内容 Element（未设置时 `nullptr`）。
  [[nodiscard]] auto content_at(std::size_t index) const noexcept -> Element*;
  void set_content(std::size_t index, std::unique_ptr<Element> content);

  // —— 展开状态 ——
  void set_mode(Mode mode);
  [[nodiscard]] auto mode() const noexcept -> Mode { return mode_; }
  /// `Single` 模式下点击已展开项是否收起（`false` = 始终保持一个展开）。
  void set_allow_collapse(bool allow);
  [[nodiscard]] auto allow_collapse() const noexcept -> bool { return allow_collapse_; }
  /// 展开第 `index` 项（`Single` 模式会先收起其它项）。
  void open(std::size_t index);
  void close(std::size_t index);
  void toggle(std::size_t index);
  void close_all();
  [[nodiscard]] auto is_open(std::size_t index) const -> bool;
  /// 全部展开项的索引（升序）。
  [[nodiscard]] auto open_indices() const -> std::vector<std::size_t>;
  /// 面板之间是否画分隔线。
  void set_separated(bool separated);
  [[nodiscard]] auto separated() const noexcept -> bool { return separated_; }

  // —— 几何（arrange 后有效）——
  /// 第 `index` 个标题行的矩形（越界为空）。
  [[nodiscard]] auto header_rect(std::size_t index) const -> math::Rect;
  /// 第 `index` 个内容区的**可见**矩形（收起的为空；动画中以插值高度为准）。
  [[nodiscard]] auto content_rect(std::size_t index) const -> math::Rect;

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

  /// 展开状态变化（实参为当前全部展开项索引）。
  std::function<void(const std::vector<std::size_t>&)> on_change{};

 private:
  struct Panel {
    std::string title{};
    std::unique_ptr<Element> content{};
    bool open{false};
    /// 动画进度 0..1（`1` = 完全展开）。收起的面板恒为 0。
    mutable float progress{1.0f};
    /// 内容区的自然高（每帧按宽度重量）。
    mutable float content_height{0.0f};
  };

  /// 推进展开动画一步（在 `arrange` 里调：几何要先于绘制就绪）。
  /// 返回是否仍在动画中（需要续帧）。
  auto advance_animation() const -> bool;
  /// 第 `index` 项当前占的可见内容高（含动画插值）。
  [[nodiscard]] auto visible_content_height(std::size_t index) const -> float;
  /// 把展开状态推进到目标（不做动画）：`open`/`close`/`toggle` 的公共回落。
  void set_open(std::size_t index, bool open);
  void apply_mode(std::size_t opened_index);
  void relayout(const RenderContext& context, math::Rect rect) const;

  std::vector<Panel> panels_{};
  Mode mode_{Mode::Single};
  bool allow_collapse_{true};
  bool separated_{true};
  float title_font_{14.0f};

  mutable std::vector<math::Rect> header_boxes_{};
  mutable std::vector<math::Rect> content_boxes_{};
  mutable int hover_index_{-1};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Accordion, "Accordion")
