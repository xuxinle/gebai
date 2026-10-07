/// 伪终端实现（双平台：Windows ConPTY / POSIX openpty）。
///
/// 平台差异**全部**在这一个文件里（`CONVENTIONS.md` §10 第 1 条）：系统头
/// （`<windows.h>` / `<pty.h>` / `<termios.h>`…）与平台 `char*` API 只出现在这里。
///
/// ## 两个平台最容易做错的点（都会表现为"能跑但不像终端"）
///
/// **Windows（ConPTY）**
/// * `CreateProcess` 必须带 `EXTENDED_STARTUPINFO_PRESENT` 并传
///   `STARTUPINFOEX.lpAttributeList = PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`；
/// * `bInheritHandles` 必须 **false**、三个标准句柄置**空**——
///   否则子进程会绕过伪控制台直接用宿主的 stdout（表现为输出泄漏到宿主控制台、
///   而 PTY 里收不到东西）；
/// * `HPCON` 必须在子进程起来后才能关掉**我们这一份句柄**（留着会让子进程退出时
///   `read` 不返回 EOF）。
///
/// **POSIX（openpty）**
/// * 子进程里要 `setsid()` + `ioctl(TIOCSCTTY)` 才能拿到**控制终端**——
///   缺了它 `SIGWINCH` 与作业控制（`Ctrl+Z`）都不工作；
/// * 主端（master）要设 `O_NONBLOCK` 或全程用 `select`/`poll` 等——否则
///   `terminate` 之后 `read` 可能仍堵着，"中止"就是假的；
/// * `TIOCSWINSZ` 要**先于**子进程 exec 设置一次，否则 shell 第一次排版按默认 80×24。

#include "st/core/pty.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

// `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` 要 Windows SDK 10.0.17763+ 才有；
// 它的**取值是 Win32 契约的一部分**（与 ABI 一起冻结），因此旧 SDK 下手工补上
// 是安全的——比“要求工具链升级”现实得多（本仓的 mingw 就缺它）。
// 定义与 `winbase.h` / `processthreadsapi.h` 一致：`ProcThreadAttributeValue(22, ...)`。
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
  (ProcThreadAttributeValue(22, FALSE, TRUE, FALSE))
#endif
#else
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace st::process {

namespace {

#if defined(_WIN32)

/// ConPTY 的三个入口是**运行时**从 kernel32 取的：Win10 1809 之前的系统没有它们，
/// 静态导入会让程序起不来。取不到就如实降级（`supported()` 返回 false）。
struct ConPtyApi {
  using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
  using ResizePseudoConsoleFn = HRESULT(WINAPI*)(HPCON, COORD);
  using ClosePseudoConsoleFn = void(WINAPI*)(HPCON);

  CreatePseudoConsoleFn create{nullptr};
  ResizePseudoConsoleFn resize{nullptr};
  ClosePseudoConsoleFn close{nullptr};
  bool available{false};

  ConPtyApi() {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) return;
    create = reinterpret_cast<CreatePseudoConsoleFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "CreatePseudoConsole")));
    resize = reinterpret_cast<ResizePseudoConsoleFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "ResizePseudoConsole")));
    close = reinterpret_cast<ClosePseudoConsoleFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "ClosePseudoConsole")));
    available = create != nullptr && resize != nullptr && close != nullptr;
  }
};

[[nodiscard]] auto conpty_api() -> const ConPtyApi& {
  static const ConPtyApi api;
  return api;
}

/// UTF-8 → UTF-16（`CreateProcessW` 要宽字符；路径/用户名里的中文必须走这条，
/// ANSI 版 `CreateProcessA` 会把它们变问号）。
[[nodiscard]] auto widen(const std::string& text) -> std::wstring {
  if (text.empty()) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
  return out;
}

/// 命令行拼装：Windows 的参数需要引号转义（空格、引号、结尾反斜杠）。
///
/// 规则来自 `CreateProcessW` 的解析口径（与 CRT 的 `argv` 约定一致）：
/// 反斜杠只有在**紧邻引号**时才是转义字符，因此要数着反斜杠的个数补。
[[nodiscard]] auto quote_arg(const std::string& arg) -> std::wstring {
  const std::wstring wide = widen(arg);
  if (!wide.empty() && wide.find_first_of(L" \t\n\v\"") == std::wstring::npos) return wide;
  std::wstring out = L"\"";
  for (auto it = wide.begin();; ++it) {
    std::size_t slashes = 0;
    while (it != wide.end() && *it == L'\\') {
      ++it;
      ++slashes;
    }
    if (it == wide.end()) {
      out.append(slashes * 2, L'\\');   // 结尾反斜杠要翻倍，免得把收尾引号转义掉
      break;
    }
    if (*it == L'"') {
      out.append(slashes * 2 + 1, L'\\');
      out.push_back(*it);
    } else {
      out.append(slashes, L'\\');
      out.push_back(*it);
    }
  }
  out.push_back(L'"');
  return out;
}

