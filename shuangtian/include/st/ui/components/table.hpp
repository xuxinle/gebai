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
  static constexpr float kScrollStepY{48.0f};       ///< 纵向滚轮每格距离（px）
  static constexpr float kScrollBarHeight{3.0f};    ///< 滚动指示条宽（px）
  static constexpr float kSelectedAccentWidth{3.0f};///< 选中行左缘主色条宽（px）

  /// 泳道色（第 `lane_index` 道的连线与圆点用色）——**自动生成，不设上限**。
  ///
  /// ## 为何按道号生成而不是用一张固定色表
  ///
  /// 分支图是“跟着一条线往下看”的读图方式：同一道在不同行必须是同一色，
  /// 不同道之间必须能分开。固定色表（如原来循环 primary/accent/success 三色）
  /// 意味着并行分支一旦超过表的长度，“第 0 道”与“第 3 道”就同色——而那往往
  /// 正是两个**不同**分支，观感上等于把两条不相干的线当成同一条。
  ///
  /// 这里用色相环上的**黄金角**（137.508°）跳步：该增量无限不循环，任意前 N 道
  /// 都不会回环重合，也不需要预知 N 或维护色表。饱和度/亮度取主题 `primary`
  /// 自身 HSL 为基准（只换色相），于是暗色主题下够亮、浅色主题下不刺眼。
  ///
  /// 公开为静态函数的理由：它是一条**纯函数、可复算的判断依据**——测试可以直接
  /// 断言“任意两道色距够远”，不必依赖截图像素采样（像素采样到的常是抗锯齿
  /// 边沿色，量出来的距离远小于真实值）。
  [[nodiscard]] static auto lane_color(std::size_t lane_index, const Palette& colors)
      -> math::Color;

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
  /// 设在该列位置画泳道（负值保持默认 28px；0 会吃掉列宽，不要传）。
  ///
  /// 这个宽度是**真的从左边留出来**的：开了泳道的表格，各列的 `bounds_` 从
  /// `x + lane_inset()` 起算（见 `column_x`），否则第一个字会与泳道圆点叠在一起——
  /// 实测症状就是“提交信息的首字被竖线和圆点盖住”。
  void set_lane_column_width(float width);

  // ── 行首连线图：**节点-边平面** ──────────────────────────────
  //
  // 不传字符、不传列号，只传“哪个节点在哪一道、哪两个节点之间有线”。
  //
  // ## 为何不提供字符网格接口
  //
  // 行首连线图只有一个通道：`set_graph`（节点 + 边）。
