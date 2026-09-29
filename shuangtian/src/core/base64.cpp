#include "st/core/base64.hpp"

#include <array>

namespace st {
namespace {

inline constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] constexpr auto decode_char(char raw) noexcept -> int {
  if (raw >= 'A' && raw <= 'Z') return raw - 'A';
  if (raw >= 'a' && raw <= 'z') return raw - 'a' + 26;
  if (raw >= '0' && raw <= '9') return raw - '0' + 52;
  if (raw == '+') return 62;
  if (raw == '/') return 63;
  return -1;
}

}  // namespace

auto base64_encode(std::span<const std::uint8_t> data) -> std::string {
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  std::size_t index = 0;
  while (index + 3 <= data.size()) {
    const std::uint32_t triple = (static_cast<std::uint32_t>(data[index]) << 16U) |
                                 (static_cast<std::uint32_t>(data[index + 1]) << 8U) |
                                 static_cast<std::uint32_t>(data[index + 2]);
    out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
    out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
    out.push_back(kAlphabet[triple & 0x3FU]);
    index += 3;
  }
  const std::size_t remaining = data.size() - index;
  if (remaining == 1) {
    const std::uint32_t triple = static_cast<std::uint32_t>(data[index]) << 16U;
    out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
    out.append("==");
  } else if (remaining == 2) {
    const std::uint32_t triple = (static_cast<std::uint32_t>(data[index]) << 16U) |
                                 (static_cast<std::uint32_t>(data[index + 1]) << 8U);
    out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
    out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
    out.push_back('=');
  }
  return out;
}

auto base64_encode(std::string_view text) -> std::string {
  std::vector<std::uint8_t> buffer;
  buffer.reserve(text.size());
  for (const char raw : text) buffer.push_back(static_cast<std::uint8_t>(raw));
  return base64_encode(std::span<const std::uint8_t>(buffer));
}

auto base64_decode(std::string_view text) -> Result<std::vector<std::uint8_t>> {
  std::vector<std::uint8_t> out;
  out.reserve((text.size() / 4) * 3);
  std::uint32_t accumulator = 0;
  int bits = 0;
  for (const char raw : text) {
    if (raw == '=' || raw == '\n' || raw == '\r' || raw == ' ') continue;
    const int value = decode_char(raw);
    if (value < 0) return unexpected(ErrorCode::Parse, "非法 base64 字符");
    accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((accumulator >> static_cast<unsigned>(bits)) & 0xFFU));
    }
  }
  return out;
}

}  // namespace st
