#pragma once

/// 子进程执行（构建驱动等）：`run()` 同步执行并捕获输出。
/// 平台实现集中在 `src/core/platform_process.cpp`（POSIX fork/exec；Windows CreateProcess）。

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::process {

struct Options {
  std::string cwd{};                            ///< 工作目录（空=继承）
  std::map<std::string, std::string> env{};     ///< 附加环境变量
  bool capture_output{true};                    ///< 捕获 stdout/stderr（false 时直接继承终端）
  bool merge_stderr{false};                     ///< stderr 并入 stdout（顺序敏感日志用）
  int timeout_ms{0};                            ///< 0 = 不限
};

struct RunResult {
  int exit_code{0};
  std::string stdout_text{};
  std::string stderr_text{};
  std::int64_t elapsed_ms{0};
};

/// 执行程序（`program` 可为绝对路径或 PATH 中的名字）。
[[nodiscard]] auto run(std::string_view program, const std::vector<std::string>& args,
                       const Options& options = {}) -> Result<RunResult>;

/// 在 PATH 中查找可执行文件（返回绝对路径）。
[[nodiscard]] auto which(std::string_view program) -> std::optional<std::string>;

/// 当前可执行文件路径（用于自举/重启场景）。
[[nodiscard]] auto executable_path() -> Result<std::string>;

/// 当前进程 ID。
///
/// 为什么要封装：上层（控制通道 `hello` 回包）需要报告 pid，而 `getpid()` 是 POSIX 接口——
/// **Windows 上不存在**（交叉编译实测报 `'::getpid' has not been declared`）。
/// 平台差异收在本层（Windows 走 `GetCurrentProcessId`），上层不得直接调 POSIX。
[[nodiscard]] auto current_id() -> std::uint64_t;

}  // namespace st::process
