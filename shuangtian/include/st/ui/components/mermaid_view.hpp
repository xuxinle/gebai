#pragma once

/// @file mermaid_view.hpp
/// @brief `MermaidView`：把一段 **Mermaid 文本**画成图（原生自绘，不依赖浏览器/JS）。
///
/// ## 它由什么组成（两层，与 `GraphView`/`TimelineView` 同一分工）
///
/// | 层 | 职责 |
/// |---|---|
/// | `st::md::mermaid`（`include/st/md/mermaid.hpp`） | 解析 + 几何布局——纯计算，可单测 |
/// | **本组件** | 把几何画成像素、接属性面/动作面/滚轮与拖拽 |
///
/// ## 为什么不做「内嵌浏览器 + mermaid.js」
///
/// 框架的定位是**零外部依赖的原生绘制**（`WebView` 是可选的降级能力，Windows 之外
/// 不一定有）。把图交给浏览器意味着：无头环境画不出来、截图回归不稳、条文与控件
/// 主题各走一套颜色。而流程图/时序图/饼图这三类正是**纯几何**，自己画反而更可控。
///
/// ## v1 支持什么（不支持的部分**如实上报**，不静默丢内容）
///
/// | 语法 | 画成 |
/// |---|---|
/// | `flowchart TD/LR/RL/BT`（`graph` 同义） | 分层框图 + 正交连线 + 边标签；节点形状 `[]`/`()`/`([ ])`/`{}`/`(( ))` |
/// | `sequenceDiagram` | 参与者头 + 生命线 + 消息箭头（实线/虚线/自消息） |
/// | `pie` | 饼图 + 图例 + 标题 |
///
/// 其余图类型（class/state/er/gantt/journey…）与语法其余部分（`subgraph`、`loop/alt`、
/// `&` 并列、样式指令）记进 `diagnostics()`：能画的照画，缺的部分有文字线索。
///
/// ## 视图变换：fit 与 zoom 是两个量
///
/// - `fit`（默认开）：把整张图**缩到装得下**（只缩不放——小图放大反而糊）。
///   实际倍率 = `min(1, 视口宽/内容宽, 视口高/内容高)`。
/// - `zoom`：在 fit 之上再乘的**用户倍率**（滚轮/Ctrl+滚轮/属性面都改它）。
///
/// 分成两个量的理由（不合成一个）：窗口变大时"图刚好装满"要自动跟随，而用户
/// 手动放大的意图不能因为窗口变化被清掉——合成一个数就必然二选一。
///
/// ## 属性面
///
/// 可写：`source`（Mermaid 文本）、`zoom`、`fit`、`font_size`、`pan_x`/`pan_y`。
/// 只读：`kind`、`error`（首条诊断）、`diagnostics`（条数）、`node_count`、`edge_count`、
/// `content_width`/`content_height`（布局尺寸，便于断言）。
///
/// 动作：`zoom_in` / `zoom_out` / `reset_view` / `fit`（重算并回到居中）。
///
/// ## 交互
///
/// - 滚轮：纵向平移；`Ctrl+滚轮`：以指针为锚缩放（与 `Viewport`/`TimelineView` 同约定）。
/// - 拖拽空白：平移。命中区就是整个组件（图不是可编辑控件，没有"点中某节点"的语义；
///   需要节点点击时用 `st::md::mermaid` 的几何自己接，或改用 `GraphView`）。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/md/mermaid.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// Mermaid 图视图。
class MermaidView : public Element {
 public:
  static constexpr float kMinZoom{0.15f};
  static constexpr float kMaxZoom{8.0f};
  /// 滚轮缩放每格倍率（与 `ThreeView` 同一手感）。
  static constexpr float kWheelZoomStep{1.12f};

  MermaidView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "MermaidView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Image; }

  // —— 数据 ——

  /// 换文本（**会立即重新解析与布局**；解析失败不抛异常，见 `diagnostics()`）。
  void set_source(std::string source);
  [[nodiscard]] auto source() const noexcept -> const std::string& { return source_; }

