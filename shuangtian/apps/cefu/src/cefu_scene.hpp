#pragma once

/// @file cefu_scene.hpp
/// @brief 把知识图谱变成 **three.js 脚本**（交给 `ui::ThreeView` 渲染）。
///
/// ## 为什么生成脚本，而不是自己画
///
/// 一开始走的是"自己算投影 + 在画布上画圆"的路（`cefu_graph` 里那套投影/拾取还在，
/// 见它的头文件）。做到一半发现框架已经有 `ui::ThreeView`：真三维后端
/// （D3D11/软件光栅器）、深度缓冲、PBR 光照、轨道相机、拾取高亮**全都是现成的**。
///
/// 而手写投影的代价是实打实的：
///
/// | 能力 | 手写投影 | ThreeView |
/// |---|---|---|
/// | 球体互相遮挡 | 只能按深度排序列画，重叠处穿帮 | 深度缓冲，天然正确 |
/// | 明暗层次 | 需要自己造假径向渐变 | 真光照 |
/// | 连线被遮挡 | 同样只能靠画家算法近似 | 正确 |
/// | 拾取 | 自己算屏幕距离 | 框架已有 `ThreeHit` |
///
/// 所以分工改成：**C++ 算布局（力导向，three.js 不提供）→ 生成本文件的脚本 → 视口渲染**。
/// `cefu_graph` 里的投影/拾取仍保留：无头截图核验与单测要用它（不经视口也能验算）。
///
/// ## 为什么要"生成脚本"而不是"生成数据再让脚本读"
///
/// three.js 跑在脚本宿主里（QuickJS），没有文件系统访问。而把几百个节点的坐标
/// 经桥逐条传进 JS 会产生大量跨语言往返。**一次性生成整份脚本**最省：
/// 一个字符串进视口，`load_script` 求值一次，下一帧就是新图。
///
/// 代价是脚本体积随节点数线性增长——但这正是渲染器要的形态（几何内联）。
/// 300 节点 × 10 条连线 ≈ 300KB 脚本，求值一次在毫秒级。

#include <string>
#include <vector>

#include "cefu_graph.hpp"

namespace cefu::scene {

/// 生成选项（颜色与尺寸都在这里，不散落在拼字符串的地方）。
struct Options {
  /// 节点半径（three.js 世界单位）。分类/人节点会按 `Node::weight` 放大。
  double node_radius{0.11};
  /// 连线半径（圆柱体宽度）。用圆柱而不是线段：线在真三维里没有体积，
  /// 远了会细到看不见；圆柱有厚度，缩放时视觉一致。
  double edge_radius{0.012};
  /// 背景色（十六进制，含 `0x` 前缀）。
  std::string background{"0x0d1117"};
  /// 是否画文字标签（用 Sprite + Canvas 纹理——three.js 的文本方案）。
  ///
  /// 默认关：几百个 Sprite 会显著拖慢软件光栅器，而标签在密集图里本来就会糊成一片。
  /// 需要时开，配合"只给近处节点加标签"（`label_limit`）。
  bool labels{false};
  /// 开标签时最多画几个（按节点权重降序取）。
  std::size_t label_limit{24};
};

/// 生成整份 three.js 脚本。
///
/// **确定性**：同一张图必得同一份脚本（布局已确定，拼串也无随机）——
/// 否则热重载会反复触发无意义的重建。
[[nodiscard]] auto build_script(const graph::Graph& graph, const Options& options = {}) -> std::string;

/// 生成"空图"脚本（册府还没有词条时用：显示一句提示而不是一片黑）。
[[nodiscard]] auto build_empty_script(const Options& options = {}) -> std::string;

/// 单节点颜色（按类型与置信度）。
///
/// 分开成函数是为了让单测能钉住"verified 与 inferred 必须可区分"——
/// 这正是图谱的语义价值：一眼看出哪些知识还没被确认。
[[nodiscard]] auto node_color(graph::NodeKind kind, const std::string& confidence) -> std::string;

}  // namespace cefu::scene
