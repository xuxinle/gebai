// 跑一个 .py 文件，打印 result 或错误。通用调试入口。
//
// ## 为什么需要
//
// `python_embed_probe` 的判据是写死的（验特定能力），而排查具体脚本时
// 需要"给我跑这个文件，告诉我结果或错误"。这个工具就干这件事——
// 它是排查时的 `python` 替代品。
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "st/python/runtime.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法：st_py <脚本.py>\n");
    return 2;
  }
  const auto source = read_file(argv[1]);
  if (source.empty()) {
    std::printf("读不到或空文件：%s\n", argv[1]);
    return 1;
  }
  if (!st::mp::Runtime::available()) {
    std::printf("MicroPython 不可用：%s\n", st::mp::Runtime::unavailable_reason().c_str());
    return 1;
  }
  auto& runtime = st::mp::Runtime::instance();
  const auto result = runtime.run(source, argv[1]);
  if (!result.has_value()) {
    std::printf("失败：%s\n", result.error().message.c_str());
    return 1;
  }
  if (!result->value.empty()) {
    std::printf("result = %s\n", result->value.c_str());
  } else {
    std::printf("（执行成功，无 result）\n");
  }
  return 0;
}
