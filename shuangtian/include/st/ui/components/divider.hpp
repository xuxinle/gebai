#pragma once

/// @file divider.hpp
/// @brief `Divider`：水平分隔线。。
///
/// 从 `basic.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `basic.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 水平分隔线（`vertical = true` 时为竖线）。
///
/// ## 量法：铺满“有上界”的那个方向，另一个方向固定线厚
///
/// 父容器给的约束形态不同，而“哪个方向能铺满”完全由它决定：
///
/// | 父容器 | `max_width` | `max_height` |
/// |---|---|---|
/// | 列（主轴纵、交叉轴横） | 内容宽（**有限**） | `kUnbounded` |
/// | 行（主轴横、交叉轴纵） | `kUnbounded` | 内容高（**有限**） |
///
/// - 列里的水平线 → 横向铺满（分隔两块上下内容），高 = `kLineThickness`；
/// - 行里的竖线 → 纵向铺满，宽 = `kLineThickness`；
/// - **行里的水平线** → 横向无上界 ⇒ 退到线厚；纵向有内容高，`align_items: Stretch`
/// 把它拉到整行高 ⇒ **视觉上就是一道竖向分隔**（行里想要分隔符时的正确结果，
/// 对应 CSS 的 `border-left`）。
///
/// ⚠ 不能直接取 `constraints.max_width`：行给子元素的交叉轴上界是 `kUnbounded`（1e9），
/// 一条水平线进工具行就量出 1e9 宽，父行把整行撑爆（线后面每个兄弟的 `x` 变成
/// 1e9、2e9……，每过一个分隔线翻一倍），而线的 `bounds` “看着就是这个数”。
/// 也不能用 `fill_width`（它在无界时退到 `available_*`，那正是行主轴空间的全部）。
/// 语义是“无上界就不参与主轴竞争”。回归：`divider_in_row_does_not_swallow_the_line`。
class Divider : public Element {
 public:
  /// 线厚（逻辑 px）。**允许亚像素**：`0.5f` 在 2 倍 DPI 下正好一个物理像素，
  /// 1 倍下会被光栅器按覆盖率画成一条淡线。比 `metrics.border_width`（1.0）轻一档，
  /// 因为分割线是“同组内的弱分隔”，不该与控件边界同重（用户 2026-10-21 要求）。
  static constexpr float kLineThickness{0.5f};

  explicit Divider(bool vertical = false);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Divider"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Separator; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;

 private:
  bool vertical_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Divider, "Divider")
