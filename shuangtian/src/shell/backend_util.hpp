#pragma once

/// 窗口后端实现之间共用的**内部**工具（不进公开头：它认识 `WindowOptions` 与 `Surface`，
/// 但只是后端自己的装配细节，应用层不该看见）。
///
/// 为什么必须收成一份：这四个后端（`headless` / `win32` / `x11` / `wayland`）各有各的
/// 建窗方式，但「这一帧的绘制面是谁画的」这件事的平台中立性完全一样——
/// `auto` 的判据（实测哪条更快）、`gpu` 不可用时的回退理由、物理尺寸怎么由逻辑尺寸
/// 与 scale 算出来，只要各写一份，就一定会分家。而分家的表现是
/// **同一个应用在不同后端下渲染器不同、且没人说得清为什么**（`metrics.renderer_note`
/// 是排查性能问题的唯一线索，谎报它等于把线索掐断）。
///
/// 谁在用：`backend.cpp`（headless）、`platform_x11.cpp`、`platform_wayland.cpp`。
/// **`platform_win32.cpp` 仍是自己那份**（`create_surface_for`）——它的注释与
/// DIB/DXGI 呈现路径绑在一起，统一它需要一台 Windows 机器做回归验证，
/// 属于独立的一笔重构，不在这轮里顺手改（未验证的重构 = 把可用的东西改坏的风险）。

#include <memory>
#include <string>
#include <string_view>

#include "st/raster/surface.hpp"
#include "st/shell/shell.hpp"

namespace st::shell::detail {

/// 绘制面选型结果：面 + 名字（`software`/`gpu`）+ **理由**。
struct SurfaceChoice {
  std::unique_ptr<raster::Surface> surface{};
  std::string name{"software"};
  std::string note{};
};

/// 按 `renderer`（`auto`/`gpu`/`software`/空=auto）建绘制面。
///
/// `auto` = **实测**：两条都建出来各跑一遍同样的缩尺负载，取更快的那条；
/// 不做"GPU 优先"的硬编码（GPU 弱、驱动差、或呈现仍需 CPU 拷贝时软件反而更快）。
[[nodiscard]] auto create_surface(const WindowOptions& options, std::string_view renderer)
    -> SurfaceChoice;

/// 合成负载微基准（中位数毫秒）：`auto` 的判据本体，与公开的 `shell::benchmark_surface`
/// 是同一份实现（后者只做转发）。
[[nodiscard]] auto benchmark_surface_impl(raster::Surface& target, int runs) -> double;

/// 运行时库探测（**不产生链接期依赖**）：候选名用 `:` 分隔，返回第一个可加载的名字；
/// 都不行返回空串。
///
/// 这是 `CONVENTIONS.md` §10 第 1 条「系统能力一律运行时 `dlopen` 可选加载」的落点——
/// 没有它，探测到 `libX11` 但没装开发包就成了链接期失败（实测本机就是这种机器）。
///
/// **实现在 `backend.cpp`**：那里是 L14（"系统头/平台 API 只能出现在 `platform_*`"）
/// 已登记的豁免点（理由："需 `dlopen` 做运行时后端探测"）。把 `dlopen` 搬到本文件
/// 会新增一处需要登记的例外，而这条能力本就是"后端探测"的一部分——收在一处更诚实。
[[nodiscard]] auto library_available(std::string_view candidates) -> std::string;

}  // namespace st::shell::detail
