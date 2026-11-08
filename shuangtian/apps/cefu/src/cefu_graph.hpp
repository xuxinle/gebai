#pragma once

/// @file cefu_graph.hpp
/// @brief 册府知识图谱的**领域适配层**：把知识词条变成一张通用关系图。
///
/// ## 这一层现在只剩什么（框架化之后）
///
/// 布局、相机、投影、拾取、三维渲染都已经在框架里
/// （`st/ui/graph_layout.hpp` 的纯计算 + `st/ui/components/graph_view.hpp` 的组件）。
/// 本文件只负责**只有册府知道的三件事**：
///
/// | 领域知识 | 落在哪里 |
/// |---|---|
/// | 有哪些实体（词条 / 分类 / 负责人 / 作用域） | `build()` 建节点 |
/// | 谁和谁有关系（从属 / 显式关联 / 取代 / 共事 / 共享硬事实） | `build()` 建边 |
/// | 长什么样（配色、标签、聚簇优先级） | `node_color()` / `edge_color()` / `label_of()` |
///
/// 这个划分不是形式主义：通用层不认识"分类"是什么，也不需要认识——它只认
/// `Node::cluster_rank`（谁该当簇心）与 `Node::argb`（什么颜色）。于是同一个
/// `GraphView` 能画知识图谱、依赖图、调用图、组织关系……
///
/// ## 为什么图要预先算一次布局并缓存
///
/// 力导向是迭代解算（O(迭代 × 节点²)）。300 节点 × 300 迭代 ≈ 2700 万次距离计算，
/// 每帧算一次会直接卡死界面。所以：**布局算一次 → 坐标存在 `graph::Graph` 里 →
/// 每帧只做投影与绘制**。词条集变化时才重算（判据是 `graph.revision` 变没变）。
///
/// ## 共享硬事实这条边为什么最有价值也最危险
///
/// 它能在没人显式声明关联的情况下发现「这三条知识其实在说同一台机器」。
/// 危险在于噪声：任何两条都共享某些平凡硬事实（比如都写了 `负责人=张三`）。
/// 所以只取**足够具体**的值（含数字或路径分隔符，且按码点算长度 ≥ 2），
/// 且至少两条共享才建边，且**只连成链**——全连接会 O(n²) 边爆炸。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cefu_index.hpp"
#include "st/ui/graph_layout.hpp"

namespace cefu::graph {

/// 节点类型常量（**只是字符串**：通用层不解释它们，册府用它们做计数与配色）。
inline constexpr std::string_view kKindCategory{"category"};
inline constexpr std::string_view kKindPerson{"person"};
inline constexpr std::string_view kKindScope{"scope"};
inline constexpr std::string_view kKindEntry{"entry"};

/// 边的语义常量（同上：通用层只看 `Edge::clustering` 与颜色）。
inline constexpr std::string_view kEdgeBelongsTo{"belongs_to"};
inline constexpr std::string_view kEdgeLinks{"links"};
inline constexpr std::string_view kEdgeSupersedes{"supersedes"};
inline constexpr std::string_view kEdgeOwnedBy{"owned_by"};
inline constexpr std::string_view kEdgeSharedFact{"shared_fact"};
inline constexpr std::string_view kEdgeScopedTo{"scoped_to"};

/// 构建选项（阈值都在这里，不散落在实现里）。
struct BuildOptions {
  /// 标签最大字数（超出截断加省略号）——太长会把画面糊满。
  std::size_t max_label_chars{14};
  /// 共享硬事实建边的最小词条数：只有 1 条时"共享"没有意义。
  std::size_t min_shared_fact_degree{2};
  /// 是否把分类/人/作用域也建成节点（false = 只有词条，用于"最干净"档）。
  bool include_meta_nodes{true};
};

/// 从词条集构建图（**纯派生**：不写库、不改索引）。
///
/// 坐标全为零——布局是下一步（`cefu::layout`）。分成两步的理由：
/// ① 布局很贵（见文件头），只该在数据真变时做；② 建图本身要能单测，
/// 混进迭代解算会让断言变慢且难定位。
[[nodiscard]] auto build(const std::vector<Entry>& entries, const BuildOptions& options = {})
    -> st::ui::graph::Graph;

/// 解算布局（对 `st::ui::graph::layout` 的领域缺省包装）。
///
/// 为何要这一层而不是让调用方直接调框架的 `layout`：册府的规模与观感参数
/// （迭代数、簇拉力）是**调出来的**，散在各调用点必然漂移。收敛到一处，
/// 将来改观感只改这里。
void layout(st::ui::graph::Graph& graph);

/// 节点主色（按类型 + 置信度）。
///
/// 单独成函数是为了让单测钉住「verified 与 inferred 必须可区分」——
/// 这正是图谱的语义价值：一眼看出哪些知识还没被确认。
[[nodiscard]] auto node_color(std::string_view kind, std::string_view confidence) -> std::uint32_t;

/// 边色（按语义）。颜色都亮但可区分：连线是背景结构，仍要能分出类型。
[[nodiscard]] auto edge_color(std::string_view kind) -> std::uint32_t;

/// 作用域类型的存储值 → 人话标签（`chat` → 「本群」）。
///
/// 为何需要：schema 里的 kind 是**存储值**，直接显示会得到 `chatoc_f71d4c…` 这种
/// 看不懂的串。图谱给人看，标签就得是人话。
[[nodiscard]] auto scope_label(std::string_view scope_kind) -> std::string;

}  // namespace cefu::graph
