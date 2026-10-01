#pragma once

/// 窗口与事件循环后端：`headless`（离屏，默认）/ `win32` / `x11` / `wayland`。
///
/// 硬约束：平台后端一律**运行时探测**（不产生链接期依赖），缺失或未实现时如实报 `Unsupported`，
/// 无桌面环境自动落到 `headless` —— 无头模式下开发/验证闭环完全可用（控制通道承担输入）。
///
/// 实现状态（**如实**）：`headless` 与 `win32` 已实现；`x11`/`wayland` 目前只做探测与
/// 明确的 `Unsupported` 答复（软件光栅器与 UI 层与平台无关，补后端是纯粹的窗口层工作量）。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/element.hpp"

namespace st::shell {

struct WindowOptions {
  int width{1280};      ///< 逻辑宽（UI 坐标单位）
  int height{720};      ///< 逻辑高
  float scale{1.0f};    ///< DPI 缩放（物理像素 = 逻辑 × scale；1.0 = 传统 96dpi）
  std::string title{"霜天应用"};
  bool headless{false};
  bool resizable{true};
  /// 渲染器：`auto`（按实测性能选）/ `gpu` / `software`。
  std::string renderer{"auto"};
};

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
