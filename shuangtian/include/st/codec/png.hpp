#pragma once

/// PNG 编解码（W3C PNG 1.2 / RFC 2083）——霜天自研，零第三方依赖（仅复用本层 deflate 与 `st::hash` 的 CRC-32）。
///
/// 能力与限制（实现细节见 `src/codec/png.cpp` 顶部说明）：
/// - **解码**：颜色类型 0(gray)/2(RGB)/3(palette)/4(gray+alpha)/6(RGBA)，位深 8；
///   五种行过滤（None/Sub/Up/Average/Paeth）全支持；`tRNS` 透明键与调色板 alpha 全支持；
///   每个 chunk 的 CRC-32 逐块校验，损坏即 `ErrorCode::Parse`。
/// - **编码**：恒输出 8 位 RGBA 真彩（颜色类型 6）+ None 过滤 + zlib(deflate)。
/// - **不支持**：位深 1/2/4/16、隔行（Adam7）→ `ErrorCode::Unsupported`；
///   `sRGB`/`iCCP`/`gAMA` 等色彩块被忽略（不做色彩空间转换）。

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "st/codec/deflate.hpp"
#include "st/core/error.hpp"

namespace st::codec {

/// 解码结果：**非预乘** RGBA8，`rgba` 长度恒为 `width * height * 4`（行优先，无行填充）。
struct PngImage {
  std::uint32_t width{0};
  std::uint32_t height{0};
  Bytes rgba{};
};

/// 解码 PNG。
/// 失败：`Parse`（签名/chunk 结构/CRC-32/IDAT/inflate 失败、tRNS 或调色板缺失、调色板索引越界）、
/// `Unsupported`（位深非 8、隔行）、`Overflow`（解压输出超限）。
[[nodiscard]] auto png_decode(std::span<const std::uint8_t> input) -> Result<PngImage>;

/// 编码为 8 位 RGBA 真彩 PNG（`level` 透传给 zlib deflate，0..9）。
/// 失败：`Invalid`（尺寸为零或 `rgba` 长度不等于 `width*height*4`）。
[[nodiscard]] auto png_encode(const PngImage& image, int level = 6) -> Result<Bytes>;

/// 读取并解码 PNG 文件。
/// 失败：`NotFound`（文件不存在）、`Io`（读失败）、以及 `png_decode` 的全部错误。
[[nodiscard]] auto png_read_file(std::string_view path) -> Result<PngImage>;

/// 编码并写出 PNG 文件（自动创建父目录）。
/// 失败：`Invalid`/`Unsupported`（同 `png_encode`）、`Io`（写失败）。
[[nodiscard]] auto png_write_file(std::string_view path, const PngImage& image, int level = 6)
    -> Status;

}  // namespace st::codec
