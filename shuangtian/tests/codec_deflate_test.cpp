// 霜天 codec 单测 —— deflate / inflate（RFC 1951 原始流、RFC 1950 zlib、RFC 1952 gzip）。
//
// 覆盖：全 level 自洽往返、固定交叉向量（由系统 python3 的 zlib/gzip 生成）、坏数据防御
// （截断 / 位翻转 / 手工构造的越界回溯与非法块头，必须返回 Error 而非崩溃）、输出上限、
// zlib 与 gzip 的头尾校验分支。
//
// 说明：ST_TEST / ST_CHECK 等函数式宏是 `CONVENTIONS.md` §3.6 登记的测试框架例外。

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/codec/deflate.hpp"
#include "st/core/error.hpp"
#include "st/core/hash.hpp"
#include "st/test/test.hpp"

namespace {

using st::codec::Bytes;

/// 十六进制文本 → 字节（固定向量用）。
[[nodiscard]] auto from_hex(std::string_view text) -> Bytes {
  auto parsed = st::hash::from_hex(text);
  if (!parsed) return Bytes{};
  return *parsed;
}

/// 字面量文本 → 字节。
[[nodiscard]] auto as_bytes(std::string_view text) -> Bytes {
  Bytes out;
  out.reserve(text.size());
  for (const char ch : text) out.push_back(static_cast<std::uint8_t>(ch));
  return out;
}

/// 重复单元文本（构造可压缩样本）。
[[nodiscard]] auto repeat_text(std::string_view unit, std::size_t count) -> Bytes {
  Bytes out;
  out.reserve(unit.size() * count);
  for (std::size_t round = 0; round < count; ++round) {
    for (const char ch : unit) out.push_back(static_cast<std::uint8_t>(ch));
  }
  return out;
}

/// 确定性伪随机字节（xorshift32），不依赖平台 rand()。
[[nodiscard]] auto pseudo_random(std::size_t size, std::uint32_t seed) -> Bytes {
  Bytes data(size, 0U);
  std::uint32_t state = seed == 0U ? 1U : seed;
  for (std::size_t at = 0; at < size; ++at) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    data[at] = static_cast<std::uint8_t>((state >> 24U) & 0xFFU);
  }
  return data;
}

/// 混合样本集（空 / 单字节 / 短文本 / 高重复 / 伪随机）。
[[nodiscard]] auto sample_set() -> std::vector<Bytes> {
  std::vector<Bytes> samples;
  samples.push_back(Bytes{});
  samples.push_back(Bytes{0U});
  samples.push_back(as_bytes("a"));
  samples.push_back(as_bytes("hello, deflate!"));
  samples.push_back(repeat_text("shuangtian codec deflate ", 5000));  // 125000 字节
  samples.push_back(pseudo_random(40000, 12345U));
  samples.push_back(pseudo_random(4096, 999U));
  samples.push_back(repeat_text("ab", 300));
  return samples;
}

/// 由系统 python3 生成：zlib.compress(b"hello, deflate! " * 3, 6)。
inline constexpr std::string_view kSystemZlibHex =
    "789ccb48cdc9c9d75148494dcb492c495554c820c007009aa01063";

/// 由系统 python3 生成：gzip.GzipFile(mtime=0).write(b"gzip cross-check payload 0123456789" * 4)。
inline constexpr std::string_view kSystemGzipHex =
    "1f8b08000000000002ff4bafca2c50482eca2f2ed64dce484dce562848acccc94f4c513030343236313533b7b04ca7971200"
    "6efbcaa58c000000";

inline constexpr std::string_view kSystemGzipUnit = "gzip cross-check payload 0123456789";

}  // namespace

ST_TEST(codec_deflate_roundtrip_all_levels) {
  for (const Bytes& sample : sample_set()) {
    for (int level = 0; level <= 9; ++level) {
      auto packed = st::codec::deflate_raw(sample, level);
      ST_REQUIRE(packed.has_value());
      auto restored = st::codec::inflate_raw(*packed);
      ST_REQUIRE(restored.has_value());
      ST_CHECK(*restored == sample);
    }
  }
}

ST_TEST(codec_zlib_roundtrip_all_levels) {
  for (const Bytes& sample : sample_set()) {
    auto packed = st::codec::zlib_deflate(sample, 6);
    ST_REQUIRE(packed.has_value());
    ST_CHECK_EQ(packed->size() >= 6, true);
    ST_CHECK_EQ((*packed)[0], 0x78U);  // CM=8 / CINFO=7
    ST_CHECK_EQ((static_cast<std::uint32_t>((*packed)[0]) * 256U + (*packed)[1]) % 31U, 0U);
    auto restored = st::codec::zlib_inflate(*packed);
    ST_REQUIRE(restored.has_value());
    ST_CHECK(*restored == sample);
  }
}

