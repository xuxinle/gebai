/// 平台崩溃处理器（Windows SEH / POSIX 信号）：把崩溃现场的最小信息打到 stderr。
///
/// **为什么单独成文件**：平台 API 与位级重解释（PE 模块遍历）必须集中在 `platform_*`
/// （CONVENTIONS.md §10；lint L6 的豁免也按文件名判定，见 `src/pkg/lint.cpp` 的 rule_exempt）。
/// 此前这段代码写在 `entry.cpp` 里，即使已用 `#if defined(_WIN32)` 包裹，仍违反
/// 「平台差异单点封装」的纪律——移动而非豁免，才是与框架其它平台代码一致的做法。
///
/// 目标不是恢复，而是让「无头应用崩溃」留下一帧可定位的信息：模块名+偏移配合
/// PDB/符号表即可定位到代码（审视报告 P1-3：AI 拿不到栈回溯只能猜）。

#include "st/core/entry.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会静默破坏 `std::min`/`std::max`/`std::numeric_limits<T>::max()`
// 的调用点（实测报 C2589“非法标记”，与真正原因相隔很远）。必须在包含前关掉。
#define NOMINMAX 1
#include <windows.h>
#include <psapi.h>  // 崩溃现场模块定位（EnumProcessModules/GetModuleInformation）
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

#if defined(_WIN32)

/// 崩溃地址落在哪个模块、偏移多少（`模块名+0x偏移`）。
[[nodiscard]] auto module_base_of(void* address) -> std::string {
  HMODULE modules[64];
  DWORD needed = 0;
  if (!::EnumProcessModules(::GetCurrentProcess(), modules, sizeof(modules), &needed)) return "?";
  const int count = static_cast<int>(needed / sizeof(HMODULE));
  for (int index = 0; index < count; ++index) {
    MODULEINFO info{};
    if (!::GetModuleInformation(::GetCurrentProcess(), modules[index], &info, sizeof(info))) continue;
    const auto begin = reinterpret_cast<std::uint8_t*>(info.lpBaseOfDll);
    const auto end = begin + info.SizeOfImage;
    const auto at = static_cast<std::uint8_t*>(address);
    if (at >= begin && at < end) {
      char name[MAX_PATH] = {};
      if (::GetModuleFileNameA(modules[index], name, sizeof(name)) == 0) return "?";
      const std::string full(name);
      const std::size_t slash = full.find_last_of("\\/");
      return std::format("{}+0x{:x}", slash == std::string::npos ? full : full.substr(slash + 1),
                         static_cast<std::size_t>(at - begin));
    }
  }
  return "?";
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
  if (info != nullptr && info->ExceptionRecord != nullptr) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    std::fprintf(stderr, "\n[st-crash] 异常码 0x%08lX 于 %s\n",
                 static_cast<unsigned long>(record->ExceptionCode),
                 module_base_of(record->ExceptionAddress).c_str());
  } else {
    std::fprintf(stderr, "\n[st-crash] 未知异常（无现场）\n");
  }
  std::fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;  // 不吞异常：交给系统默认处理（生成转储/WER）
}

#else

void crash_signal(int signal_number) {
  const char* name = signal_number == SIGSEGV ? "SIGSEGV"
                    : signal_number == SIGABRT ? "SIGABRT"
                    : signal_number == SIGBUS ? "SIGBUS"
                    : signal_number == SIGFPE ? "SIGFPE" : "?";
  char buffer[256];
  const int written = std::snprintf(buffer, sizeof(buffer), "\n[st-crash] 信号 %s(%d)\n", name,
                                     signal_number);
  if (written > 0) {
    const ssize_t ignored = ::write(STDERR_FILENO, buffer, static_cast<std::size_t>(written));
    (void)ignored;
  }
  // 恢复默认处置并重发：保持退出码语义（外部工具靠它判断崩溃）
  ::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
}

#endif

}  // namespace

void st::install_crash_handler() {
#if defined(_WIN32)
  ::SetUnhandledExceptionFilter(crash_filter);
#else
  ::signal(SIGSEGV, crash_signal);
  ::signal(SIGABRT, crash_signal);
  ::signal(SIGBUS, crash_signal);
  ::signal(SIGFPE, crash_signal);
#endif
}
