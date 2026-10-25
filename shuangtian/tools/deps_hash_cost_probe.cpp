// 构建产物哈希判定的成本实测：给 .meta 加内容哈希值不值。
//
// 背景：现在的增量判定是 (mtime, size) 二元组，两者都不变时必然判"没变"
// （同长度改动 + 保留 mtime 就会拿旧产物当新结果）。
// 根治要加内容哈希，代价是**每次构建都要读一遍全部源文件与头**。
//
// 本探针量的就是这笔代价，口径与真实构建一致：
//   · 框架的全部 .cpp 与 .hpp
//   · 两种候选算法：sha256_file（强）与 fnv1a64_bytes（弱但快）
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"

namespace fsys = std::filesystem;
using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main(int argc, char** argv) {
  const fsys::path root = (argc > 1) ? fsys::path(argv[1]) : fsys::current_path();

  std::vector<std::pair<std::string, std::uintmax_t>> files;
  std::uintmax_t total_bytes = 0;
  for (const auto& entry : fsys::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".hpp") continue;
    const auto size = entry.file_size();
    files.emplace_back(entry.path().generic_string(), size);
    total_bytes += size;
  }

  std::printf("文件数 %zu，合计 %.1f MB\n\n", files.size(),
              static_cast<double>(total_bytes) / 1048576.0);

  // ① 只 stat（当前口径）
  auto t0 = Clock::now();
  std::uint64_t acc = 0;
  for (const auto& [path, size] : files) {
    acc += st::fs::modified_ns(path).value_or(0);
    acc += size;
  }
  const double stat_ms = ms_since(t0);
  std::printf("① 只 stat（现状）      %7.2f ms/次   （校验和 %llu）\n", stat_ms,
              static_cast<unsigned long long>(acc & 0xFFFF));

  // ② 读全部 + fnv1a64
  t0 = Clock::now();
  std::uint64_t fnv_acc = 0;
  for (const auto& [path, size] : files) {
    const auto bytes = st::fs::read_bytes(path);
    if (!bytes.has_value()) continue;
    fnv_acc ^= st::hash::fnv1a64_bytes(
        std::span<const std::uint8_t>(bytes->data(), bytes->size()));
  }
  const double fnv_ms = ms_since(t0);
  std::printf("② 读全部 + fnv1a64     %7.2f ms/次   （校验和 %llu）\n", fnv_ms,
              static_cast<unsigned long long>(fnv_acc & 0xFFFF));

  // ③ 读全部 + sha256（含文件 I/O，与 ② 同口径可比）
  t0 = Clock::now();
  std::uint64_t sha_acc = 0;
  for (const auto& [path, size] : files) {
    const auto digest = st::hash::sha256_file(path);
    if (!digest.has_value()) continue;
    sha_acc ^= st::hash::fnv1a64(*digest);
  }
  const double sha_ms = ms_since(t0);
  std::printf("③ 读全部 + sha256      %7.2f ms/次   （校验和 %llu）\n", sha_ms,
              static_cast<unsigned long long>(sha_acc & 0xFFFF));

  std::printf("\n相对只 stat 的倍数：fnv %.1f×、sha256 %.1f×\n", fnv_ms / stat_ms,
              sha_ms / stat_ms);
  return 0;
}
