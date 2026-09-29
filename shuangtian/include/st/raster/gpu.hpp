#pragma once

/// GPU 渲染目标（首选路径）与设备探测。
///
/// 设计立场：**GPU 是首选、软件是兜底**，但两者通过同一个 `raster::Surface` 接口暴露，
/// 上层（UI/文本/组件/控制通道）不知道这一帧是谁画的。
///
/// 硬约束（与窗口后端一致）：
/// 1. **运行时探测，不产生链接期依赖**：GPU 库由平台层动态加载（Windows 上是
///    `LoadLibrary("d3d11.dll")`），因此"没装显卡驱动 / 没有 GPU / 非 Windows 平台"
///    的机器上框架照常构建、照常运行（自动落到软件光栅器），不会出现"链接不到 d3d11"。
/// 2. **无显示服务也能用**：GPU 设备与窗口无关——无头模式同样走 GPU
///    （硬件不行还有 WARP：微软的 GPU 指令集软件实现），因此"GPU 路径"能在 CI 里被回归。
/// 3. **失败要如实说明**：探测失败给出具体原因（缺 DLL / 驱动 / 设备创建失败码），
///    而不是一个笼统的 false。

#include <cstdint>
#include <memory>
#include <string>

#include "st/core/error.hpp"
#include "st/raster/surface.hpp"

namespace st::raster::gpu {

/// 设备信息（用于上报"这一帧真的是显卡画的"以及是哪一块卡）。
struct DeviceInfo {
  std::string backend{};         ///< 后端名："d3d11" / ""（不可用）
  std::string adapter{};         ///< 适配器名（显卡型号）
  std::string feature_level{};   ///< 特性级别："11_0" / "10_1" …
  bool warp{false};              ///< 是否落在 WARP（CPU 模拟的 GPU 管线）
  std::uint64_t vram_bytes{0};   ///< 专用显存（WARP 为 0）
};

/// 建画布时的设备选择。
struct Options {
  /// 硬件设备不可用时是否回退 WARP（默认允许）。关闭它可用来**确认**硬件路径真的可用。
  bool allow_warp{true};
  /// 要求当前进程设备**必须**是 WARP（用于验证"无 GPU 机器"的代码路径）。
  ///
  /// 注意：它不会"把设备切成 WARP"（设备是进程级的，建一次共享用）——
  /// 而是在设备不是 WARP 时**如实报错**。要真的用 WARP 跑，设环境变量
  /// `ST_GPU_FORCE_WARP=1` 后重启进程（见 `DeviceHolder` 的注释）。
  bool require_warp{false};
};

/// GPU 是否可用（首次调用会真建设备，结果缓存）。
[[nodiscard]] auto available() noexcept -> bool;

/// 探测 GPU 并返回设备信息；不可用时给出**具体原因**。
[[nodiscard]] auto probe() -> Result<DeviceInfo>;

/// 创建 GPU 绘制目标（离屏，物理像素尺寸；无需窗口，无头可用）。
[[nodiscard]] auto create_canvas(int physical_width, int physical_height, float device_scale,
                                 const Options& options = {}) -> Result<std::unique_ptr<Surface>>;

/// 当前进程已创建 GPU 画布数量（诊断用；设备与上下文按进程共享）。
[[nodiscard]] auto live_canvas_count() noexcept -> std::uint32_t;

}  // namespace st::raster::gpu
