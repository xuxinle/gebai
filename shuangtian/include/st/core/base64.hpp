#pragma once

/// Base64 编解码（控制通道传输截图数据用）。

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st {

[[nodiscard]] auto base64_encode(std::span<const std::uint8_t> data) -> std::string;
[[nodiscard]] auto base64_encode(std::string_view text) -> std::string;
[[nodiscard]] auto base64_decode(std::string_view text) -> Result<std::vector<std::uint8_t>>;

}  // namespace st
