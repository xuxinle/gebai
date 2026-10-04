#pragma once

/// 组件库 A · 窗框容器（`DESIGN.md` §4.5）：`WindowFrame`——**组件化的窗口**。
///
/// 存在的理由（`CONVENTIONS.md` §10 第 7 条）：窗口装饰一律自绘之后，**窗口没有系统边框**，
/// 于是"窗口可拖动、可缩放、有标题栏"这件事**只能由界面自己提供**。只有 `TitleBar`
/// 是不够的——它只占了顶部那一条，窗口的八向边缘仍然是死的（拖不动、也缩不了）。
/// `WindowFrame` 把这三样收进一个容器：**标题栏（置顶）+ 内容槽（铺满剩余）+ 缩放边缘条（八向）**。
///
/// 为什么做成容器而不是让每个应用自己拼：
/// 1. **一致性**：示例/应用外壳只需要写一次（`WindowFrame(title, content)`），不会出现
///    "这个应用有边缘、那个应用没有"；
/// 2. **判定只有一份**：边缘条与平台后端共用 `ui::resize_edge_at`（`st/ui/window_control.hpp`），
///    Windows 上由 `WM_NCHITTEST` 覆盖（系统级、更跟手），X11/Wayland 走 `begin_resize`；
/// 3. **跨平台无差异**：边缘条**在所有平台都画/都存在**（会不会被真正消费由 `WindowControl`
///    如实回答），因此画面在无头/三平台完全一致——这是本项目对"无差异"的定义。
///
/// 布局：列 —— 标题栏（`TitleBar`，高 `TitleBar::bar_height()`）在上，内容槽 `grow` 占满其余。
/// 子元素约定（与 `ScrollView` 同族）：**载体的自有部件在后**（标题栏 + 8 条边缘），
/// 内容槽是 `content_child_count()` 内的那些（节点插入时自动放进内容区）。
///
/// 控制通道：`title` 属性转发标题栏；动作面 `minimize`/`maximize`/`close` 转发窗口动作；
/// `#<id>/content` 是内容区的稳定寻址点（宿主把界面挂进去）。
///
/// 用法：
/// ```cpp
/// auto frame = std::make_unique<WindowFrame>("霜天 · 组件画廊");
/// frame->title_bar()->set_icon("sparkles");
/// frame->title_bar()->add_trailing(std::move(theme_button));   // 宿主控件进窗框
/// frame->content()->add_child(std::move(body));                // 内容区
/// root.set_content(std::move(frame));
/// app.attach_window_frame(frame.get());                        // 接上窗口动作
/// ```

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/element.hpp"
#include "st/ui/window_control.hpp"

namespace st::ui {

class WindowFrame : public Element {
 public:
  /// `title` 为空则不显示标题栏（仅边缘条 + 内容槽的纯内容窗框）。
  explicit WindowFrame(std::string title = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "WindowFrame"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  /// 标题栏（自持；`show_title_bar == false` 时为 `nullptr` 之外恒有效）。
  [[nodiscard]] auto title_bar() noexcept -> TitleBar* { return bar_; }
  [[nodiscard]] auto title_bar() const noexcept -> const TitleBar* { return bar_; }
  /// 内容区（自持的列布局面板）：宿主的界面挂在这里。
  [[nodiscard]] auto content() noexcept -> Element* { return content_; }
  [[nodiscard]] auto content() const noexcept -> const Element* { return content_; }

  /// 是否显示标题栏（默认显示；`false` 时内容区独占，边缘条仍在）。
  void set_show_title_bar(bool show);
  [[nodiscard]] auto show_title_bar() const noexcept -> bool { return show_title_bar_; }
  /// 是否保留八向缩放边缘（默认保留）。
  ///
  /// 为什么**默认保留**：没有它，无边框窗口在拖边时毫无反应（`WM_NCHITTEST` 之外
  /// 的平台全靠这个区域）。关掉只在"确实不想让用户缩放"的场合用。
  void set_show_resize_edges(bool show);
  [[nodiscard]] auto show_resize_edges() const noexcept -> bool { return show_resize_edges_; }
  /// 绑定窗口控制端口（缺省遍历到 `UiRoot` 宿主——`Application` 挂着它；也可显式指定）。
  void set_window_control(WindowControl* control) noexcept;
  /// 显式绑定的端口（为空时回落到宿主）。
  [[nodiscard]] auto window_control() const noexcept -> WindowControl* { return control_; }

  /// 边缘条厚度（逻辑像素）：与后端判定带**同一常量**（判定只有一份）。
  [[nodiscard]] static constexpr auto edge_thickness() noexcept -> float {
    return kWindowResizeBorder;
  }
  /// 第 `index` 条边缘的矩形（绝对坐标；index 见 `EdgeIndex`）。不可用时为空矩形。
  [[nodiscard]] auto edge_rect(std::size_t index) const -> math::Rect;
  /// 点所在的边缘（复用 `ui::resize_edge_at`；不接收则返回 `None`）。
  [[nodiscard]] auto edge_at(math::Point point) const -> WindowEdge;

  /// 载体子元素 = 内容槽内的子元素（标题栏与边缘条是自持部件，恒在末尾）。
  [[nodiscard]] auto content_child_count() const noexcept -> std::size_t override;
  /// 插入**内容区**（调用方语义；自持部件位置由本组件维护）。
  auto add_child(std::unique_ptr<Element> child) -> Element* override;
  auto insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element* override;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 边缘条枚举（与 `WindowEdge` 一一对应；绘制/命中共用同一套下标）。
  enum EdgeIndex : std::size_t {
    kEdgeLeft = 0,
    kEdgeRight,
    kEdgeTop,
    kEdgeBottom,
    kEdgeTopLeft,
    kEdgeTopRight,
    kEdgeBottomLeft,
    kEdgeBottomRight,
    kEdgeCount,
  };
  /// 下标 → `WindowEdge`。
  [[nodiscard]] static auto edge_of(std::size_t index) noexcept -> WindowEdge;

 private:
  /// 边缘条与边界之间按 `edge_thickness()` 内缩（条画在**窗口内侧**，不越界）。
  /// 命中判定也走它：与后端 `resize_edge_at` 的口径一致（坐标从窗口左上角起算）。
  [[nodiscard]] auto edge_rect_internal(std::size_t index, math::Rect frame) const -> math::Rect;
  /// 取出显式端口或宿主端口。
  [[nodiscard]] auto resolve_control() const -> WindowControl*;

  TitleBar* bar_{nullptr};
  Element* content_{nullptr};
  bool show_title_bar_{true};
  bool show_resize_edges_{true};
  WindowControl* control_{nullptr};
  /// 当前按下的边缘（`WindowEdge::None` = 无）：按压期间给条一点反馈色。
  WindowEdge pressed_edge_{WindowEdge::None};
  /// 悬浮的边缘（同上）。
  mutable WindowEdge hovered_edge_{WindowEdge::None};
};

}  // namespace st::ui
