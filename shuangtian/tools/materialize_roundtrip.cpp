// 直接测 materialize_server 的端到端耗时（重复多次，取中位）
// 用真实 gebai-desktop.exe，量「从启动到侧车已就绪」中属于物化的那部分
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
}  // namespace

int main(int argc, char** argv) {
  const std::string self = argc > 1 ? argv[1]
                                    : "D:/code/gebai-pro/packages/desktop/dist/gebai-desktop.exe";
  const std::string dest = argc > 2 ? argv[2]
                                    : "C:/Users/xu.xinle/AppData/Local/gebai/gebai-server.exe";
  constexpr std::size_t kTrailerSize = 32;

  for (int round = 0; round < 3; ++round) {
    auto t_all = Clock::now();

    // ① read_payload_from_self 的等价流程
    auto t = Clock::now();
    const auto self_size = st::fs::file_size(self);
    const auto trailer = st::fs::read_span(self, *self_size - kTrailerSize, kTrailerSize);
    const auto read_u64 = [&](std::size_t off) {
      std::uint64_t v = 0;
      for (std::size_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>((*trailer)[off + i]) << (i * 8U);
      return v;
    };
    const std::uint64_t payload_len = read_u64(8);
    const std::uint64_t image_len = read_u64(16);
    const std::uint64_t expected = read_u64(24);
    const auto body = st::fs::read_span(self, image_len, static_cast<std::size_t>(payload_len));
    const std::uint64_t actual = st::hash::content_hash64(*body);
    const double phase1 = ms_since(t);

    // ② materialize_server 的等价流程
    t = Clock::now();
    bool mismatch = true;
    if (const auto cs = st::fs::file_size(dest); cs.has_value()) {
      if (*cs == static_cast<std::uint64_t>(body->size())) {
        if (const auto current = st::fs::read_bytes(dest); current.has_value()) {
          mismatch = st::hash::content_hash64(*current) != st::hash::content_hash64(*body);
        }
      }
    }
    const double phase2 = ms_since(t);
    const double total = ms_since(t_all);

    std::printf("轮 %d：payload读取+校验 %6.1f ms · 侧车比对 %6.1f ms · 合计 %6.1f ms  %s\n",
                round + 1, phase1, phase2, total,
                actual == expected ? "(哈希一致)" : "(哈希不符!)");
    if (mismatch) std::printf("      → 侧车与 payload 不同，需重写（%.0f ms 写盘）\n", 0.0);
  }
  return 0;
}
