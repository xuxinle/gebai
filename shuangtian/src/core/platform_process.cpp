// 平台边界：本文件集中封装系统进程 API（CONVENTIONS §2 R6 的受控例外——execvp/char* 边界）。
#include "st/core/process.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <system_error>
#include <thread>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会**静默破坏**所有 `std::min`/`std::max`/
// `std::numeric_limits<T>::max()` 调用点（报错文本是“C2589 非法标记”，离真正原因很远）。
// 必须在包含前关掉。
#define NOMINMAX 1
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace st::process {
namespace {

#if defined(_WIN32)

/// UTF-8 → UTF-16（宽字符才是 Windows 的真 Unicode 接口；框架内部一律 UTF-8）。
[[nodiscard]] auto to_wide(std::string_view utf8) -> std::wstring {
  if (utf8.empty()) return {};
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

/// UTF-16 → UTF-8。
[[nodiscard]] auto to_utf8(std::wstring_view wide) -> std::string {
  if (wide.empty()) return {};
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed,
                        nullptr, nullptr);
  return out;
}

/// 按 MSVCRT 的参数解析规则给单个参数加引号。
///
/// **为什么必须自己写**：`CreateProcessW` 只收一整条命令行字符串，参数边界靠引号与反斜杠的约定
/// （引号前连续的反斜杠要成对翻倍，末尾反斜杠同理）。拼错不会报错，只会把编译器的路径参数
/// 截断或粘成两个参数——而 `C:\Program Files\...` 这类路径在本仓库里到处都是。
[[nodiscard]] auto quote_argument(std::wstring_view text) -> std::wstring {
  if (!text.empty() && text.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(text);
  }
  std::wstring out;
  out.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t ch : text) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(ch);
  }
  // 结尾的反斜杠必须成对：否则它会转义我们随后写入的收尾引号
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

/// 组成 `CreateProcessW` 的命令行（程序路径 + 逐个加引号的参数）。
[[nodiscard]] auto build_command_line(std::string_view program, const std::vector<std::string>& args)
    -> std::wstring {
  std::wstring line = quote_argument(to_wide(program));
  for (const auto& argument : args) {
    line.push_back(L' ');
    line.append(quote_argument(to_wide(argument)));
  }
  return line;
}

/// 环境变量条目的**比较键**（Windows 的环境变量名不区分大小写：`Path` 与 `PATH` 是同一个）。
[[nodiscard]] auto env_key(std::wstring_view entry) -> std::wstring {
  const std::size_t position = entry.find(L'=');
  std::wstring key(entry.substr(0, position == std::wstring_view::npos ? entry.size() : position));
  for (auto& ch : key) {
    if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
  }
  return key;
}

/// 构造子进程环境块（宽字符、双 NUL 结尾、按名排序）。
///
/// **为什么不能“直接追加同名变量”**：环境变量名大小写不敏感，追加会同时留下两份 `PATH`/`INCLUDE`，
/// 生效的是哪一份由内部查找顺序决定——表现为“明明注入了工具链环境，编译器却找不到头文件”。
/// 因此先剔除被覆盖的旧条目，再合并。
[[nodiscard]] auto build_environment(const std::map<std::string, std::string>& overrides)
    -> std::vector<wchar_t> {
  std::vector<std::wstring> entries;
  std::vector<std::wstring> replaced_keys;
  replaced_keys.reserve(overrides.size());
  for (const auto& [key, value] : overrides) {
    entries.push_back(to_wide(std::format("{}={}", key, value)));
    replaced_keys.push_back(env_key(entries.back()));
  }
  if (wchar_t* const source = ::GetEnvironmentStringsW(); source != nullptr) {
    for (const wchar_t* entry = source; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
      if (std::ranges::find(replaced_keys, env_key(entry)) != replaced_keys.end()) continue;
      entries.emplace_back(entry);
    }
    (void)::FreeEnvironmentStringsW(source);
  }
  std::ranges::sort(entries);
  std::vector<wchar_t> block;
  for (const auto& entry : entries) {
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

/// CRLF → LF：Windows 的编译器输出行尾是 `\r\n`，原样带进错误信息/依赖清单会污染解析
/// （也保证同一个构建在三个平台上给出同样的文本）。
void normalize_newlines(std::string& text) {
  std::string cleaned;
  cleaned.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') continue;
    cleaned.push_back(text[index]);
  }
  text = std::move(cleaned);
}

/// 系统错误码 → 可读消息。
[[nodiscard]] auto system_message(unsigned long code) -> std::string {
  return std::system_category().message(static_cast<int>(code));
}

/// 句柄的 RAII 包装（系统句柄是 `void*`，本文件是 CONVENTIONS §2 R6 的受控例外）。
struct Handle {
  Handle() = default;
  explicit Handle(HANDLE handle) noexcept : value_(handle) {}
  Handle(const Handle&) = delete;
  auto operator=(const Handle&) -> Handle& = delete;
  Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
  auto operator=(Handle&& other) noexcept -> Handle& {
    if (this != &other) {
      reset(other.value_);
      other.value_ = nullptr;
    }
    return *this;
  }
  ~Handle() { reset(nullptr); }
  void reset(HANDLE next) noexcept {
    if (value_ != nullptr) (void)::CloseHandle(value_);
    value_ = next;
  }
  [[nodiscard]] auto get() const noexcept -> HANDLE { return value_; }

 private:
  HANDLE value_{nullptr};
};

/// 从管道读到 EOF（子进程退出后写端关闭，`ReadFile` 读到 0）。
void read_pipe_to_end(HANDLE pipe, std::string& sink) {
  std::array<char, 64 * 1024> buffer{};
  DWORD read = 0;
  while (::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != 0 &&
         read > 0) {
    sink.append(buffer.data(), read);
  }
}

/// 非 UTF-8 文本 → UTF-8（按本地 ANSI 代码页解释）。
///
/// **为什么需要**：Windows 上大量工具（MSVC 的 `cl.exe`、各种本地化 CLI）按本地代码页
/// （中文机器上是 GBK）写诊断信息，而管道拿到的是**原始字节**——直接当 UTF-8 用就是满屏乱码，
/// 而"编译失败"时最需要的就是那段信息。
/// 判定方式：先按 UTF-8 校验，通过就原样保留（本框架自己的子进程输出就是 UTF-8，
/// 例如 `st run gallery` 捕获的应用输出）；不通过才按 ANSI 转。
void normalize_encoding(std::string& text) {
  if (text.empty() || st::utf8_is_valid(text)) return;
  const int count = static_cast<int>(text.size());
  const int needed = ::MultiByteToWideChar(CP_ACP, 0, text.data(), count, nullptr, 0);
  if (needed <= 0) return;
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_ACP, 0, text.data(), count, wide.data(), needed);
  text = to_utf8(wide);
}

#else

struct PipeSet {
  int read_fd{-1};
  int write_fd{-1};
};

[[nodiscard]] auto make_pipe(PipeSet& pipe) -> bool {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) return false;
  pipe.read_fd = fds[0];
  pipe.write_fd = fds[1];
  return true;
}

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

