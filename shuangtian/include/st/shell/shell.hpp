#pragma once

/// 窗口与事件循环后端：`headless`（离屏，默认）/ `win32` / `x11` / `wayland`。
///
/// 硬约束：平台后端一律**运行时探测**（不产生链接期依赖），缺失或未实现时如实报 `Unsupported`，
/// 无桌面环境自动落到 `headless` —— 无头模式下开发/验证闭环完全可用（控制通道承担输入）。
///
/// 实现状态（**如实**）：`headless` 与 `win32` 已实现；`x11`/`wayland` 目前只做探测与
/// 明确的 `Unsupported` 答复（软件光栅器与 UI 层与平台无关，补后端是纯粹的窗口层工作量）。

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/element.hpp"
#include "st/ui/window_control.hpp"  // 窗框契约（WindowEdge / 缩放带）——定义在 ui 层，理由见该头文件

namespace st::shell {

struct WindowOptions {
  int width{1280};      ///< 逻辑宽（UI 坐标单位）
  int height{720};      ///< 逻辑高
  float scale{1.0f};    ///< DPI 缩放（物理像素 = 逻辑 × scale；1.0 = 传统 96dpi）
  std::string title{"霜天应用"};
  bool headless{false};
  /// 是否可缩放（拖边改尺寸）。
  ///
  /// ⚠ 长期是个**静默失效字段**（只有声明、无人读取，建窗恒用 `WS_OVERLAPPEDWINDOW`）；
  /// 自绘窗框落地时一并接上：`false` = 不带 `WS_THICKFRAME`、边缘不参与缩放命中。
  bool resizable{true};
  /// 是否让窗口系统画标题栏/边框。
  ///
  /// **默认 `false`（窗框一律自绘）**——这是框架契约（`CONVENTIONS.md` §10 第 7 条）：
  /// 三平台自带标题栏的字号/高度/圆角/配色各不相同，"一块代码三平台外观一致"会从窗框
  /// 处漏掉；而自绘窗框才与 UI 共用同一套设计令牌、同一套 DPI 口径与**同一份无头截图**。
  /// `true` 仅留给"确实想要系统窗框"的宿主（如系统级调试工具），不是常规路径。
  bool decorations{false};
  /// 渲染器：`auto`（按实测性能选）/ `gpu` / `software`。
  std::string renderer{"auto"};
};

/// 窗口边缘/角落枚举由 `st/ui/window_control.hpp` 定义并在本命名空间**转发**：
///
/// 为什么不在这里另立一个：平台中立的判定规则必须只有一份（Win32 的 `HT*` 命中码、
/// X11 自算、Wayland 交合成器，API 各不相同——"同一次拖拽在两平台差几像素"是最难查的
/// 那类幽灵）。而**两端都要用它**：后端起窗/命中要用，UI 层（`ui::TitleBar`）决定
/// "这次按下是拖动还是缩放"也要用。依赖方向决定了落点——`shell.hpp` 已包含 ui 头，
/// 反向包含会成环，故定义住 ui，这里只转发（调用方仍可写 `shell::WindowEdge`）。
using ui::WindowEdge;
/// 缩放命中带（逻辑像素）：与 UI 层同一常量（见 `ui::kWindowResizeBorder`）。
inline constexpr float k_resize_border = ui::kWindowResizeBorder;

class Backend {
 public:
  virtual ~Backend() = default;

  /// 后端名（"headless" / "x11" / "wayland" / "win32"）。
  [[nodiscard]] virtual auto name() const noexcept -> std::string_view = 0;
  [[nodiscard]] virtual auto headless() const noexcept -> bool = 0;