[[nodiscard]] auto build_command_line(const std::string& program,
                                      const std::vector<std::string>& argv) -> std::wstring {
  std::wstring line = quote_arg(program);
  for (const auto& arg : argv) {
    line.push_back(L' ');
    line.append(quote_arg(arg));
  }
  return line;
}

#else

/// POSIX：`argv` 要 `char* const[]` 并且以 nullptr 收尾。
[[nodiscard]] auto to_argv(const std::string& program, const std::vector<std::string>& argv)
    -> std::vector<char*> {
  std::vector<char*> out;
  out.reserve(argv.size() + 2);
  out.push_back(const_cast<char*>(program.c_str()));
  for (const auto& arg : argv) out.push_back(const_cast<char*>(arg.c_str()));
  out.push_back(nullptr);
  return out;
}

#endif

}  // namespace

struct PtySession::Impl {
#if defined(_WIN32)
  HPCON console{nullptr};
  HANDLE in_write{nullptr};    ///< 我们写 → 子进程读
  HANDLE out_read{nullptr};    ///< 子进程写 → 我们读
  PROCESS_INFORMATION child{};
  std::atomic<bool> done{false};
  int code{0};
#else
  int master{-1};
  pid_t child{-1};
  std::atomic<bool> done{false};
  int code{0};
#endif
};

PtySession::PtySession() = default;

PtySession::~PtySession() {
  if (impl_ == nullptr) return;
  // 析构要收干净：先杀子进程（否则留在后台），再放句柄/描述符。
  terminate();
  (void)wait();
#if defined(_WIN32)
  const auto& api = conpty_api();
  if (impl_->console != nullptr && api.available) api.close(impl_->console);
  if (impl_->in_write != nullptr) CloseHandle(impl_->in_write);
  if (impl_->out_read != nullptr) CloseHandle(impl_->out_read);
  if (impl_->child.hThread != nullptr) CloseHandle(impl_->child.hThread);
  if (impl_->child.hProcess != nullptr) CloseHandle(impl_->child.hProcess);
#else
  if (impl_->master >= 0) close(impl_->master);
#endif
}

PtySession::PtySession(PtySession&&) noexcept = default;
auto PtySession::operator=(PtySession&&) noexcept -> PtySession& = default;

auto PtySession::supported() -> bool {
#if defined(_WIN32)
  return conpty_api().available;
#else
  return true;   // openpty 在 POSIX 上是基础能力
#endif
}

void PtySession::open(const std::string& program, const std::vector<std::string>& argv,
                      const std::string& cwd, PtySize size) {
  if (impl_ != nullptr) {
    error_ = "会话已打开";
    return;
  }
  auto impl = std::make_unique<Impl>();
  // 尺寸兜底：0 会让 PTY 创建失败（Windows 上直接返回 E_INVALIDARG）。
  if (size.cols <= 0) size.cols = 80;
  if (size.rows <= 0) size.rows = 24;

#if defined(_WIN32)
  const auto& api = conpty_api();
  if (!api.available) {
    error_ = "本机不支持伪控制台（需要 Windows 10 1809+）";
    return;
  }
  // 两个管道：一个给子进程读（我们写），一个给孩子写（我们读）。
  HANDLE in_read = nullptr, out_write = nullptr;
  SECURITY_ATTRIBUTES attrs{};
  attrs.nLength = sizeof(attrs);
  attrs.bInheritHandle = TRUE;   // 这两个端点要能被子进程继承
  if (!CreatePipe(&in_read, &impl->in_write, &attrs, 0) ||
      !CreatePipe(&impl->out_read, &out_write, &attrs, 0)) {
    error_ = "创建管道失败";
    return;
  }
  if (api.create(COORD{static_cast<short>(size.cols), static_cast<short>(size.rows)}, in_read,
                 out_write, 0, &impl->console) != S_OK) {
    error_ = "CreatePseudoConsole 失败";
    CloseHandle(in_read);
    CloseHandle(out_write);
    CloseHandle(impl->in_write);
    CloseHandle(impl->out_read);
    impl->in_write = nullptr;
    impl->out_read = nullptr;
    return;
  }
  // 伪控制台**已经持有**这两个端点的一份；我们这一份要放掉，
  // 否则子进程退出时读端不会 EOF（会一直挂着）。
  CloseHandle(in_read);
  CloseHandle(out_write);

  // 属性表里挂上伪控制台句柄——这是 ConPTY 唯一的挂接方式。
  SIZE_T list_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, list_size));
  if (list == nullptr || !InitializeProcThreadAttributeList(list, 1, 0, &list_size) ||
      !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, impl->console,
                                 sizeof(impl->console), nullptr, nullptr)) {
    error_ = "设置伪控制台属性失败";
    if (list != nullptr) HeapFree(GetProcessHeap(), 0, list);
    api.close(impl->console);
    impl->console = nullptr;
    return;
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = list;
  // **三个标准句柄必须留空**：留非空会让子进程绕过伪控制台直接用宿主 stdio
  //（输出泄漏到宿主控制台，PTY 里收不到东西——本文件开头的两个高频错之一）。
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = nullptr;
  startup.StartupInfo.hStdOutput = nullptr;
  startup.StartupInfo.hStdError = nullptr;

  std::wstring command = build_command_line(program, argv);
  std::wstring workdir = widen(cwd);
  // 可写缓冲：`CreateProcessW` 会**就地改写**命令行（Unicode 版要求）。
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
  const BOOL ok = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr,
                                 FALSE,   // bInheritHandles 必须 false（同上）
                                 flags, nullptr, cwd.empty() ? nullptr : workdir.c_str(),
                                 &startup.StartupInfo, &impl->child);
  DeleteProcThreadAttributeList(list);
  HeapFree(GetProcessHeap(), 0, list);
  if (!ok) {
    error_ = "启动子进程失败（错误码 " + std::to_string(GetLastError()) + "）";
    api.close(impl->console);
    impl->console = nullptr;
    return;
  }
