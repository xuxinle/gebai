// 霜天 codec —— png.cpp：PNG 编解码（自研，仅用本层 deflate 与 st::hash 的 CRC-32）。
//
// 算法：
//   • 解码：8 字节签名 → chunk 迭代（长度/类型/数据/CRC-32 逐块校验）→ IHDR 校验 → 拼接全部 IDAT →
//     zlib_inflate → 逐行反过滤（None/Sub/Up/Average/Paeth，逐字节重建，bpp = 通道数）→ 输出非预乘 RGBA8。
//     颜色类型 0(gray)/2(RGB)/3(palette)/4(gray+alpha)/6(RGBA)，位深 8；tRNS 透明键（gray/RGB）
//     与调色板 alpha 表（palette）均支持。
//   • 编码：8 位 RGBA 真彩（颜色类型 6），行过滤恒为 None（0），zlib_deflate 压缩后写 IDAT；
//     每个 chunk 的 CRC-32 由 st::hash::crc32 计算（覆盖 type+data）。
//
// 限制（有意为之，非缺陷）：
//   • 位深只支持 8（1/2/4/16 位返回 Unsupported，不做位展开与 16→8 缩放）；
//   • 隔行（Adam7）返回 Unsupported；
//   • 未知/辅助 chunk（gAMA/sRGB/iCCP/tEXt…）忽略，不做色彩管理与元数据保留；
//   • 编码只做 None 过滤（压缩率非最优，但兼容性最好、可被任何 PNG 解码器解析）；
//   • 全部输入经 std::span 访问，越界即返回 Error，无指针算术、无异常、无第三方依赖。

#include "st/codec/png.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"

