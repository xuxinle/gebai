#include "st/core/print.hpp"

#include <cstdio>

namespace st {
namespace {

/// 原样写出到文件描述符；部分写要写完整（管道场景下 write 可能只写一部分）。
void write_all(std::FILE* stream, std::string_view text) noexcept {
  if (text.empty()) return;
  const std::size_t written = std::fwrite(text.data(), 1, text.size(), stream);
  (void)written;  // 输出失败（如管道断裂）不改变程序行为——CLI 不因写日志失败而中断
  std::fflush(stream);
}

}  // namespace

void write_stdout(std::string_view text) noexcept { write_all(stdout, text); }

void write_stderr(std::string_view text) noexcept { write_all(stderr, text); }

}  // namespace st
