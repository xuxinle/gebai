#include <cstdio>
#include <string_view>

#include "st/test/test.hpp"

/// 测试可执行入口：
///   st_tests [filter]                 跑全部（或名称子串匹配的）用例
///   st_tests --list [filter]          只列出用例名
///   st_tests --slow                   额外跑“慢/环境敏感”用例（默认**跳过**，见下）
///   ST_JUNIT_XML=<路径>               运行后把逐用例结果写成 JUnit XML（CI 消费）
///
/// **为何默认不跑慢用例**：这类用例量的是**机器性能**（帧耗时相对清屏的比值）
/// 或需要真编译，耗时由环境决定而非被测代码。它们占测试壁钟的近三分之一，
/// 在共享机器上还会被邻居抬高、**偶发红灯**——而它们的偶发失败与代码质量无关。
/// 迭代内循环要的是“代码对不对”的快速信号，所以默认只跑确定性用例；
/// 发版/性能轮显式 `--slow`（`st test` 转发，见 `tests/README`）。
auto main(int argc, char** argv) -> int {
  std::string_view filter;
  bool list_only = false;
  bool include_slow = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--slow") {
      include_slow = true;
    } else if (filter.empty()) {
      filter = argument;
    }
  }
  st::test::set_include_slow(include_slow);
  if (list_only) return st::test::list_cases(filter);
  const int failed = st::test::run_all(filter);
  if (const char* junit = std::getenv("ST_JUNIT_XML"); junit != nullptr && junit[0] != '\0') {
    const std::size_t written = st::test::write_junit(junit);
    if (written == 0) std::fprintf(stderr, "JUnit 报告写入失败: %s\n", junit);
  }
  return failed;
}
