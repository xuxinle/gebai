#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "st/core/console_log.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

/// 测试可执行入口：
///   st_tests [filter]                 跑全部（或名称子串匹配的）用例
///   st_tests --list [filter]          只列出用例名
///   st_tests --slow                   额外跑“慢/环境敏感”用例（默认**跳过**，见下）
///   st_tests --shard i/n              只跑第 i 片（i 从 1 起；`st test --jobs` 驱动）
///   ST_JUNIT_XML=<路径>               运行后把**本次**跑过的用例写成 JUnit XML（CI 消费）
///   ST_TEST_SHARD_LOG=<路径>          把本次输出 tee 到该文件（`st test` 分片时下发）
///
/// **日志两个出口**：`ST_TEST_SHARD_LOG` 设了就调 `console_log::open`，
/// 于是本进程的 `st::print` 输出同时到**终端与文件**（与 `st` 自己同一套 tee 实现）。
/// 为何要有：分片并行时十几片输出交错在终端里，而调用方（智能体工具/CI/回滚缓冲）
/// 往往只看得到尾部——逐用例结果与断言消息恰好落在看到的那段之外。实测踩过：
/// 失败详情只在终端、`.st/logs` 只剩一行“── 失败 ──”，定位只能靠重跑。
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
  // 日志第二个出口：由 `st test` 分片时下发，子进程自己在启动时打开镜像。
  // 放在 `run_all` **之前**：用例的第一行输出也必须进文件。
  if (const char* shard_log = std::getenv("ST_TEST_SHARD_LOG");
      shard_log != nullptr && shard_log[0] != '\0') {
    (void)st::console_log::open(shard_log);
  }
  // 分片参数写错**必须报错退出**：静默当成"不分片"会让每个子进程都跑全部用例
  // （8 路并行跑出 8 倍的总时长、还互相抢 CPU），看起来"并行了"而实际更慢。
  if (!shard_text.empty()) {
    const auto [shard_index, shard_count] = st::test::parse_shard(shard_text);
    if (shard_index == 0) {
      st::log::err("分片参数非法: {}（应为 i/n，i 从 1 起且 i <= n）\n", shard_text);
      return 2;
    }
    st::test::set_shard(shard_index, shard_count);
  }
  if (list_only) return st::test::list_cases(filter);
  const int failed = st::test::run_all(filter);
  if (const char* junit = std::getenv("ST_JUNIT_XML"); junit != nullptr && junit[0] != '\0') {
    // `write_junit` 返回 0 有两种含义：**0 用例**（空分片/全被 filter 掉，合法形态
    // ——`merge_junit_files` 对 0 用例片同语义：「没有结果不如不产出」）与**写失败**。
    // 修复前两者不可区分，空分片会打出误导性的「JUnit 报告写入失败」（实测：
    // `--test-jobs 4` + filter 只命中一个用例时，3 个空片各报一条 stderr 噪声）。
    // 只报后者；0 用例是完全正常的状态，静默跳过。
    if (!st::test::last_results().empty() && st::test::write_junit(junit) == 0) {
      st::log::err("JUnit 报告写入失败: {}\n", junit);
    }
  }
  return failed;
}
