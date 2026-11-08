#pragma once

/// @file toolbar.hpp
/// @brief `Toolbar`：工具条（横向/纵向的按钮组 + 分组分隔 + 溢出折叠 + 可选标题）。
///
/// ## 为什么工具条要是一个组件
///
/// 「一排按钮」看似用 `Panel(Row)` 就够了，但工具条有四件只有它才该管的事：
///
/// 1. **溢出**：窗口变窄时按钮必须**折叠进"更多"菜单**，而不是被父级裁掉半个。
///    这是工具条与普通行的根本差别（普通行可以把溢出交给外层滚动）。
/// 2. **分组分隔**：`Divider` 是竖直的、高度要**比按钮矮**（否则看起来像边框），
///    且两侧间距比按钮间距大（分组比并列更疏）。
/// 3. **优先级**：溢出时该先折谁？从右往左折（右侧通常次要），
///    且允许调用方给某项标 `pinned`（永不折叠，如「保存」）。
/// 4. **一致密度**：按钮间距、内边距、容器高固定，不随内容跳动。
///
/// ## 几何
///
/// ```
/// ┌─────────────────────────────────────────────────┬──────┐
/// │ [新建] [打开] │ [保存] [撤销] [重做] ···        │ ≡ ⋯  │
/// └─────────────────────────────────────────────────┴──────┘
///   固定宽 16px 的溢出  ⋯ 按钮（只在真溢出时出现）
/// ```
///
/// ## 用法
///
/// 条目通过 `add_item` / `add_separator` 声明（**不要**直接 `add_child`：
/// 工具条要自己算溢出，直接塞子元素它管不了）：
///
/// ```cpp
/// auto bar = std::make_unique<Toolbar>();
/// bar->add_item(Button{"新建", ...}, /*pinned=*/false);
/// bar->add_separator();
/// bar->add_item(Button{"保存", ...}, /*pinned=*/true);
/// ```
///
/// ## 属性面
///
/// `items`（`|` 分隔的按钮文案，只读；改它请用 API）、`collapsed`（只读：当前折叠了几项）、
/// `count`（只读：条目总数）、`overflow_label`（可写）。

#include <cstddef>
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

class Button;

/// 工具条。
class Toolbar : public Element {
 public:
  static constexpr float kHeight{34.0f};         ///< 默认高（px）
  static constexpr float kPaddingX{8.0f};        ///< 横向内边距（px）
  static constexpr float kItemGap{4.0f};         ///< 按钮间距（px）
  static constexpr float kGroupGap{8.0f};        ///< 分组分隔两侧的间距（px）
  static constexpr float kSeparatorHeight{16.0f};///< 分隔线高（比按钮矮，否则像边框）
  static constexpr float kOverflowWidth{28.0f};  ///< 溢出按钮宽（px）

  /// 条目类型。
  enum class Kind : std::uint8_t { Item, Separator };

  Toolbar();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Toolbar"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  // —— 条目 ——
  /// 追加一个按钮（获得所有权）。`pinned` 的条目**永不折叠**。
  void add_item(std::unique_ptr<Element> item, bool pinned = false);
  /// 追加一个分组分隔。
  void add_separator();
  void clear_items();
  [[nodiscard]] auto item_count() const noexcept -> std::size_t { return items_.size(); }
  /// 第 `index` 个条目（含分隔；`Kind` 区分）。
  [[nodiscard]] auto item_at(std::size_t index) const noexcept -> Element*;
  [[nodiscard]] auto kind_at(std::size_t index) const noexcept -> Kind;
  /// 第 `index` 个条目的矩形（arrange 后有效；被折叠的项为空）。
  [[nodiscard]] auto item_rect(std::size_t index) const -> math::Rect;
  /// 该条目当前是否被折叠（折进溢出菜单）。
  [[nodiscard]] auto is_collapsed(std::size_t index) const -> bool;

  /// 溢出按钮的文案后缀（默认 `+N`，即 `"+3"`）。
  void set_overflow_label(std::string label);
  [[nodiscard]] auto overflow_label() const noexcept -> const std::string& {
    return overflow_label_;
  }
  /// 当前折叠了几项（0 = 没折叠）。
  [[nodiscard]] auto collapsed_count() const noexcept -> std::size_t { return collapsed_; }
  /// 溢出按钮矩形（没折叠时为空）。
  [[nodiscard]] auto overflow_rect() const noexcept -> math::Rect { return overflow_box_; }
  /// 展开溢出菜单（宿主可在此弹 `MenuPanel`）。
  void show_overflow();

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;

  /// 点溢出按钮。
  std::function<void()> on_overflow{};

 private:
  struct Entry {
    std::unique_ptr<Element> element{};   ///< 按钮（分隔项为 `nullptr`）
    Kind kind{Kind::Item};
    bool pinned{false};
    float width{0.0f};                    ///< 缓存的自然宽
  };

  /// 按可用宽算出折叠集合（从右往左折，跳过 pinned）。
  void resolve_collapse(const RenderContext& context, float available_width) const;
  void relayout(const RenderContext& context, math::Rect rect) const;

  std::vector<Entry> items_{};
  std::string overflow_label_{"+"};
  mutable std::size_t collapsed_{0};
  mutable std::vector<math::Rect> rects_{};
  mutable math::Rect overflow_box_{};
  mutable bool hover_overflow_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Toolbar, "Toolbar")
