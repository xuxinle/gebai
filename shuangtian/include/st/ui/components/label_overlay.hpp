#pragma once

/// @file label_overlay.hpp
/// @brief 把三维场景里的物体**投影到屏幕**，画出文字标签与它们之间的连线。
///
/// ## 为什么要这个组件（三维里为什么没有文字）
///
/// three.js 跑在 QuickJS 里，而脚本宿主**没有 Canvas 2D**——
/// 常规的「Canvas 画字 → 转纹理 → Sprite」这条路根本走不通。
/// 用几何体拼汉字更不现实。
///
/// 所以分工变成：
/// - **三维层**（`ThreeView`）：只提供坐标与光照，不画任何可见内容；
/// - **本组件**：投影到屏幕，画**文字**和**文字之间的线**。
///
/// 两层叠在同一块区域——这正是"文字永远面向观察者、不会随视角倾斜"的做法，
/// 也是三维软件里标签的常见实现（billboard 的二维等价物）。
///
/// ## 为什么连线也在这里（换掉一版的关键决定）
///
/// 第一版把连线放在三维层（细圆柱）。结果线永远"连不上字"：
/// - 三维画的是**节点中心**的投影点；
/// - 文字被避让算法挪到了别处（右侧 4px，重叠时上下让位几十像素）。
///
/// 两套坐标对不上，调参数治不好——避让位置是运行时才算的，三维层拿不到。
/// 而本层知道每个文字框的**精确屏幕矩形**，在这里画线才能真的连到字上。
///
/// 附带收益：二维直线没有圆柱几何的锯齿，也不受三维渲染分辨率影响。
///
/// ## 为什么标签是主角
///
/// 知识图谱的用途是"看清知识之间的关系"。一堆彩色点只能看出"有几个簇"，
/// 看不出"这条是什么"。所以标签不该是装饰：**文字本身就是节点**。

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "st/ui/element.hpp"

namespace st::ui {

/// 一个待绘制的标签。
///
/// ## 为何存的是「世界坐标 + 投影器」而不是屏幕坐标
///
/// 屏幕坐标由**视口自己算**（`ThreeView::project_to_screen`），
/// 不在这里重算一遍。曾在这里手写轨道相机公式，结果是两套计算，
/// 转镜头后文字与几何逐渐错位。
///
/// 现在本类只负责“摆文字”（避让、底板、字号），投影交给视口。
struct LabelAnchor {
  /// 稳定标识（连线靠它指向本标签）。空 = 不参与连线。
  std::string id;
  /// 世界坐标（交给投影器换算）。
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
  /// 文字（空 = 不画）。
  std::string text;
  /// 重要度（0~1）：决定字号与排列优先级。
  float weight{0.35f};
  /// 颜色（ARGB）。
  unsigned int argb{0xFFD8DEE9U};
  /// 是否画底板。
  ///
  /// 两者都要底板（踩了两轮）：词条背景里可能有线，无底板字被切成两半；
  /// 分类无底板时叠在深背景上**整团发虚**（比有底板更糟）。
  bool plate{true};
  /// 底板浓度（0~255）：分类用淡的，让它在视觉上退后一层。
  unsigned char plate_alpha{0xE6};
};

/// 一条连线（两端用 `LabelAnchor::id` 指向）。
struct LabelEdge {
  /// 两端标签的 id。任一端找不到（被避让挤掉或没画）则不画这条线。
  std::string from;
  std::string to;
  /// 颜色（ARGB）。
  unsigned int argb{0xFF8593A5U};
  /// 线宽（逻辑像素）。
  float width{1.6f};
};

/// 节点的“锚点”是怎么画的（写在这里是因为它决定了整体观感）
///
/// ## 三版的失败链与最终形态
///
/// 1. 线连三维层的节点中心 → 文字被避让到别处 → 线“悬空、连不上字”；
/// 2. 线改连文字框边缘 → 相邻两个节点被避让到相反方向 → 线被拉得很长、
///    而且指向看起来不相干的字；
/// 3. **现在**：投影点自己画一个小圆点，连线连**圆点**，文字只是挂在旁边的标签。
///
/// 根因是**文字位置不等于节点位置**：避让是屏幕空间的，会把文字推离真实投影点。
/// 所以需要两层：圆点是稳定的位置（只跟相机有关），文字是可以避让的附属物。
/// 这也是所有成熟图谱工具的形态（Obsidian / Graphviz / Gephi 都是“点 + 标签”）。
///
/// 圆点的尺寸与颜色直接从 `LabelAnchor` 推（见 paint_content）——
/// 不引入单独的 `LabelDot` 类型：它会与 LabelAnchor 的坐标重复，
/// 而重复的两份坐标一定会不同步。
/// 标签叠层：把一组世界坐标标签投影到屏幕并绘制（含连线）。
class LabelOverlay : public Element {
 public:
  LabelOverlay();
  ~LabelOverlay() override;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "LabelOverlay"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  /// 换标签集（由 bridge 在图谱重建时调用）。
  void set_labels(std::vector<LabelAnchor> labels);
  /// 换连线集（两端用标签 id 指向）。
  void set_edges(std::vector<LabelEdge> edges);

