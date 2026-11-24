#pragma once

/// @file mermaid.hpp
/// @brief Mermaid 图文本的**解析与几何布局**（纯计算层，零 `st::ui` 依赖）。
///
/// ## 为什么单独一层（而不是全塞进组件）
///
/// 与 `graph_layout` / `timeline` 同一分工：**能算的与能画的必须分开**。
///
/// | 层 | 工作 | 能否单测 |
/// |---|---|---|
/// | 本文件 | 文本 → 文档模型 → 几何（节点框、边路径、消息行、扇形角度） | ✅ 纯函数，可穷举 |
/// | `components/mermaid_view.hpp` | 把几何画成像素、接属性面与交互 | 只能截图核验 |
///
/// 布局算法出错的表现是「图看着怪」（节点重叠、边穿框、标签压在线上），
/// 靠截图一条条看是查不动的；而在这一层它可以被断言钉死。
///
/// ## 三类可画的图（v1 的可执行子集）
///
/// | 语法 | 画成 |
/// |---|---|
/// | `flowchart` / `graph` + `TD/TB/LR/RL/BT` | 分层框图（矩形/圆角/菱形/圆）+ 连线 + 边标签 |
/// | `sequenceDiagram` | 参与者头 + 生命线 + 消息箭头（实线/虚线） |
/// | `pie` | 饼图 + 图例 + 百分比 |
///
/// 其余图类型（class/state/er/gantt/journey/…）与语法的其余部分（`subgraph`、
/// `loop/alt` 块、`&` 并列节点、`style` 之外的指令）**不猜**：解析器把它们记进
/// `Document::diagnostics` 并如实上报，能画的照画，不静默丢内容、不抛异常——
/// 与 `st::md::parse` 同一宽容口径（大模型流式输出本来就是残缺语法）。
///
/// ## 确定性是硬要求
///
/// 同一份文本必得同一份几何：布局不含随机、不依赖容器迭代外的任何状态，
/// 排序一律按**声明顺序**（稳定）。否则截图回归会飘，缺陷也无法复现。
///
/// ## 度量为什么由调用方注入
///
/// 本层不认识字体（`ui` 层对 `text` 层的依赖倒置同理）。节点要多大取决于
/// 标签排出来多宽，所以布局收一个 `MeasureText`；测试可注入定宽桩，
/// 应用注入真实 `TextPort`。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"

