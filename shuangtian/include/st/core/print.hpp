#pragma once

/// 控制台输出：`std::format` 风格的格式化打印。
///
/// ## 定位（2026-10-28）：**纯用户控制台交互通道**
///
/// `st::print` / `st::eprint` 是**用户实时交互的输出**——交互式提示、用户直读的
/// 原样文本（如调试台的输入回显、向导式问题的题干）。它们：
///
/// - **原样输出**：不做格式化、不携带任何扩展信息（无时间戳/级别/来源）；
/// - **不经日志管道**：不写 tail、不进 sink/listener、不带镜像元数据——
///   用户交互不该被日志系统搅扰（日志是给机器与事后看的，交互是给人当下看的）；
/// - **永不裁剪**：函数（非宏），构建开关影响不到。
///
/// 程序**主输出**（构建进度、测试结果、命令结果、诊断）请用 `st::log::out`/`err`
/// （带 tail/sink/listener/镜像落盘一条管道，统一入口）。
///
/// 为什么不用 `printf`：格式串与实参类型不匹配是 C 风格输出的经典缺陷，而 `std::format_string`
/// 在**编译期**校验每一个占位符与实参类型（CONVENTIONS §8 的 L7 因此能全局禁止 printf）。
/// 为什么不用 `std::cout`：iostream 会引入语言环境与同步开销，且对二进制/二进制安全输出不友好；
/// 这里直接写文件描述符，语义明确（CLI 工具/诊断输出用）。

#include <cstdio>

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace st {

/// 写到标准输出（不经 iostream，保证与子进程管道行为一致）。
void write_stdout(std::string_view text) noexcept;
/// 写到标准错误。
void write_stderr(std::string_view text) noexcept;

/// 格式化打印到标准输出（编译期校验格式串）。
/// 纯用户控制台交互：原样输出，不经日志管道。
template <class... Args>
void print(std::format_string<Args...> format_text, Args&&... args) {
  const std::string text = std::format(format_text, std::forward<Args>(args)...);
  write_stdout(text);
}

/// 格式化打印到标准错误。
/// 纯用户控制台交互：原样输出，不经日志管道。
template <class... Args>
void eprint(std::format_string<Args...> format_text, Args&&... args) {
  const std::string text = std::format(format_text, std::forward<Args>(args)...);
  write_stderr(text);
}

/// 原样打印（不含格式化，等价于 fputs）。
inline void print_raw(std::string_view text) noexcept { write_stdout(text); }

}  // namespace st
