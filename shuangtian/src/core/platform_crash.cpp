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

#include "st/core/fs.hpp"
#include "st/core/log_file.hpp"
#include "st/ui/state_trace.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会静默破坏 `std::min`/`std::max`/`std::numeric_limits<T>::max()`
// 的调用点（实测报 C2589“非法标记”，与真正原因相隔很远）。必须在包含前关掉。
#define NOMINMAX 1
#include <windows.h>
#include <dbghelp.h>   // StackWalk64/SymFromAddr：异常过滤器里的调用栈符号化
#include <fcntl.h>     // O_CREAT/O_TRUNC（MSVC 与 mingw 都在 <fcntl.h>）
#include <io.h>        // _write/_close（与 POSIX 的 write/close 同一套 fd 语义）
#include <psapi.h>     // 崩溃现场模块定位（EnumProcessModules/GetModuleInformation）
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <execinfo.h>   // backtrace/backtrace_symbols（glibc；缺它崩溃只有信号名）
#include <fcntl.h>      // open/O_CREAT：预打开报告文件
#include <unistd.h>

#include <dlfcn.h>      // dladdr：逐帧符号化（比 backtrace_symbols 更可控）
#include <cxxabi.h>     // __cxa_demangle：把修饰名变成人读的名字

#include <cstdlib>      // std::free（backtrace_symbols / demangle 的产物）
#include <memory>       // unique_ptr（L1：不裸 free）
#endif

