// 直接跑脚本，读出 `result`——它就是最后一个成功执行的 MARK。
//
// ## 为什么这样做最省事
//
// 前一个尝试（bisect 截断）一直卡在"切点落在 docstring 里"的问题上，
// 修了三轮还是不准。而脚本里**自己插 `result = 'MARK X'`** 更直接：
// 执行到哪崩溃，`result` 就停在前一个 MARK——零歧义，也不依赖任何
// 源码分析。
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
  const std::string path = argc > 1 ? argv[1] : "apps/jianmu/scenes/bpy_watch.py";
  const auto source = read_file(path);
  if (source.empty()) {
    std::printf("读不到：%s\n", path.c_str());
    return 1;
  }
  if (!st::mp::Runtime::available()) {
    std::printf("MicroPython 不可用\n");
    return 1;
  }
  auto& runtime = st::mp::Runtime::instance();

  // ① 完整跑
  const auto full = runtime.run(source, "<script>");
  if (full.has_value()) {
    std::printf("脚本执行成功，result = %s\n", full->value.c_str());
    return 0;
  }
  std::printf("脚本失败：%s\n\n", full.error().message.c_str());

  // ② 逐段跑：从"跑完第 k 个 MARK"推最后一次成功的位置
  //
  // 做法与上面 §注释一致：把源码截到每个 `result = 'MARK` 行**之后**，
  // 那些截断点在语法上一定是完整的（那句是顶层语句）。
  std::istringstream stream(source);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(stream, line)) lines.push_back(line);

  std::vector<std::size_t> marks;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].find("result = 'MARK") != std::string::npos) marks.push_back(i);
  }
  std::printf("找到 %zu 个 MARK 行\n", marks.size());

  std::string last_ok;
  for (const auto cut : marks) {
    std::string slice;
    for (std::size_t i = 0; i <= cut; ++i) {
      slice += lines[i];
      slice += '\n';
    }
    const auto result = runtime.run(slice, "<slice>");
    if (!result.has_value()) {
      std::printf("第一个失败段：MARK 行 %zu\n", cut + 1);
      std::printf("  错误：%s\n", result.error().message.c_str());
      std::printf("  上一个成功：%s\n", last_ok.c_str());
      // 打印该段内容（从上个 MARK 到这里）
      const std::size_t from = (marks.size() > 1 && cut > marks[0]) ? 0 : 0;
      (void)from;
      std::printf("\n--- 该段源码（行 %zu..%zu）---\n", cut > 12 ? cut - 12 : 1, cut + 1);
      for (std::size_t i = (cut > 12 ? cut - 12 : 0); i <= cut; ++i) {
        std::printf("%4zu %s\n", i + 1, lines[i].c_str());
      }
      return 0;
    }
    last_ok = result->value;
  }
  std::printf("所有 MARK 段都通过，但完整跑失败 —— 问题在标记之后的部分\n");
  return 0;
}
