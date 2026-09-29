#pragma once

/// 控制台输出：`std::format` 风格的格式化打印。
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
template <class... Args>
void print(std::format_string<Args...> format_text, Args&&... args) {
  const std::string text = std::format(format_text, std::forward<Args>(args)...);
  write_stdout(text);
}

/// 格式化打印到标准错误。
template <class... Args>
void eprint(std::format_string<Args...> format_text, Args&&... args) {
  const std::string text = std::format(format_text, std::forward<Args>(args)...);
  write_stderr(text);
}

/// 原样打印（不含格式化，等价于 fputs）。
inline void print_raw(std::string_view text) noexcept { write_stdout(text); }

}  // namespace st
