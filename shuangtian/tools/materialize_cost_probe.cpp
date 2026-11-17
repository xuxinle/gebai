// 复刻 materialize_server 的真实开销：逐阶段计时（读自身 payload / 读既有侧车 / 两次哈希 / 写盘）
#include <chrono>
#include <cstdio>
#include <format>
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

  auto total0 = Clock::now();
  const auto self_size = st::fs::file_size(self);
  if (!self_size) {
    std::printf("读自身大小失败\n");
    return 1;
  }
  constexpr std::size_t kTrailerSize = 32;
  auto t = Clock::now();
  const auto trailer = st::fs::read_span(self, *self_size - kTrailerSize, kTrailerSize);
  std::printf("① 读 trailer:              %8.1f ms\n", ms_since(t));
  if (!trailer) return 1;

  const auto read_u64 = [&](std::size_t off) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>((*trailer)[off + i]) << (i * 8U);
    return v;
  };
  const std::uint64_t payload_len = read_u64(8);
  const std::uint64_t image_len = read_u64(16);

  t = Clock::now();
  const auto body = st::fs::read_span(self, image_len, static_cast<std::size_t>(payload_len));
  const double read_self_ms = ms_since(t);
  std::printf("② 读 payload（%.1f MB）:   %8.1f ms\n", payload_len / 1048576.0, read_self_ms);
  if (!body) return 1;

  t = Clock::now();
  const auto h_payload_fast = st::hash::content_hash64(*body);
  const double hash_fast_ms = ms_since(t);
  std::printf("③ 哈希 payload（快）:      %8.1f ms  => %016llx\n", hash_fast_ms,
              static_cast<unsigned long long>(h_payload_fast));

  t = Clock::now();
  const auto h_payload_fnv = st::hash::fnv1a64_bytes(*body);
  const double hash_fnv_ms = ms_since(t);
  std::printf("④ 哈希 payload（FNV）:     %8.1f ms  => %016llx\n", hash_fnv_ms,
              static_cast<unsigned long long>(h_payload_fnv));

  // 既有侧车
  const auto dest_size = st::fs::file_size(dest);
  if (dest_size.has_value()) {
    t = Clock::now();
    const auto current = st::fs::read_bytes(dest);
    const double read_dest_ms = ms_since(t);
    std::printf("⑤ 读既有侧车（%.1f MB）:   %8.1f ms\n", *dest_size / 1048576.0, read_dest_ms);
    if (current.has_value()) {
      t = Clock::now();
      const auto h_dest_fast = st::hash::content_hash64(*current);
      std::printf("⑥ 哈希既有（快）:          %8.1f ms  %s\n", ms_since(t),
                  h_dest_fast == h_payload_fast ? "(相同→跳过重写)" : "(不同→需重写)");
    }
  } else {
    std::printf("⑤ 既有侧车不存在（首次物化）\n");
  }

  // 写盘（仅测，写到临时文件，不动真侧车）
  const std::string tmp = dest + ".probe-tmp";
  t = Clock::now();
  const auto wrote = st::fs::write_bytes(tmp, *body);
  std::printf("⑦ 写 259MB 到磁盘:         %8.1f ms  %s\n", ms_since(t), wrote ? "ok" : "FAIL");
  (void)st::fs::remove_file(tmp);

  std::printf("\n合计（不含写盘）: %.1f ms\n", ms_since(total0));
  return 0;
}