[[nodiscard]] auto drain(int fd, std::string& sink, bool blocking) -> bool {
  std::array<char, 8192> buffer{};
  while (true) {
    const auto count = ::read(fd, buffer.data(), buffer.size());
    if (count > 0) {
      sink.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0) return true;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (!blocking) return true;
      pollfd entry{};
      entry.fd = fd;
      entry.events = POLLIN;
      if (::poll(&entry, 1, 100) <= 0) return true;
      continue;
    }
    return false;
  }
}

#endif

}  // namespace

auto run(std::string_view program, const std::vector<std::string>& args, const Options& options)
    -> Result<RunResult> {
  const std::int64_t start_ns = time::now_ns();
  const std::string program_text(program);

#if defined(_WIN32)
  // Windows 分支：`CreateProcessW` 直接启动，不经 cmd.exe。
  //
  // **为什么不能再用 `std::system`**（本文件曾经如此，四个问题都造成了实际故障）：
  // ① 整条命令行交给 cmd.exe 二次解析——`&`、`^`、`%VAR%`、`>` 都是 cmd 的元字符，
  //    编译器命令行里出现它们会被静默改写；
  // ② `cwd` 与 `env` 只能靠 `cd`/`set` 前缀拼接，参数里一旦含引号就拼错（而 `Options` 里的
  //    这两个字段在 Windows 上**被直接丢掉了**）；
  // ③ 输出重定向到**固定文件名**的临时文件——本框架默认并行编译，多个进程互相踩踏；
  // ④ 拿不到真实退出码，编译失败可能被当成成功。
  // 注意：`CreateProcessW` 的第二个参数是 `LPWSTR`（非 const）——它可能在原地修改缓冲区，
  // 因此命令行必须放在**可写**的 `std::wstring` 里。
  std::wstring command_line = build_command_line(program_text, args);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child{};
  Handle out_read;
  Handle out_write;
  Handle err_read;
  Handle err_write;
  bool inherit_handles = false;
  DWORD creation_flags = 0;
  if (options.capture_output) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE raw_out_read = nullptr;
    HANDLE raw_out_write = nullptr;
    if (::CreatePipe(&raw_out_read, &raw_out_write, &attributes, 0) == 0) {
      return unexpected(ErrorCode::Io,
                        std::format("创建输出管道失败: {}", system_message(::GetLastError())));
    }
    out_read.reset(raw_out_read);
    out_write.reset(raw_out_write);
    // 读端不得继承：否则子进程自己再启动进程时会把读端一并传下去，读端永远不关闭 → 读线程死等
    (void)::SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0);
    if (!options.merge_stderr) {
      HANDLE raw_err_read = nullptr;
      HANDLE raw_err_write = nullptr;
      if (::CreatePipe(&raw_err_read, &raw_err_write, &attributes, 0) == 0) {
        return unexpected(ErrorCode::Io,
                          std::format("创建错误输出管道失败: {}", system_message(::GetLastError())));
      }
      err_read.reset(raw_err_read);
      err_write.reset(raw_err_write);
      (void)::SetHandleInformation(err_read.get(), HANDLE_FLAG_INHERIT, 0);
    }
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdOutput = out_write.get();
    startup.hStdError = options.merge_stderr ? out_write.get() : err_write.get();
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    inherit_handles = true;
    // 输出已经重定向，子进程不需要控制台：不设该标志时，无控制台的宿主（服务/守护进程）下
    // 每个编译器进程都会创建/弹出一个控制台窗口，并行构建时刷屏。
    creation_flags |= CREATE_NO_WINDOW;
  }
  std::vector<wchar_t> environment_block;
  // `CreateProcessW` 的环境块参数是 `LPVOID`（非 const）：必须传可写指针（`data()` 即可）
  wchar_t* environment = nullptr;
  if (!options.env.empty()) {
    environment_block = build_environment(options.env);
    environment = environment_block.data();
    creation_flags |= CREATE_UNICODE_ENVIRONMENT;
  }
  const std::wstring working_directory = to_wide(options.cwd);
  const BOOL created =
      ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, inherit_handles ? TRUE : FALSE,
                       creation_flags, environment,
                       working_directory.empty() ? nullptr : working_directory.c_str(), &startup,
                       &child);
  const DWORD create_error = ::GetLastError();
  // 父进程立刻关掉写端：子进程退出后读端才会读到 EOF（否则读线程会永久阻塞在 ReadFile）
  out_write.reset(nullptr);
  err_write.reset(nullptr);
  if (created == 0) {
    return unexpected(ErrorCode::NotFound, std::format("无法启动 '{}': {}", program_text,
                                                       system_message(create_error)));
  }
  Handle process_handle(child.hProcess);
  Handle thread_handle(child.hThread);

  RunResult result;
  std::optional<std::jthread> out_reader;
  std::optional<std::jthread> err_reader;
  if (options.capture_output) {
    const HANDLE out_target = out_read.get();
    out_reader.emplace([&result, out_target]() { read_pipe_to_end(out_target, result.stdout_text); });
    if (!options.merge_stderr) {
      const HANDLE err_target = err_read.get();
      err_reader.emplace(
          [&result, err_target]() { read_pipe_to_end(err_target, result.stderr_text); });
    }
  }
  if (::WaitForSingleObject(process_handle.get(), INFINITE) == WAIT_FAILED) {
    return unexpected(ErrorCode::Internal, std::format("等待子进程失败 '{}': {}", program_text,
                                                       system_message(::GetLastError())));
  }
  // 子进程已退出 → 写端全部关闭 → 读线程读到 EOF 后自然结束（reset 即 join，不丢输出）
  out_reader.reset();
  err_reader.reset();

  DWORD exit_code = 0;
  if (::GetExitCodeProcess(process_handle.get(), &exit_code) == 0) {
    return unexpected(ErrorCode::Internal,
                      std::format("获取子进程退出码失败 '{}': {}", program_text,
                                  system_message(::GetLastError())));
  }
  result.exit_code = static_cast<int>(exit_code);
  normalize_newlines(result.stdout_text);
  normalize_newlines(result.stderr_text);
  normalize_encoding(result.stdout_text);
  normalize_encoding(result.stderr_text);
  result.elapsed_ms = (time::now_ns() - start_ns) / 1'000'000;
  return result;