  virtual auto create_window(const WindowOptions& options) -> Status = 0;
  /// 提交一帧（headless 时写入离屏缓冲）。
  virtual void present() = 0;
  /// 取回输入事件（无则空）。headless 恒为空——输入由控制通道直接投递。
  [[nodiscard]] virtual auto poll_event() -> std::optional<ui::Event> = 0;
  virtual void set_title(std::string_view title) = 0;
  [[nodiscard]] virtual auto clipboard_text() -> Result<std::string> = 0;
  [[nodiscard]] virtual auto set_clipboard_text(std::string_view text) -> Status = 0;
  /// 目标帧缓冲（应用直接绘制到这里）。
  [[nodiscard]] virtual auto framebuffer() -> raster::Surface& = 0;
  /// 已提交帧数。
  [[nodiscard]] virtual auto frame_count() const noexcept -> std::uint64_t = 0;
  /// 当前 DPI 缩放（物理像素 / 逻辑像素）。
  [[nodiscard]] virtual auto device_scale() const noexcept -> float = 0;
  /// 当前实际使用的渲染器名（"software" / "gpu"）与选择理由。
  ///
  /// 为什么必须可上报：`auto` 是**按实测性能**选的，用户有权知道这一帧是谁画的、
  /// 以及为什么——"不知道自己用的是哪个渲染器"会让性能问题无从排查。
  [[nodiscard]] virtual auto renderer_name() const noexcept -> std::string_view {
    return "software";
  }
  [[nodiscard]] virtual auto renderer_note() const -> std::string { return {}; }
  /// 运行时切换 DPI（重分配帧缓冲；无头与窗口模式一致）。
  virtual auto set_device_scale(float scale) -> Status = 0;
  /// 逻辑视口尺寸（窗口尺寸，不含 DPI 放大）。
  [[nodiscard]] virtual auto logical_size() const noexcept -> math::Size = 0;
  /// 窗口系统是否请求关闭（用户点关闭按钮）。
  ///
  /// 由平台后端实现；应用主循环据此收尾（不能直接 `exit`——进程内还有控制通道、
  /// 脚本宿主等资源需要正常停止）。
  [[nodiscard]] virtual auto close_requested() const noexcept -> bool { return false; }

  /// 把窗口显式展示出来（首帧画完之后调）。
  ///
  /// **为什么窗口不能建完就显**：`CreateWindowExW` 后立即 `ShowWindow` 会让窗口
  /// 先露出「系统预备的白底 / 未初始化内容」，而此时画布还在分配（内含渲染器实测
  /// 基准，实测 ~570ms）；`resize_window_to_scale` 放大到真实 DPI 尺寸时，多出来
  /// 的区域又没任何东西可贴 → **启动时右下/底部一大块黑**（实测 0.6 秒后自行消失，
  /// 因为主循环第一帧终于画上了）。
  ///
  /// 约定：`create_window` 只建不显；应用在**首帧 `present()` 之后**调本函数。
  /// 无头后端不需观感，默认空实现即可。
  virtual void show_when_ready() {}

  /// 安装「拖动/缩放时立即重绘」的钩子（由 app 层安装；后端不认识 app 层）。
  ///
  /// **为什么必须有它**：用户拖边框走的是窗口系统的模态循环
  /// （Win32：`DefWindowProc` 收 `SC_SIZE` 后自己跑消息循环），它**阻塞应用主循环**——
  /// 期间 `WM_SIZE`/`WM_PAINT` 是唯一的执行机会。不在这里驱动一次渲染，屏幕就只能
  /// 停在旧内容上：实测拖动 30 步、`metrics.frames` 只涨 **0~2**，而"旧尺寸的帧被
  /// DXGI 拉到新客户区"的逐像素差异高达 `mean|Δ|=19.3`（**21.8% 像素明显不同**）——
  /// 这就是用户看到的"拖动时被拉伸扭曲、松手才重画"。
  ///
  /// 语义：后端在"尺寸已变、需要重画"时调用它；实现必须**同步完成一帧**
  /// （重建缓冲 → 布局 → 绘制 → 送显）。后端要保证重入安全（多次调用不会嵌套成环）。
  virtual void set_resize_repaint(std::function<void()> repaint) { (void)repaint; }

