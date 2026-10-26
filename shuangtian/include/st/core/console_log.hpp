#pragma once

/// 控制台输出的**文件镜像**（tee）：把 `st::print` / `st::eprint` 写出去的东西
/// 同时抄一份到文件。
///
/// ## 为什么要有它（与 `st::log` 的文件落盘不是一回事）
///
/// `st::log` 落的是**日志事件**（info/warn/error 三级消息）。而构建/测试的输出
/// 绝大部分走的是 `st::print`——编译进度、单元统计、"编译 X 秒 · 链接 Y 秒"、
/// 失败诊断……这些**不是日志级别事件**，`st::log` 那一路看不见它们。
///
/// 实测代价：`st build` 失败后，`build/bin/logs/st.log` 里只有一行"日志落盘：…"，
/// 真正的失败原因（`ld.lld: error: duplicate symbol …`）一行都没有——于是要
/// 重新跑一遍才能看见。构建是秒级到分钟级的操作，"为了看一眼错误而重跑"是
/// 迭代速度的直接损失。
///
/// ## 口径
///
/// - **tee 语义**：终端照旧看得到（人不受影响），文件同时得到**完整**输出。
/// - **顺序敏感**：stdout 与 stderr 汇到**同一个文件**，靠这里的互斥锁串行化，
///   不依赖调用方 `2>&1` 的 shell 拼法（Windows 的 PowerShell 与 cmd 两种写法不同，
///   而 tee 在进程内做，跨平台一致）。
/// - **失败静默降级**：文件打不开就当没开过（不阻断构建），只在 stderr 提示一次。
/// - **不缓冲**：每条写完即 `fflush`——进程被杀（OOM/取消）时已写的内容不会丢，
///   这正是"事后查日志"最需要的那一段。
///
/// ## 与 `st log` 子命令的关系
///
/// 写在这里的是**给机器与人事后一起看**的原始输出；`st log` 负责按路径规则
/// 找最新一份并给出 tail/grep，两者分工不重叠。

#include <cstddef>
#include <string>
#include <string_view>

namespace st::console_log {

/// 打开镜像文件（幂等：重复调用先关旧的）。
///
/// 父目录不存在时自动创建。返回是否真的打开成功——失败只意味着"没有镜像"，
/// 调用方**不该**因此改变行为。
[[nodiscard]] auto open(std::string_view path) -> bool;

/// 关闭镜像（flush 并复位）。
void close();

/// 当前是否在用。
[[nodiscard]] auto active() -> bool;

/// 当前镜像文件路径（未打开时为空）。
[[nodiscard]] auto path() -> std::string;

/// 写一段到镜像（由 `st::print` / `st::eprint` 调用；未打开时是空操作）。
///
/// 不做格式化、不加时间戳：镜像是**原样副本**，用户看到的与文件里的一致。
void mirror(std::string_view text) noexcept;

/// 已写字节数（诊断用：确认"确实写进去了"）。
[[nodiscard]] auto bytes_written() -> std::size_t;

}  // namespace st::console_log