// lint-allow: L8 崩溃处理器的状态必须是**进程级**：信号处理器由系统按固定签名回调，
// 拿不到任何参数，状态只能放静态存储；且它们必须在崩溃发生前就绪（见下方注释）。
namespace {
/// 崩溃时追加输出的提供者（宿主注册；见 `set_crash_extra_provider`）。
std::string (*crash_extra_provider)() = nullptr;

/// 崩溃时用的日志尾部（信号安全版）。
///
/// ⚠ **必须 `try_lock`，不能阻塞等锁**：若崩溃恰好发生在另一个线程持有尾部锁时
/// （或就发生在本函数内），等锁 = 死锁，结果是**连一行都拿不到**——比“拿不到尾部”
/// 更坏。拿不到就跳过，如实写一行说明。
[[nodiscard]] auto try_recent_tail(std::size_t max_lines) -> std::string {
  return st::log::detail::try_recent_tail(max_lines);
}

/// 崩溃报告文件（预先打开，见 `install_crash_handler`）。
///
/// 为何**预先打开**而不是崩溃时再开：信号处理器里只能调**异步信号安全**的函数，
/// `fopen`/`CreateFileW` 都不在其中（它们会分配、可能死锁）。启动时把文件开好，
/// 崩溃时只剩 `write`——那一个是信号安全的。
///
/// 代价：启动后就会留下一个空文件。用启动时先写一行表头 + 正常退出时删除
/// 来解决（见 `finalize_crash_report`）。
// lint-allow: L9 尾部采样需要在“普通锁”与“try_lock”两种形态间切换
// （容器锁定不可用：`std::scoped_lock` 不能“尝试取”，而崩溃处理器必须能放弃等锁）。
int crash_fd = -1;
std::string crash_report_target{};

/// 崩溃时把文本写到报告文件（信号安全的 `write`）。
void report_write(const char* text, std::size_t length) {
  if (crash_fd < 0 || length == 0) return;
#if defined(_WIN32)
  // `_write` 在 Windows 上走的是同一套 CRT fd，信号处理器里可用。
  (void)::_write(crash_fd, text, static_cast<unsigned>(length));
#else
  std::size_t written = 0;
  while (written < length) {
    const ssize_t step = ::write(crash_fd, text + written, length - written);
    if (step <= 0) {
      if (step < 0 && errno == EINTR) continue;
      break;
    }
    written += static_cast<std::size_t>(step);
  }
#endif
}

void report_write(std::string_view text) { report_write(text.data(), text.size()); }

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

/// 模块基址（用于把运行时地址换成模块内偏移——`addr2line` 要的是后者）。
[[nodiscard]] auto module_offset_base(DWORD64 address) -> DWORD64 {
  HMODULE modules[64];
  DWORD needed = 0;
  if (!::EnumProcessModules(::GetCurrentProcess(), modules, sizeof(modules), &needed)) return 0;
  const int count = static_cast<int>(needed / sizeof(HMODULE));
  for (int index = 0; index < count; ++index) {
    MODULEINFO info{};
    if (!::GetModuleInformation(::GetCurrentProcess(), modules[index], &info, sizeof(info))) continue;
    const auto begin = reinterpret_cast<DWORD64>(info.lpBaseOfDll);
    if (address >= begin && address < begin + info.SizeOfImage) return begin;
  }
  return 0;
}

/// 模块**完整路径**（`addr2line -e` 要它，而 `module_base_of` 只给文件名）。
[[nodiscard]] auto module_full_path(void* address) -> std::string {
  HMODULE modules[64];
  DWORD needed = 0;
  if (!::EnumProcessModules(::GetCurrentProcess(), modules, sizeof(modules), &needed)) return {};
  const int count = static_cast<int>(needed / sizeof(HMODULE));
  for (int index = 0; index < count; ++index) {
    MODULEINFO info{};
    if (!::GetModuleInformation(::GetCurrentProcess(), modules[index], &info, sizeof(info))) continue;
    const auto begin = reinterpret_cast<std::uint8_t*>(info.lpBaseOfDll);
    const auto end = begin + info.SizeOfImage;
    const auto at = static_cast<std::uint8_t*>(address);
    if (at >= begin && at < end) {
      char name[MAX_PATH] = {};
      if (::GetModuleFileNameA(modules[index], name, sizeof(name)) == 0) return {};
      return name;
    }
  }
  return {};
}

/// 遍历调用栈并符号化（best effort：任一环失败就停，已拿到的照样输出）。
///
/// 为何不用 `SymGetLineFromAddr64`：它要读 PDB，而交付场景经常只有 exe。
/// 模块名 + 偏移 + 函数名已足够配合 `st build` 的符号定位（与 POSIX 侧的
/// “文件名 + 符号名”同一精度口径）。
void walk_stack(EXCEPTION_POINTERS* info, const EXCEPTION_RECORD* record);

/// 未处理异常过滤器：写异常码 + 调用栈 + 崩溃前日志尾部，**不吞异常**
/// （交回系统默认处置，保留生成转储/WER 的行为与退出码语义）。
LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
  if (info != nullptr && info->ExceptionRecord != nullptr) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    char header[256];
    const int length = std::snprintf(header, sizeof(header), "\n[st-crash] 异常码 0x%08lX 于 %s\n",
                                     static_cast<unsigned long>(record->ExceptionCode),
                                     module_base_of(record->ExceptionAddress).c_str());
    if (length > 0) {
      std::fwrite(header, 1, static_cast<std::size_t>(length), stderr);
      report_write(header, static_cast<std::size_t>(length));
    }
    // 调用栈（与 POSIX 侧对齐）。只用 `StackWalk64` + `SymFromAddr`：
    // 它们是 DbgHelp 里**不分配堆**的那几个（`SymInitialize` 已在启动时做过），
    // 因此可以在异常过滤器里用——`CaptureStackBackTrace` 只能拿到地址，
    // 没有模块名就无法定位（交付物是给 AI/开发者看的，纯地址等于没给）。
    walk_stack(info, record);
  } else {
    const std::string_view note = "\n[st-crash] 未知异常（无现场）\n";
    std::fwrite(note.data(), 1, note.size(), stderr);
    report_write(note);
  }
  // 崩溃前的日志尾部：与 POSIX 侧同一条理由（见 `crash_signal` 里对应注释）。
  if (const std::string tail = try_recent_tail(200); !tail.empty()) {
    const std::string_view title =
        "\n  ── 崩溃前日志尾部（最多 200 行）────────────────\n";
    std::fwrite(title.data(), 1, title.size(), stderr);
    report_write(title);
    std::fwrite(tail.data(), 1, tail.size(), stderr);
    report_write(tail);
  }
  if (crash_extra_provider != nullptr) {
    if (const std::string extra = crash_extra_provider(); !extra.empty()) {
      std::fwrite(extra.data(), 1, extra.size(), stderr);
      report_write(extra);
    }
  }
  std::fflush(stderr);
  if (crash_fd >= 0) {
    ::_close(crash_fd);   // 交回系统前先把报告收尾（关闭即 flush）
    crash_fd = -1;
  }
  return EXCEPTION_CONTINUE_SEARCH;  // 不吞异常：交给系统默认处理（生成转储/WER）
}

