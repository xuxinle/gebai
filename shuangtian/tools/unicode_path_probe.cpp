// 验证：中文标点目录下的 fopen 是否失败（fs 层窄字符路径的根因）
#include <cstdio>
#include <string>

#include "st/core/fs.hpp"

int main() {
  // 造一个带中文标点的目录
  const std::string base = "D:/code/gebai-pro/users/admin/sessions/b9/88/b98829633cd347ecb3c8e90cacc5a3f7/tmp/中文目录，测试";
  std::printf("目录: %s\n", base.c_str());
  if (auto s = st::fs::create_directories(base); !s) {
    std::printf("  创建目录失败: %s\n", s.error().message.c_str());
    return 1;
  }
  std::printf("  创建目录: ok（create_directories 走 std::filesystem/宽字符 → 成功）\n");

  const std::string file = st::fs::join(base, "payload.bin");
  const std::string content(1024, 'A');

  // ① write_bytes（内部 std::fopen 窄字符）
  auto w = st::fs::write_bytes(file, std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
  std::printf("  write_bytes: %s\n", w ? "ok" : w.error().message.c_str());

  // ② file_size（std::filesystem 宽字符）
  auto sz = st::fs::file_size(file);
  std::printf("  file_size:   %s\n", sz ? std::to_string(*sz).c_str() : sz.error().message.c_str());

  // ③ read_bytes（内部 std::fopen 窄字符）
  auto r = st::fs::read_bytes(file);
  std::printf("  read_bytes:  %s\n", r ? ("ok, " + std::to_string(r->size()) + " 字节").c_str() : r.error().message.c_str());

  // ④ read_span（materialize 用的正是它）
  auto sp = st::fs::read_span(file, 0, 16);
  std::printf("  read_span:   %s\n", sp ? ("ok, " + std::to_string(sp->size()) + " 字节").c_str() : sp.error().message.c_str());

  // 对照：直接用 std::fopen（ASCII 路径）应当成功
  const std::string ascii = "D:/code/gebai-pro/users/admin/sessions/b9/88/b98829633cd347ecb3c8e90cacc5a3f7/tmp/ascii-test.bin";
  std::FILE* f = std::fopen(ascii.c_str(), "wb");
  std::printf("\n对照 std::fopen(ASCII 路径): %s\n", f ? "ok" : "失败");
  if (f) std::fclose(f);

  (void)st::fs::remove_all(base);
  (void)st::fs::remove_file(ascii);
  return 0;
}
