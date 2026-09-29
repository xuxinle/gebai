// 霜天 codec 单测 —— PNG 编解码。
//
// 覆盖：编码→解码往返、固定交叉向量（由系统 python3 用 zlib+struct 手工构造：RGBA 行过滤 0/1/4 混合、
// 调色板 + tRNS）、隔行/非法位深报 Unsupported、坏数据防御（签名/chunk CRC/截断/位翻转）、
// 非法编码入参、以及 fs 集成（png_write_file / png_read_file 往返）。
//
// 说明：ST_TEST / ST_CHECK 等函数式宏是 `CONVENTIONS.md` §3.6 登记的测试框架例外。

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/test/test.hpp"

namespace {

using st::codec::Bytes;
using st::codec::PngImage;

/// 十六进制文本 → 字节（固定向量用）。
[[nodiscard]] auto from_hex(std::string_view text) -> Bytes {
  auto parsed = st::hash::from_hex(text);
  if (!parsed) return Bytes{};
  return *parsed;
}

/// 构造确定性测试图像（每像素 RGBA 各不相同，便于定位行列错位）。
[[nodiscard]] auto make_image(std::uint32_t width, std::uint32_t height) -> PngImage {
  PngImage image;
  image.width = width;
  image.height = height;
  image.rgba.resize(static_cast<std::size_t>(width) * height * 4U);
  for (std::uint32_t row = 0; row < height; ++row) {
    for (std::uint32_t column = 0; column < width; ++column) {
      const std::size_t at = (static_cast<std::size_t>(row) * width + column) * 4U;
      image.rgba[at] = static_cast<std::uint8_t>(column * 17U + 3U);
      image.rgba[at + 1] = static_cast<std::uint8_t>(row * 29U + 7U);
      image.rgba[at + 2] = static_cast<std::uint8_t>(column * 5U + row * 11U);
      image.rgba[at + 3] = static_cast<std::uint8_t>((column + row) % 2U == 0U ? 255U : 96U);
    }
  }
  return image;
}

/// 由系统 python3 构造：4×3 RGBA8，第 0/1/2 行分别用 filter None / Sub / Paeth。
inline constexpr std::string_view kSystemRgbaPngHex =
    "89504e470d0a1a0a0000000d4948445200000004000000030806000000b4f4aec6"
    "0000003149444154789c63e01261fd6f24a2dc1025e2f8bf4924be81912b40b9418341ae1e881b41340b838d5c3d039003c4"
    "601a003eb50b36ed96f8370000000049454e44ae426082";
/// 上图像素（非预乘 RGBA8，行优先）。
inline constexpr std::string_view kSystemRgbaPixelsHex =
    "0a1405ff321423805a1441ff82145f800a502380325041ff5a505f8082507dff0a8c41ff328c5f805a8c7dff828c9b80";

/// 由系统 python3 构造：4×1 调色板 PNG（PLTE 4 色 + tRNS alpha 表）。
inline constexpr std::string_view kSystemPalettePngHex =
    "89504e470d0a1a0a0000000d4948445200000004000000010803000000cee2ffff"
    "0000000c504c5445ff000000ff000000fffffffffb0060f6"
    "0000000474524e53ff8000ffa1a19466"
    "0000000d49444154789c63606064620600000f0007848e996d0000000049454e44ae426082";
/// 上图像素（palette + tRNS 展开后的 RGBA）。
inline constexpr std::string_view kSystemPalettePixelsHex =
    "ff0000ff00ff00800000ff00ffffffff";

/// 同 RGBA 向量但 IHDR interlace = 1（Adam7），必须报 Unsupported。
inline constexpr std::string_view kSystemInterlacedPngHex =
    "89504e470d0a1a0a0000000d4948445200000004000000030806000001c3f39e50"
    "0000003149444154789c63e01261fd6f24a2dc1025e2f8bf4924be81912b40b9418341ae1e881b41340b838d5c3d039003c4"
    "601a003eb50b36ed96f8370000000049454e44ae426082";

}  // namespace

ST_TEST(codec_png_encode_decode_roundtrip) {
  for (const std::uint32_t width : {1U, 3U, 17U, 64U}) {
    for (const std::uint32_t height : {1U, 2U, 33U}) {
      const PngImage source = make_image(width, height);
      auto encoded = st::codec::png_encode(source, 6);
      ST_REQUIRE(encoded.has_value());
      auto decoded = st::codec::png_decode(*encoded);
      ST_REQUIRE(decoded.has_value());
      ST_CHECK_EQ(decoded->width, width);
      ST_CHECK_EQ(decoded->height, height);
      ST_CHECK(decoded->rgba == source.rgba);
    }
  }
}

