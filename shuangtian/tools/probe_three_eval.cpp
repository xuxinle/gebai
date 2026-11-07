/// @file probe_three_eval.cpp
/// @brief 一次性探针：把一份 three.js 脚本交给 ThreeRuntime::evaluate，
/// 打印它判定的结果与真实错误。用于离线定位「脚本没有产出三维场景」。
///
/// 为何要这个探针：走 GUI 验证一轮要 1~2 分钟（起窗口→切视图→截图），
/// 而这个问题纯属求值语义，与画面无关。离线一次几毫秒。

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "st/raster/three_runtime.hpp"

auto main(int argc, char** argv) -> int {
  if (argc < 2) {
    std::fprintf(stderr, "用法: probe_three_eval <脚本路径>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "读不到 %s\n", argv[1]);
    return 2;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string script = buffer.str();
  std::printf("脚本 %zu 字节\n", script.size());

  const auto result = st::raster::ThreeRuntime::instance().evaluate(script, "<probe>");
  if (!result.has_value()) {
    std::printf("失败：%s\n", result.error().message.c_str());
    return 1;
  }
  std::printf("成功：objects=%zu\n", result->objects.size());
  for (std::size_t i = 0; i < result->objects.size() && i < 5; ++i) {
    std::printf("  [%zu] name=%s\n", i, result->objects[i].name.c_str());
  }
  return 0;
}
