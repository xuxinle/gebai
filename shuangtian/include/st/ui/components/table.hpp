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
#include <utility>
#include <vector>

#include "st/ui/element.hpp"
#include "st/ui/style.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 列定义（`width <= 0` 表示自适应：按容器剩余宽度均分，并夹取到 `Table::kMinColumnWidth`）。
///
/// 窄容器下的压缩策略（`min_width` / `shrink`）只在"固定列加起来装不下"时生效：
/// 按 `shrink` **从小到大**依次让位，每列最多让到 `min_width`。
struct TableColumn {
  std::string name{};
  float width{0.0f};
  TextAlign align{TextAlign::Start};
  /// 固定列可被压到的下限（px）；`0` = 不参与压缩（保持 `width`）。
  ///
  /// 为何需要它：原先固定列**只拉伸不压缩**，于是窄容器里宽度全被固定列吃光，
  /// 主列（自适应那一列）被压到最小宽——实测 Git 日志表在 380px 宽时，
  /// 提交信息只剩十几字、日期被截成 `2026-11-06 ...`，而那些"背景信息列"
  /// 一点也没让位。
  float min_width{0.0f};
  /// 压缩优先级：**数值越小越先被压缩**（同值按列序），`0` = 最后才压。
  ///
  /// 为何不写成"等比缩"：等比缩会把主列（提交信息）与背景列（作者/日期）
  /// 一起削，而用户真正要看的是主列。显式优先级让"谁该让"成为调用方的决定，
  /// 不必在组件里猜。
  int shrink{0};
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
  static constexpr float kSelectedAccentWidth{3.0f};///< 选中行左缘主色条宽（px）

  /// 单元格**色调**（主/次信息分层）。
  ///
  /// 为何需要它（2026-11-05，做 Git 日志表时）：一列一色的表格读不了——
  /// 提交信息该是主色（用户在看的东西）、作者与日期该是弱化色（背景信息）。
  /// 全用同一色就只能靠列位置区分，而“哪一部分是重点”正是列表要表达的。
  /// 设为 `inherit` 时跟表格自身的文字色（旧行为，零影响）。
  enum class CellTone : std::uint8_t { Inherit, Normal, Muted, Accent, Danger };

