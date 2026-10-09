#pragma once

/// 子进程执行（构建驱动等）：`run()` 同步执行并捕获输出。
/// 平台实现集中在 `src/core/platform_process.cpp`（POSIX fork/exec；Windows CreateProcess）。

#include <cstdint>
#include <map>
#include <memory>
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

  /// 流式子进程句柄（`st::process::StreamHandle`）——**长命令的实时输出**。
  ///
  /// 为何需要它（2026-10-06，来自 gbcode 终端实战）：`run()` 是**一次性的**——
  /// 它等子进程结束才返回，期间调用方（GUI 主循环）什么都做不了。一条几十秒的
  /// `st test` 会把界面变成一张静止的画，连“中止”按钮都点不到。
  ///
  /// 这个句柄把子进程拆成三件**可分开的事**：
  /// `open()`（起进程，非阻塞）→ `read_line()`（逐行取，**可长时间阻塞在调用者的线程上**）
  /// → `finish()`（回收，返回退出码）。典型用法是在工作线程上循环 `read_line`，
  /// 主线程只负责把攒下的行搬进界面状态。
  ///
  /// `merge_stderr` 默认开：终端要的是**时间顺序**，不是两条独立流。
  /// 行尾统一成 `\n`（Windows 的 `\r\n` 会让终端多出看不见的回车）。
  class StreamHandle {
   public:
    // 构造函数也不能 `= default`：成员 `impl_{}` 的初始化需要一个可用的析构器
    // （异常回滚路径），同样要求 `Impl` 是完整类型。
    StreamHandle();
    ~StreamHandle();
    StreamHandle(const StreamHandle&) = delete;
    auto operator=(const StreamHandle&) -> StreamHandle& = delete;
    StreamHandle(StreamHandle&&) noexcept;
    auto operator=(StreamHandle&&) noexcept -> StreamHandle&;

    /// 启动子进程。失败时**不抛异常**：调完用 `valid()` 判定，`error()` 拿原因。
    void open(std::string_view program, const std::vector<std::string>& args, std::string_view cwd,
              bool merge_stderr = true);

    /// 读一行（不含行尾）。返回 false = 流已结束（EOF/出错）。
    /// **会阻塞**——这是给工作线程用的，不要在 GUI 主线里调。
    [[nodiscard]] auto read_line(std::string& out) -> bool;

    /// 读一块原始字节（返回实读数；0 = EOF）。**会阻塞**（同 `read_line`）。
    ///
    /// 为何 `read_line` 不够：LSP 消息**不是按行的**（JSON 体内可能有换行），
    /// 而且帧头已明确给出字节数——按行读会把一条消息切碎再拼。需要"按已知长度
    /// 精确读取"的协议（LSP、HTTP chunk、任何带长度前缀的帧）都得用这个。
    [[nodiscard]] auto read_some(char* buffer, std::size_t capacity) -> std::size_t;

    /// 向子进程 stdin 写入（**双向通信**：LSP 的语言服务器靠它接收请求）。
    ///
    /// 为何与 `read_line` 分在两个方向：终端场景只用读（输入经 PTY），而 LSP
    /// 是**同一对管道上双向跑**（请求写进去、响应读出来）。写端在 `open` 时建好、
    /// 持有到 `close_write()`（发完消息不要关——关了 server 会以为客户端跑了）。
    /// 返回 false = 写失败（子进程已死/管道断开），调用方应如实上报，不静默。
    ///
    /// 本函数**不附加锁**：调用方自己保证“同一时刻只有一个写者”（LSP 客户端
    /// 用一个发送队列串行化，见 `st/lsp/client`）。
    [[nodiscard]] auto write(std::string_view data) -> bool;

    /// 关闭 stdin（只在“不再发任何消息”时调）。
    void close_write();

    /// stdin 是否可用（未 open / 已关 → false）。
    [[nodiscard]] auto can_write() const noexcept -> bool;

    /// 等待退出并回收资源；返回退出码（异常终止返回 128+signal）。
    [[nodiscard]] auto finish() -> int;

    /// 终止子进程（SIGTERM / TerminateProcess）。“中止”按钮的实现。
    void terminate();

    [[nodiscard]] auto valid() const noexcept -> bool;
    [[nodiscard]] auto error() const -> std::string_view;

   private:
    // `Impl` 的**定义在实现文件里**，但析构/移动必须能看到完整类型。
    // 做法：在头里把 unique_ptr 的非删除析构器声明出来，实现在 `Impl` 之后——
    // 否则 `unique_ptr<Impl>` 的默认删除器在头里内联展开时要求 `sizeof(Impl)`（实测报错）。
    struct Impl;
    std::unique_ptr<Impl> impl_{};
    std::string error_{};
  };

/// 当前进程 ID。
///
/// 为什么要封装：上层（控制通道 `hello` 回包）需要报告 pid，而 `getpid()` 是 POSIX 接口——
/// **Windows 上不存在**（交叉编译实测报 `'::getpid' has not been declared`）。
/// 平台差异收在本层（Windows 走 `GetCurrentProcessId`），上层不得直接调 POSIX。
[[nodiscard]] auto current_id() -> std::uint64_t;

}  // namespace st::process
