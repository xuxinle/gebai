#include "st/core/entry.hpp"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会静默破坏 `std::min`/`std::max`/`std::numeric_limits<T>::max()`
// 的调用点（实测报 C2589“非法标记”，与真正原因相隔很远）。必须在包含前关掉。
#define NOMINMAX 1
#include <windows.h>
// 平台边界：Windows 宽字符 API 与 UTF-8 的转换只能在这里发生（CONVENTIONS §10）。
// 中文命令行参数在 Windows 上是 ANSI（如 GBK）；不转就必然乱码。
#include <shellapi.h>
#endif

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

}  // namespace st
