// 平台边界：本文件集中封装系统进程 API（CONVENTIONS §2 R6 的受控例外——execvp/char* 边界）。
#include "st/core/process.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <format>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"

#if defined(_WIN32)
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

#if !defined(_WIN32)

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
  // Windows 分支：把输出重定向到临时文件后经 cmd 执行（保持跨平台可用；POSIX 为完整实现）。
  std::string command = std::format("\"{}\"", program_text);
  for (const auto& argument : args) command.append(std::format(" \"{}\"", argument));
  const std::string out_path = fs::join(fs::temp_dir(), "st-proc-out.txt");
  const std::string err_path = fs::join(fs::temp_dir(), "st-proc-err.txt");
  command.append(std::format(" > \"{}\" 2> \"{}\"", out_path, err_path));
  const int code = std::system(command.c_str());
  RunResult result;
  result.exit_code = code;
  if (auto text = fs::read_text(out_path); text) result.stdout_text = *text;
  if (auto text = fs::read_text(err_path); text) result.stderr_text = *text;
  // 清理临时文件：失败不影响结果（返回状态显式丢弃，规避 nodiscard 告警）
  (void)fs::remove_file(out_path);
  (void)fs::remove_file(err_path);
  result.elapsed_ms = (time::now_ns() - start_ns) / 1'000'000;
  (void)options;
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
  if (name.find('/') != std::string::npos) {
    return fs::is_regular_file(name) ? std::optional<std::string>(name) : std::nullopt;
  }
  const auto path = fs::read_env("PATH");
  if (!path) return std::nullopt;
  for (const auto part : st::split(*path, ':')) {
    if (part.empty()) continue;
    const std::string candidate = fs::join(part, name);
    if (fs::is_regular_file(candidate)) return candidate;
  }
  return std::nullopt;
}

auto executable_path() -> Result<std::string> {
#if defined(_WIN32)
  std::array<char, 4096> buffer{};
  const DWORD length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0) return unexpected(ErrorCode::Io, "无法获取可执行文件路径");
  return std::string(buffer.data(), length);
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