  // —— 自绘窗框所需的窗口控制 ——
  //
  // 存在的理由（`CONVENTIONS.md` §10 第 7 条）：窗框一律自绘，而"最小化/最大化/关闭/
  // 拖动/缩放"只有窗口系统能做。两端之间必须有**一份平台中立的契约**，否则每个应用
  // 都要自己写三份平台分支（那正是 §10 第 1 条禁止的）。
  //
  // 与 UI 层的 `ui::WindowControl` 端口是**两件事**：这一层是平台后端的能力，
  // `ui::WindowControl` 是组件看到的抽象（`ui` 不认识 `shell`，依赖方向见
  // `st/ui/window_control.hpp`）。`st::app::Application` 把两者接上（转发 + 把
  // `Status` 折成 `bool`），因此 UI 与后端之间不产生任何直接依赖。
  //
  // 契约：**所有平台语义一致**；不支持时**如实**报 `Unsupported`（不静默无效——
  // "点了没反应"与"压根没这个能力"是两件事，调用方要靠返回值区分）。

  /// 是否具备窗口控制能力（无头后端为 `false`；UI 据此如实禁用按钮而不是画一个点不动的装饰）。
  [[nodiscard]] virtual auto supports_window_control() const noexcept -> bool { return false; }
  /// 最小化。
  [[nodiscard]] virtual auto minimize() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }
  /// 最大化/还原（切换）。
  [[nodiscard]] virtual auto toggle_maximize() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }
  /// 请求关闭——走与用户点窗口关闭按钮**同一条收尾路径**（置 `close_requested`，
  /// 由应用主循环退出；不能直接 `exit`，进程内还有控制通道/脚本宿主要正常停止）。
  [[nodiscard]] virtual auto request_close() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }
  /// 开始拖动窗口（自绘标题栏按下时调用）：进入窗口系统的移动模态循环。
  [[nodiscard]] virtual auto begin_move() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }
  /// 开始缩放窗口（自绘窗框边缘按下时调用；`edge` 为 `ui::resize_edge_at` 的判定结果）。
  [[nodiscard]] virtual auto begin_resize(WindowEdge edge) -> Status {
    (void)edge;
    return unexpected(ErrorCode::Unsupported, "该后端不支持窗口控制");
  }
  /// 窗口当前是否最大化（自绘窗框据此切换按钮形态）。
  [[nodiscard]] virtual auto maximized() const noexcept -> bool { return false; }
};

/// 是否检测到显示服务（DISPLAY / WAYLAND_DISPLAY）。
[[nodiscard]] auto has_display() noexcept -> bool;
/// 探测可用的图形后端名（无显示时返回 "headless"）。
[[nodiscard]] auto probe_backend() -> std::string;
/// 系统显示缩放（物理像素 / 逻辑像素）：Windows 取主显示器 DPI/96（不声明 DPI 感知
/// 的进程会被虚拟化为 96，因此实现里先声明感知）；其它平台无系统级缩放概念，返回 1.0。
///
/// 用途：无头后端在未显式指定 `scale` 时**跟随系统**——内置通道的截图/坐标
/// 与桌面窗口取同一像素密度，“内置里看着对不对”与“实机跑起来”才不会两套。
[[nodiscard]] auto system_display_scale() noexcept -> float;
/// Windows 实现（platform_win32.cpp）。经 `system_display_scale` 调用，非 Windows 不会触达。
[[nodiscard]] auto win32_display_scale() noexcept -> float;
/// 创建平台专属后端（各 `platform_*.cpp` 提供；未实现的平台在工厂里如实报 `Unsupported`）。
[[nodiscard]] auto create_win32_backend() -> Result<std::unique_ptr<Backend>>;

/// 创建后端（`name` 为空=自动选择：有显示则尝试平台后端，失败回退 headless）。
[[nodiscard]] auto create_backend(std::string_view name = {}) -> Result<std::unique_ptr<Backend>>;

/// 合成负载微基准（真实画布尺寸上跑一组有代表性的原语，返回**中位数**毫秒）。
///
/// 用途：`--renderer auto` 的判据。两个后端共用同一份实现——"更快"的定义只能有一处，
/// 否则窗口与离屏会得出不同结论。
///
/// ⚠ 它**不含 `present()`**：窗口呈现的开销（是否走 swapchain）不在这个数字里，
/// 由 `renderer_note` 如实报出，不混进本基准。
[[nodiscard]] auto benchmark_surface(raster::Surface& target, int runs) -> double;

}  // namespace st::shell
