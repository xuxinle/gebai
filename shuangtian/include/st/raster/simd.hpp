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

/// 遮蔽（常量 alpha）整行（Src 模式用）。
void erase_row(std::uint32_t* dst, std::size_t count, std::uint8_t alpha) noexcept;

}  // namespace st::raster::simd
