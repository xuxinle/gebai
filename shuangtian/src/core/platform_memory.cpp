// 平台边界：本文件集中封装系统内存统计 API（`CONVENTIONS` §10 的受控例外——
// 系统头只出现在 `platform_*` 里）。
#include "st/core/memory.hpp"

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会静默破坏 `std::min`/`std::max`
// （报错文本是"C2589 非法标记"，离真正原因很远）。必须在包含前关掉。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#else
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace st::memory {

namespace {

#if defined(_WIN32)
/// 取一次 `PROCESS_MEMORY_COUNTERS_EX`（失败返回 false）。
[[nodiscard]] auto counters(PROCESS_MEMORY_COUNTERS_EX& out) -> bool {
  return GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&out),
                              sizeof(out)) != 0;
}
#else
[[nodiscard]] auto usage_of_self(rusage& out) -> bool { return getrusage(RUSAGE_SELF, &out) == 0; }
#endif

}  // namespace

auto page_fault_count() -> std::uint64_t {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX value{};
  if (!counters(value)) return 0;
  return static_cast<std::uint64_t>(value.PageFaultCount);
#else
  rusage value{};
  if (!usage_of_self(value)) return 0;
  return static_cast<std::uint64_t>(value.ru_minflt) + static_cast<std::uint64_t>(value.ru_majflt);
#endif
}

auto working_set_bytes() -> std::uint64_t {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX value{};
  if (!counters(value)) return 0;
  return static_cast<std::uint64_t>(value.WorkingSetSize);
#else
  rusage value{};
  if (!usage_of_self(value)) return 0;
  // `ru_maxrss` 的单位各平台不同：Linux 是 KB，macOS 是字节。
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(value.ru_maxrss);
#else
  return static_cast<std::uint64_t>(value.ru_maxrss) * 1024ULL;
#endif
#endif
}

auto has_stats() noexcept -> bool {
#if defined(_WIN32)
  return true;
#else
  rusage value{};
  return usage_of_self(value);
#endif
}

}  // namespace st::memory