  /// 解析结果（只读）。
  [[nodiscard]] auto document() const noexcept -> const st::md::mermaid::Document& {
    return document_;
  }
  /// 首条诊断（空 = 没有不支持/无法解析的内容）。属性面 `error` 就是它。
  [[nodiscard]] auto first_diagnostic() const -> std::string;
  [[nodiscard]] auto diagnostics() const noexcept -> const std::vector<std::string>& {
    return document_.diagnostics;
  }

  // —— 视图 ——

  void set_zoom(float zoom);
  [[nodiscard]] auto zoom() const noexcept -> float { return zoom_; }
  /// 是否"缩到装得下"（默认开；关掉时按 1:1 内容像素绘制）。
  void set_fit(bool fit);
  [[nodiscard]] auto fit() const noexcept -> bool { return fit_; }
  void set_layout_font_size(float size);
  [[nodiscard]] auto layout_font_size() const noexcept -> float { return options_.font_size; }
  void pan_by(float dx, float dy);
  void set_pan(float x, float y);
  [[nodiscard]] auto pan_x() const noexcept -> float { return pan_x_; }
  [[nodiscard]] auto pan_y() const noexcept -> float { return pan_y_; }
  /// 回到居中、zoom = 1（fit 仍生效）。
  void reset_view();

  /// 布局选项（改完调 `relayout()` 生效）。
  [[nodiscard]] auto layout_options() -> st::md::mermaid::LayoutOptions& { return options_; }
  void relayout();

  /// 内容尺寸（逻辑像素，已含布局留白；属性面便于断言）。
  [[nodiscard]] auto content_size() const noexcept -> math::Size { return content_size_; }
  /// 当前生效倍率（`fit` 与 `zoom` 的合成值；`arrange` 后有效）。
  [[nodiscard]] auto effective_scale() const noexcept -> float { return scale_; }

  // —— Element 接口 ——

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 内容坐标 → 屏幕坐标（`paint` 与交互反算共用同一份公式）。
  [[nodiscard]] auto content_origin() const -> math::Point;
  /// 视口内是否有内容（无内容时画一句提示）。
  void paint_placeholder(const RenderContext& context, raster::Surface& canvas) const;
  void paint_flowchart(const RenderContext& context, raster::Surface& canvas) const;
  void paint_sequence(const RenderContext& context, raster::Surface& canvas) const;
  void paint_pie(const RenderContext& context, raster::Surface& canvas) const;
  /// 按 `fit`/`zoom` 重算 `scale_` 与居中偏移（`arrange` 与尺寸变化后调用）。
  void update_view();
  /// 用给定度量重跑一遍布局（`relayout` 用估值，`measure` 用真实端口）。
  ///
  /// 两条路走**同一个入口**：布局公式只能有一份，否则估值与真实值会各自演化，
  /// 症状是“第一帧看着对、排版后跑偏”这类只在特定时刻出现的错位。
  void apply_layout(const st::md::mermaid::MeasureText& measure);

  std::string source_{};
  st::md::mermaid::Document document_{};
  st::md::mermaid::LayoutOptions options_{};
  st::md::mermaid::FlowLayout flow_{};
  st::md::mermaid::SequenceLayout sequence_{};
  st::md::mermaid::PieLayout pie_{};
  math::Size content_size_{};

  float zoom_{1.0f};
  bool fit_{true};
  float pan_x_{0.0f};
  float pan_y_{0.0f};
  /// 上一帧算出的合成倍率（`fit × zoom`）。
  float scale_{1.0f};

  // 主题色（`apply_theme` 填，绘制只读）。
  math::Color surface_{};
  math::Color border_{};
  math::Color text_color_{};
  math::Color text_muted_{};
  math::Color edge_color_{};
  math::Color accent_{};
  math::Color label_background_{};
  /// 饼图配色（按序取用；不足时回落到 accent）。
  std::vector<math::Color> slice_colors_{};

  // 拖拽平移（`MouseDown` 起、`MouseUp` 止）。
  bool dragging_{false};
  math::Point drag_origin_{};
  float drag_pan_x_{0.0f};
  float drag_pan_y_{0.0f};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::MermaidView, "MermaidView")
