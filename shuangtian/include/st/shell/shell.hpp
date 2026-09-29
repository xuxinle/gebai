#pragma once

/// 窗口与事件循环后端：`headless`（离屏，默认）/ `x11` / `wayland` / `win32`。
/// 硬约束：X11/Wayland/Win32 一律**运行时探测**（不产生链接期依赖），缺失或未实现时如实报 `Unsupported`，
/// 无桌面环境自动落到 `headless` —— 无头模式下开发/验证闭环完全可用（控制通道承担输入）。

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
  [[nodiscard]] virtual auto framebuffer() -> raster::Canvas& = 0;
  /// 已提交帧数。
  [[nodiscard]] virtual auto frame_count() const noexcept -> std::uint64_t = 0;
  /// 当前 DPI 缩放（物理像素 / 逻辑像素）。
  [[nodiscard]] virtual auto device_scale() const noexcept -> float = 0;
  /// 运行时切换 DPI（重分配帧缓冲；无头与窗口模式一致）。
  virtual auto set_device_scale(float scale) -> Status = 0;
  /// 逻辑视口尺寸（窗口尺寸，不含 DPI 放大）。
  [[nodiscard]] virtual auto logical_size() const noexcept -> math::Size = 0;
};

/// 是否检测到显示服务（DISPLAY / WAYLAND_DISPLAY）。
[[nodiscard]] auto has_display() noexcept -> bool;
/// 探测可用的图形后端名（无显示时返回 "headless"）。
[[nodiscard]] auto probe_backend() -> std::string;
/// 创建后端（`name` 为空=自动选择：有显示则尝试平台后端，失败回退 headless）。
[[nodiscard]] auto create_backend(std::string_view name = {}) -> Result<std::unique_ptr<Backend>>;

}  // namespace st::shell