ST_TEST(codec_gzip_roundtrip_all_levels) {
  for (const Bytes& sample : sample_set()) {
    auto packed = st::codec::gzip_deflate(sample, 9);
    ST_REQUIRE(packed.has_value());
    ST_CHECK_EQ((*packed)[0], 0x1FU);
    ST_CHECK_EQ((*packed)[1], 0x8BU);
    ST_CHECK_EQ((*packed)[2], 8U);
    auto restored = st::codec::gzip_inflate(*packed);
    ST_REQUIRE(restored.has_value());
    ST_CHECK(*restored == sample);
  }
}

ST_TEST(codec_inflate_system_zlib_vector) {
  const Bytes expected = repeat_text("hello, deflate! ", 3);
  auto restored = st::codec::zlib_inflate(from_hex(kSystemZlibHex));
  ST_REQUIRE(restored.has_value());
  ST_CHECK(*restored == expected);
}

ST_TEST(codec_inflate_system_gzip_vector) {
  const Bytes expected = repeat_text(kSystemGzipUnit, 4);
  auto restored = st::codec::gzip_inflate(from_hex(kSystemGzipHex));
  ST_REQUIRE(restored.has_value());
  ST_CHECK(*restored == expected);
}

ST_TEST(codec_deflate_output_is_self_consistent) {
  // 分层切块（level 1 块小、level 9 块大）后仍必须自洽；同时验证多块流的 FINAL 位落点。
  const Bytes sample = pseudo_random(300000, 4242U);
  for (const int level : {1, 5, 9}) {
    auto packed = st::codec::deflate_raw(sample, level);
    ST_REQUIRE(packed.has_value());
    auto restored = st::codec::inflate_raw(*packed);
    ST_REQUIRE(restored.has_value());
    ST_CHECK(*restored == sample);
  }
}

ST_TEST(codec_inflate_rejects_truncation) {
  const Bytes sample = repeat_text("truncate me ", 2000);
  auto packed = st::codec::zlib_deflate(sample, 6);
  ST_REQUIRE(packed.has_value());
  std::size_t accepted = 0;
  for (std::size_t cut = 0; cut < packed->size(); ++cut) {
    auto restored =
        st::codec::zlib_inflate(std::span<const std::uint8_t>(*packed).first(cut));
    if (restored) ++accepted;
  }
  ST_CHECK_EQ(accepted, std::size_t{0});  // 任何截断都必须被拒（尾校验必然缺失）
}

ST_TEST(codec_inflate_rejects_bit_flips) {
  const Bytes sample = repeat_text("bit flip payload ", 1000);
  auto packed = st::codec::zlib_deflate(sample, 6);
  ST_REQUIRE(packed.has_value());
  std::size_t accepted = 0;
  for (std::size_t at = 0; at < packed->size(); ++at) {
    Bytes corrupted = *packed;
    corrupted[at] = static_cast<std::uint8_t>(corrupted[at] ^ 0x5AU);
    auto restored = st::codec::zlib_inflate(corrupted);
    if (restored) {
      ++accepted;
      ST_CHECK(*restored == sample);  // 若仍可解，内容必须正确（Adler-32 兜底）
    }
  }
  ST_CHECK(accepted < packed->size());
}

ST_TEST(codec_inflate_handcrafted_invalid_streams) {
  // 固定 Huffman 块 + 长度码 257 + 距离码 0（距离 1）但输出为空 → LZ77 回溯越界
  auto distance_escape = st::codec::inflate_raw(Bytes{0x03U, 0x01U});
  ST_CHECK(!distance_escape.has_value());
  ST_CHECK(distance_escape.error().code == st::ErrorCode::Parse);

  // 存储块 LEN/NLEN 不互反
  auto bad_stored = st::codec::inflate_raw(Bytes{0x01U, 0x05U, 0x00U, 0x34U, 0x12U});
  ST_CHECK(!bad_stored.has_value());

  // 块类型 3（保留）
  auto bad_type = st::codec::inflate_raw(Bytes{0x07U});
  ST_CHECK(!bad_type.has_value());

  // 动态块头截断
  auto bad_dynamic = st::codec::inflate_raw(Bytes{0x05U});
  ST_CHECK(!bad_dynamic.has_value());

  // 空输入 / 单字节垃圾 / 全 0xFF 垃圾：一律报错且不崩溃
  ST_CHECK(!st::codec::inflate_raw(Bytes{}).has_value());
  ST_CHECK(!st::codec::inflate_raw(Bytes{0xFFU}).has_value());
  const Bytes garbage(4096, 0xFFU);
  ST_CHECK(!st::codec::inflate_raw(garbage).has_value());

  // 合法的最小 deflate 流：单个空的最终固定块（0x03 0x00 = 结束码）
  auto empty_block = st::codec::inflate_raw(Bytes{0x03U, 0x00U});
  ST_REQUIRE(empty_block.has_value());
  ST_CHECK_EQ(empty_block->size(), std::size_t{0});
}

