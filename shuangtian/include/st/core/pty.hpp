/// 伪终端（PTY）：给子进程一个**真终端**的接口。
///
/// ## 为什么必须有 PTY，管道不行
///
/// 管道（`StreamHandle`）与 PTY 的差别不是"快慢"，而是**子进程看到的自己是什么**：
///
/// | | 管道 | PTY |
/// |---|---|---|
/// | `isatty()` | 假 | **真** |
/// | 行缓冲 | 全缓冲（`\n` 才吐） | **行缓冲按 tty 规则** |
/// | 颜色输出 | 程序自动关闭（如 `ls --color=auto`） | **开着** |
/// | 回显/行编辑 | 无 | **内核行规程做**（`\b` `\t` `Ctrl+U`） |
/// | 全屏程序（`vim`/`htop`） | **直接报错** | **可用** |
/// | 提示符 | 应用自己拼 | **shell 自己发**（PS1 展开、含颜色） |
/// | 窗口尺寸 | 不存在 | **可上报**（TUI 据此排版） |
///
/// 所以"真终端"的**硬前提**是 PTY；拿管道做出来的只能是"输出区 + 输入框"，
/// 那正是要摆脱的形态。
///
/// ## 双向字节流，不是行流
///
/// PTY 的两端都是**原始字节**：输出里混着 ANSI 转义序列（光标定位、颜色、清屏），
/// 输入要**逐键**送达（方向键、`Ctrl+C` 都是字节）。按行读的接口在这里是错的——
/// `read_line` 会把提示符（没有换行）永远卡在缓冲里。
///
/// 因此本接口只认字节：`read()` / `write()`。
///
/// ## 平台
///
/// * Windows：`CreatePseudoConsole`（ConPTY，Win10 1809+）
/// * POSIX：`openpty` + `setsid` + `TIOCSCTTY`
///
/// 平台差异全部封在 `platform_pty.cpp`（`CONVENTIONS.md` §10 第 1 条）。

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace st::process {

/// 终端的窗口尺寸（列 × 行）。TUI 程序据此排版。
struct PtySize {
  int cols{80};
  int rows{24};
};

/// 伪终端会话。
///
/// 生命周期：`open` →（反复 `read`/`write`）→ `close`（或析构）。
/// 读线程模型与 `StreamHandle` 一致：`read` **阻塞**，给工作线程用。
class PtySession {
 public:
  PtySession();
  ~PtySession();
  PtySession(const PtySession&) = delete;
  auto operator=(const PtySession&) -> PtySession& = delete;
  PtySession(PtySession&&) noexcept;
  auto operator=(PtySession&&) noexcept -> PtySession&;

  /// 起一个 shell（或任意程序）并连上伪终端。
  ///
  /// 失败**不抛异常**（与 `StreamHandle` 同一约定）：调完用 `valid()` 判定。
  /// `argv` 为**完整参数表**（`argv[0]` 是程序名——与 POSIX `execvp` 的约定一致）；
  /// 空时只把 `program` 当 `argv[0]` 起交互 shell。
  void open(const std::string& program, const std::vector<std::string>& argv,
            const std::string& cwd, PtySize size);

  /// 读一片原始字节（**阻塞**，给工作线程）。返回读到的字节数；0 = 已结束。
  ///
  /// 不按行、不解析——ANSI 序列与提示符都在里面，交调用方（屏幕模型）处理。
  auto read(char* buffer, std::size_t capacity) -> std::size_t;

  /// 写原始字节到子进程的 stdin（键盘输入、粘贴、控制字符）。
  ///
  /// 返回实际写入的字节数。部分写入是可能的——调用方负责补齐剩余部分。
  auto write(const char* data, std::size_t length) -> std::size_t;

  /// 上报新的窗口尺寸（子进程收到 `SIGWINCH` / `WINDOW_BUFFER_SIZE_EVENT`）。
  void resize(PtySize size);

  /// 等子进程退出并回收；返回退出码（异常终止返回 128+signal）。
  auto wait() -> int;

  /// 终止子进程（“中止”按钮）。
  ///
  /// 契约（与 `Channel::terminate` 一致）：调用后 `read` 要**尽快返回 0**——
  /// 否则中止在界面上是假的（用户点了，线程还堵在读里）。
  void terminate();

  [[nodiscard]] auto valid() const noexcept -> bool;
  [[nodiscard]] auto error() const -> std::string;
  /// 子进程是否已退出（`read` 返回 0 之后可读退出码）。
  [[nodiscard]] auto exited() const noexcept -> bool;
  [[nodiscard]] auto exit_code() const noexcept -> int;

  /// 本机是否支持伪终端（Windows 上需要 Win10 1809+）。
  ///
  /// 为什么要问：老系统上 `CreatePseudoConsole` 不存在，此时**如实降级**
  ///（调用方回落到管道行模式）比"假装有终端但子进程行为不对"好。
  [[nodiscard]] static auto supported() -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_{};
  std::string error_{};
};

}  // namespace st::process
