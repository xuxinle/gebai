#include <cstdio>
#include <string_view>

#include "st/test/test.hpp"

/// 测试可执行入口：
///   st_tests [filter]          跑全部（或名称子串匹配的）用例
///   st_tests --list [filter]   只列出用例名
///   ST_JUNIT_XML=<路径>        运行后把逐用例结果写成 JUnit XML（CI 消费）
auto main(int argc, char** argv) -> int {
  std::string_view filter;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--list") {
      list_only = true;
    } else if (filter.empty()) {
      filter = argument;
    }
  }
  if (list_only) return st::test::list_cases(filter);
  const int failed = st::test::run_all(filter);
  if (const char* junit = std::getenv("ST_JUNIT_XML"); junit != nullptr && junit[0] != '\0') {
    const std::size_t written = st::test::write_junit(junit);
    if (written == 0) std::fprintf(stderr, "JUnit 报告写入失败: %s\n", junit);
  }
  return failed;
}