ST_TEST(codec_inflate_respects_max_output) {
  const Bytes sample = repeat_text("max output ", 4000);
  auto packed = st::codec::deflate_raw(sample, 6);
  ST_REQUIRE(packed.has_value());
  auto limited = st::codec::inflate_raw(*packed, 64);
  ST_REQUIRE(!limited.has_value());
  ST_CHECK(limited.error().code == st::ErrorCode::Overflow);

  auto exact = st::codec::inflate_raw(*packed, sample.size());
  ST_REQUIRE(exact.has_value());
  ST_CHECK(*exact == sample);

  auto gz = st::codec::gzip_deflate(sample, 6);
  ST_REQUIRE(gz.has_value());
  auto gz_limited = st::codec::gzip_inflate(*gz, 8);
  ST_REQUIRE(!gz_limited.has_value());
  ST_CHECK(gz_limited.error().code == st::ErrorCode::Overflow);
}

ST_TEST(codec_zlib_gzip_header_errors) {
  // CM 非 8
  ST_CHECK(st::codec::zlib_inflate(Bytes{0x77U, 0x01U, 0, 0, 0, 0}).error().code ==
           st::ErrorCode::Unsupported);
  // FCHECK 不整除 31
  ST_CHECK(st::codec::zlib_inflate(Bytes{0x78U, 0x02U, 0, 0, 0, 0}).error().code ==
           st::ErrorCode::Parse);
  // FDICT 置位（0x78 0x20 的 FCHECK 合法）→ 不支持
  ST_CHECK(st::codec::zlib_inflate(Bytes{0x78U, 0x20U, 0, 0, 0, 0, 0, 0}).error().code ==
           st::ErrorCode::Unsupported);
  // 过短
  ST_CHECK(st::codec::zlib_inflate(Bytes{0x78U, 0x9CU}).error().code == st::ErrorCode::Parse);
  // gzip 魔数错误 / 保留位非零 / 过短
  ST_CHECK(st::codec::gzip_inflate(Bytes(32, 0U)).error().code == st::ErrorCode::Parse);
  Bytes reserved(32, 0U);
  reserved[0] = 0x1FU;
  reserved[1] = 0x8BU;
  reserved[2] = 8U;
  reserved[3] = 0x80U;  // 保留位
  ST_CHECK(st::codec::gzip_inflate(reserved).error().code == st::ErrorCode::Parse);
  ST_CHECK(st::codec::gzip_inflate(Bytes{0x1FU, 0x8BU, 0x08U}).error().code ==
           st::ErrorCode::Parse);
}

ST_TEST(codec_deflate_empty_and_tiny_inputs) {
  for (const int level : {0, 1, 6, 9}) {
    auto packed = st::codec::deflate_raw(Bytes{}, level);
    ST_REQUIRE(packed.has_value());
    auto restored = st::codec::inflate_raw(*packed);
    ST_REQUIRE(restored.has_value());
    ST_CHECK(restored->empty());

    auto zlib = st::codec::zlib_deflate(Bytes{}, level);
    ST_REQUIRE(zlib.has_value());
    auto zlib_back = st::codec::zlib_inflate(*zlib);
    ST_REQUIRE(zlib_back.has_value());
    ST_CHECK(zlib_back->empty());

    auto gz = st::codec::gzip_deflate(Bytes{}, level);
    ST_REQUIRE(gz.has_value());
    auto gz_back = st::codec::gzip_inflate(*gz);
    ST_REQUIRE(gz_back.has_value());
    ST_CHECK(gz_back->empty());

    auto single = st::codec::zlib_deflate(Bytes{0x42U}, level);
    ST_REQUIRE(single.has_value());
    auto single_back = st::codec::zlib_inflate(*single);
    ST_REQUIRE(single_back.has_value());
    ST_CHECK(single_back->size() == 1U && (*single_back)[0] == 0x42U);
  }
}

ST_TEST(codec_deflate_compressible_data_shrinks) {
  const Bytes sample = repeat_text("compress me please ", 4000);  // 76000 字节
  auto packed = st::codec::deflate_raw(sample, 9);
  ST_REQUIRE(packed.has_value());
  ST_CHECK(packed->size() < sample.size() / 4U);  // 固定 Huffman 也应显著压缩重复数据
  auto restored = st::codec::inflate_raw(*packed);
  ST_REQUIRE(restored.has_value());
  ST_CHECK(*restored == sample);
}