/// 遍历调用栈并符号化（best effort：任一环失败就停，已拿到的照样输出）。
///
/// 为何不用 `SymGetLineFromAddr64`：它要读 PDB，而交付场景经常只有 exe。
/// 模块名 + 偏移 + 函数名已足够配合 `st build` 的符号定位（与 POSIX 侧的
/// “文件名 + 符号名”同一精度口径）。
void walk_stack(EXCEPTION_POINTERS* info, const EXCEPTION_RECORD* record) {
  const HANDLE process = ::GetCurrentProcess();
  const HANDLE thread = ::GetCurrentThread();
  CONTEXT context = *info->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;

  std::string report;
  report += "  调用栈（最近的在前）:\n";
  for (int index = 0; index < 32; ++index) {
    if (::StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                      ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr) == FALSE) {
      break;
    }
    if (frame.AddrPC.Offset == 0) break;
    const DWORD64 address = frame.AddrPC.Offset;
    // ⚠ `SYMBOL_INFO` 的 `Name` 是**柔性数组**：不能直接 `reinterpret_cast` 一个
    // `char[N]`——`SizeOfStruct`/`MaxNameLen` 没设、名字区也不够，`SymFromAddr`
    // 只会返回失败（实测：每个帧都落到“无符号”分支，看起来像“二进制没符号”，
    // 实际上是我这块缓冲用错了）。按 msdn 的用法整块分配。
    constexpr std::size_t kNameLength = 512;
    std::vector<std::uint8_t> storage(sizeof(SYMBOL_INFO) + kNameLength, 0U);
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = static_cast<ULONG>(kNameLength);
    DWORD64 displacement = 0;
    if (::SymFromAddr(process, address, &displacement, symbol) != FALSE) {
      report += std::format("    #-{:2d} {}  {}+0x{:x}\n", index,
                            module_base_of(reinterpret_cast<void*>(address)),
                            std::string_view(symbol->Name, symbol->NameLen),
                            static_cast<std::size_t>(displacement));
    } else {
      // **兜底要可直接执行**：MinGW 构建带的是 DWARF，而 DbgHelp 只读 PDB，
      // 所以自己的帧在这里必然解析不出名字（系统 DLL 有 PDB，所以能出）。
      //
      // ⚠ `addr2line` 要的是 **link-time 地址**（含 PE 首选基址 0x140000000），
      // 不是模块内偏移——实测：传偏移全部得到 `??:0`，加上首选基址才出函数名。
      static constexpr DWORD64 kPreferredBase = 0x0000000140000000ULL;
      report += std::format(
          "    #-{:2d} {}  (MinGW/DWARF：addr2line -f -C -e \"{}\" 0x{:x})\n", index,
          module_base_of(reinterpret_cast<void*>(address)),
          module_full_path(reinterpret_cast<void*>(address)),
          static_cast<std::size_t>(kPreferredBase + address - module_offset_base(address)));
    }
  }
  report_write(report);
  std::fwrite(report.data(), 1, report.size(), stderr);
  (void)record;
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

  /// ① 信号名与编号（stderr 与报告文件各写一份）。
  char header[128];
  const int header_length =
      std::snprintf(header, sizeof(header), "\n[st-crash] 信号 %s(%d)\n", name, signal_number);
  if (header_length > 0) {
    safe_write(header, static_cast<std::size_t>(header_length));
    report_write(header, static_cast<std::size_t>(header_length));
  }

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
    report_write("  调用栈（最近的在前）:\n");
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
      if (length > 0) {
        safe_write(line, static_cast<std::size_t>(length));
        report_write(line, static_cast<std::size_t>(length));
      }
    }
    const std::string_view note =
        "  （无符号名 = 该帧来自静态库/未导出符号；`st build` 已带 -rdynamic，"
        "主要符号应可见）\n";
    safe_write(note);
    report_write(note);
  } else {
    const std::string_view note = "  （调用栈不可用：backtrace 未采集到帧）\n";
    safe_write(note);
    report_write(note);
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
    if (const std::string extra = crash_extra_provider(); !extra.empty()) {
      safe_write(extra);
      report_write(extra);
    }
  }

  // ⑤ 崩溃前的日志尾部：**这是“不必复述问题”的核心**。
  //
  // 崩溃现场只能说明“死在哪里”，而“死之前程序在干什么”（哪次操作、什么参数、
  // 前一条警告）全在日志里。把尾部随报告一起落盘，用户只要交一个文件。
  //
  // 取不到（锁被占）时**如实说**，而不是静默留白——留白会让人以为“崩溃前无日志”。
  if (const std::string tail = try_recent_tail(200); !tail.empty()) {
    const std::string_view title =
        "\n  ── 崩溃前日志尾部（最多 200 行）────────────────\n";
    safe_write(title);
    report_write(title);
    safe_write(tail);
    report_write(tail);
  } else {
    const std::string_view note = "\n  （崩溃前日志尾部不可用：未开启落盘，或采样时锁被占）\n";
    safe_write(note);
    report_write(note);
  }

  // ④ 恢复默认处置并重发：保持退出码语义（外部工具靠它判断崩溃）
  if (crash_fd >= 0) {
    ::close(crash_fd);   // 关闭即 flush，不依赖信号处理器里的 fsync
    crash_fd = -1;
  }
  ::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
}