#else
  // POSIX：`openpty` 一把拿到主/从两端，子进程 `login_tty` 挂上从端。
  int master = -1, slave = -1;
  winsize ws{};
  ws.ws_col = static_cast<unsigned short>(size.cols);
  ws.ws_row = static_cast<unsigned short>(size.rows);
  if (openpty(&master, &slave, nullptr, nullptr, &ws) != 0) {
    error_ = "openpty 失败";
    return;
  }
  const pid_t child = fork();
  if (child < 0) {
    error_ = "fork 失败";
    close(master);
    close(slave);
    return;
  }
  if (child == 0) {
    // —— 子进程 ——
    close(master);
    // `login_tty` = setsid + TIOCSCTTY + dup2 三个标准描述符。
    // **控制终端**靠它拿到：缺了 `SIGWINCH`/作业控制都不工作（本文件开头的高频错）。
    if (login_tty(slave) != 0) _exit(127);
    if (!cwd.empty()) {
      if (chdir(cwd.c_str()) != 0) { /* 目录不存在时保持继承的 cwd，不阻断 */ }
    }
    auto args = to_argv(program, argv);
    execvp(program.c_str(), args.data());
    _exit(127);   // exec 失败（PATH 里没有）
  }
  // —— 父进程 ——
  close(slave);
  impl->master = master;
  impl->child = child;
#endif

  impl_ = std::move(impl);
}

auto PtySession::read(char* buffer, std::size_t capacity) -> std::size_t {
  if (impl_ == nullptr || capacity == 0) return 0;
#if defined(_WIN32)
  DWORD got = 0;
  if (!ReadFile(impl_->out_read, buffer, static_cast<DWORD>(capacity), &got, nullptr)) {
    impl_->done.store(true);
    return 0;
  }
  if (got == 0) impl_->done.store(true);
  return got;
#else
  const ssize_t got = ::read(impl_->master, buffer, capacity);
  if (got <= 0) {
    impl_->done.store(true);
    return 0;
  }
  return static_cast<std::size_t>(got);
#endif
}

auto PtySession::write(const char* data, std::size_t length) -> std::size_t {
  if (impl_ == nullptr || length == 0) return 0;
#if defined(_WIN32)
  DWORD written = 0;
  if (!WriteFile(impl_->in_write, data, static_cast<DWORD>(length), &written, nullptr)) return 0;
  return written;
#else
  const ssize_t written = ::write(impl_->master, data, length);
  return written > 0 ? static_cast<std::size_t>(written) : 0;
#endif
}

void PtySession::resize(PtySize size) {
  if (impl_ == nullptr) return;
  if (size.cols <= 0) size.cols = 80;
  if (size.rows <= 0) size.rows = 24;
#if defined(_WIN32)
  const auto& api = conpty_api();
  if (impl_->console != nullptr && api.available) {
    api.resize(impl_->console,
               COORD{static_cast<short>(size.cols), static_cast<short>(size.rows)});
  }
#else
  winsize ws{};
  ws.ws_col = static_cast<unsigned short>(size.cols);
  ws.ws_row = static_cast<unsigned short>(size.rows);
  (void)ioctl(impl_->master, TIOCSWINSZ, &ws);
  (void)kill(impl_->child, SIGWINCH);
#endif
}