// 不提供字符网格接口：`git log --graph` 的 ASCII 图区是渲染产物，没有稳定契约
//（列宽、斜线落在奇/偶格、为一个合并插几个连接行都是排版决定）——拿它当数据模型，
// 排版一变就错；它还逼着列表为“纯连线行”留空行。也不提供“每行一个列号”的简化接口，
// 因为它表达不了跨道连线（合并/分叉的斜边）。
  //
  // 正解是把**拓扑**直接交进来：节点是行，边是显式的（行，道）对。

  /// 一个提交节点：第 `row` 行在第 `lane` 道。
  struct GraphNodeSpec {
    std::size_t row{0};
    std::size_t lane{0};
  };
  /// 一条边：从（`from_row`, `from_lane`）到（`to_row`, `to_lane`）。
  ///
  /// 同道边画竖线，跳道边画曲线——这是“换道”的唯一表达方式。
  struct GraphEdgeSpec {
    std::size_t from_row{0};
    std::size_t from_lane{0};
    std::size_t to_row{0};
    std::size_t to_lane{0};
  };
  /// 设行首连线图的**节点与边**（坐标全用“行号 / 道号”，不涉像素）。
  ///
  /// 不变式（绘制端假定，调用方负责）：① 边从**小行号指向大行号**（子 → 父）；
  /// ② 两个端点都在 `nodes` 里（越界的边会被裁掉，表现为“线断一截”）。
  void set_graph(std::vector<GraphNodeSpec> nodes, std::vector<GraphEdgeSpec> edges);
  void clear_graph();
  [[nodiscard]] auto graph_nodes() const noexcept -> const std::vector<GraphNodeSpec>& {
    return graph_nodes_;
  }
  [[nodiscard]] auto graph_edges() const noexcept -> const std::vector<GraphEdgeSpec>& {
    return graph_edges_;
  }

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

  /// **逐行高度倍率**（空 = 全部用 `row_height_`）。
  ///
  /// ## 为何需要它（不只是一个“美化开关”）
  ///
  /// 提交图里的**纯连线行**（`|\`、`|/`）只为画斜边而存在，不承载任何文字。
  /// 给它一个完整行高，就会把两个相邻提交拉开 **数倍**距离（实测：合并处
  /// `merge side` 到 `side 1` 之间被 3 个连线行撑出 4 倍行距），整张表看起来
  /// “稀稀拉拉”。倍率 0.5 一压回去，行距立刻回到正常。
  ///
  /// 用**倍率**而不是绝对值：行高会被字号/主题改（`apply_theme` 里跟随字号），
  /// 存绝对值会在换字号后与正常行不同步。
  void set_row_scales(std::vector<float> scales);
  [[nodiscard]] auto row_scales() const noexcept -> const std::vector<float>& {
    return row_scales_;
  }
  /// 第 `row` 行的高度（px；越界回 `row_height_`）。
  [[nodiscard]] auto row_height_at(std::size_t row) const noexcept -> float;
  /// 行数据总高（含逐行高度；`row_offsets_` 的末值）。
  [[nodiscard]] auto content_height() const noexcept -> float;
  /// 行偏移前缀和（第 i 行的上边距；长度 = 行数 + 1；供只读几何用）。
  ///
  /// 为何暴露：`visible_row_range` 是自由函数（要遍历全量行的可见区间），
  /// 而行高逐行不同时“除行高求下标”不成立——必须用前缀和二分。
  [[nodiscard]] auto row_offsets() const noexcept -> const std::vector<float>& {
    ensure_row_offsets();
    return row_offsets_;
  }
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

  // —— 纵向滚动 ——
  //
  // ## 为何 Table 必须自己管纵向
  //
  // 旧实现在 `on_event` 的 Wheel 分支里写着「纵向由外层容器管」——但**这个约定
  // 从来没被兑现过**：全仓零处把 Table 放进 `ScrollView`（`grep` 实测），
  // 而 gbcode 的日志表就是直接挂在 `Panel` 下。后果：200 行提交只看得见 6 行，
  // 而 `max_scroll`（当时只有横向）恒为 0——**完全滞不动**，且没有任何报错。
  //
  // 现在两种摆法都成立：
  // - 直接放在容器里：`max_scroll_y() > 0`，滚轮纵向滚（**Shift 保持横向**，
  //   与“Shift = 另一个轴”的通行约定一致）；
  // - 放进 `ScrollView`：`bounds_.height` 是**内容全高**（> 行数据高），
  //   `max_scroll_y()` 自然为 0、滚轮让给外层（`return false` 冒泡）——
  //   这正是旧行为，不破坏既有用法。
  [[nodiscard]] auto scroll_offset_y() const noexcept -> float { return scroll_offset_y_; }
  [[nodiscard]] auto max_scroll_y() const noexcept -> float;
  void set_scroll_offset_y(float offset);
  void scroll_by_y(float delta) { set_scroll_offset_y(scroll_offset_y_ + delta); }

  /// 显隐**表头**（默认显示）。关掉后表头高度归 0，行区直接从顶边开始。
  ///
  /// 存在的理由：有些表格的列含义**自明**（提交日志的“提交信息/作者/日期/哈希”），
  /// 而表头要吃掉 `kHeaderHeight` 那么高的黄金视野；它也不随纵向滚动，永远是
  /// 一块不动的色带。这类表格的列宽本就固定，不需要表头让用户自己对齐。
  ///
  /// ⚠ 关掉后 `header_rect()` / `header_cell_rect()` 返回**空矩形**（而不是
  /// “表头位置但高 0”）：自动化判“表头在不在”应当看它是否 `is_empty()`，
  /// 否则会拿到一个“存在但看不见”的矩形，判据失效。
  void set_show_header(bool show);
  [[nodiscard]] auto show_header() const noexcept -> bool { return show_header_; }
  /// 表头+行区共用的**头部占高**（关掉表头时为 0）——所有几何计算走它，
  /// 不直接写 `kHeaderHeight`，否则关表头后行会错位一整条。
  [[nodiscard]] auto header_height() const noexcept -> float {
    return show_header_ ? kHeaderHeight : 0.0f;
  }
  /// 表头之下、可见区域的行数（扣表头与横向滚动条；不受纵向偏移影响）。
  [[nodiscard]] auto viewport_row_count() const noexcept -> std::size_t;

  // —— 布局几何（测试/控制通道用；arrange 后有效）——
  [[nodiscard]] auto header_rect() const noexcept -> math::Rect;
  [[nodiscard]] auto row_rect(std::size_t row) const noexcept -> math::Rect;
  /// 实际绘制矩形（已含横向与**纵向**偏移）。
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
  std::function<void(std::size_t)> on_row_click_{};
  float total_width_{0.0f};
  float row_height_{kDefaultRowHeight};

  /// 行偏移前缀和（惰性重建；见 `row_offsets()`）。
  void ensure_row_offsets() const;
  /// 调用方显式设过 `set_row_height`：字号覆盖不再改写行高（见 apply_theme）。
  bool explicit_row_height_{false};
  /// 节点-边通道的绘制（见 `set_graph`）：只读本对象的行几何与本帧的裁剪参数。
  void paint_node_edge_graph(raster::Surface& canvas, const Palette& colors,
                            std::size_t first_row, std::size_t last_row, float clip_left,
                            float clip_right) const;
  /// 每行的**高度倍率**（空 = 全部 1.0）。见 `set_row_scales`。
  std::vector<float> row_scales_{};
  /// `row_scales_` 的**前缀和**前缀（第 i 行的上边距；长度 = 行数 + 1）。
  ///
  /// 为何缓存：行矩形/命中/可见区间/滚动量四处都要“第 n 行的 y”，每处各自累加
  /// 就会在数据变化时错位（且是 O(n²)）。一次算好，四处共用。
  mutable std::vector<float> row_offsets_{};
  /// `row_offsets_` 是否与当前 `rows_`/`row_scales_`/`row_height_` 一致。
  mutable bool row_offsets_valid_{false};
  float scroll_offset_{0.0f};
  /// 纵向滚动偏移（px；`0` = 第一行贴着表头）。与 `row_rect` 同一坐标系。
  float scroll_offset_y_{0.0f};
  std::size_t hovered_row_{kNoRow};
  std::optional<std::size_t> selected_row_{};
  bool zebra_{false};
 /// 表头是否绘制（关掉后 `header_height()` 返回 0，行区顶到边）。
 bool show_header_{true};
   /// 行首连线图：**节点-边**（`set_graph`）。空 = 不画泳道图。
 std::vector<GraphNodeSpec> graph_nodes_{};
 std::vector<GraphEdgeSpec> graph_edges_{};
 /// 泳道带宽（“字符格”数）的缓存：`set_graph` 时按最大道号 × 2 算。
 ///
 /// 那个 2 来自早期字符网格的度量（`* ` / `| ` 两字符一道），与 `lane_inset()`
 /// 和绘制端的 `cell` 计算共用同一口径——改一处就要三处一起改。
 std::size_t lane_columns_{0};
 float lane_column_width_{28.0f};

  /// 上一帧实际绘制的可见行区间（半开）——与 `Tree::last_visible_first_` 同一手法。
  /// `mutable`：`paint_content` 是 const（「绘制不改元素状态」是框架硬约束）。
  mutable std::size_t last_visible_first_{0};
  mutable std::size_t last_visible_last_{0};
};

}  // namespace st::ui

// ── 自注册声明（`st/ui/component_registry.hpp`）───────────────────────
// 只声明导入函数；真正的登记在实现文件里（静态初始化在静态库中不可靠，
// 见 component_registry.hpp 文件头的实测记录）。
//
// ⚠ include 本头 + 调用 `st::ui::import_components<类名>()` 才真正可用；
//   `st/ui/components/all.hpp` 一次做完全部导入。
#include "st/ui/component_registry.hpp"

ST_UI_COMPONENT(::st::ui::Table, "Table")
