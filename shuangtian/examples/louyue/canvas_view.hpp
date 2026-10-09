#pragma once

/// 镂月（Lunaris）的**自绘画布视口**。
///
/// ## 为什么是 `Element` 子类而不是声明式节点
///
/// 画布是高频自绘面：每一笔都要立刻反映到像素上，而声明式重组（重建元素树）
/// 的成本与笔触频率不匹配。`dsl::custom<T>` 逃生舱正是为这种场景留的——
/// 与 gbcode 持有 `CodeEditor` 同构（见设计文档 §9「逃生舱裂缝」一条）。
///
/// 状态变化走**显式 invalidate**（`mark_dirty`），不进重组树。

#include <functional>
#include <string>
#include <vector>

#include "document.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace louyue {

/// 工具枚举（左工具栏的每一项）。
enum class Tool : std::uint8_t { Move, Brush, Eraser, Bucket, Eyedropper, Select, Crop };

[[nodiscard]] auto tool_name(Tool tool) -> const char*;
[[nodiscard]] auto tool_icon(Tool tool) -> const char*;

/// 一个**纯色块**（颜色面板的前景色预览）。
///
/// ⚠ 为什么不能用 `Panel` + `style().background`：`apply_theme` 在**每次布局**
/// 都跑（`UiRoot::layout` 会遍历全树下线主题），它会用主题色盖掉调用方写的
/// `background`——于是在 `custom<Panel>` 的 configure 里设色只一帧内有效，
/// 下一帧就变回主题底色（实测：色块看着像“没画”）。
/// 自绘元素的路子才稳：`paint_content` 里直接画，不经主题。
class Swatch final : public st::ui::Element {
 public:
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Swatch"; }
  void set_color(st::math::Color color) {
    color_ = color;
    mark_dirty();
  }
  void apply_theme(const st::ui::Theme& theme) override { border_ = theme.colors().border_strong; }
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  st::math::Color color_{0, 0, 0, 255};
  st::math::Color border_{};
};

/// 画布视口：棋盘格底 + 缩放平移 + 笔触输入。
class CanvasView final : public st::ui::Element {
 public:
  CanvasView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CanvasView"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Panel; }

  void set_document(Document* document);
  void set_tool(Tool tool);
  [[nodiscard]] auto tool() const noexcept -> Tool { return tool_; }

  /// 缩放档位（1.0 = 100%）。
  void set_zoom(float zoom);
  [[nodiscard]] auto zoom() const noexcept -> float { return zoom_; }
  /// 在档位表里前进/后退一格（Ctrl+滚轮与快捷键都用它，保证档位是**离散**的）。
  void zoom_step(int delta);

  void set_foreground(st::math::Color color);
  [[nodiscard]] auto foreground() const noexcept -> st::math::Color { return foreground_; }
  void set_background(st::math::Color color);

  void set_brush_radius(float radius);
  [[nodiscard]] auto brush_radius() const noexcept -> float { return brush_radius_; }

  /// 光标所在的**原图坐标**（视口外返回 false）——状态栏与吸管用。
  [[nodiscard]] auto cursor_image_pos() const noexcept -> st::math::Point { return cursor_; }
  [[nodiscard]] auto cursor_inside() const noexcept -> bool { return cursor_inside_; }
  /// 光标处的**合成结果**颜色（吸管读的就是它）。
  [[nodiscard]] auto cursor_color() const -> st::math::Color;

  /// 命令式作画（e2e 的 `invoke stroke` 走这里）：
  /// 在原图坐标画一段，入历史栈。返回是否真的改了像素。
  auto stroke(st::math::Point from, st::math::Point to) -> bool;
  /// 命令式取色（e2e 的 `invoke pick <x> <y>`）。
  auto pick_at(st::math::Point point) -> bool;

  /// 内容变化后的回调（宿主用它刷新图层面板/历史面板/状态栏）。
  std::function<void()> on_document_changed{};

  // —— Element 接口 ——
  void apply_theme(const st::ui::Theme& theme) override;
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 内容变化后的统一收尾（标记重绘 + 通知宿主）。
  void notify();

  /// 画布矩形（逻辑坐标）：原图按 zoom 缩放并居中到视口。
  [[nodiscard]] auto canvas_rect() const -> st::math::Rect;
  /// 视口坐标 → 原图坐标（可能落在图外）。
  [[nodiscard]] auto to_image(st::math::Point view) const -> st::math::Point;
  /// 原图坐标 → 视口坐标。
  [[nodiscard]] auto to_view(st::math::Point image) const -> st::math::Point;
  /// 棋盘格（透明底指示）。
  void paint_checkerboard(st::raster::Surface& canvas, st::math::Rect area) const;

  Document* document_{nullptr};
  Tool tool_{Tool::Brush};
  float zoom_{1.0f};
  float brush_radius_{6.0f};
  st::math::Color foreground_{32, 34, 44, 255};
  st::math::Color background_{255, 255, 255, 255};

  st::math::Point cursor_{};
  bool cursor_inside_{false};
  bool dragging_{false};
  /// 上一笔的落点（原图坐标）——高频 MouseMove 下用它做插值起点。
  st::math::Point last_{};

  st::math::Color checker_light_{};
  st::math::Color checker_dark_{};
  st::math::Color border_{};
};

}  // namespace louyue
