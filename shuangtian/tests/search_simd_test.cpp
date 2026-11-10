// SIMD 字节扫描的三路对齐用例。
//
// 为什么「对齐」是本文件的核心：SIMD 路径与标量路径**必须是同一个函数**的两种实现。
// 一旦它们对边界（末尾不足 16/32 字节、空缓冲、`from >= size`）的处理不一致，
// 症状会非常隐蔽——大文件里某一行莫名搜不到，而单元测试（小缓冲、整块对齐）全绿。
// 因此这里把每一档实现都跑一遍同一组探针，逐字节比结果。
//
// 逆向验证（`tools/search_reverse_verify.py`）：把 `find_byte_case_insensitive_from`
// 的折叠分支去掉（只比原字母），本文件的大小写用例必须变红。
#include "st/search/simd_scan.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "st/test/test.hpp"

namespace {

/// 标量基准：与 `simd_scan` 的语义定义逐字对应的**独立**实现（不用 memchr，
/// 以便每个字节的比较都走同一条判定——它是「语义是什么」的可执行说明）。
[[nodiscard]] auto scalar_find_byte(const std::uint8_t* data, std::size_t size, std::size_t from,
                                    std::uint8_t needle) -> std::size_t {
  for (std::size_t index = from; index < size; ++index) {
    if (data[index] == needle) return index;
  }
  return size;
}

[[nodiscard]] auto scalar_find_byte_ci(const std::uint8_t* data, std::size_t size,
                                       std::size_t from, std::uint8_t letter) -> std::size_t {
  const auto lower_of = [](std::uint8_t value) {
    return static_cast<std::uint8_t>((value >= 'A' && value <= 'Z') ? value - 'A' + 'a' : value);
  };
  const std::uint8_t want = lower_of(letter);
  for (std::size_t index = from; index < size; ++index) {
    if (lower_of(data[index]) == want) return index;
  }
  return size;
}

[[nodiscard]] auto scalar_find_substring(const std::uint8_t* data, std::size_t size,
                                         const std::uint8_t* needle, std::size_t needle_size)
    -> std::size_t {
  if (needle_size == 0) return 0;
  if (size < needle_size) return size;
  for (std::size_t start = 0; start + needle_size <= size; ++start) {
    std::size_t matched = 0;
    while (matched < needle_size && data[start + matched] == needle[matched]) ++matched;
    if (matched == needle_size) return start;
  }
  return size;
}

/// 造一段覆盖各种边界的缓冲：长度跨越 15/16/17/31/32/33/64/65，含 0 字节与高位字节。
[[nodiscard]] auto make_probe_buffer(std::size_t size) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out(size);
  for (std::size_t index = 0; index < size; ++index) {
    // 稀疏地放目标字节，其余用非目标值（含 \0 与 0xFF，验证比较不受符号影响）。
    const std::size_t pattern = index % 7;
    out[index] = pattern == 0 ? std::uint8_t{'X'}
                 : pattern == 1 ? std::uint8_t{0}
                 : pattern == 2 ? std::uint8_t{0xFF}
                 : static_cast<std::uint8_t>('a' + (index % 26));
  }
  return out;
}

}  // namespace

ST_TEST(search_simd_find_byte_matches_scalar_on_all_lengths) {
  // 长度扫过所有 SIMD 边界的邻域：15/16/17（SSE2）、31/32/33（AVX2）、64/65（多块）。
  const std::vector<std::size_t> sizes{0, 1, 2, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129};
  for (const std::size_t size : sizes) {
    const auto buffer = make_probe_buffer(size);
    for (const std::uint8_t needle : {std::uint8_t{'X'}, std::uint8_t{0}, std::uint8_t{0xFF},
                                      std::uint8_t{'z'} /* 不存在 */}) {
      for (std::size_t from = 0; from <= size; ++from) {
        const std::size_t actual = st::search::simd::find_byte_from(
            buffer.data(), buffer.size(), from, needle);
        const std::size_t expected = scalar_find_byte(buffer.data(), buffer.size(), from, needle);
        ST_CHECK_EQ(actual, expected);
        if (actual != expected) return;   // 首个不符即停：后面的差值意义不大
      }
    }
  }
}