namespace st::codec {
namespace {

/// PNG 文件签名（`\x89PNG\r\n\x1a\n`）。
inline constexpr std::array<std::uint8_t, 8> kSignature{0x89U, 0x50U, 0x4EU, 0x47U,
                                                        0x0DU, 0x0AU, 0x1AU, 0x0AU};

inline constexpr std::uint32_t kChunkIhdr = 0x49484452U;  // "IHDR"
inline constexpr std::uint32_t kChunkPlte = 0x504C5445U;  // "PLTE"
inline constexpr std::uint32_t kChunkIdat = 0x49444154U;  // "IDAT"
inline constexpr std::uint32_t kChunkIend = 0x49454E44U;  // "IEND"
inline constexpr std::uint32_t kChunkTrns = 0x74524E53U;  // "tRNS"

/// 解码期输出上限（相对 256MB 的 deflate 上限，PNG 另按像素数二次校验）。
inline constexpr std::size_t kMaxDecodedBytes = 256u * 1024u * 1024u;

[[nodiscard]] auto read_be32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

void append_be32(Bytes& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

[[nodiscard]] auto chunk_name(std::uint32_t type) noexcept -> std::string_view {
  switch (type) {
    case kChunkIhdr: return "IHDR";
    case kChunkPlte: return "PLTE";
    case kChunkIdat: return "IDAT";
    case kChunkIend: return "IEND";
    case kChunkTrns: return "tRNS";
    default: return "?";
  }
}

/// 各颜色类型的通道数（位深 8 时即每像素字节数）。
[[nodiscard]] constexpr auto channel_count(std::uint8_t color_type) noexcept -> std::size_t {
  switch (color_type) {
    case 0: return 1;  // gray
    case 2: return 3;  // RGB
    case 3: return 1;  // palette index
    case 4: return 2;  // gray + alpha
    case 6: return 4;  // RGBA
    default: return 0;
  }
}

[[nodiscard]] constexpr auto is_supported_color_type(std::uint8_t color_type) noexcept -> bool {
  return color_type == 0 || color_type == 2 || color_type == 3 || color_type == 4 ||
         color_type == 6;
}

[[nodiscard]] constexpr auto abs_int(int value) noexcept -> int {
  return value < 0 ? -value : value;
}

/// PNG Paeth 预测（RFC 2083 §9.4）。
[[nodiscard]] constexpr auto paeth_predictor(std::uint8_t left, std::uint8_t up,
                                             std::uint8_t up_left) noexcept -> std::uint8_t {
  const int a = static_cast<int>(left);
  const int b = static_cast<int>(up);
  const int c = static_cast<int>(up_left);
  const int estimate = a + b - c;
  const int dist_left = abs_int(estimate - a);
  const int dist_up = abs_int(estimate - b);
  const int dist_up_left = abs_int(estimate - c);
  if (dist_left <= dist_up && dist_left <= dist_up_left) return left;
  if (dist_up <= dist_up_left) return up;
  return up_left;
}

/// IHDR 解析结果。
struct PngHeader {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint8_t bit_depth{0};
  std::uint8_t color_type{0};
  std::uint8_t interlace{0};
};

[[nodiscard]] auto parse_header(std::span<const std::uint8_t> data) -> Result<PngHeader> {
  if (data.size() != 13) {
    return st::unexpected(ErrorCode::Parse, "PNG IHDR 长度非 13 字节");
  }
  PngHeader header{};
  header.width = read_be32(data.first(4));
  header.height = read_be32(data.subspan(4, 4));
  header.bit_depth = data[8];
  header.color_type = data[9];
  if (header.width == 0 || header.height == 0) {
    return st::unexpected(ErrorCode::Parse, "PNG 尺寸为零");
  }
  if (data[10] != 0U) {
    return st::unexpected(ErrorCode::Unsupported, "PNG 压缩方法非 0（deflate）");
  }
  if (data[11] != 0U) {
    return st::unexpected(ErrorCode::Unsupported, "PNG 行过滤方法非 0（自适应过滤）");
  }
  header.interlace = data[12];
  if (header.interlace != 0U) {
    return st::unexpected(ErrorCode::Unsupported, "PNG 隔行（Adam7）尚不支持");
  }
  if (header.bit_depth != 8U) {
    return st::unexpected(ErrorCode::Unsupported,
                          std::format("PNG 位深 {} 尚不支持（仅 8 位）", header.bit_depth));
  }
  if (!is_supported_color_type(header.color_type)) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("PNG 颜色类型非法: {}", header.color_type));
  }
  return header;
}

/// 逐行反过滤（就地写入 `raw`，长度为 `height * stride`）。
[[nodiscard]] auto unfilter_rows(std::span<const std::uint8_t> filtered, std::uint32_t width,
                                 std::uint32_t height, std::size_t bytes_per_pixel, Bytes& raw)
    -> Status {
  const std::size_t stride = static_cast<std::size_t>(width) * bytes_per_pixel;
  const std::size_t expected = (stride + 1) * static_cast<std::size_t>(height);
  if (filtered.size() != expected) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("PNG 解压数据长度不符: {} 字节，期望 {}", filtered.size(),
                                      expected));
  }
  raw.assign(stride * static_cast<std::size_t>(height), 0);
  const std::span<std::uint8_t> target(raw);
  for (std::size_t row = 0; row < height; ++row) {
    const std::size_t filter = filtered[row * (stride + 1)];
    const std::span<const std::uint8_t> source = filtered.subspan(row * (stride + 1) + 1, stride);
    const std::size_t row_base = row * stride;
    const std::span<std::uint8_t> current = target.subspan(row_base, stride);
    const std::span<const std::uint8_t> previous =
        row == 0 ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>(raw).subspan(
                                                          (row - 1) * stride, stride);
    if (filter > 4) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("PNG 行过滤类型非法: {}（行 {}）", filter, row));
    }
    for (std::size_t at = 0; at < stride; ++at) {
      const std::uint8_t left = at >= bytes_per_pixel ? current[at - bytes_per_pixel] : 0U;
      const std::uint8_t up = previous.empty() ? 0U : previous[at];
      const std::uint8_t up_left =
          (!previous.empty() && at >= bytes_per_pixel) ? previous[at - bytes_per_pixel] : 0U;
      const std::uint8_t raw_byte = source[at];
      std::uint8_t value = raw_byte;
      switch (filter) {
        case 0: value = raw_byte; break;
        case 1: value = static_cast<std::uint8_t>(raw_byte + left); break;
        case 2: value = static_cast<std::uint8_t>(raw_byte + up); break;
        case 3: {
          const unsigned sum = static_cast<unsigned>(left) + static_cast<unsigned>(up);
          value = static_cast<std::uint8_t>(raw_byte + static_cast<std::uint8_t>(sum / 2U));
          break;
        }
        default:
          value = static_cast<std::uint8_t>(raw_byte + paeth_predictor(left, up, up_left));
          break;
      }
      current[at] = value;
    }
  }
  return st::ok();
}

