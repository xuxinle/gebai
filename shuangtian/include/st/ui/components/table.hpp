#pragma once

/// 组件库 C（数据展示与反馈）——数据表格 `Table`。
/// 视觉规格：表头高 36px（`surface_alt` 底 + `text_muted` 12px 文本 + 底部分隔线 `border`）；
/// 行高默认 32px，`zebra` 开启时奇数行铺 `surface_alt`，hover 行同样铺 `surface_alt`；
/// 单元格文本按列的 `TextAlign` 对齐，超宽时经 `TextPort::ellipsize` 省略；
/// 交互规格：表宽超出容器时内部横向偏移（Shift + 滚轮 / ←→ / Home / End），点击行派发 `on_row_click`。
/// 依赖纪律：本组件只经 `RenderContext::text` 取文本能力（可为空 → `NullTextPort`），不 include text 层。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/element.hpp"
#include "st/ui/style.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 列定义（`width <= 0` 表示自适应：按容器剩余宽度均分，并夹取到 `Table::kMinColumnWidth`）。
struct TableColumn {
  std::string name{};
  float width{0.0f};
  TextAlign align{TextAlign::Start};
};

/// 数据表格：列定义 + 行数据，表头/斑马纹/hover/横向滚动全部自绘，无子节点。
class Table : public Element {
 public:
  static constexpr float kHeaderHeight{36.0f};      ///< 表头高（px）
  static constexpr float kDefaultRowHeight{32.0f};  ///< 默认行高（px）
  static constexpr float kMinColumnWidth{72.0f};    ///< 自适应列的最小宽（px）
  static constexpr float kCellPadding{12.0f};       ///< 单元格左右内边距（px，= space_md）
  static constexpr float kScrollStep{48.0f};        ///< 横向滚轮每格距离（px）
  static constexpr float kScrollBarHeight{3.0f};    ///< 横向滚动指示条高（px）

  explicit Table(std::vector<TableColumn> columns = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Table"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Table; }

  // —— 数据 ——
  /// 设定列（会清空既有列宽缓存；行数据保留）。
  void set_columns(std::vector<TableColumn> columns);
  [[nodiscard]] auto columns() const noexcept -> const std::vector<TableColumn>& { return columns_; }
  /// 追加一行（单元格数不足的按空串补齐，多余的忽略）。
  void add_row(std::vector<std::string> cells);
  void clear_rows();
  [[nodiscard]] auto row_count() const noexcept -> std::size_t { return rows_.size(); }
  [[nodiscard]] auto column_count() const noexcept -> std::size_t { return columns_.size(); }
  /// 单元格文本（越界返回空串）。
  [[nodiscard]] auto cell(std::size_t row, std::size_t column) const -> std::string_view;

  // —— 外观开关 ——
  void set_zebra(bool value);
  [[nodiscard]] auto zebra() const noexcept -> bool { return zebra_; }
  void set_row_height(float height);
  [[nodiscard]] auto row_height() const noexcept -> float { return row_height_; }
  /// 行点击回调（参数为行序号）。
  void set_on_row_click(std::function<void(std::size_t)> callback) {
    on_row_click_ = std::move(callback);
  }
  [[nodiscard]] auto hovered_row() const noexcept -> std::size_t { return hovered_row_; }

  // —— 横向滚动 ——
  [[nodiscard]] auto scroll_offset() const noexcept -> float { return scroll_offset_; }
  [[nodiscard]] auto max_scroll() const noexcept -> float;
  void set_scroll_offset(float offset);
  void scroll_by(float delta) { set_scroll_offset(scroll_offset_ + delta); }

  // —— 布局几何（测试/控制通道用；arrange 后有效）——
  [[nodiscard]] auto header_rect() const noexcept -> math::Rect;
  [[nodiscard]] auto row_rect(std::size_t row) const noexcept -> math::Rect;
  /// 实际绘制矩形（已含横向偏移）。
  [[nodiscard]] auto cell_rect(std::size_t row, std::size_t column) const noexcept -> math::Rect;
  [[nodiscard]] auto header_cell_rect(std::size_t column) const noexcept -> math::Rect;
  [[nodiscard]] auto column_width(std::size_t column) const noexcept -> float;
  /// 列内容总宽（不含容器裁剪）。
  [[nodiscard]] auto total_width() const noexcept -> float { return total_width_; }

  // —— Element ——
  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 按可用宽度重算列宽与总宽（自适应列均分剩余宽度）。
  void recompute_columns(float available);
  [[nodiscard]] auto column_x(std::size_t column) const noexcept -> float;
  /// 指针所在行（不在数据行内返回 kNoRow）。
  [[nodiscard]] auto row_at(math::Point point) const noexcept -> std::size_t;

  static constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

  std::vector<TableColumn> columns_{};
  std::vector<std::vector<std::string>> rows_{};
  std::vector<float> column_widths_{};
  std::function<void(std::size_t)> on_row_click_{};
  float total_width_{0.0f};
  float row_height_{kDefaultRowHeight};
  float scroll_offset_{0.0f};
  std::size_t hovered_row_{kNoRow};
  bool zebra_{false};
};

}  // namespace st::ui