  explicit Table(std::vector<TableColumn> columns = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Table"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Table; }

  /// 设定列的**默认色调**（每列一个；未设则 `Inherit`）。
  /// 与 `set_cell_tone` 的分工：这里定“这一列大体是什么角色”，那里做个别覆盖。
  void set_column_tones(std::vector<CellTone> tones);
  /// 设某个单元格的色调（个别覆盖列默认；越界忽略）。
  void set_cell_tone(std::size_t row, std::size_t column, CellTone tone);
  [[nodiscard]] auto cell_tone(std::size_t row, std::size_t column) const -> CellTone;

  /// 行首**泳道标记**（图形分支线）：每行一个列号（0 起），负值 = 不画。
  ///
  /// 为何进框架而不是应用自己画：泳道是**行属性**，它要和行高/滚动/斑马纹
  /// 同一套几何算——应用自己画就得到表格外面去覆一层，而覆层拿不到
  /// “第 n 行现在在屏幕上第几个像素”（那是 `row_rect` 的内部知识）。
  /// 取值语义与 git 泳道图一致：0 = 主干，n = 第 n 条分支线。
  void set_row_lanes(std::vector<int> lanes);
  /// 设在该列位置画泳道（负值保持默认 28px；0 会吃掉列宽，不要传）。
  ///
  /// 这个宽度是**真的从左边留出来**的：开了泳道的表格，各列的 `bounds_` 从
  /// `x + lane_inset()` 起算（见 `column_x`），否则第一个字会与泳道圆点叠在一起——
  /// 实测症状就是“提交信息的首字被竖线和圆点盖住”。
  void set_lane_column_width(float width);

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
  // —— 选中行（v0.1.5：协议可读写、语义树可断言） ——
  /// 选中行序号（`nullopt` = 无选中）。点击行不自动改选中——选中是**数据语义**，
  /// 由应用决定（与 `List::selected` 同一口径），避免"点击即选中"在多选/命令面板里错。
  void set_selected_row(std::optional<std::size_t> row);
  [[nodiscard]] auto selected_row() const noexcept -> std::optional<std::size_t> {
    return selected_row_;
  }
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
  /// 各列**实际**宽度（`arrange` 后有效；逗号分隔，供控制通道/测试断言）。
  [[nodiscard]] auto column_widths_text() const -> std::string;
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
  /// 只读属性（`column_widths` / `lane_inset`）：它们是**布局结果**，写回去没有意义。
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  // —— 视口契约（与 `Tree`/`List` 的列表类通用属性面对齐） ——
  //
  // 为何补：`Table` 之前只有 `rows`/`scroll_offset`，**没有** `first_visible`
  // 与 `visible_rows`——而“用户现在看得到哪一块”是 AI 驱动界面时最常问的判断
  //（见 DESIGN 里列表类通用视口契约那段）。“不可断言的效果 = 不可复用”。
  [[nodiscard]] auto visible_row_count() const noexcept -> std::size_t;
  /// 上一帧实际绘制的首/末可见行（半开；从未绘制过时为空区间）。
  [[nodiscard]] auto visible_row_range_now() const noexcept
      -> std::pair<std::size_t, std::size_t> {
    return {last_visible_first_, last_visible_last_};
  }

 private:
  /// 按可用宽度重算列宽与总宽（自适应列均分剩余宽度）。
  void recompute_columns(float available);
  [[nodiscard]] auto column_x(std::size_t column) const noexcept -> float;
  /// 指针所在行（不在数据行内返回 kNoRow）。
  [[nodiscard]] auto row_at(math::Point point) const noexcept -> std::size_t;
  /// 泳道带占掉的左缘宽度（没开泳道时为 0）。
  [[nodiscard]] auto lane_inset() const noexcept -> float;

  static constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

  std::vector<TableColumn> columns_{};
  std::vector<std::vector<std::string>> rows_{};
  std::vector<float> column_widths_{};
  /// 每列的默认色调 / 行×列的个别覆盖（与 `rows_` 同形，缺省 `Inherit`）。
  std::vector<CellTone> column_tones_{};
  std::vector<std::vector<CellTone>> cell_tones_{};
  /// 每行的泳道列号（`-1` = 不画）。
  std::vector<int> row_lanes_{};
  float lane_column_width_{28.0f};
  std::function<void(std::size_t)> on_row_click_{};
  float total_width_{0.0f};
  float row_height_{kDefaultRowHeight};
  /// 调用方显式设过 `set_row_height`：字号覆盖不再改写行高（见 apply_theme）。
  bool explicit_row_height_{false};
  float scroll_offset_{0.0f};
  std::size_t hovered_row_{kNoRow};
  std::optional<std::size_t> selected_row_{};
  bool zebra_{false};

  /// 上一帧实际绘制的可见行区间（半开）——与 `Tree::last_visible_first_` 同一手法。
  /// `mutable`：`paint_content` 是 const（「绘制不改元素状态」是框架硬约束）。
  mutable std::size_t last_visible_first_{0};
  mutable std::size_t last_visible_last_{0};
};

}  // namespace st::ui

// ── 自注册声明（`st/ui/component_id.hpp`）───────────────────────
// 只声明导入函数；真正的登记在实现文件里（静态初始化在静态库中不可靠，
// 见 component_id.hpp 文件头的实测记录）。
//
// ⚠ include 本头 + 调用 `st::ui::import_components<类名>()` 才真正可用；
//   `st/ui/components/all.hpp` 一次做完全部导入。
#include "st/ui/component_id.hpp"

ST_UI_COMPONENT(::st::ui::Table, "Table")
