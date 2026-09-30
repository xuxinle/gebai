// 平台内存探测的 Windows 实现（单点封装：`<windows.h>` 只允许出现在 platform_*）。
// 供 `pkg/memory.cpp` 的 `detect_memory_limit` 在 Windows 上给出真实上限——
// 原先只有 cgroup/meminfo 路径，Windows 恒“不可知”→ 并发退回满核，
// 恰好违背“按内存预算防 OOM”的机制初衷（见 DESIGN.md §7.5.1）。
#include <cstdint>

#include "st/pkg/memory.hpp"

#if defined(_WIN32)
#include <windows.h>

namespace st::pkg {

/// GlobalMemoryStatusEx 取物理总量与可用量的**较小者**。
/// 为什么取较小者：编译并发受的是“现在还能用多少”的约束——
/// 总量 64GiB 但已被其它进程占掉 60GiB 时，按总量推导并发必被 OOM。
auto platform_memory_limit_mb() -> std::uint64_t {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return 0;
  const auto to_mb = [](std::uint64_t bytes) -> std::uint64_t {
    return bytes / (1024ULL * 1024ULL);
  };
  const std::uint64_t total = to_mb(status.ullTotalPhys);
  const std::uint64_t avail = to_mb(status.ullAvailPhys);
  return avail < total ? avail : total;
}

}  // namespace st::pkg
#else
// 非 Windows 平台此文件为空实现（探测走 cgroup//proc 路径，见 memory.cpp）。
namespace st::pkg {
auto platform_memory_limit_mb() -> std::uint64_t { return 0; }
}  // namespace st::pkg
#endif
