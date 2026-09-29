#pragma once

/// UI 树根：布局 → 绘制 → 事件分发 → 焦点 → 语义/视觉快照（控制通道的数据源）。
/// 无头模式与窗口模式走同一条路径：`layout()` + `paint(canvas)`，差异只在 shell 后端。

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/ui/element.hpp"
#include "st/ui/selector.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

class UiRoot {
 public:
  UiRoot();
  ~UiRoot();
  UiRoot(const UiRoot&) = delete;
  auto operator=(const UiRoot&) -> UiRoot& = delete;

  void set_content(std::unique_ptr<Element> content);
  [[nodiscard]] auto content() const noexcept -> Element* { return content_.get(); }

  [[nodiscard]] auto theme() noexcept -> Theme& { return theme_; }
  [[nodiscard]] auto theme() const noexcept -> const Theme& { return theme_; }
  void set_theme(Theme theme);

  /// 文本绘制端口（非拥有；为空时使用 `NullTextPort`）。
  void set_text_port(const TextPort* port);
  [[nodiscard]] auto text_port() const noexcept -> const TextPort* { return text_port_; }

  void set_viewport(math::Size size);
  [[nodiscard]] auto viewport() const noexcept -> math::Size { return viewport_; }

  /// 全树布局（脏时才重算）。
  void layout(bool force = false);
  /// 全树绘制（含遮罩裁剪与叠加层）。
  void paint(raster::Canvas& canvas);

  /// 事件分发（命中测试 → 捕获链 → 冒泡；焦点/悬停状态随之更新）。
  [[nodiscard]] auto dispatch(Event& event) -> bool;

  [[nodiscard]] auto find(std::string_view id) -> Element*;
  [[nodiscard]] auto query(const Selector& selector, std::size_t limit = 0) -> std::vector<Element*>;
  [[nodiscard]] auto hit_test(math::Point point) -> Element*;

  void set_focus(Element* element);
  [[nodiscard]] auto focused() const noexcept -> Element* { return focused_; }
  void focus_next(bool backwards = false);

  /// 语义树（`tree` 协议；`max_depth == 0` 表示不限）。
  [[nodiscard]] auto semantics(std::uint32_t max_depth = 0) const -> SemanticsNode;
  /// 视觉树（`visual` 协议）。
  [[nodiscard]] auto visual_tree() const -> VisualNode;

  /// 变更计数（每次布局/状态变化自增；控制通道事件与等待逻辑据此判定"已重绘"）。
  [[nodiscard]] auto version() const noexcept -> std::uint64_t { return version_; }
  void bump_version() noexcept { ++version_; }

  void mark_dirty_all();
  [[nodiscard]] auto dirty() const noexcept -> bool { return dirty_; }
  [[nodiscard]] auto dirty_rect() const noexcept -> math::IntRect { return dirty_rect_; }
  void clear_dirty() noexcept;

  /// 叠加层（对话框 / Toast / 菜单）：绘制在内容之上，事件优先命中。
  void add_overlay(std::unique_ptr<Element> overlay);
  [[nodiscard]] auto overlay_count() const noexcept -> std::size_t { return overlays_.size(); }
  [[nodiscard]] auto overlay_at(std::size_t index) const noexcept -> Element*;
  void remove_overlay(Element* overlay);
  void clear_overlays();

  [[nodiscard]] auto render_context() const -> RenderContext;

 private:
  void assign_ids(Element& element, const std::string& prefix);
  void layout_subtree(Element& element, math::Rect rect);
  void paint_subtree(const RenderContext& context, Element& element, raster::Canvas& canvas);
  [[nodiscard]] auto hit_test_subtree(Element& element, math::Point point) -> Element*;
  [[nodiscard]] auto dispatch_to(Element& element, Event& event) -> bool;
  void collect_focus_order(Element& element, std::vector<Element*>& order);
  void update_hover(Element* target);

  Theme theme_{};
  std::unique_ptr<Element> content_{};
  std::vector<std::unique_ptr<Element>> overlays_{};
  const TextPort* text_port_{nullptr};
  math::Size viewport_{1280.0f, 720.0f};
  Element* focused_{nullptr};
  Element* hovered_{nullptr};
  Element* pressed_{nullptr};
  std::uint64_t version_{1};
  bool dirty_{true};
  math::IntRect dirty_rect_{};
  double time_seconds_{0.0};
};

}  // namespace st::ui
