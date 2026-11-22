/// 运行中跳过（`ST_SKIP`）的**可见性**回归（BACKLOG 3903-3905 原条目）。
///
/// ## 被钉的契约
///
/// `independent_project_builds_links_and_runs` 在拿不到 `ST_TEST_FRAMEWORK_ROOT` 时
/// 跳过（猜仓库布局比跳过更糟：会构造假失败）。跳过本身是对的，但**旧行为把它计成
/// PASS、汇总行不体现**——CI 门禁看到全绿，实际上一条集成用例根本没验（实测：
/// st-tests-slow.log 里打出「→ 跳过」后紧跟 `PASS independent_project... 500.30us`）。
///
/// 修复后的三份证据都必须成立：
/// 1. **结果行**：用例名所在行报 `SKIP`（黄色）而不是 `PASS`；
/// 2. **汇总行**：`..., N skipped（环境前置不满足，ST_SKIP）`——门禁扫一行就能发现
///    「这条没验」，不用翻全量输出；
/// 3. **退出码**：跳过**不是失败**（环境没配好 ≠ 被测对象坏了；报错退出码会把
///    所有裸跑 st_tests 的环境全变红）。
///
/// ## 怎么钉（与 `shard_parity_test.cpp` 同构的自启动范式）
///
/// 哨兵用例平时秒过，只在 `ST_SKIP_PROBE=1` 时执行 `ST_SKIP`——同一份二进制既是
/// 「被跑的套件」又是「取证的一方」。主用例自启动本二进制、filter 只命中哨兵、
/// **不注入 `ST_TEST_FRAMEWORK_ROOT`**，从 stdout 找三份证据。这比「手工假设框架
/// 根缺失」可靠：探测逻辑就在被测路径上，环境构造只差一个开关。
///
/// ## 为什么不直接跑 `independent_project_builds_links_and_runs`
///
/// 它是 SLOW 用例（默认不跑）；显式指名会跑，但那要真建真编真跑一个工程（~10s 且
/// 依赖本机工具链）——环境跳过分支只需要「探测失败」，哨兵把该分支的**机制**钉住，
/// 不付真构建的成本。pkg_integration 侧的接线（ST_SKIP 替代 print+return）由
/// 代码评审与缺失环境实跑覆盖（见文件头验证清单）。

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/process.hpp"
#include "st/test/test.hpp"

namespace {

/// 哨兵身份开关：只有自启动的子进程才带它（空值条目不当「已设置」）。
[[nodiscard]] bool skip_probe_enabled() {
  const char* value = std::getenv("ST_SKIP_PROBE");
  return value != nullptr && *value != '\0';
}

/// 自启动本二进制跑哨兵（**不注入任何框架根**——探测路径按子进程继承的环境走）。
[[nodiscard]] auto run_self(const std::vector<std::string>& arguments)
    -> st::Result<st::process::RunResult> {
  auto exe = st::process::executable_path();
  if (!exe) return st::unexpected(exe.error());
  st::process::Options options;
  options.env["ST_SKIP_PROBE"] = "1";
  return st::process::run(*exe, arguments, options);
}

}  // namespace

/// 哨兵：子进程里主动跳过（模拟「环境前置不满足」的探测分支）。
ST_TEST(skipprobe_skips_when_probe_armed) {
  if (!skip_probe_enabled()) return;
  ST_SKIP("哨兵跳过：环境前置不满足（ST_SKIP_PROBE）");
}