/// `raw`（每像素 `channels` 字节、位深 8）→ 非预乘 RGBA8。
[[nodiscard]] auto expand_to_rgba(std::span<const std::uint8_t> raw, const PngHeader& header,
                                  std::span<const std::uint8_t> palette,
                                  std::span<const std::uint8_t> transparency) -> Result<Bytes> {
  const std::size_t pixels = static_cast<std::size_t>(header.width) * header.height;
  Bytes rgba(pixels * 4, 255U);
  const std::size_t channels = channel_count(header.color_type);
  for (std::size_t index = 0; index < pixels; ++index) {
    const std::span<const std::uint8_t> pixel = raw.subspan(index * channels, channels);
    const std::size_t at = index * 4;
    switch (header.color_type) {
      case 0: {  // gray（tRNS = 透明确切值，2 字节大端）
        const std::uint8_t gray = pixel[0];
        rgba[at] = gray;
        rgba[at + 1] = gray;
        rgba[at + 2] = gray;
        if (transparency.size() >= 2 && transparency[1] == gray) rgba[at + 3] = 0U;
        break;
      }
      case 2: {  // RGB（tRNS = 透明 RGB，6 字节大端）
        rgba[at] = pixel[0];
        rgba[at + 1] = pixel[1];
        rgba[at + 2] = pixel[2];
        if (transparency.size() >= 6 && transparency[1] == pixel[0] &&
            transparency[3] == pixel[1] && transparency[5] == pixel[2]) {
          rgba[at + 3] = 0U;
        }
        break;
      }
      case 3: {  // palette（tRNS = 调色板 alpha 表）
        const std::size_t entry = static_cast<std::size_t>(pixel[0]) * 3;
        if (entry + 2 >= palette.size()) {
          return st::unexpected(ErrorCode::Parse,
                                std::format("PNG 调色板索引越界: {}", pixel[0]));
        }
        rgba[at] = palette[entry];
        rgba[at + 1] = palette[entry + 1];
        rgba[at + 2] = palette[entry + 2];
        rgba[at + 3] = static_cast<std::size_t>(pixel[0]) < transparency.size()
                           ? transparency[pixel[0]]
                           : 255U;
        break;
      }
      case 4: {  // gray + alpha
        rgba[at] = pixel[0];
        rgba[at + 1] = pixel[0];
        rgba[at + 2] = pixel[0];
        rgba[at + 3] = pixel[1];
        break;
      }
      default: {  // 6: RGBA
        rgba[at] = pixel[0];
        rgba[at + 1] = pixel[1];
        rgba[at + 2] = pixel[2];
        rgba[at + 3] = pixel[3];
        break;
      }
    }
  }
  return rgba;
}

