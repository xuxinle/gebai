/// 平台刻度读取（见 `include/st/perf/platform_ticks.hpp` 的契约）。
///
/// x86/x64 用 TSC；其他平台（ARM/macOS 等）如实报告"不可用"，由上层退化为
/// `steady_clock`——**不假装有高精度时钟**（报告里要说真话，见 `clock.hpp`）。
///
/// 平台宏（`_MSC_VER` / `__x86_64__`）只出现在本文件：这是 §10「平台差异集中到
/// `platform_*` 单点」的要求，也是"换平台要改哪里"只有一个答案的原因。

#include "st/perf/platform_ticks.hpp"

#include <chrono>

#if defined(_MSC_VER)
#if defined(_M_X64) || defined(_M_IX86)
#define ST_PERF_TSC 1
#include <intrin.h>
#endif
#elif defined(__x86_64__) || defined(__i386__)
#define ST_PERF_TSC 1
#include <x86intrin.h>
#endif

namespace st::perf::platform {
namespace {

#if !defined(ST_PERF_TSC)
using SteadyClock = std::chrono::steady_clock;

/// 无高精度刻度时的退化读数（纳秒）。只在 `read_ticks` 的退化分支用到。
[[nodiscard]] auto steady_ns() noexcept -> std::int64_t {
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch())
          .count());
}
#endif

}  // namespace

auto ticks_available() noexcept -> bool {
#if defined(ST_PERF_TSC)
  return true;
#else
  return false;
#endif
}

auto read_ticks() noexcept -> std::int64_t {
#if defined(ST_PERF_TSC)
  return static_cast<std::int64_t>(__rdtsc());
#else
  return steady_ns();
#endif
}

}  // namespace st::perf::platform
