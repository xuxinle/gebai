#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "st/test/test.hpp"

/// 测试可执行入口：
///   st_tests [filter]                 跑全部（或名称子串匹配的）用例
///   st_tests --list [filter]          只列出用例名
///   st_tests --slow                   额外跑“慢/环境敏感”用例（默认**跳过**，见下）
///   st_tests --shard i/n              只跑第 i 片（i 从 1 起；`st test --jobs` 驱动）
///   ST_JUNIT_XML=<路径>               运行后把**本次**跑过的用例写成 JUnit XML（CI 消费）
///
/// **为何默认不跑慢用例**：这类用例量的是**机器性能**（帧耗时相对清屏的比值）
/// 或需要真编译，耗时由环境决定而非被测代码。它们占测试壁钟的近三分之一，
/// 在共享机器上还会被邻居抬高、**偶发红灯**——而它们的偶发失败与代码质量无关。
/// 迭代内循环要的是“代码对不对”的快速信号，所以默认只跑确定性用例；
/// 发版/性能轮显式 `--slow`（`st test` 转发，见 `tests/README`）。
///
/// **分片（`--shard`）**：把用例按注册顺序分给多个进程并行跑。测试套件单进程跑时
/// 只吃一个核，而它是迭代里最大的单项成本（编译只占零头）。分片由
/// `st test --jobs N` 自动驱动，也可以手工拼多进程命令（CI 矩阵）。
auto main(int argc, char** argv) -> int {
  std::string_view filter;
  bool list_only = false;
  bool include_slow = false;
  std::string_view shard_text;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--slow") {
      include_slow = true;
    } else if (argument == "--shard" && index + 1 < argc) {
      shard_text = std::string_view(argv[++index]);
    } else if (argument.starts_with("--shard=")) {
      shard_text = argument.substr(std::string_view("--shard=").size());
    } else if (filter.empty()) {
      filter = argument;
    }
  }
  st::test::set_include_slow(include_slow);
  // 分片参数写错**必须报错退出**：静默当成"不分片"会让每个子进程都跑全部用例
  // （8 路并行跑出 8 倍的总时长、还互相抢 CPU），看起来"并行了"而实际更慢。
  if (!shard_text.empty()) {
    const auto [shard_index, shard_count] = st::test::parse_shard(shard_text);
    if (shard_index == 0) {
      std::fprintf(stderr, "分片参数非法: %s（应为 i/n，i 从 1 起且 i <= n）\n",
                   std::string(shard_text).c_str());
      return 2;
    }
    st::test::set_shard(shard_index, shard_count);
  }
  if (list_only) return st::test::list_cases(filter);
  const int failed = st::test::run_all(filter);
  if (const char* junit = std::getenv("ST_JUNIT_XML"); junit != nullptr && junit[0] != '\0') {
    const std::size_t written = st::test::write_junit(junit);
    if (written == 0) std::fprintf(stderr, "JUnit 报告写入失败: %s\n", junit);
  }
  return failed;
}
