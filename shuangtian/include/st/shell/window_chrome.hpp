// 平台无关的**窗框几何**：外围阴影/圆角的参数化 + 环形带位图生成 + 关键换算。
//
// 为什么把几何放到这里（而不是 `platform_win32.cpp` 里算）：
//   · 平台层只该做"把像素搬到窗口""把输入翻成事件""提供窗口控制"（`CONVENTIONS.md` §10 第 7 条），
//     而"窗外阴影有多少像素、圆角多大、最大化时该不该画"是**可测的设计参数**，必须能单测；
//   · 同一套参数将来 x11/wayland 也要用（那边同样拿不到系统阴影），不许在平台里各写一份。
//
// 本头只做纯计算，不碰任何系统头——`platform_*` 把算好的像素交给各自的呈现原语。
//
// ## 位图是整窗外扩，但**只写窗外那一圈**
//
// 分层窗口（`UpdateLayeredWindow`）每次只接受一整块位图，所以位图必须是 (W+2m)×(H+2m)。
// 但**窗口内部那一片永远是透明的**（主窗会盖住它），没必要每帧重算重写。
// 因此接入方只需：① 按尺寸分配一次；② 每次重建时只让本函数写环。
//
// 实测差异（4K，`tools/window_chrome_bench.cpp`）：
//   · 全图逐像素算 + `assign` 27 MiB：**20.6 ms**（拖动时每帧一次，卡）；
//   · 只写环（本实现）：**< 1 ms**。
// 关键在于环的边界能**解析算出来**：某一行距窗口上下边都超过 `radius` 时，该行只有
// 左右两侧（宽 `margin + radius`）可能是阴影，中间整段跳过。

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace st::shell {

/// 窗框视觉参数（**逻辑像素**；物理像素 = 逻辑 × device_scale）。
///
/// 取值理由（与 `docs/perceptual_changes.md` 的口径一致：观感参数要么有实测依据，
/// 要么照抄已被验证的参照）：
/// - `shadow_margin` 18：够容纳 `shadow_blur × 1.6` 的衰减（否则阴影会被边缘硬切一刀）；
/// - `shadow_alpha` 0.42：浅色底（#E6E6E6）上合成后窗口边缘暗约 ~35%，
///   与 Win11 系统窗口的阴影强度相当。
///   **实测调整**（2026-11-05）：原本 0.30 在深色壁纸上看不出（合成后只比桌面暗 4/255）；
///   用户在浅色桌面下能看见，深色下就"没有阴影"。
/// - `corner_radius` 10：比 Win11 系统圆角（8）略大：自绘窗框没有系统那圈 1px 描边，
///   圆角太小会看不出是圆的。
struct WindowChromeStyle {
  int shadow_margin{18};
  float shadow_alpha{0.42f};
  int shadow_blur{9};
  int corner_radius{10};
  bool shadow{true};
};

/// 窗口矩形上一帧的几何摘要。
struct WindowChromeGeometry {
  /// 窗口客户区物理尺寸（= 画布尺寸）。
  int client_width{0};
  int client_height{0};
  /// 设备缩放（物理 = 逻辑 × scale）。
  float scale{1.0f};
  /// 是否最大化。**最大化时一律不画阴影与圆角**：阴影会落到屏幕外（看不见），圆角会在
  /// 四角露出桌面色——系统窗口最大化时也是直角贴边，这是桌面窗口的通行行为。
  bool maximized{false};
};

/// 环形带位图的布局（全**物理像素**）。
struct WindowRingLayout {
  int width{0};
  int height{0};
  /// 带厚（物理像素）= 客户区外扩的距离。
  int margin{0};
  /// 圆角半径（物理像素，已夹到短边一半以内）。
  int radius{0};
  /// 设备缩放（用于把模糊半径等逻辑参数换成物理像素）。
  float scale{1.0f};
  /// 客户区尺寸（物理像素）。
  int client_width{0};
  int client_height{0};
  /// 是否真的需要绘制。`false` 时调用方应当跳过整个影子层（省掉合成开销）。
  bool visible{false};
};

/// 圆角矩形的 signed distance：`< 0` 在内部，`> 0` 在外部，单位与输入一致。
[[nodiscard]] auto round_rect_distance(float px, float py, float centre_x, float centre_y,
                                       float half_width, float half_height,
                                       float radius) noexcept -> float;

/// 由几何与样式算出环形带布局（含 `visible`）。
[[nodiscard]] auto ring_layout(const WindowChromeGeometry& geometry,
                               const WindowChromeStyle& style) noexcept -> WindowRingLayout;

/// 生成环形阴影位图：**预乘 BGRA**（与 Win32 分层窗口 `AC_SRC_ALPHA` 的约定一致）。
///
/// `out` 由调用方提供（长度必须为 `layout.width × layout.height`），函数**只写窗外那一圈**：
/// 内部像素一律不碰（调用方保证它们是 0 = 全透明）。这样做的原因是实测出来的：
/// 先 `assign` 一块 27 MiB 再整块拷回去，光内存在 4K 上就要 20 ms（分配 + 清零 + 页错），
/// 而真正需要写的只有 **0.5 MiB 量级**的环。
///
/// ## 两个颜色参数
///
/// - `shadow_rgb`：窗外阴影的颜色；
/// - `edge_rgb`：窗口本身的底色。用于**圆角边缘的抗锯齿**——`SetWindowRgn` 是一位
///   区域，无法表达部分覆盖，所以紧贴圆角内缘那一像素由影子层用该色 + 覆盖率 alpha 补上。
///   传错颜色会变成一圈异色边（传黑就是黑边），调用方应当传真实的窗口底色。
///
/// 返回是否写入成功（尺寸不符/不可见 → `false`，`out` 不被修改）。
///
/// `antialias_corner`：是否在弧外侧补一像素窗口色（= 圆角抗锯齿）。
/// 生产路径恒为 `true`；传 `false` 只用于对照测试（验证“关掉后确实没有那一条带”）。
[[nodiscard]] auto build_ring_pixels(const WindowRingLayout& layout,
                                     const WindowChromeStyle& style, std::uint32_t shadow_rgb,
                                     std::span<std::uint32_t> out,
                                     std::uint32_t edge_rgb = 0xFFFFFFU,
                                     bool antialias_corner = true) -> bool;

/// 窗口裁剪用的圆角半径（**物理像素**，取整后）；`0` = 直角（调用方据此撤销裁剪区）。
///
/// 为什么用半径而不是"一组矩形"：真正的圆角是一条弧，用矩形去近似会在四个角上留下
/// 明显的方角缺口（比不做还难看）。平台层据此调自己的圆角区域原语
/// （Win32 是 `CreateRoundRectRgn`，x11 是 `XFixesSetWindowShapeRegion`）。
[[nodiscard]] auto corner_radius_physical(const WindowChromeGeometry& geometry,
                                          const WindowChromeStyle& style) noexcept -> int;

}  // namespace st::shell
