// 平台边界：本文件集中封装「用系统默认程序打开 URL」（CONVENTIONS §2 R6 的受控例外）。
//
// 三平台各有自己的正门：
//   - Windows：`ShellExecuteW(..., "open", ...)`——**不要**用 `system("start ...")`：
//     后者会闪一个控制台黑框，且把 URL 拼进命令行意味着引号/`&` 都可能被解释
//     （浏览器的 URL 里出现 `&` 是常态）。
//   - macOS：`open <url>`。
//   - Linux/BSD：`xdg-open <url>`（freedesktop 标准；无桌面环境时通常不存在，
//     这不算错误——返回 false 让调用方如实上报）。
//
// **不等**子进程结束：浏览器通常比我们活得久，等它会挂住调用线程。
// Windows 的 `ShellExecuteW` 本身就是「投递后返回」，不存在等待问题。

#include "st/core/platform.hpp"

#if defined(_WIN32)
#define NOMINMAX 1
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace st::platform {

namespace {

#if defined(_WIN32)

/// UTF-8 → UTF-16（Windows 宽字符 API 的入参）。
/// 不引 `st::string` 的转换：这里就是平台边界，就地做最直接。
auto widen(std::string_view utf8) -> std::wstring {
  if (utf8.empty()) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                          static_cast<int>(utf8.size()), out.data(), needed);
  if (written <= 0) return {};
  out.resize(static_cast<std::size_t>(written));
  return out;
}

auto open_external_impl(std::string_view url) -> bool {
  const std::wstring wide = widen(url);
  if (wide.empty()) return false;
  // 返回 HINSTANCE > 32 表示成功（Win32 的约定；≤32 是错误码）。
  const HINSTANCE result = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr,
                                         SW_SHOWNORMAL);
  return reinterpret_cast<std::intptr_t>(result) > 32;
}

#else

auto open_external_impl(std::string_view url) -> bool {
  // macOS 的前门是 `open`；其他 POSIX 用 `xdg-open`（freedesktop 标准）。
  // 两端都经 fork+exec（不走 shell）——URL 里的 `&`/引号因此不可能被解释。
#if defined(__APPLE__)
  constexpr const char* kOpener = "open";
#else
  constexpr const char* kOpener = "xdg-open";
#endif
  const pid_t pid = fork();
  if (pid < 0) return false;   // fork 失败
  if (pid == 0) {
    // 子进程：把 stderr 丢掉（找不到打开器时的报错不该污染应用日志），
    // 然后 exec。`execlp` 失败 = 没有这个程序 → `_exit` 一个非零码。
    const pid_t grandchild = fork();
    if (grandchild == 0) {
      execlp(kOpener, kOpener, std::string(url).c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    _exit(grandchild < 0 ? 1 : 0);
  }
  // 等**中间子进程**（它 fork 完就退，不会挂住我们）；真正的 xdg-open 被
  // 过继给 init，与我们脱钩——这正是「不等目标程序结束」的实现方式。
  int status = 0;
  (void)waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif

}  // namespace

auto open_external(std::string_view url) -> bool {
  if (url.empty()) return false;
  return open_external_impl(url);
}

}  // namespace st::platform