/// 写一个 chunk（长度 + 类型 + 数据 + CRC-32）。
[[nodiscard]] auto write_chunk(Bytes& out, std::uint32_t type, std::span<const std::uint8_t> data)
    -> Status {
  if (data.size() > 0x7FFFFFFFU) {
    return st::unexpected(ErrorCode::Overflow, "PNG chunk 数据超过 2^31-1 字节");
  }
  append_be32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t crc_from = out.size();
  append_be32(out, type);
  out.insert(out.end(), data.begin(), data.end());
  append_be32(out, st::hash::crc32(std::span<const std::uint8_t>(out).subspan(crc_from)));
  return st::ok();
}

}  // namespace

auto png_decode(std::span<const std::uint8_t> input) -> Result<PngImage> {
  if (input.size() < 8 + 25 || !std::equal(kSignature.begin(), kSignature.end(), input.begin())) {
    return st::unexpected(ErrorCode::Parse, "PNG 签名不符或文件过短");
  }

  PngHeader header{};
  bool have_header = false;
  bool have_end = false;
  Bytes palette;
  Bytes transparency;
  Bytes compressed;

  std::size_t pos = 8;
  while (pos + 12 <= input.size()) {
    const std::uint32_t length = read_be32(input.subspan(pos, 4));
    if (pos + 12 + static_cast<std::size_t>(length) > input.size()) {
      return st::unexpected(ErrorCode::Parse, "PNG chunk 长度越界");
    }
    const std::uint32_t type = read_be32(input.subspan(pos + 4, 4));
    const std::span<const std::uint8_t> data = input.subspan(pos + 8, length);
    const std::uint32_t expected_crc = read_be32(input.subspan(pos + 8 + length, 4));
    const std::uint32_t actual_crc = st::hash::crc32(input.subspan(pos + 4, 4 + length));
    if (expected_crc != actual_crc) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("PNG {} chunk CRC-32 校验失败（期望 {:08x} 实际 {:08x}）",
                                        chunk_name(type), expected_crc, actual_crc));
    }
    pos += 12 + static_cast<std::size_t>(length);

    if (type == kChunkIhdr) {
      if (have_header || !compressed.empty() || pos != (8 + 25)) {
        return st::unexpected(ErrorCode::Parse, "PNG IHDR 必须是首个且唯一的 chunk");
      }
      auto parsed = parse_header(data);
      if (!parsed) return st::forward_error(parsed.error());
      header = *parsed;
      have_header = true;
      const std::size_t raw_bytes =
          (static_cast<std::size_t>(header.width) * channel_count(header.color_type) + 1) *
          static_cast<std::size_t>(header.height);
      if (raw_bytes > kMaxDecodedBytes) {
        return st::unexpected(ErrorCode::Overflow,
                              std::format("PNG 图像数据过大: {} 字节（上限 {}）", raw_bytes,
                                          kMaxDecodedBytes));
      }
      continue;
    }
    if (type == kChunkIdat) {
      if (!have_header) {
        return st::unexpected(ErrorCode::Parse, "PNG IDAT 出现在 IHDR 之前");
      }
      if (compressed.size() + data.size() > kMaxDecodedBytes) {
        return st::unexpected(ErrorCode::Overflow, "PNG IDAT 累积数据过大");
      }
      compressed.insert(compressed.end(), data.begin(), data.end());
      continue;
    }
    if (type == kChunkPlte) {
      if (data.empty() || data.size() % 3 != 0 || data.size() > 256 * 3) {
        return st::unexpected(ErrorCode::Parse, "PNG PLTE 长度非法（应为 3 的倍数且 ≤ 768）");
      }
      palette.assign(data.begin(), data.end());
      continue;
    }
    if (type == kChunkTrns) {
      transparency.assign(data.begin(), data.end());
      continue;
    }
    if (type == kChunkIend) {
      have_end = true;
      break;
    }
    // 其余 chunk（gAMA/sRGB/iCCP/tEXt/zTXt/…）忽略：本层不做色彩管理与元数据保留。
  }

  if (!have_header) {
    return st::unexpected(ErrorCode::Parse, "PNG 缺少 IHDR");
  }
  if (!have_end) {
    return st::unexpected(ErrorCode::Parse, "PNG 缺少 IEND（文件截断）");
  }
  if (compressed.empty()) {
    return st::unexpected(ErrorCode::Parse, "PNG 缺少 IDAT 图像数据");
  }
  if (header.color_type == 3 && palette.empty()) {
    return st::unexpected(ErrorCode::Parse, "PNG 调色板图像缺少 PLTE");
  }

  auto inflated = zlib_inflate(compressed, kMaxDecodedBytes);
  if (!inflated) return st::forward_error(inflated.error());

  const std::size_t channels = channel_count(header.color_type);
  Bytes raw;
  const Status status =
      unfilter_rows(*inflated, header.width, header.height, channels, raw);
  if (!status) return st::forward_error(status.error());

  auto rgba = expand_to_rgba(raw, header, palette, transparency);
  if (!rgba) return st::forward_error(rgba.error());

  PngImage image;
  image.width = header.width;
  image.height = header.height;
  image.rgba = std::move(*rgba);
  return image;
}

