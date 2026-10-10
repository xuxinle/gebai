#pragma once

/// 日志的**文件落盘**与**崩溃时取尾部**。
///
/// ## 为什么需要它
///
/// 默认 sink 只写 stderr。而无头应用（智能体驱动的场景）根本没有控制台可看——
/// 崩溃时那几行 stderr 随进程一起消失，事后只剩"程序退出码非 0"这一条信息。
/// 有了文件落盘，一条命令就能把「日志文件 + 崩溃报告」交给 AI：
/// **不必复述问题、不必复现**。
///
/// ## 与 `st::log` 的关系
///
/// 不是替代 `set_sink`，而是在 sink 之外**再挂一路**：stderr 照常（开发时看得见），
/// 文件同时写（事后查得到）。因此它挂在 `add_listener` 上——那条路是"旁路镜像"，
/// 不影响任何现有 sink 的语义。
///
/// ## 两个设计要点
///
/// 1. **写盘失败不能让程序出问题**：日志系统自身成为故障源是最坏的设计。
///    文件打不开就退化成"只有 stderr"，并 warn 一条（且**只 warn 一次**，
///    否则磁盘满的场景会被日志刷爆）。
/// 2. **尾部必须在内存里留一份**：崩溃发生在任意时刻，那时再去读文件不可靠
///    （缓冲未 flush、或被截断）。环形缓冲常驻内存，崩溃处理器直接从内存取。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace st::log {

/// 文件落盘选项。
struct FileOptions {
  /// 日志文件路径（UTF-8）。空 = 不落盘。
  std::string path{};
  /// 单个文件大小上限（字节）。超过后滚动：`app.log` → `app.1.log`，最老的一档丢弃。
  /// 0 = 不滚动。
  std::size_t max_bytes{8U * 1024U * 1024U};
  /// 保留的历史档数（不含当前文件）。0 = 不保留历史。
  std::size_t keep_files{2};
  /// 写上**毫秒级**时间戳与前缀（与 stderr 那路同一形态）。
  bool timestamps{true};
};

/// 启动文件落盘（幂等：重复调用会先关闭旧文件）。失败时返回 false 并保持"只 stderr"。
[[nodiscard]] auto open_file(const FileOptions& options) -> bool;

/// 关闭文件落盘（flush 并复位）。进程退出前自动做一次。
void close_file();

/// 当前是否在落盘。
[[nodiscard]] auto file_active() -> bool;

/// 当前日志文件路径（未落盘时为空）。
[[nodiscard]] auto file_path() -> std::string;

/// 崩溃时用的**日志尾部**：取最近若干行（从常驻环形缓冲读，不碰文件）。
///
/// 为何带 `level` 过滤：崩溃现场最需要的是 warn/error，而 info 级的高频行
/// （如每帧日志）会把有用的那几行挤出窗口。默认取全部级别，调用方可调。
[[nodiscard]] auto recent_tail(std::size_t max_lines) -> std::string;

/// 环形缓冲容量（行）。崩溃时最多能回溯这么多行。
void set_tail_capacity(std::size_t lines);

namespace detail {

/// **信号安全**的尾部采样：`try_lock`，拿不到锁立即返回空串（不阻塞）。
///
/// 为何单独开一个：`recent_tail` 用普通锁，在信号处理器里调用它可能**死锁**——
/// 若崩溃恰好发生在另一线程持有该锁、或就发生在本函数内，等锁就等于永远拿不到。
/// 崩溃处理器必须能“放弃并继续”，而不是陪着一个死锁一起消失。
///
/// 放在 `detail` 而不是直接当公开 API：正常代码没有理由用“可能返回空”的版本。
[[nodiscard]] auto try_recent_tail(std::size_t max_lines) -> std::string;

}  // namespace detail

}  // namespace st::log
