#include "st/core/print.hpp"

#include <cstdio>

#include "st/core/console_log.hpp"

namespace st {
namespace {

/// 原样写出到文件描述符；部分写要写完整（管道场景下 write 可能只写一部分）。
void write_all(std::FILE* stream, std::string_view text) noexcept {
  if (text.empty()) return;
  const std::size_t written = std::fwrite(text.data(), 1, text.size(), stream);
  (void)written;  // 输出失败（如管道断裂）不改变程序行为——CLI 不因写交互失败而中断
  std::fflush(stream);
}

}  // namespace

void write_stdout(std::string_view text) noexcept {
  // 纯用户控制台交互：原样写 stdout，不经日志管道（见 print.hpp 的定位说明）。
  // 文件镜像（未开启时是空操作）：终端与日志文件**原样一致**（tee 语义），
  // 不携带扩展信息——镜像只是“用户看到的，事后也能看”。
  write_all(stdout, text);
  console_log::mirror(text);
}

void write_stderr(std::string_view text) noexcept {
  write_all(stderr, text);
  console_log::mirror(text);
}

}  // namespace st
