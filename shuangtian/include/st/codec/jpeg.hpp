#pragma once

/// @file jpeg.hpp
/// @brief JPEG 解码（基线 + 渐进式）：生态素材的绝大多数 albedo 就是它。
///
/// ## 为什么自研而不引第三方
///
/// 与 PNG 同一取舍（本仓的 PNG 也是自研的）：**零外部依赖**是本框架的硬约束
///（可离线构建、可交叉编译、不引入 CVE 面）。JPEG 基线解码约 600 行，
/// 是可控的量；而 libjpeg-turbo 会带来 3 万行 + 构建系统改动。
///
/// ## 支持范围（**明确声明边界，不假装完整**）
///
/// | 特性 | 状态 |
/// |---|---|
/// | 基线顺序（SOF0） | ✅ 支持 |
/// | 扩展顺序（SOF1） | ✅ 支持 |
/// | 渐进式（SOF2） | ✅ 支持（多扫描累加） |
/// | 灰度（1 分量） | ✅ 支持 |
/// | YCbCr（3 分量，4:4:4 / 4:2:2 / 4:2:0 / 4:1:1） | ✅ 支持 |
/// | CMYK / YCCK（4 分量） | ⚠️ 读成 RGB（近似，不声明为精确） |
/// | 算术编码（SOF9-11） | ❌ 报错（生态里极罕见） |
/// | 无损 JPEG（SOF3） | ❌ 报错 |
/// | 12 位精度 | ❌ 报错（8 位之外不做） |
///
/// 不支持时**明确报错并说明是哪一项**——而不是解出一张乱码图
///（那会让用户以为素材坏了）。
///
/// ## 输出
///
/// RGBA8、非预乘、行优先（与 `png_decode` 同一契约）。

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "st/core/error.hpp"

namespace st::codec {

struct JpegImage {
  std::vector<std::uint8_t> rgba{};
  int width{0};
  int height{0};
};

[[nodiscard]] auto jpeg_decode(std::span<const std::uint8_t> bytes) -> Result<JpegImage>;

}  // namespace st::codec
