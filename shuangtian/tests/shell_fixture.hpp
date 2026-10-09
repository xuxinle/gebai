#pragma once

/// 跨平台 **shell 脚本夹具**（`tests/` 内部用）。
///
/// ## 为什么需要
///
/// 一批测试用「写个脚本当假 LSP server」的办法验证子进程管线。原先写死了
/// POSIX 形态：`/bin/sh` + `#!/bin/sh` 脚本 + `chmod +x`。这在 Linux 上没问题，
/// 在 Windows 上**必失败**（没有 `/bin/sh`，且 `.sh` 不是可执行扩展名，
/// `exec` 认不出来）。
///
/// 这些用例不是「代码不对」，而是「这台机器上有没有那个 shell」——
/// 正是 `TEST_CASE.slow` 的适用面（见 `include/st/test/test.hpp`：
/// 「耗时或成败取决于环境，而非代码正确性」）。
///
/// ## 口径（与 `core_pty_test.cpp` 同一套）
///
/// POSIX 用 `/bin/sh`（最小、一定存在）；Windows 用 PowerShell
/// （`pwsh` 优先、回落 `powershell`，与 `Terminal` 组件的 `default_shell()`
/// 同口径——测试不该验一个产品不用的 shell）。
///
/// ⚠ **Windows 上调 PowerShell 逐条 `-Command` 启动非常慢**（单次 ~300ms~1s，
/// 受 profile/模块探测影响），一个用例里起三五次就够呛。所以 Windows 上
/// `kSlow{true}`：默认不跑，`--slow` 显式开启。POSIX 上很快，留在默认路径。

#include <cstdlib>
#include <string>
#include <vector>

#include "st/core/process.hpp"

namespace st_test_shell {

/// 起一个「把 stdin 原样回显到 stdout」并退出码 0 的壳：
///   ① 双向管道用例（写→读回显）用它；
///   ② 「立刻退出、退出码 N」用例用 `exit N` 那条。
struct ShellSpec {
  std::string program;
  std::vector<std::string> argv;
};

/// 平台是否有可用的 shell（Windows 上连 PowerShell 都找不到时为 false）。
[[nodiscard]] inline auto available() -> bool {
#ifdef _WIN32
  return st::process::which("pwsh").has_value() || st::process::which("powershell").has_value();
#else
  return st::process::which("sh").has_value();
#endif
}

/// 本平台的 shell 程序名（找不到时给一个兜底名，让用例自行断言 `valid()`）。
[[nodiscard]] inline auto program() -> std::string {
#ifdef _WIN32
  if (auto found = st::process::which("pwsh"); found.has_value()) return *found;
  if (auto found = st::process::which("powershell"); found.has_value()) return *found;
  return "powershell.exe";
#else
  return "/bin/sh";
#endif
}

/// **回显 stdin** 的那条命令（等价于 POSIX 的 `cat`）。
///
/// PowerShell 的等价物是 `$input | ForEach-Object { $_ }`——注意
/// `Get-Content` 会**先读完整流再输出**（对「边写边读」的管道用例是死锁），
/// 而 `$input` 是**逐行流式**，语义才与 `cat` 一致。
[[nodiscard]] inline auto echo_spec() -> ShellSpec {
#ifdef _WIN32
  return ShellSpec{program(), {"-NoProfile", "-NonInteractive", "-Command",
                               "$input | ForEach-Object { $_ }"}};
#else
  return ShellSpec{program(), {"-c", "cat"}};
#endif
}

/// **立刻退出、给定退出码**的命令（等价于 POSIX 的 `exit N`）。
[[nodiscard]] inline auto exit_spec(int code) -> ShellSpec {
#ifdef _WIN32
  return ShellSpec{program(), {"-NoProfile", "-NonInteractive", "-Command",
                               std::string("exit ") + std::to_string(code)}};
#else
  return ShellSpec{program(), {"-c", std::string("exit ") + std::to_string(code)}};
#endif
}

/// **打印一行文本**的命令（等价于 POSIX 的 `echo <text>`）。
[[nodiscard]] inline auto echo_text_spec(const std::string& text) -> ShellSpec {
#ifdef _WIN32
  // PowerShell 里 `"a b"` 会带引号输出；用 `Write-Output` 明确一次。
  return ShellSpec{program(), {"-NoProfile", "-NonInteractive", "-Command",
                               "Write-Output '" + text + "'"}};
#else
  return ShellSpec{program(), {"-c", "echo " + text}};
#endif
}

/// 假 LSP server 的**脚本正文**：把 POSIX 的 shell 片段翻成当前平台的写法。
///
/// 只覆盖测试实际用到的两种正文（读 stdin 回显、打印一行）。用不到全量翻译——
/// 需要复杂脚本时改用 `st::process` 直接起一个受控进程，别在这里堆解释器。
struct ScriptSpec {
  std::string program;
  std::vector<std::string> argv;
  /// 需要在脚本正文里插入的「逐行读 stdin」片段（POSIX：`read line`）。
  std::string body;
};

/// 把一段**面向行的**脚本正文包成当前平台可执行的脚本。
/// POSIX：写入 `.sh` 并 `chmod +x`（调用方负责落盘）。Windows：交给后续调用方
/// 用 `st::process` 起 PowerShell 并 `-Command` 传入——见下面的 `wrap`。
[[nodiscard]] inline auto shebang() -> std::string {
#ifdef _WIN32
  return "";   // Windows 不用 shebang；由调用方以 -Command/-File 驱动
#else
  return "#!/bin/sh\n";
#endif
}

}  // namespace st_test_shell
