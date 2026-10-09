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

#include "st/ui/state_trace.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会静默破坏 `std::min`/`std::max`/`std::numeric_limits<T>::max()`
// 的调用点（实测报 C2589“非法标记”，与真正原因相隔很远）。必须在包含前关掉。
#define NOMINMAX 1
#include <windows.h>
#include <psapi.h>  // 崩溃现场模块定位（EnumProcessModules/GetModuleInformation）
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <execinfo.h>   // backtrace/backtrace_symbols（glibc；缺它崩溃只有信号名）
#include <unistd.h>

#include <dlfcn.h>      // dladdr：逐帧符号化（比 backtrace_symbols 更可控）
#include <cxxabi.h>     // __cxa_demangle：把修饰名变成人读的名字

#include <cstdlib>      // std::free（backtrace_symbols / demangle 的产物）
#include <memory>       // unique_ptr（L1：不裸 free）
#endif

namespace {
/// 崩溃时追加输出的提供者（宿主注册；见 `set_crash_extra_provider`）。
std::string (*crash_extra_provider)() = nullptr;

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

/// 信号安全的写（不经 stdio 缓冲与堆分配——信号处理器里 `fprintf` 可能死锁）。
void safe_write(const char* text, std::size_t length) {
  std::size_t written = 0;
  while (written < length) {
    const ssize_t step = ::write(STDERR_FILENO, text + written, length - written);
    if (step <= 0) {
      if (step < 0 && errno == EINTR) continue;
      break;   // 管道关闭等：尽力而为，不能在这里死循环
    }
    written += static_cast<std::size_t>(step);
  }
}

void safe_write(std::string_view text) { safe_write(text.data(), text.size()); }

/// `free` 的 deleter。
///
/// 模板化是为了同时服务 `char*`（demangle 结果）与 `char**`（backtrace_symbols 数组）。
///
/// L1 例外说明：`abi::__cxa_demangle` 与 `backtrace_symbols` 回的是 **malloc 缓冲区**
/// （C ABI，不是 `new`），只能配 `free`——这不是"懒得用 RAII"，而是唯一正确的释放方式。
/// 调用点只有本结构体（`unique_ptr` 的 deleter），即"裸 free 集中在单点"，
/// 与 L1 的意图（把裸资源管理收口）一致。
struct FreeDeleter {
  template <class T>
  void operator()(T* pointer) const {
    if (pointer != nullptr) std::free(pointer);  // lint-allow: L1 C ABI 缓冲区（demangle/backtrace）
  }
};

/// 符号名反修饰（失败则原样返回）。
///
/// 用 `abi::__cxa_demangle`（libstdc++）而不是 `dladdr` 的短名：崩溃栈的可读性
/// 在这里体现——`_ZN7gbcode8Language2pumpEv` 与 `gbcode::Language::pump()` 对
/// 排查的价值差一个数量级。代价是它内部 malloc（信号处理器里严格说不可用），
/// 但这是业界普遍的实践折中（backtrace_symbols 自身也如此）。
[[nodiscard]] auto demangle_or_raw(const std::string& raw) -> std::string {
  // L1（禁裸 free）：`__cxa_demangle` 回的是 malloc 的缓冲区——用 unique_ptr
  // 带 deleter 收口。
  int status = 0;
  std::unique_ptr<char, FreeDeleter> demangled(
      ::abi::__cxa_demangle(raw.c_str(), nullptr, nullptr, &status));
  if (status == 0 && demangled != nullptr) {
    std::string result(demangled.get());
    if (!result.empty()) return result;
  }
  return raw;
}

/// 崩溃栈的最大帧数。
///
/// 32 帧的理由：够看到"崩溃点 → 它的调用者 → 框架派发链 → 入口"（实测框架调用深度
/// 在 10~20 层）。再多会淹没关键信息，且 `backtrace` 抓全栈的开销不值得。
constexpr int kMaxFrames = 32;

void crash_signal(int signal_number) {
  const char* name = signal_number == SIGSEGV ? "SIGSEGV"
                    : signal_number == SIGABRT ? "SIGABRT"
                    : signal_number == SIGBUS ? "SIGBUS"
                    : signal_number == SIGFPE ? "SIGFPE" : "?";

  // ① 头一行：信号名与编号。
  char header[128];
  const int header_length =
      std::snprintf(header, sizeof(header), "\n[st-crash] 信号 %s(%d)\n", name, signal_number);
  if (header_length > 0) safe_write(header, static_cast<std::size_t>(header_length));

  // ② 调用栈（**必须在恢复默认处置之前抓**——重发信号后进程立刻死亡）。
  //
  // 为什么要栈：本框架实测过一次"切标签 SIGSEGV"，只有信号名时排查靠手工包 gdb
  // 跑三轮；有栈可以直接看到 `AnsiScreen::feed ← Terminal::pump ← pump_terminal`
  // ——一眼定位到"悬垂的 terminal_ptr"。
  //
  // 分两段做（信号安全）：
  //   * `backtrace` 只采集地址（不做符号化，纯栈遍历，安全）；
  //   * 随后 `backtrace_symbols_fd` 直接写 fd——它内部虽会分配，但这是**已知的
  //     实践折中**（glibc 的 backtrace_symbols_fd 用 malloc；崩溃现场很少正好在
  //     malloc 里，且它自己就是为崩溃诊断设计的接口）。
  // 若符号化不可用，退回 `dladdr` 逐帧解析（也只用地址空间查询）。
  void* frames[kMaxFrames];
  const int frame_count = ::backtrace(frames, kMaxFrames);
  if (frame_count > 1) {
    safe_write("  调用栈（最近的在前）:\n");
    // 跳过第 0 帧（就是本处理器自己），从真正的崩溃点开始。
    for (int index = 1; index < frame_count; ++index) {
      const auto address = frames[index];
      std::string_view symbol = "?";
      std::string resolved;   // 需要符号名时才有内容
      Dl_info info{};
      if (::dladdr(address, &info) != 0 && info.dli_sname != nullptr) {
        // 先做**名字反修饰**（C++ 的 `_Z7run_appiPPc` → `run_app(int, char**)`）——
        // 未修饰名对排查几乎没有帮助（模板/重载还会长到看不清）。
        resolved = demangle_or_raw(info.dli_sname);
        if (info.dli_fname != nullptr) {
          const std::string_view file(info.dli_fname);
          const std::size_t slash = file.find_last_of('/');
          resolved += "  (";
          resolved += (slash == std::string_view::npos) ? file : file.substr(slash + 1);
          resolved += ")";
        }
        symbol = resolved;
      } else {
        // 动态符号表里没有（**匿名命名空间/static 函数**就是这种——它们不进 dynsym，
        // 但 `.symtab` 里有）。用 `backtrace_symbols` 兜底：它读 `.symtab`
        //（与 `-rdynamic` 无关，只要二进制没 strip），能把这些帧的名字捞出来。
        // `backtrace_symbols` 回的是 malloc 的指针数组。
        std::unique_ptr<char*, FreeDeleter> lines(::backtrace_symbols(&frames[index], 1));
        if (lines != nullptr) {
          // 形态：`./bin/app(_ZN...fnEv+0x12) [0x55...]`——取出括号内的符号部分。
          const std::string_view line(lines.get()[0]);
          const std::size_t open = line.find('(');
          const std::size_t plus = line.find('+', open == std::string_view::npos ? 0 : open + 1);
          if (open != std::string_view::npos && plus != std::string_view::npos &&
              plus > open + 1) {
            resolved = demangle_or_raw(std::string(line.substr(open + 1, plus - open - 1)));
            symbol = resolved;
          }
        }
      }
      char line[512];
      const int length = std::snprintf(line, sizeof(line), "    #%-2d %p  %.*s\n", index - 1,
                                       address, static_cast<int>(symbol.size()), symbol.data());
      if (length > 0) safe_write(line, static_cast<std::size_t>(length));
    }
    safe_write("  （无符号名 = 该帧来自静态库/未导出符号；`st build` 已带 -rdynamic，"
               "主要符号应可见）\n");
  } else {
    safe_write("  （调用栈不可用：backtrace 未采集到帧）\n");
  }

  // ③ 附加诊断钩子（可选）：宿主可注册一个"崩溃时输出额外信息"的回调——
  // State 写入追踪（`ST_TRACE_STATE=1`）走这里。
  //
  // **为什么用钩子而不是直接调用 `st::ui::trace`**：`platform_crash.cpp` 在
  // `core` 层，`state_trace` 在 `ui` 层，而 `st` 构建器（工具链自身）只链 core
  // 不链 ui——直接调用会让 `st` 链接失败（实测：undefined reference to
  // `st::ui::trace::has_records()`）。**依赖方向必须是 core ← ui**，
  // 让它反过来是层次污染，不是"加个 ifdef 就完事"的问题。
  if (crash_extra_provider != nullptr) {
    if (const std::string extra = crash_extra_provider(); !extra.empty()) safe_write(extra);
  }

  // ④ 恢复默认处置并重发：保持退出码语义（外部工具靠它判断崩溃）
  ::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
}

#endif

}  // namespace

void st::set_crash_extra_provider(std::string (*provider)()) { crash_extra_provider = provider; }

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