namespace st::md::mermaid {

// ── 文档模型 ──────────────────────────────────────────────────────────────────

/// 图类型（由首行关键字决定）。
enum class DiagramKind : std::uint8_t {
  Unknown,    ///< 认不出的语法（画占位，不报错）
  Flowchart,  ///< `flowchart` / `graph`
  Sequence,   ///< `sequenceDiagram`
  Pie,        ///< `pie`
};

/// 稳定短名（日志 / 控制通道 / 测试断言用）。
[[nodiscard]] auto to_string(DiagramKind kind) noexcept -> std::string_view;

/// 流向。
enum class Direction : std::uint8_t {
  TopDown,    ///< TD / TB
  LeftRight,  ///< LR
  RightLeft,  ///< RL
  BottomUp,   ///< BT
};

[[nodiscard]] auto to_string(Direction direction) noexcept -> std::string_view;

/// 节点形状（由包裹符号决定）。
enum class NodeShape : std::uint8_t {
  Rect,     ///< `A[文字]`
  Rounded,  ///< `A(文字)`
  Stadium,  ///< `A([文字])`
  Diamond,  ///< `A{文字}`
  Circle,   ///< `A((文字))`
};

[[nodiscard]] auto to_string(NodeShape shape) noexcept -> std::string_view;

/// 连线线型。
enum class EdgeStyle : std::uint8_t {
  Solid,   ///< `-->` / `---`
  Dashed,  ///< `-.->` / `-.-`
  Thick,   ///< `==>` / `===`
};

[[nodiscard]] auto to_string(EdgeStyle style) noexcept -> std::string_view;

/// 流程图节点。`id` 是语法里的引用键（`A`），`label` 是显示文字。
struct FlowNode {
  std::string id{};
  std::string label{};
  NodeShape shape{NodeShape::Rect};
};

/// 流程图连线（`from`/`to` 为 `Flowchart::nodes` 下标）。
struct FlowEdge {
  std::size_t from{0};
  std::size_t to{0};
  std::string label{};
  EdgeStyle style{EdgeStyle::Solid};
  bool arrow{true};
  /// 自环（`A --> A`）：单独画一个小回环，不能按"中心到中心"裁剪（会塌成一点）。
  bool self{false};
};

struct Flowchart {
  Direction direction{Direction::TopDown};
  std::vector<FlowNode> nodes{};
  std::vector<FlowEdge> edges{};
};

/// 时序图参与者。
struct SequenceParticipant {
  std::string id{};
  std::string label{};
  /// `actor` 声明（画人形而不是方框；v1 两者画法相同，字段留着供后续区分）。
  bool actor{false};
};

/// 时序图消息（`from`/`to` 为 `Sequence::participants` 下标）。
struct SequenceMessage {
  std::size_t from{0};
  std::size_t to{0};
  std::string text{};
  bool dashed{false};
  bool arrow{true};
};

struct Sequence {
  std::vector<SequenceParticipant> participants{};
  std::vector<SequenceMessage> messages{};
};

/// 饼图一片。
struct PieSlice {
  std::string label{};
  double value{0.0};
};

struct Pie {
  std::string title{};
  std::vector<PieSlice> slices{};
};

/// 一次解析的结果。
///
/// 三类图各自一段模型（而不是联合体/`variant`）：字段都小，用得上的那一段就是全部内容；
/// 换 `variant` 只会让每个使用点都多一层 `get_if`。
struct Document {
  DiagramKind kind{DiagramKind::Unknown};
  Flowchart flowchart{};
  Sequence sequence{};
  Pie pie{};
  /// 解析期的如实上报：不支持的语句、认不出的形状、坏掉的数值行……
  /// **不静默**是本层最重要的行为（否则用户看到的是"少了几块"，而没有任何线索）。
  std::vector<std::string> diagnostics{};
  /// 是否解析出了可画的内容（空文本 / 认不出的类型 ⇒ false）。
  bool ok{false};
};

/// 解析 mermaid 文本。不抛异常、无全局状态：同一输入必得同一结果。
[[nodiscard]] auto parse(std::string_view text) -> Document;

// ── 几何布局 ──────────────────────────────────────────────────────────────────

/// 文本宽度度量（纯计算层不认识字体，由调用方注入）。
using MeasureText = std::function<float(std::string_view text, float font_size)>;

/// 布局参数。**所有尺寸都从 `font_size` 派生或按倍数放**：直接用像素常量会在
/// 放大查看时比例失衡（字变大了、内边距没变），而按倍数放必然同比例。
struct LayoutOptions {
  float font_size{14.0f};
  /// 行高倍数（多行标签、图例）。
  float line_gap{1.35f};
  /// 节点内边距。
  float pad_x{14.0f};
  float pad_y{9.0f};
  /// 节点最小宽（窄标签不该缩成一小块）。
  float min_node_width{72.0f};
  /// 相邻层（rank）之间的间距。
  float rank_gap{64.0f};
  /// 同层相邻节点之间的间距。
  float sibling_gap{28.0f};
  /// 边标签与线之间的空隙。
  float edge_label_gap{5.0f};
};

/// 一个节点的最终框（**内容坐标**：原点在图左上角，不含视图的缩放与平移）。
struct NodeBox {
  math::Rect rect{};
  /// **标签可排区域**（同一坐标系；已按形状算好内接范围）。
  ///
  /// 为何由布局层给而不是绘制层自己缩：不同形状的可用区域是**几何结论**
  /// （菱形要满足 `tw/W + th/H <= 1`、圆要内接正方形），绘制层再算一遗就是
  /// 两套口径——实测症状是「菱形里的字横向溢出到尖角外」，而属性面全对。
  math::Rect text_rect{};
  /// 换行后的标签行（`<br/>` 已切开；绘制方按行画，不再自己拆）。
  std::vector<std::string> lines{};
};

/// 一条连线的折线（首点在起点形状边界上、末点在终点形状边界上）。
struct EdgePath {
  std::size_t edge{0};
  std::vector<math::Point> points{};
  math::Rect label{};
  bool has_label{false};
  std::vector<std::string> label_lines{};
  EdgeStyle style{EdgeStyle::Solid};
  /// 箭头方向（起点→终点）。`self` 回环时为 false（回环用弧线自身表达方向）。
  bool arrow{true};
  bool self{false};
};

struct FlowLayout {
  std::vector<NodeBox> nodes{};
  std::vector<EdgePath> edges{};
  /// 整张图的包围尺寸（已含边标签与留白）。
  math::Size size{};
};

/// 分层布局流程图。
///
/// 步骤（每一步都是可断言的纯函数行为）：
/// ① 去掉回边（DFS 着色）得到 DAG；② Kahn 拓扑序求 `rank = 最长路径`；
/// ③ 层内按**声明顺序**排列；④ 按流向映射到屏幕轴；⑤ 连线从中心连线
/// **夹取到形状边界**（矩形/圆用几何公式，菱形用 |dx|/a + |dy|/b = 1）。
[[nodiscard]] auto layout_flowchart(const Flowchart& chart, const LayoutOptions& options,
                                    const MeasureText& measure) -> FlowLayout;

/// 参与者头（方框 + 标签；生命线不在框里）。
struct ParticipantBox {
  math::Rect rect{};
  std::vector<std::string> lines{};
};

/// 一条消息的几何。
struct MessageGeometry {
  float y{0.0f};
  float x_from{0.0f};
  float x_to{0.0f};
  math::Rect label{};
  std::vector<std::string> lines{};
  bool dashed{false};
  bool arrow{true};
  bool self{false};
  /// 消息序号（绘制方按需显示；0 起）。
  std::size_t index{0};
};

struct SequenceLayout {
  std::vector<ParticipantBox> participants{};
  /// 每个参与者的生命线 x（与 `participants` 同序）。
  std::vector<float> lifelines{};
  float lifeline_top{0.0f};
  float lifeline_bottom{0.0f};
  std::vector<MessageGeometry> messages{};
  math::Size size{};
};

[[nodiscard]] auto layout_sequence(const Sequence& sequence, const LayoutOptions& options,
                                   const MeasureText& measure) -> SequenceLayout;

/// 一片扇形的几何。角度单位是**弧度**，0 = 12 点方向，**顺时针为正**
/// （与钟表直觉一致；数学零点在 3 点，直接拿来画会得到相反的图）。
struct PieSliceGeometry {
  double start_angle{0.0};
  double sweep{0.0};
  double percent{0.0};
  /// 图例项矩形（含色块）。
  math::Rect legend{};
  std::vector<std::string> lines{};
};

struct PieLayout {
  math::Point center{};
  float radius{0.0f};
  std::vector<PieSliceGeometry> slices{};
  math::Rect title_box{};
  std::vector<std::string> title_lines{};
  math::Size size{};
};

/// 饼图布局。数值全为 0 或为负时画**空环**（不报错、不除零）。
[[nodiscard]] auto layout_pie(const Pie& pie, const LayoutOptions& options,
                              const MeasureText& measure) -> PieLayout;

// ── 共用小工具 ────────────────────────────────────────────────────────────────

/// 按 `<br/>`（及 `\n`）切行——mermaid 的标签换行写法，与绘制方的分行走同一个入口。
[[nodiscard]] auto split_label_lines(std::string_view label) -> std::vector<std::string>;

}  // namespace st::md::mermaid