#endif

}  // namespace

void st::set_crash_extra_provider(std::string (*provider)()) { crash_extra_provider = provider; }

void st::install_crash_handler() {
  // 崩溃报告文件：**预先打开**（见 `crash_fd` 的说明）。只在尚未打开时做一次。
  if (crash_fd < 0 && !crash_report_target.empty()) {
    crash_fd = ::open(crash_report_target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (crash_fd < 0) {
      // **必须出声**：静默失败会让使用者以为“已开启崩溃报告”，
      // 直到真崩了才发现什么都没有（实测踩到：文件名含 `:` 时在 Windows 上
      // 必然失败，而报告一个字都没写）。
      std::fprintf(stderr, "[st-crash] 崩溃报告文件打不开（崩溃时只有 stderr）：%s\n",
                   crash_report_target.c_str());
      crash_report_target.clear();
    }
  }
#if defined(_WIN32)
  // DbgHelp 初始化：异常过滤器里的 `SymFromAddr` 依赖它。
  // `SymSetOptions` 必须包 `SYMOPT_DEFERRED_LOADS`——它会延迟到真正查符号时才
  // 去读 PDB，避免启动时卡几百毫秒（实测大二进制上明显）。
  ::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
  (void)::SymInitialize(::GetCurrentProcess(), nullptr, TRUE);
  ::SetUnhandledExceptionFilter(crash_filter);
#else
  ::signal(SIGSEGV, crash_signal);
  ::signal(SIGABRT, crash_signal);
  ::signal(SIGBUS, crash_signal);
  ::signal(SIGFPE, crash_signal);
#endif
}

auto st::crash_report_path() -> std::string { return crash_report_target; }

auto st::set_crash_report_path(std::string path) -> bool {
  if (crash_fd >= 0) return false;   // 已打开：不接受中途改（会丢已写内容）
  if (!path.empty()) {
    if (const std::size_t slash = path.find_last_of("/\\"); slash != std::string::npos) {
      (void)st::fs::create_directories(path.substr(0, slash));
    }
  }
  crash_report_target = std::move(path);
  return true;
}

auto st::finalize_crash_report() -> bool {
  if (crash_fd >= 0) {
    ::close(crash_fd);
    crash_fd = -1;
  }
  if (crash_report_target.empty()) return false;
  // 正常退出：报告里只有启动表头，没有诊断价值——删掉，免得事后误以为“崩过”。
  // 这也是“启动就建文件”这个代价的对价。
  (void)st::fs::remove_file(crash_report_target);
  crash_report_target.clear();
  return true;
}
