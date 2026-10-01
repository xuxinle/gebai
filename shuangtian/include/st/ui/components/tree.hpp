#pragma once

/// 组件库 B · 树（`Tree`）：文件树/导航树的自绘组件（无子 Element，扁平数组驱动）。
///
/// 数据契约：调用方维护「**可见行**扁平数组」——收起的子树不进数组（展开状态由数据
/// 表达，本组件不持有层级）。刷新用 `sync_nodes`（按 key 对齐：同 key 视为同一行，
/// 选中态跟 key 走而不是索引），语义与 `List::sync_items` 同源。
///
/// 懒加载：点击目录行（或 → 键展开）只触发 `on_toggle(key, expanded)`，由调用方
/// `list_dir` 后重新 `sync_nodes` 追加子节点行——大工程首屏不卡。
///
/// 视觉（与 `List` 同规格）：行高 40、`radius_sm`、hover `surface_alt`、选中
/// `primary_soft` + 左侧 2px `primary` 指示条；目录行前置展开指示（收起
/// `chevron-right`、展开 `chevron-down`），行内容随 `depth` 缩进（每级 16px）。
///
/// 交互：点击目录行 toggle（`on_toggle`）；点击文件行选中（`on_select`）；
/// 键盘 ↑/↓ 移动选中（可见行间）、Enter 激活（文件=选中、目录=toggle）、
/// ←/→ 收起/展开（目录行）。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 一行树数据（扁平数组元素；`key` 是业务身份，同 key 视为同一行）。
struct TreeNode {
  std::string key{};
  std::string label{};
  bool expanded{false};  ///< 目录当前是否展开（数据表达状态，组件不改写）
  bool is_dir{false};    ///< 目录行（有展开指示、点击 toggle）；文件行点击选中
  int depth{0};          ///< 缩进深度（每级 16px）
};

class Tree : public Element {
 public:
  static constexpr float kRowHeight{40.0f};
  static constexpr float kIndentStep{16.0f};
  static constexpr float kIndicatorWidth{2.0f};
  static constexpr float kChevronSize{14.0f};
  /// 无选中哨兵（与 `List::kNoSelection` 同值；本组件不依赖 list.hpp）。
  static constexpr std::size_t kNoSelection = static_cast<std::size_t>(-1);

  Tree();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Tree"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Tree; }

  /// 按行同步数据：同 key 行保持逻辑身份（选中态跟随 key）；消失的 key 移除。
  /// 行序完全由数组顺序决定（扁平化由调用方负责）。
  void sync_nodes(const std::vector<TreeNode>& nodes);
  void clear_nodes();

  [[nodiscard]] auto node_count() const noexcept -> std::size_t { return rows_.size(); }
  /// 越界返回 nullptr。
  [[nodiscard]] auto node_at(std::size_t index) const -> const TreeNode*;
  /// 按 key 定位行号（不存在返回 nullopt）。
  [[nodiscard]] auto index_of_key(std::string_view key) const -> std::optional<std::size_t>;

  /// 选中指定行（key 不存在则清空选中）；`notify` 为 true 时触发 `on_select`。
  void select_key(std::string_view key, bool notify = false);
  /// 当前选中行的 key（无选中为空串）。
  [[nodiscard]] auto selected_key() const -> std::string_view { return selected_key_; }
  /// 当前选中行号（无选中为 `kNoSelection`）。
  [[nodiscard]] auto selected_index() const noexcept -> std::size_t;

  /// 整行命中矩形（含缩进区；未布局/不存在为空矩形）。
  [[nodiscard]] auto node_rect(std::string_view key) const -> math::Rect;
  /// 第 `index` 行的命中矩形（越界为空矩形）。
  [[nodiscard]] auto row_rect(std::size_t index) const -> math::Rect;
  /// 某行的内容缩进（depth × 16px；不存在为 0）——深度断言与测试用。
  [[nodiscard]] auto node_indent(std::string_view key) const -> float;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

  /// 目录行展开状态切换（点击目录行 / Enter / ← →）：调用方据此加载子节点并
  /// 重新 `sync_nodes`。`expanded` 是**目标状态**。
  std::function<void(std::string_view key, bool expanded)> on_toggle{};
  /// 文件行激活（点击/Enter/`invoke(click)`）：参数为行 key。
  std::function<void(std::string_view key)> on_select{};

 private:
  /// 行命中（含展开指示区的整行判定）；返回行号。
  [[nodiscard]] auto row_index_at(math::Point point) const -> std::optional<std::size_t>;
  /// 对某行执行「激活」语义（文件=选中、目录=toggle）——点击/Enter/invoke 共用。
  void activate_row(std::size_t index);
  /// 触发 toggle（目标状态 = 当前行数据的反值）。
  void toggle_row(std::size_t index);

  struct Row {
    TreeNode data{};
  };
  std::vector<Row> rows_{};
  std::string selected_key_{};
  int hover_index_{-1};
};

}  // namespace st::ui