auto PtySession::wait() -> int {
  if (impl_ == nullptr) return -1;
  if (impl_->done.load() && impl_->code != 0) return impl_->code;
#if defined(_WIN32)
  // ⚠ **句柄可能已被 `terminate` 释放**（它杀完子进程就把 `hProcess` 关了）。
  // 拿一个已关闭的句柄去 `WaitForSingleObject` 是未定义行为（实测会挂住 ——
  // 那是“关会话把界面卡死”的另一条路径）。已释放时直接报“已终止”。
  if (impl_->child.hProcess == nullptr) {
    impl_->done.store(true);
    if (impl_->code == 0) impl_->code = -1;
    return impl_->code;
  }
  // 等待封顶：UI 线程会调 `wait()`（收尾时），**不能无限等**——
  // 万一子进程卡在不可中断的状态（或伪控制台内部分状况），
  // 整个界面就冻住了。给 5 秒上限，超时就按“已终止”处理（进程由系统回收）。
  const DWORD waited = WaitForSingleObject(impl_->child.hProcess, 5000);
  if (waited == WAIT_TIMEOUT) {
    impl_->done.store(true);
    impl_->code = -1;
    return impl_->code;
  }
  DWORD code = 0;
  GetExitCodeProcess(impl_->child.hProcess, &code);
  impl_->code = static_cast<int>(code);
  impl_->done.store(true);
  return impl_->code;
#else
  if (impl_->child > 0) {
    int status = 0;
    (void)waitpid(impl_->child, &status, 0);
    if (WIFEXITED(status)) impl_->code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) impl_->code = 128 + WTERMSIG(status);
    impl_->child = -1;
  }
  impl_->done.store(true);
  return impl_->code;
#endif
}

void PtySession::terminate() {
  if (impl_ == nullptr) return;
#if defined(_WIN32)
  if (impl_->child.hProcess != nullptr) {
    TerminateProcess(impl_->child.hProcess, 1);
  }
  // 顺序很重要：
  //
  // ① **先关伪控制台**。它一关，ConPTY 输出端的写者就没了，读端会收到 EOF
  //    （`ReadFile` 返回 0）——**不需要**去 `CloseHandle` 一个正被读的句柄。
  //    在 `ReadFile` 阻塞期间 `CloseHandle` 那个句柄是**未定义行为**
  //（实测：会让读线程永远退不出 ⇒ UI 线程 `join()` 时主线程死锁）。
  // ② `TerminateProcess` 保证子进程一定不在（`wait()` 不会永久阻塞）。
  const auto& api = conpty_api();
  if (impl_->console != nullptr && api.available) {
    api.close(impl_->console);
    impl_->console = nullptr;
  }
  // ③ 子进程早已杀，句柄可以安全释放（此时已无并发的读）。
  if (impl_->child.hProcess != nullptr) {
    CloseHandle(impl_->child.hProcess);
    impl_->child.hProcess = nullptr;
  }
  if (impl_->child.hThread != nullptr) {
    CloseHandle(impl_->child.hThread);
    impl_->child.hThread = nullptr;
  }
#else
  if (impl_->child > 0) {
    (void)kill(impl_->child, SIGTERM);
    // 稍等一档再 SIGKILL：给 shell 清理的机会（不留僵尸）。
    for (int i = 0; i < 20; ++i) {
      int status = 0;
      const pid_t r = waitpid(impl_->child, &status, WNOHANG);
      if (r == impl_->child) {
        impl_->child = -1;
        return;
      }
      struct timespec ts{0, 5 * 1000 * 1000};   // 5ms
      nanosleep(&ts, nullptr);
    }
    (void)kill(impl_->child, SIGKILL);
  }
  // 主端关掉 → `read` 必然返回 0（"中止"不能是假的）。
  if (impl_->master >= 0) {
    close(impl_->master);
    impl_->master = -1;
  }
#endif
}

auto PtySession::valid() const noexcept -> bool {
#if defined(_WIN32)
  return impl_ != nullptr && impl_->console != nullptr;
#else
  return impl_ != nullptr && impl_->master >= 0;
#endif
}

auto PtySession::error() const -> std::string { return error_; }

auto PtySession::exited() const noexcept -> bool {
  return impl_ != nullptr && impl_->done.load();
}

auto PtySession::exit_code() const noexcept -> int {
  return impl_ != nullptr ? impl_->code : -1;
}

}  // namespace st::process
