#include "st/core/entry.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/log_file.hpp"
#include "st/core/process.hpp"
#include "st/core/time.hpp"

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会静默破坏 `std::min`/`std::max`/`std::numeric_limits<T>::max()`
// 的调用点（实测报 C2589“非法标记”，与真正原因相隔很远）。必须在包含前关掉。
#define NOMINMAX 1
#include <windows.h>
// 平台边界：Windows 宽字符 API 与 UTF-8 的转换只能在这里发生（CONVENTIONS §10）。
// 中文命令行参数在 Windows 上是 ANSI（如 GBK）；不转就必然乱码。
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include <cstdint>
#include <format>

namespace st {
namespace {

#if defined(_WIN32)
/// 宽字符 → UTF-8（Windows 的宽字符是真 Unicode，框架内部是 UTF-8）。
[[nodiscard]] auto wide_to_utf8(const wchar_t* text) -> std::string {
  if (text == nullptr) return {};
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed - 1), '\0');  // 去掉结尾 NUL
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
  return out;
}
#endif

}  // namespace

// 崩溃处理器实现见 platform_crash.cpp（平台 API 与位级重解释单点封装，CONVENTIONS.md §10）。

void startup_configure_console() {
#if defined(_WIN32)
  // 控制台代码页设为 UTF-8：否则 `st::print` 输出的中文在 cmd/PowerShell 里是乱码。
  // 失败无副作用（例如被重定向到管道时会失败），因此不检查返回值。
  ::SetConsoleOutputCP(CP_UTF8);
  ::SetConsoleCP(CP_UTF8);
#endif
}

auto startup_arguments() -> std::vector<std::string> {
  std::vector<std::string> arguments;
#if defined(_WIN32)
  // **Windows：必须走宽字符命令行**。`argv` 是 ANSI 编码，中文参数会坏；
  // `GetCommandLineW` + `CommandLineToArgvW` 才是它的真命令行，转换后即为 UTF-8。
  int count = 0;
  wchar_t** wide_arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
  if (wide_arguments == nullptr) return arguments;
  arguments.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    arguments.push_back(wide_to_utf8(wide_arguments[index]));
  }
  ::LocalFree(wide_arguments);
#else
  // POSIX：`argv` 已是 UTF-8（约定），但 `startup_arguments()` 需要自己拿到它。
  // 用 `/proc/self/cmdline`（字段以 NUL 分隔）而不是声明 main：这样本函数可以在
  // 任意位置调用，不依赖宏展开顺序。
  const std::string path = "/proc/self/cmdline";
  if (FILE* file = std::fopen(path.c_str(), "rb"); file != nullptr) {
    std::string buffer;
    char chunk[512];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) buffer.append(chunk, read);
    std::fclose(file);
    std::size_t begin = 0;
    while (begin < buffer.size()) {
      const std::size_t end = buffer.find('\0', begin);
      if (end == std::string::npos) break;
      arguments.emplace_back(buffer.substr(begin, end - begin));
      begin = end + 1;
    }
  }
#endif
  return arguments;
}

namespace {

/// 从 `name=value` 形态的命令行项里取值（`--log-file X` 与 `--log-file=X` 两种都认）。
///
/// 为何在这里粗解析一遍而不是等 `parse_common_options`：诊断必须在**尽可能早**的
/// 时刻就绪（越早开，早期日志越不容易丢），而通用选项解析要等应用把 `AppOptions`
/// 建起来。两处都认同一组开关，优先级以命令行显式值最高。
[[nodiscard]] auto scan_option(int argc, char** argv, std::string_view name) -> std::string {
  const std::string prefix = std::string(name) + "=";
  for (int index = 1; index < argc; ++index) {
    if (argv[index] == nullptr) break;
    const std::string_view item(argv[index]);
    if (item == name) {
      if (index + 1 < argc && argv[index + 1] != nullptr) return argv[index + 1];
      return {};
    }
    if (item.rfind(prefix, 0) == 0) return std::string(item.substr(prefix.size()));
  }
  return {};
}

/// 环境变量（未设置时返回 `fallback`）。
[[nodiscard]] auto env_or(const char* name, std::string fallback) -> std::string {
  const char* value = std::getenv(name);   // NOLINT：跨平台（Windows 上 CRT 已兼容）
  return (value == nullptr || *value == '\0') ? std::move(fallback) : std::string(value);
}

}  // namespace

auto crash_report_file_name(std::int64_t unix_millis, unsigned long pid) -> std::string {
  // 时间戳里的 `:` 换成 `-`（见头文件里那条契约）。
  std::string stamp = st::time::iso8601_utc(unix_millis);
  for (char& ch : stamp) {
    if (ch == ':') ch = '-';
  }
  return std::format("crash-{}-{}.log", stamp, pid);
}

void startup_configure_diagnostics(int argc, char** argv) {
  using namespace st::log;
  // 日志文件：命令行 > 环境变量 > 不落盘（保持旧行为，不静默改变已有应用的输出）。
  std::string log_path = scan_option(argc, argv, "--log-file");
  if (log_path.empty()) log_path = env_or("ST_LOG_FILE", {});
  if (!log_path.empty()) {
    FileOptions options{};
    options.path = log_path;
    if (const std::string from_argv = scan_option(argc, argv, "--log-level"); !from_argv.empty()) {
      set_level(level_from_name(from_argv));
    } else if (const std::string from_env = env_or("ST_LOG_LEVEL", {}); !from_env.empty()) {
      set_level(level_from_name(from_env));
    }
    // 失败不中止：日志系统自身不能成为故障源（`open_file` 已打一条 stderr 告警）。
    (void)open_file(options);
    st::log::info("日志落盘：{}", options.path);
  }

  // 崩溃报告：目录 + 时间戳 + pid 组文件名。
  // 为何带 pid 与时间戳：同一台机器可能同时跑多个实例（智能体并行驱动），
  // 固定文件名会互相覆盖，而崩溃报告是“不能丢”的东西。
  std::string crash_dir = scan_option(argc, argv, "--crash-dir");
  if (crash_dir.empty()) crash_dir = env_or("ST_CRASH_DIR", {});
  if (!crash_dir.empty()) {
    // 文件名交给 `crash_report_file_name`——那里有一条只能靠回归测试钉住的契约
    // （文件名不得含 `:`，否则 Windows 上 open 失败、报告静默不生成）。
    const std::string name =
        crash_report_file_name(st::time::unix_ms(), static_cast<unsigned long>(st::process::current_id()));
    if (!set_crash_report_path(st::fs::join(crash_dir, name))) {
      st::log::warn("崩溃报告路径设置失败（崩溃时将只有 stderr）");
    }
  }
}

void finalize_diagnostics() {
  (void)st::log::file_active();   // 保序：先关日志再删崩溃报告
  st::log::close_file();
  (void)st::finalize_crash_report();
}

}  // namespace st