#else
  PipeSet out_pipe;
  PipeSet err_pipe;
  if (options.capture_output) {
    if (!make_pipe(out_pipe)) return unexpected(ErrorCode::Io, "创建管道失败");
    if (!options.merge_stderr && !make_pipe(err_pipe)) {
      close_fd(out_pipe.read_fd);
      close_fd(out_pipe.write_fd);
      return unexpected(ErrorCode::Io, "创建管道失败");
    }
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (options.capture_output) {
    posix_spawn_file_actions_adddup2(&actions, out_pipe.write_fd, STDOUT_FILENO);
    if (!options.merge_stderr) {
      posix_spawn_file_actions_adddup2(&actions, err_pipe.write_fd, STDERR_FILENO);
    } else {
      posix_spawn_file_actions_adddup2(&actions, out_pipe.write_fd, STDERR_FILENO);
    }
    posix_spawn_file_actions_addclose(&actions, out_pipe.read_fd);
    posix_spawn_file_actions_addclose(&actions, out_pipe.write_fd);
    if (!options.merge_stderr) {
      posix_spawn_file_actions_addclose(&actions, err_pipe.read_fd);
      posix_spawn_file_actions_addclose(&actions, err_pipe.write_fd);
    }
  }
  if (!options.cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());

  std::vector<std::string> storage;
  storage.reserve(args.size() + 1);
  storage.push_back(program_text);
  for (const auto& argument : args) storage.push_back(argument);
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (auto& item : storage) argv.push_back(item.data());
  argv.push_back(nullptr);

  std::vector<std::string> env_storage;
  for (const auto& [key, value] : options.env) env_storage.push_back(std::format("{}={}", key, value));
  std::vector<char*> envp;
  char** base_env = environ;
  if (!env_storage.empty()) {
    for (char** item = environ; item != nullptr && *item != nullptr; ++item) envp.push_back(*item);
    for (auto& item : env_storage) envp.push_back(item.data());
    envp.push_back(nullptr);
    base_env = envp.data();
  }

  pid_t child = -1;
  const int spawn_result =
      posix_spawnp(&child, program_text.c_str(), &actions, nullptr, argv.data(), base_env);
  posix_spawn_file_actions_destroy(&actions);
  if (options.capture_output) {
    close_fd(out_pipe.write_fd);
    close_fd(err_pipe.write_fd);
  }
  if (spawn_result != 0) {
    close_fd(out_pipe.read_fd);
    close_fd(err_pipe.read_fd);
    return unexpected(ErrorCode::NotFound,
                      std::format("无法启动 '{}': {}", program_text, std::strerror(spawn_result)));
  }

  RunResult result;
  bool reaped = false;
  if (options.capture_output) {
    const bool has_err = !options.merge_stderr;
    if (has_err) {
      std::array<pollfd, 2> entries{};
      entries[0].fd = out_pipe.read_fd;
      entries[0].events = POLLIN;
      entries[1].fd = err_pipe.read_fd;
      entries[1].events = POLLIN;
      bool out_done = false;
      bool err_done = false;
      while (!out_done || !err_done) {
        for (auto& entry : entries) entry.revents = 0;
        const int ready = ::poll(entries.data(), entries.size(), 200);
        if (ready < 0 && errno != EINTR) break;
        if ((entries[0].revents & (POLLIN | POLLHUP)) != 0) {
          (void)drain(out_pipe.read_fd, result.stdout_text, false);
        }
        if ((entries[1].revents & (POLLIN | POLLHUP)) != 0) {
          (void)drain(err_pipe.read_fd, result.stderr_text, false);
        }
        int status = 0;
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
          (void)drain(out_pipe.read_fd, result.stdout_text, false);
          (void)drain(err_pipe.read_fd, result.stderr_text, false);
          out_done = true;
          err_done = true;
          reaped = true;
          result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
      }
    } else {
      (void)drain(out_pipe.read_fd, result.stdout_text, true);
    }
    close_fd(out_pipe.read_fd);
    close_fd(err_pipe.read_fd);
  }

  // 只在尚未回收时 waitpid——重复回收会以 ECHILD 失败并**把 status 留在 0**，
  // 从而把「失败退出码」错当成功（曾直接掩盖编译/链接失败）。
  if (!reaped) {
    int status = 0;
    pid_t waited = -1;
    do {
      waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == child) {
      result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    } else if (waited < 0) {
      return unexpected(ErrorCode::Internal,
                        std::format("等待子进程失败 '{}': {}", program_text, std::strerror(errno)));
    }
  }
  result.elapsed_ms = (time::now_ns() - start_ns) / 1'000'000;
  return result;
#endif
}

