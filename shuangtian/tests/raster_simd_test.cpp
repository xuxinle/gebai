// SIMD 快路径对齐测试：fill_row / blend_row / erase_row 的 SIMD 路径必须与
// 标量语义逐像素一致，且 backend() 自报与实际路径吻合。
//
// 为什么值得测：simd.cpp 是 CONVENTIONS §2 的受控例外（指针运算 + reinterpret_cast），
// AVX2 路径又是运行时分派——两条向量路径（sse2/avx2）与标量参考三路对齐，
// 才能保证「换一台机器、换一个档位，像素结果不变」。
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <format>

#include "st/raster/simd.hpp"
#include "st/test/test.hpp"

namespace {

/// 标量参考实现（与 simd.cpp 内部 blend_scalar 同语义，独立展开以便对照）。
[[nodiscard]] auto blend_reference(std::uint32_t dst, std::uint32_t src_premul,
                                   std::uint8_t alpha) noexcept -> std::uint32_t {
  const auto scale = [alpha](std::uint32_t value) noexcept -> std::uint32_t {
    return (value * alpha + 127U) / 255U;
  };
  const auto channel = [](std::uint32_t pixel, unsigned shift) noexcept -> std::uint32_t {
    return (pixel >> shift) & 0xFFU;
  };
  const std::uint32_t sa = scale(channel(src_premul, 0));
  if (sa == 0U) return dst;
  const std::uint32_t inverse = 255U - sa;
  const auto over = [inverse](std::uint32_t source, std::uint32_t destination) noexcept {
    return source + (destination * inverse + 127U) / 255U;
  };
  const std::uint32_t r = over(scale(channel(src_premul, 24)), channel(dst, 24));
  const std::uint32_t g = over(scale(channel(src_premul, 16)), channel(dst, 16));
  const std::uint32_t b = over(scale(channel(src_premul, 8)), channel(dst, 8));
  const std::uint32_t a = over(sa, channel(dst, 0));
  return (r << 24U) | (g << 16U) | (b << 8U) | a;
}

constexpr std::uint32_t kPremulSource = 0x20304040U;  // 合法预乘 RGBA（各通道 ≤ alpha=0x40）
constexpr std::uint32_t kFillValue = 0xAABBCCFFU;

/// 生成合法预乘 RGBA 行：**画布不变量要求每通道 ≤ alpha**。
/// 测试必须落在合法域内——越域输入会让 over 的中间值 > 255，
/// 标量与 SIMD 的溢出进位行为不同，此时比较无意义（曾因此误报）。
std::vector<std::uint32_t> make_row(std::size_t count, std::uint32_t seed_pixel) {
  std::vector<std::uint32_t> row(count);
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint32_t raw = seed_pixel ^ (static_cast<std::uint32_t>(index) * 0x9E3779B9U);
    const std::uint32_t alpha = raw & 0xFFU;
    const auto clamp = [alpha](std::uint32_t value) -> std::uint32_t {
      return value < alpha ? value : alpha;
    };
    row[index] = (clamp((raw >> 24U) & 0xFFU) << 24U) | (clamp((raw >> 16U) & 0xFFU) << 16U) |
                 (clamp((raw >> 8U) & 0xFFU) << 8U) | alpha;
  }
  return row;
}

}  // namespace

ST_TEST(simd_backend_reports_actual_capability) {
  // backend() 必须返回已知档位之一；MSVC x64 上至少是 sse2。
  const std::string_view name = st::raster::simd::backend();
  const bool known = name == "avx2" || name == "sse2" || name == "neon" || name == "scalar";
  ST_CHECK(known);
#if defined(_M_X64) || defined(__x86_64__)
  ST_CHECK(name == "avx2" || name == "sse2");
#endif
}

ST_TEST(simd_fill_row_matches_reference) {
  // 覆盖 0/1/3/4/7/8/9/17 等 SIMD 边界长度：向量化主循环 + 尾部处理的每一条路径。
  for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{3},
                                  std::size_t{4}, std::size_t{7}, std::size_t{8},
                                  std::size_t{9}, std::size_t{17}, std::size_t{100}}) {
    std::vector<std::uint32_t> row(count, 0x12345678U);
    st::raster::simd::fill_row(row.data(), count, kFillValue);
    for (std::size_t index = 0; index < count; ++index) {
      ST_CHECK(row[index] == kFillValue);
    }
  }
}

ST_TEST(simd_blend_row_matches_scalar_reference) {
  // blend_row 与标量参考在多种 alpha（含 0/255 快速档）与边界长度下逐像素一致。
  for (const int alpha_int : {0, 1, 64, 128, 254, 255}) {
    const auto alpha = static_cast<std::uint8_t>(alpha_int);
    for (const std::size_t count : {std::size_t{1}, std::size_t{7}, std::size_t{8},
                                    std::size_t{9}, std::size_t{16}, std::size_t{33}}) {
      std::vector<std::uint32_t> actual = make_row(count, 0x55AA5501U);
      const std::vector<std::uint32_t> expected_base = actual;
      st::raster::simd::blend_row(actual.data(), count, kPremulSource, alpha);
      for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t expected = blend_reference(expected_base[index], kPremulSource, alpha);
        ST_CHECK_EQ(actual[index], expected);
      }
    }
  }
}

ST_TEST(simd_erase_row_matches_reference) {
  for (const std::size_t count : {std::size_t{1}, std::size_t{8}, std::size_t{20}}) {
    std::vector<std::uint32_t> row(count, 0xFFFFFFFFU);
    st::raster::simd::erase_row(row.data(), count, 0x40);
    for (std::size_t index = 0; index < count; ++index) {
      ST_CHECK(row[index] == 0x40U);
    }
    // alpha=0 必须完全不碰
    std::vector<std::uint32_t> untouched = make_row(count, 0x77777777U);
    st::raster::simd::erase_row(untouched.data(), count, 0);
    for (std::size_t index = 0; index < count; ++index) {
      ST_CHECK(untouched[index] == make_row(count, 0x77777777U)[index]);
    }
  }
}
