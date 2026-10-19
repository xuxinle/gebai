#pragma once

/// SIMD 快路径（内部接口）：整行填充与整行常量 alpha 混合。
/// 语义基准是标量实现；SIMD 路径必须逐像素与标量一致（单测断言）。

#include <cstddef>
#include <cstdint>

namespace st::raster::simd {

/// 用预乘像素填充一行（count 像素）。
void fill_row(std::uint32_t* dst, std::size_t count, std::uint32_t premultiplied) noexcept;

/// 以常量 alpha（0..255）把预乘源 src-over 混合到目标行。
void blend_row(std::uint32_t* dst, std::size_t count, std::uint32_t premultiplied,
               std::uint8_t alpha) noexcept;

/// 当前编译单元启用的加速档位（"sse2" / "avx2" / "neon" / "scalar"）。
[[nodiscard]] auto backend() noexcept -> const char*;

/// 逐像素 `src-over`：`dst[i] = src[i] over dst[i]`（源与目标都是**预乘**）。
///
/// 与 `blend_row` 的分工：后者是「**一个**源色 × 常量覆盖率」贴一整行（覆盖率来自扫描线），
/// 本函数是「**每像素各自的**源色」——图片/阴影贴图/几何 sprite 的合成都是这个形态。
/// 旧实现对这些场景只能逐像素调 `over_premul`（4 次除法-移位 + 4 次解包/打包），
/// 是阴影与图标合成的主要开销。
///
/// ## 语义契约（两条，缺一不可）
///
/// 1. **两侧都是预乘 RGBA8**（`0xRRGGBBAA`），且满足预乘不变量 `c ≤ a`；
/// 2. 于是逐通道结果**不会超过 255**（`src_c + dst_c·(255−sa)/255 ≤ sa + (255−sa) = 255`），
///    SIMD 侧的饱和打包与标量侧的普通打包**逐位一致**。传违反不变量的数据时
///    标量路径会溢出串色、SIMD 路径饱和——那是调用方违约，不是两条路径不一致。
/// 3. 源完全透明（`a == 0`）的像素**保持目标原值**（与 `over_premul` 的早退同义）。
void over_row(std::uint32_t* dst, const std::uint32_t* src, std::size_t count) noexcept;

/// 遮蔽（常量 alpha）整行（Src 模式用）。
void erase_row(std::uint32_t* dst, std::size_t count, std::uint8_t alpha) noexcept;

/// 画布行（`0xRRGGBBAA` 预乘）→ 窗口系统行（小端 `0xFFRRGGBB`）。
///
/// 为什么需要它：窗口后端每帧都要把画布重排成窗口系统要的像素布局
/// （X11 的 ZPixmap 在 24/32 位 TrueColor 上是小端 `0x00RRGGBB`；Wayland 的
/// `WL_SHM_FORMAT_XRGB8888` 同布局）。这段逐像素重排是**整屏线性开销**
/// （1080p 实测 ~0.96 ms/帧，标量），且两端共用同一份语义——下沉到这里，
/// 三路（AVX2/SSE2/标量）逐像素一致由测试钉住。
///
/// 语义：`dst[i] = ((src[i] >> 8) & 0x00FFFFFF) | 0xFF000000`。
/// **高字节填 `0xFF` 而不是 0**：窗口内容不透明（自绘窗框不依赖系统透明度），
/// 而画布清屏用的 `alpha=0` 像素若不置不透明标记会在 depth 32 的可视化上
/// 表现为黑边/穿透；depth 24 下该字节是 padding，填什么都不影响像素值
/// （与 Win32 的 DIB 路径、旧版 X11 实现逐字节一致）。
/// 行内允许重叠（`dst == src` 时即原地转换，逐元素无依赖）。
void convert_rgba_to_xrgb_row(std::uint32_t* dst, const std::uint32_t* src,
                              std::size_t count) noexcept;

}  // namespace st::raster::simd
