#pragma once

/// 自绘窗框的**平台中立**契约：窗口边缘判定 + 窗口控制端口。
///
/// 为什么这两样放在 `ui` 而不是 `shell`：**依赖方向**。`shell.hpp` 已经包含
/// `st/ui/element.hpp`（事件类型），`ui` 反向包含 shell 会成环；而窗框组件（`ui::TitleBar`）
/// 必须能直接调"最小化/最大化/关闭/拖动"，平台后端也必须用**同一个**边缘判定
/// （否则"同一次拖拽在 Windows 与 Linux 上差几像素"就成了没法复现的幽灵）。
/// 于是：契约放 `ui`（无平台依赖），`shell::Backend` 实现它，`app` 层把两者接起来。
///
/// 本文件**不包含任何系统头**，也不认识窗口系统（`CONVENTIONS.md` §10 第 1 条）。

#include <cstdint>

#include "st/math/geometry.hpp"

namespace st::ui {

/// 窗口边缘/角落（自绘窗框的缩放命中区）。
enum class WindowEdge : std::uint8_t {
  None,
  Left,
  Right,
  Top,
  Bottom,
  TopLeft,
  TopRight,
  BottomLeft,
  BottomRight,
};

/// 缩放命中带宽（逻辑像素）。
///
/// 6px 是实测手感阈值：窗口边缘没有系统边框可"看着抓"，太窄会让自绘窗框变得难用。
/// 后端与 UI 必须取同一个值——这是"判定只有一份"的一部分。
inline constexpr float kWindowResizeBorder = 6.0f;

/// 窗口边缘判定（**纯函数，所有平台共用**）：`point` 为窗口内（或邻近）的**逻辑**坐标，
/// `size` 为窗口逻辑尺寸，`border` 为缩放带宽。
///
/// 落在带外（含远离窗口的点）一律返回 `None`——Win32 的 `WM_NCHITTEST` 在拖拽期间会收到
/// 远离窗口的坐标（鼠标可以跑到屏幕另一角），不设上界就会把"指针在屏幕另一头"当成
/// "正在抓左边框"，表现为"鼠标在别处、窗口却跟着变宽"。
[[nodiscard]] auto resize_edge_at(math::Point point, math::Size size, float border) noexcept
    -> WindowEdge;

/// 窗口控制端口：自绘窗框把窗口级动作转给宿主（"存在"与"可用"是两件事）。
///
/// 为什么是**抽象端口**而不是直接调后端：`ui` 层不认识 `shell::Backend`（依赖方向），
/// 而单测里也不该为了测一个标题栏去开真窗口。宿主实现它（`st::app::Application` 转发给
/// 后端），标题栏只认这个接口。
///
/// 契约：**不支持就返回 `false`，不静默无效**——"点了没反应"与"这个环境压根没窗口"
/// 是两件事，调用方（以及自动化验证）要靠返回值区分。另：`available()` 为假时
/// **像素仍然照画**（`cross_platform.md` §7：无头与窗口的窗框画面必须一致），
/// 只是交互如实拒绝。
class WindowControl {
 public:
  virtual ~WindowControl() = default;

  /// 当前环境是否具备窗口控制能力（无头后端为 `false`）。
  [[nodiscard]] virtual auto window_control_available() const -> bool = 0;
  [[nodiscard]] virtual auto window_minimize() -> bool = 0;
  /// 最大化/还原（切换）。
  [[nodiscard]] virtual auto window_toggle_maximize() -> bool = 0;
  [[nodiscard]] virtual auto window_request_close() -> bool = 0;
  /// 开始拖动窗口（标题栏按下时调用；等价于拖系统标题栏，吸附等手势随之生效）。
  [[nodiscard]] virtual auto window_begin_move() -> bool = 0;
  /// 开始缩放窗口（边缘按下时调用；X11/Wayland 用得上，Win32 由 `WM_NCHITTEST` 覆盖）。
  [[nodiscard]] virtual auto window_begin_resize(WindowEdge edge) -> bool = 0;
  /// 窗口当前是否最大化（标题栏据此切换"最大化/还原"按钮形态）。
  [[nodiscard]] virtual auto window_maximized() const -> bool = 0;
};

}  // namespace st::ui