auto png_encode(const PngImage& image, int level) -> Result<Bytes> {
  if (image.width == 0 || image.height == 0) {
    return st::unexpected(ErrorCode::Invalid, "PNG 尺寸不可为零");
  }
  const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
  if (image.rgba.size() != pixels * 4) {
    return st::unexpected(ErrorCode::Invalid,
                          std::format("PNG 像素缓冲长度不符: {} 字节，期望 {}", image.rgba.size(),
                                      pixels * 4));
  }
  if (pixels * 4 > kMaxDecodedBytes) {
    return st::unexpected(ErrorCode::Overflow, "PNG 图像数据过大");
  }

  const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
  Bytes raw;
  raw.reserve(stride * static_cast<std::size_t>(image.height) + image.height);
  for (std::size_t row = 0; row < image.height; ++row) {
    raw.push_back(0U);  // 行过滤 None
    const std::span<const std::uint8_t> line(image.rgba);
    raw.insert(raw.end(), line.begin() + static_cast<std::ptrdiff_t>(row * stride),
               line.begin() + static_cast<std::ptrdiff_t>((row + 1) * stride));
  }

  auto compressed = zlib_deflate(raw, level);
  if (!compressed) return st::forward_error(compressed.error());

  const std::array<std::uint8_t, 13> ihdr{
      static_cast<std::uint8_t>((image.width >> 24U) & 0xFFU),
      static_cast<std::uint8_t>((image.width >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((image.width >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(image.width & 0xFFU),
      static_cast<std::uint8_t>((image.height >> 24U) & 0xFFU),
      static_cast<std::uint8_t>((image.height >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((image.height >> 8U) & 0xFFU),
      static_cast<std::uint8_t>(image.height & 0xFFU),
      8U,  // 位深
      6U,  // 颜色类型 RGBA
      0U,  // 压缩方法
      0U,  // 过滤方法
      0U};  // 非隔行

  Bytes out;
  out.reserve(compressed->size() + 64);
  out.insert(out.end(), kSignature.begin(), kSignature.end());
  Status status = write_chunk(out, kChunkIhdr, ihdr);
  if (!status) return st::forward_error(status.error());
  status = write_chunk(out, kChunkIdat, *compressed);
  if (!status) return st::forward_error(status.error());
  status = write_chunk(out, kChunkIend, std::span<const std::uint8_t>{});
  if (!status) return st::forward_error(status.error());
  return out;
}

auto png_read_file(std::string_view path) -> Result<PngImage> {
  auto bytes = st::fs::read_bytes(path);
  if (!bytes) return st::forward_error(bytes.error());
  return png_decode(*bytes);
}

auto png_write_file(std::string_view path, const PngImage& image, int level) -> Status {
  auto encoded = png_encode(image, level);
  if (!encoded) return st::forward_error(encoded.error());
  return st::fs::write_bytes(path, *encoded);
}

}  // namespace st::codec