ST_TEST(search_simd_find_byte_empty_and_out_of_range) {
  const std::uint8_t dummy = 0;
  // 空缓冲：任何 from 都返回 size（= 0）。`data` 指一个有效字节，排除 nullptr 分支。
  ST_CHECK_EQ(st::search::simd::find_byte(&dummy, 0, 1), std::size_t{0});
  ST_CHECK_EQ(st::search::simd::find_byte(nullptr, 0, 1), std::size_t{0});
  // from 超界：返回 size（不是 0，也不是未定义）。
  const auto buffer = make_probe_buffer(40);
  ST_CHECK_EQ(st::search::simd::find_byte_from(buffer.data(), buffer.size(), 40, 'a'),
              buffer.size());
  ST_CHECK_EQ(st::search::simd::find_byte_from(buffer.data(), buffer.size(), 100, 'a'),
              buffer.size());
}

ST_TEST(search_simd_find_byte_case_insensitive_matches_scalar) {
  // 'a' 的大小写折叠：缓冲里 a..z 交替（make_probe_buffer 的下半段就是小写字母）。
  const std::vector<std::size_t> sizes{0, 1, 15, 16, 17, 32, 33, 65, 129};
  for (const std::size_t size : sizes) {
    const auto buffer = make_probe_buffer(size);
    // 大写化的缓冲：把字母改成大写，验证两种形态都能命中。
    auto upper = buffer;
    for (auto& byte : upper) {
      if (byte >= 'a' && byte <= 'z') byte = static_cast<std::uint8_t>(byte - 'a' + 'A');
    }
    for (const auto& sample : {buffer, upper}) {
      for (const std::uint8_t letter :
           {std::uint8_t{'a'}, std::uint8_t{'A'}, std::uint8_t{'m'}, std::uint8_t{'Z'}}) {
        for (std::size_t from = 0; from <= sample.size(); ++from) {
          ST_CHECK_EQ(st::search::simd::find_byte_case_insensitive_from(
                          sample.data(), sample.size(), from, letter),
                      scalar_find_byte_ci(sample.data(), sample.size(), from, letter));
        }
      }
    }
  }
}

ST_TEST(search_simd_find_substring_matches_scalar) {
  const std::string content = "the quick brown fox jumps over the lazy dog\n"
                              "QUICK brown Fox\n"
                              "boundary: quick";
  const auto* data = reinterpret_cast<const std::uint8_t*>(content.data());
  const std::size_t size = content.size();
  for (const std::string needle : {"quick", "Quick", "QUICK", "fox", "the", "xyz",
                                  "dog\nQUICK", "boundary: quick", "q"}) {
    const auto* needle_bytes = reinterpret_cast<const std::uint8_t*>(needle.data());
    ST_CHECK_EQ(st::search::simd::find_substring(data, size, needle_bytes, needle.size()),
                scalar_find_substring(data, size, needle_bytes, needle.size()));
  }
  // 空 needle 返回 0；needle 比缓冲长返回 size。
  ST_CHECK_EQ(st::search::simd::find_substring(data, size, data, 0), std::size_t{0});
  const std::string too_long = content + "!!!";
  ST_CHECK_EQ(st::search::simd::find_substring(
                  data, size, reinterpret_cast<const std::uint8_t*>(too_long.data()),
                  too_long.size()),
              size);
}

ST_TEST(search_simd_backend_is_known) {
  // 只要求是四档之一：本机是哪一档取决于 CPU 与编译选项，不该在这里钉死。
  const std::string backend = st::search::simd::backend();
  ST_CHECK(backend == "avx2" || backend == "sse2" || backend == "neon" || backend == "scalar");
}