ST_TEST(codec_png_decode_system_rgba_vector) {
  const Bytes png = from_hex(kSystemRgbaPngHex);
  auto decoded = st::codec::png_decode(png);
  ST_REQUIRE(decoded.has_value());
  ST_CHECK_EQ(decoded->width, 4U);
  ST_CHECK_EQ(decoded->height, 3U);
  ST_CHECK(decoded->rgba == from_hex(kSystemRgbaPixelsHex));
}

ST_TEST(codec_png_decode_system_palette_trns_vector) {
  const Bytes png = from_hex(kSystemPalettePngHex);
  auto decoded = st::codec::png_decode(png);
  ST_REQUIRE(decoded.has_value());
  ST_CHECK_EQ(decoded->width, 4U);
  ST_CHECK_EQ(decoded->height, 1U);
  ST_CHECK(decoded->rgba == from_hex(kSystemPalettePixelsHex));
}

ST_TEST(codec_png_rejects_interlaced) {
  auto decoded = st::codec::png_decode(from_hex(kSystemInterlacedPngHex));
  ST_REQUIRE(!decoded.has_value());
  ST_CHECK(decoded.error().code == st::ErrorCode::Unsupported);
}

ST_TEST(codec_png_rejects_corruption) {
  const Bytes png = from_hex(kSystemRgbaPngHex);
  const Bytes expected = from_hex(kSystemRgbaPixelsHex);

  // 非 PNG 数据
  ST_CHECK(!st::codec::png_decode(Bytes{}).has_value());
  ST_CHECK(!st::codec::png_decode(Bytes(64, 0U)).has_value());

  // 任意截断都必须被拒
  std::size_t accepted = 0;
  for (std::size_t cut = 0; cut < png.size(); ++cut) {
    auto decoded = st::codec::png_decode(std::span<const std::uint8_t>(png).first(cut));
    if (decoded) ++accepted;
  }
  ST_CHECK_EQ(accepted, std::size_t{0});

  // 单字节位翻转：不崩溃；若仍可解则像素必须与原文一致（CRC-32 逐块兜底）
  std::size_t still_valid = 0;
  for (std::size_t at = 0; at < png.size(); ++at) {
    Bytes corrupted = png;
    corrupted[at] = static_cast<std::uint8_t>(corrupted[at] ^ 0xA5U);
    auto decoded = st::codec::png_decode(corrupted);
    if (decoded) {
      ++still_valid;
      ST_CHECK(decoded->rgba == expected);
    }
  }
  ST_CHECK(still_valid < png.size());
}

ST_TEST(codec_png_rejects_invalid_encode_input) {
  PngImage empty;
  ST_CHECK(st::codec::png_encode(empty).error().code == st::ErrorCode::Invalid);

  PngImage mismatched;
  mismatched.width = 4;
  mismatched.height = 4;
  mismatched.rgba.assign(10, 0U);
  ST_CHECK(st::codec::png_encode(mismatched).error().code == st::ErrorCode::Invalid);

  PngImage zero_height;
  zero_height.width = 2;
  zero_height.height = 0;
  ST_CHECK(st::codec::png_encode(zero_height).error().code == st::ErrorCode::Invalid);
}

ST_TEST(codec_png_single_row_and_column) {
  // 行过滤边界：宽度 1（bpp 等于 stride）与单行图像
  const PngImage single_column = make_image(1, 8);
  auto encoded = st::codec::png_encode(single_column, 9);
  ST_REQUIRE(encoded.has_value());
  auto decoded = st::codec::png_decode(*encoded);
  ST_REQUIRE(decoded.has_value());
  ST_CHECK(decoded->rgba == single_column.rgba);

  const PngImage single_row = make_image(64, 1);
  auto encoded_row = st::codec::png_encode(single_row, 0);
  ST_REQUIRE(encoded_row.has_value());
  auto decoded_row = st::codec::png_decode(*encoded_row);
  ST_REQUIRE(decoded_row.has_value());
  ST_CHECK(decoded_row->rgba == single_row.rgba);
}

ST_TEST(codec_png_file_roundtrip) {
  auto dir = st::fs::make_temp_dir("st-codec-png");
  ST_REQUIRE(dir.has_value());
  const std::string path = st::fs::join(*dir, "image.png");

  const PngImage source = make_image(23, 11);
  const st::Status written = st::codec::png_write_file(path, source, 6);
  ST_REQUIRE(written.has_value());
  ST_CHECK(st::fs::is_regular_file(path));

  auto loaded = st::codec::png_read_file(path);
  ST_REQUIRE(loaded.has_value());
  ST_CHECK_EQ(loaded->width, source.width);
  ST_CHECK_EQ(loaded->height, source.height);
  ST_CHECK(loaded->rgba == source.rgba);

  auto missing = st::codec::png_read_file(st::fs::join(*dir, "absent.png"));
  ST_CHECK(!missing.has_value());
  ST_CHECK(missing.error().code == st::ErrorCode::NotFound);

  const st::Status cleaned = st::fs::remove_all(*dir);
  ST_CHECK(cleaned.has_value());
}