  /// 投影器：把世界坐标换成屏幕坐标 + 深度。
  ///
  /// 由调用方提供（通常是 `ThreeView::project_to_screen`）。
  /// 返回 `visible=false` 的标签不画。
  using Projector = std::function<std::pair<std::pair<float, float>, float>(float, float, float)>;
  void set_projector(Projector projector);

  /// 字体缩放（提高可读性；缺省按元素高度自适应）。
  void set_font_scale(float scale);

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;

  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  /// 诊断：本帧画了几个标签（证明"该画的画了"，而不是靠看截图）。
  [[nodiscard]] auto painted_labels() const noexcept -> std::size_t { return painted_; }
  /// 诊断：本帧画了几条连线（两端都在场的才算）。
  [[nodiscard]] auto painted_edges() const noexcept -> std::size_t { return painted_edges_; }
  /// 诊断：本帧真正做出锚点的标签 id → 屏幕位置。
  ///
  /// 为何要暴露：截图里“线连向空白”只能看出有毛病，看不出是哪一端。
  /// 有这个就能直接断言「每一条线的两端都在锚点里」——
  /// 而不是靠看截图猜。
  [[nodiscard]] auto anchor_snapshot() const
      -> const std::unordered_map<std::string, std::pair<float, float>>& {
    return anchors_;
  }
  /// 诊断：同 id 的重复标签数量（重复会让线连到错的位置）。
  [[nodiscard]] auto duplicate_ids() const -> std::size_t { return duplicate_ids_; }
  /// 诊断：边列表（两端 id）。
  [[nodiscard]] auto edge_snapshot() const -> std::vector<std::pair<std::string, std::string>>;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<LabelAnchor> labels_;
  std::vector<LabelEdge> edges_;
  Projector projector_{};
  float font_scale_{1.0f};
  mutable std::size_t painted_{0};
  mutable std::size_t painted_edges_{0};
  /// 本帧的锚点（id → 屏幕坐标），供诊断用。`mutable`：`paint_content` 是 const。
  mutable std::unordered_map<std::string, std::pair<float, float>> anchors_{};
  mutable std::size_t duplicate_ids_{0};
};

}  // namespace st::ui

// ── 本组件**不进声明式注册表** ─────────────────────────────────────────
//
// 它是三维族：构造本身不报错，但声明式创建（`dsl::make_element`）在无头环境
// 没有意义，而且 gallery 对账要求"注册了就得上卡或写豁免"。它由 C++ 直接
// `new` 并挂树，不走 `make_element`。
//
// 需要它参与声明式创建时：加 `ST_UI_COMPONENT` + `.cpp` 里 `ST_UI_COMPONENT_IMPL`，
// 并在 `gallery_exhibits.hpp` 补一张卡（或写一条豁免理由）。