/// 主用例：自启动取证，钉「跳过必须在结果行、汇总行、退出码三处如实体现」。
ST_TEST(skip_is_visible_in_report_line_summary_and_exit_code) {
  if (skip_probe_enabled()) return;  // 哨兵运行里空转（防重入）

  auto ran = run_self({"skipprobe"});
  ST_REQUIRE(ran.has_value());

  // ① 结果行：哨兵名所在行是 SKIP 而不是 PASS（旧缺陷正是「跳过被计成 PASS」）。
  //    逐行找名字行再断言前缀——不能整段 find("SKIP")：别处的提示行（如慢用例
  //    跳过说明）也可能含该词，行级核对才钉得住「这一条没被计成通过」。
  bool saw_skip_line = false;
  bool saw_pass_line = false;
  std::size_t cursor = 0;
  const std::string& text = ran->stdout_text;
  while (cursor < text.size()) {
    const std::size_t end = text.find('\n', cursor);
    const std::size_t line_end = end == std::string::npos ? text.size() : end;
    const std::string_view line(text.data() + cursor, line_end - cursor);
    if (line.find("skipprobe_skips_when_probe_armed") != std::string_view::npos) {
      // ANSI 色码在 "SKIP"/"PASS" 与名字之间，判据用「行内有标记词」而非列位置。
      saw_skip_line = saw_skip_line || line.find("SKIP") != std::string_view::npos;
      saw_pass_line = saw_pass_line || line.find("PASS") != std::string_view::npos;
    }
    cursor = end == std::string::npos ? text.size() : end + 1;
  }
  ST_CHECK(saw_skip_line);
  ST_CHECK(!saw_pass_line);

  // ② 汇总行：`1 skipped` 必须出现（这正是 BACKLOG 3903 要的「跳过在汇总里可见」）。
  ST_CHECK(text.find("1 skipped") != std::string::npos);

  // ③ 退出码：跳过不是失败（环境前置缺失 ≠ 被测对象缺陷）。
  ST_CHECK_EQ(ran->exit_code, 0);
}

/// 集成用例必须**真地**在 SLOW 集里（不是只在注释里说它是 SLOW）。
///
/// ## 为何需要这条回归
///
/// `independent_project_builds_links_and_runs` 一直被当作“慢用例”讨论：
/// `src/test/test_main.cpp` 的注释说「为何默认不跑慢用例」并点名那类用例包括
/// 「需要真编译的集成用例」，本文件头也说「它是 SLOW 用例（默认不跑）」——
/// 但它实际用的是 `ST_TEST_WITH_TIMEOUT`（**普通**用例），于是它每轮全量都跑。
/// 实测代价（2026-11-22）：全量 16 片时它独占一片跑 18s，其余 15 片 6s 就完了，
/// 测试程序壁钟 32.3s 里它占 60%。注释与代码相悖时，**代码才是运行时真相**。
///
/// ## 判据为何是「直接查注册表」而不是跑一遍
///
/// `Case::slow` 正是 `run_all` 用来决定“默认跑不跑”的那个字段（`item.slow &&
/// !include_slow()`），所以查它就是查运行时真相，且零成本。
/// 曾经想用 `--list` 判定，但那不行：`list_cases` **不过滤慢用例**（它列的是
/// “注册了哪些”，不是“默认会跑哪些”）——拿它做判据会得出相反结论。
///
/// 同时钉 `timeout_ms == 60000`：只把 slow 标上、把超时降回默认 10s 的话，
/// `--slow` 轮会把它误报成「超过软超时上限」（而那是真正想避免的那类假红）。
ST_TEST(slow_set_contains_integration_case) {
  constexpr std::string_view kCase = "independent_project_builds_links_and_runs";
  const auto& cases = ::st::test::Registry::instance().cases();
  const auto found = std::find_if(cases.begin(), cases.end(),
                                  [&](const ::st::test::Case& item) { return item.name == kCase; });
  ST_REQUIRE(found != cases.end());       // 用例被删了也要报出来（否则本回归静默失效）
  ST_CHECK(found->slow);                  // ← 被钉的那个字段（默认不跑）
  ST_CHECK_EQ(found->timeout_ms, std::int64_t{60'000});   // 慢≠可以短超时
  ST_CHECK(!found->file.empty());         // 分片按 TU 装箱靠它（为空会被当成独立组）
}
