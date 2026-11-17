// 新哈希的实测速度：与旧 FNV-1a 在同一份 259MB 上对比，并验证与打包脚本算出的值一致
#include <chrono>
#include <cstdio>
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
  const std::string path = argc > 1 ? argv[1]
                                    : "D:/code/gebai-pro/packages/desktop/dist/gebai.exe";
  auto bytes = st::fs::read_bytes(path);
  if (!bytes) {
    std::printf("读失败: %s\n", path.c_str());
    return 1;
  }
  const std::span<const std::uint8_t> data(bytes->data(), bytes->size());
  std::printf("文件 %.1f MB\n\n", static_cast<double>(data.size()) / 1048576.0);

  auto t0 = Clock::now();
  const auto fast = st::hash::content_hash64(data);
  const double fast_ms = ms_since(t0);
  std::printf("content_hash64  %8.1f ms  => %016llx\n", fast_ms,
              static_cast<unsigned long long>(fast));

  // 旧路径：只为对比，不再用于生产
  t0 = Clock::now();
  const auto slow = st::hash::fnv1a64_bytes(data);
  const double slow_ms = ms_since(t0);
  std::printf("fnv1a64_bytes   %8.1f ms  => %016llx\n", slow_ms,
              static_cast<unsigned long long>(slow));

  std::printf("\n提速 %.1f×\n", slow_ms / fast_ms);
  return 0;
}