auto which(std::string_view program) -> std::optional<std::string> {
  const std::string name(program);
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
    return fs::is_regular_file(name) ? std::optional<std::string>(name) : std::nullopt;
  }
  const auto path = fs::read_env("PATH");
  if (!path) return std::nullopt;
#if defined(_WIN32)
  // Windows 的 PATH 分隔符是 `;`。用 `:` 切会得到“整条 PATH 当作一个目录”，
  // 于是**任何**查找都必然失败（编译器探测因此恒失败）；可执行文件还必须补扩展名
  // （PATH 里不会写 `g++.exe`），否则同样找不到。
  constexpr char kPathSeparator = ';';
  constexpr std::array<std::string_view, 4> kExtensions = {".exe", ".cmd", ".bat", ".com"};
#else
  constexpr char kPathSeparator = ':';
  constexpr std::array<std::string_view, 1> kExtensions = {""};
#endif
  for (const auto part : st::split(*path, kPathSeparator)) {
    if (part.empty()) continue;
    const std::string directory(part);
    for (const auto extension : kExtensions) {
      const std::string candidate = fs::join(directory, std::string(name).append(extension));
      if (fs::is_regular_file(candidate)) return candidate;
    }
  }
  return std::nullopt;
}

auto executable_path() -> Result<std::string> {
#if defined(_WIN32)
  // 必须用宽字符版：`GetModuleFileNameA` 按**本地 ANSI 代码页**输出，中文/日文路径会被改写
  // （安装目录带非 ASCII 时，“重启自身”这类功能会指向一个不存在的路径）。
  std::array<wchar_t, 4096> buffer{};
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0) return unexpected(ErrorCode::Io, "无法获取可执行文件路径");
  return to_utf8(std::wstring_view(buffer.data(), static_cast<std::size_t>(length)));
#else
  std::array<char, 4096> buffer{};
  const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) return unexpected(ErrorCode::Io, "无法获取可执行文件路径");
  return std::string(buffer.data(), static_cast<std::size_t>(length));
#endif
}

auto current_id() -> std::uint64_t {
#if defined(_WIN32)
  // Windows 没有 `getpid()`（POSIX 接口）；对应的是 `GetCurrentProcessId()`。
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace st::process
