// `content_hash64` 的口径守卫：它必须是 **SHA-256 前 8 字节、小端读作 u64**。
//
// ## 为何需要这个对拍
//
// 这个函数的唯一用途是「打包脚本写进 trailer 的值」与「宿主启动时重算的值」对得上。
// 两侧语言不同（TypeScript 与 C++），两侧实现也不同（`crypto.createHash` 与 CNG）。
// **任何一侧改了口径而另一侧没改，症状是“每次启动都把侧车重写一遍”**——
// 能跑、只是慢，而且慢得不明所以（实测代价：见 `docs/PITFALLS.md` 的物化条目）。
//
// 所以这里用**已知的真实 SHA-256 值**钉住口径。JS 侧对应断言在
// `packages/desktop/scripts/pack-desktop.test.ts`。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "st/core/hash.hpp"

namespace {

/// 把 `content_hash64` 的结果与「SHA-256 前 8 字节小端」独立复算一遍比对。
std::uint64_t reference(const std::vector<std::uint8_t>& bytes) {
  const std::string hex = st::hash::sha256_hex(
      std::span<const std::uint8_t>(bytes.data(), bytes.size()));
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    const auto nibble = [](char c) -> std::uint64_t {
      if (c >= '0' && c <= '9') return static_cast<std::uint64_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<std::uint64_t>(c - 'a' + 10);
      return static_cast<std::uint64_t>(c - 'A' + 10);
    };
    const std::uint64_t byte = (nibble(hex[index * 2]) << 4U) | nibble(hex[index * 2 + 1]);
    value |= byte << (index * 8U);
  }
  return value;
}

int failures = 0;

void check(const char* label, const std::vector<std::uint8_t>& bytes,
           const char* expected_hex) {
  const auto actual = st::hash::content_hash64(
      std::span<const std::uint8_t>(bytes.data(), bytes.size()));
  const auto ref = reference(bytes);
  std::printf("%-14s => %016llx", label, static_cast<unsigned long long>(actual));
  bool ok = true;
  if (actual != ref) {
    std::printf("  ✗ 与 SHA-256 口径不符（参考 %016llx）", static_cast<unsigned long long>(ref));
    ok = false;
  }
  if (expected_hex != nullptr) {
    if (actual != std::stoull(expected_hex, nullptr, 16)) {
      std::printf("  ✗ 与已知值不符（期望 %s）", expected_hex);
      ok = false;
    }
  }
  std::printf("%s\n", ok ? "  ✓" : "");
  if (!ok) ++failures;
}

}  // namespace

int main() {
  // 空输入的 SHA-256 = e3b0c44298fc1c14…，前 8 字节（e3 b0 c4 42 98 fc 1c 14）
  // 按**小端**拼成 u64 = 0x141cfc9842c4b0e3。
  check("empty", {}, "141cfc9842c4b0e3");
  // "abc" 的 SHA-256 = ba7816bf8f01cfea…，前 8 字节（ba 78 16 bf 8f 01 cf ea）
  // 按小端拼 = 0xeacf018fbf1678ba。
  check("abc", {'a', 'b', 'c'}, "eacf018fbf1678ba");
  check("a", {'a'}, nullptr);
  check("a-nul", {'a', 0x00}, nullptr);
  check("64-zero", std::vector<std::uint8_t>(64, 0x00), nullptr);
  check("1MiB-pattern", [] {
    std::vector<std::uint8_t> v(1 << 20);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>(i & 0xFF);
    return v;
  }(), nullptr);

  // 长度必须参与（SHA-256 本身已保证：长度进了末块填充）
  const auto empty_hash = st::hash::content_hash64({});
  const std::vector<std::uint8_t> zero{0x00};
  std::printf("\nempty != zero-byte ? %s\n",
              empty_hash != st::hash::content_hash64(
                                 std::span<const std::uint8_t>(zero.data(), zero.size()))
                  ? "true"
                  : "false");

  std::printf("\n%s（失败 %d 项）\n", failures == 0 ? "全部通过" : "有失败", failures);
  return failures == 0 ? 0 : 1;
}
