#include "st/codec/image.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "st/codec/jpeg.hpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"

namespace st::codec {

namespace {

/// PNG 魔数与 JPEG 魔数（够判别，不必看更多字节）。
constexpr std::array<std::uint8_t, 8> kPngMagic{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

[[nodiscard]] auto starts_with(std::span<const std::uint8_t> bytes,
                               std::span<const std::uint8_t> prefix) noexcept -> bool {
  return bytes.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

/// 把前若干字节拼成十六进制（报错时给出"这到底是不是图"的证据）。
[[nodiscard]] auto hex_prefix(std::span<const std::uint8_t> bytes, std::size_t limit) -> std::string {
  std::string out;
  const std::size_t count = std::min(limit, bytes.size());
  out.reserve(count * 3U);
  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0) out.push_back(' ');
    out += std::format("{:02X}", bytes[i]);
  }
  return out;
}

}  // namespace

auto sniff_image_format(std::span<const std::uint8_t> bytes) -> std::string {
  if (starts_with(bytes, kPngMagic)) return "png";
  // JPEG 以 SOI（FF D8）开始，后面紧跟一个标记（FF xx）
  constexpr std::array<std::uint8_t, 3> kJpegMagic{0xFF, 0xD8, 0xFF};
  if (starts_with(bytes, kJpegMagic)) return "jpeg";
  return {};
}

auto decode_image(std::span<const std::uint8_t> bytes) -> Result<DecodedImage> {
  const std::string format = sniff_image_format(bytes);
  DecodedImage out{};
  out.format = format;

  if (format == "png") {
    auto image = png_decode(bytes);
    if (!image.has_value()) return st::forward_error(image.error());
    out.rgba = std::move(image->rgba);
    out.width = static_cast<int>(image->width);
    out.height = static_cast<int>(image->height);
    return out;
  }
  if (format == "jpeg") {
    auto image = jpeg_decode(bytes);
    if (!image.has_value()) return st::forward_error(image.error());
    out.rgba = std::move(image->rgba);
    out.width = image->width;
    out.height = image->height;
    return out;
  }

  // ## 识别不出时给出**证据**而不是一句"不支持"
  //
  // 最常见的三种情况：文件是空的、下回来的是 HTML 错误页、真是个未支持的格式
  //（WebP / KTX2 / TGA）。前 8 字节足以区分，比"Unsupported"有用得多。
  return st::unexpected(
      ErrorCode::Unsupported,
      std::format("无法识别的图片格式（{} 字节，前 {} 字节：{}）；支持 PNG / JPEG",
                  bytes.size(), std::min<std::size_t>(8, bytes.size()),
                  hex_prefix(bytes, 8)));
}

auto decode_image_file(std::string_view path) -> Result<DecodedImage> {
  const auto bytes = st::fs::read_bytes(path);
  if (!bytes.has_value()) {
    return st::unexpected(bytes.error().code,
                          std::format("读取图片 \"{}\" 失败：{}", path, bytes.error().message));
  }
  auto decoded = decode_image(*bytes);
  if (!decoded.has_value()) {
    return st::unexpected(decoded.error().code,
                          std::format("\"{}\"：{}", path, decoded.error().message));
  }
  return decoded;
}

}  // namespace st::codec
