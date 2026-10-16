/// 分片与全跑的**口径统一**回归（BACKLOG P2「分片与全跑的口径统一」）。
///
/// ## 为什么必须有这一条
///
/// `st test --test-jobs N` 跑分片、不带参数跑全量，历史上**不等价**（分片碰巧避开了
/// 某些跨用例状态污染、而全跑整进程崩溃——见 BACKLOG C 轮说明与 PITFALLS「分片绿
/// 不等于测试全绿」）。异常边界修掉后根因已消，但「两条路径给出同一结论」这个性质
/// **没有回归钉住**：将来再出现「只有一条路径崩」的缺陷，仍会先被当成环境抖动。
///
/// ## 怎么钉（不猜、可复算）
///
/// 本文件的哨兵用例 + 主用例配合，构成一次**可复算的自检**：
///
/// 1. 本文件注册 12 个哨兵（`shardprobe_case_00..11`，下文哨兵段）。它们平时**秒过**
///    （不做任何事），只在环境变量 `ST_PARITY_PROBE=1` 时才激活身份——这样同一份
///    二进制既是「被分片的套件」又是「执行核对的一方」，不需要第二个测试产物。
/// 2. 主用例（`shard_parity_selection_and_results_match_unsharded`）**自启动本二进制**：
///    - `--list` 三类：不分片、`--shard i/4`（i=1..4），核对**选择口径**——
///      各片名单并集为全集且互不重叠，且**同 TU 用例不跨片**（TU 装箱调度：
///      同翻译单元用例集中到单片同族串行，git_service 根修；`run_all` 与
///      `case_names`/`list_cases` 走同一个 `in_shard`，这是「`st test --jobs N`
///      预先算片」依赖的不变量）；
///    - 过滤执行运行：`--shard i/4 shardprobe`（带 filter）跑一遍并核对**执行口径**——
///      各片执行到的哨兵名单 == 该片 `--list` 的哨兵名单（执行即通过，退出码 0），
///      全跑 `shardprobe` 的哨兵名单 == 四片并集。过滤运行是关键：整套件跑一遍太贵且
///      其余用例的成败与分片无关；filter 命中哨兵（名字唯一前缀），两条路径的候选集
///      算法完全一致（`st test --jobs N foo` 走的就是这条）。
/// 3. **失败口径**也在钉的范围内：`ST_PARITY_PROBE=1 ST_PARITY_FAIL_07=1` 时哨兵 07
///    故意 FAIL。此时「7 号哨兵落进哪一片」必须与选择口径一致——该片执行退出码为 1、
///    其余片为 0，四片退出码之和 == 全跑退出码 == 1。这保证「失败也不会被分片吞掉/放大」。
///
/// ## 防重入与环境坑（都有前科，见各处行内注释）
///
/// - 主用例只在 `ST_PARITY_PROBE` **未设**时执行（哨兵运行里它自动空转）；
/// - 子进程**不设 `ST_JUNIT_XML`**，也不试图清空它：分片驱动下本进程带着它，子进程会
///   继承并在退出时把父分片的报告写一遍。这是**安全的**：子进程的写入发生在
///   `run_all` 执行期间，而父分片自己的 `write_junit` 在进程退出前才写——最后一次写
///   按序胜出，合并时拿到的是父分片的完整报告（分片驱动下实测 exit 0）。
///   （历史注：POSIX 侧 env 曾是追加式、同名键压不住，“清空隔离”在当时不可靠，
///   2026-10-15 已改覆盖语义——BACKLOG 315；如今显式置空可行，但“继承写入被父进程
///   按序覆盖”的现有论证仍成立，不改。）
/// - 所有证据都取自 stdout 与退出码，**不写中间文件**（分片运行时多片并行写同一路径
///   会互踩——`shard_suffix()` 存在的原因；这里干脆不用文件）。

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/test/test.hpp"

namespace {

/// 哨兵身份开关：只有自启动的子进程才带它。
/// 取值后判空串：某些环境注入器会写入空值条目，空值不当「已设置」。
[[nodiscard]] bool parity_probe_enabled() {
  const char* value = std::getenv("ST_PARITY_PROBE");
  return value != nullptr && *value != '\0';
}

/// 哨兵失败开关是否打开（只作用于 07 号哨兵）。
[[nodiscard]] bool parity_fail_armed() {
  const char* value = std::getenv("ST_PARITY_FAIL_07");
  return value != nullptr && *value != '\0';
}

/// 从一段输出（`--list` 的裸名行，或 PASS/FAIL 报告行）里提取哨兵用例名。
///
/// 两类行的形状不同：`--list` 每行只有名字；报告行是 `PASS <名字（右补空格至 44）> 耗时`。
/// 共同点是名字里含 `shardprobe` 且不含空白——按「从匹配处截到下一个空白」提取，
/// 两种形态都得到裸名。（`process::run` 已把子进程输出归一化成 LF + UTF-8，可放心按行扫。）
[[nodiscard]] auto extract_probe_names(const std::string& text) -> std::vector<std::string> {
  std::vector<std::string> names;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const std::size_t end = text.find('\n', cursor);
    const std::size_t line_end = end == std::string::npos ? text.size() : end;
    const std::string_view line(text.data() + cursor, line_end - cursor);
    const std::size_t hit = line.find("shardprobe");
    if (hit != std::string::npos) {
      // 名字后截到首个空白：`--list` 行是裸名（截到行尾），报告行是
      // `PASS <右补至 44 的名字> 耗时`——两种形态都得到裸名。
      const std::size_t stop = line.find_first_of(" \t", hit);
      names.emplace_back(line.substr(hit, stop == std::string_view::npos
                                             ? std::string_view::npos
                                             : stop - hit));
    }
    cursor = end == std::string::npos ? text.size() : end + 1;
  }
  return names;
}

/// 逐元素核对两组名单（顺序敏感）：`ST_CHECK_EQ` 不能直接比 vector（不可格式化），
/// 拆开比每一对名字，错在哪一位一目了然。
void check_names_equal(const std::vector<std::string>& actual, const std::vector<std::string>& expected) {
  ST_CHECK_EQ(actual.size(), expected.size());
  const std::size_t common = actual.size() < expected.size() ? actual.size() : expected.size();
  for (std::size_t index = 0; index < common; ++index) {
    ST_CHECK_EQ(actual[index], expected[index]);
  }
}

/// 自启动本二进制。`fail_armed` 时额外带 `ST_PARITY_FAIL_07=1`（哨兵 07 故意 FAIL）。
/// **不设 `ST_JUNIT_XML`**（见文件头）：即便父进程带着它，也绝不能让它漏进子进程语义。
[[nodiscard]] auto run_self(const std::vector<std::string>& arguments, bool fail_armed)
    -> st::Result<st::process::RunResult> {
  auto exe = st::process::executable_path();
  if (!exe) return st::unexpected(exe.error());
  st::process::Options options;
  options.env["ST_PARITY_PROBE"] = "1";
  if (fail_armed) options.env["ST_PARITY_FAIL_07"] = "1";
  return st::process::run(*exe, arguments, options);
}

}  // namespace

// —— 哨兵段：12 个占位用例（名字即契约，两位数下标让序号分布对 4 片是 3/3/3/3、
// 对 3 片是 4/4/4——都是能看出错位的非平凡分布）——
//
// 宏只拼名字、**不在表达式里使用下标值**：`08`/`09` 作为 token 出现在表达式里是
// 非法八进制字面量（`-Werror` 直接打断编译）。07 号要失败注入，单独手写。
// lint-allow: L3 生成哨兵注册的 X 宏——注册器本身也是函数式宏（CONVENTIONS §3.6 的既有例外面）
#define ST_SHARDPROBE_CASE(index)                                    \
  ST_TEST(shardprobe_case_##index) {                                 \
    if (!parity_probe_enabled()) return;                             \
  }
ST_SHARDPROBE_CASE(00)
ST_SHARDPROBE_CASE(01)
ST_SHARDPROBE_CASE(02)
ST_SHARDPROBE_CASE(03)
ST_SHARDPROBE_CASE(04)
ST_SHARDPROBE_CASE(05)
ST_SHARDPROBE_CASE(06)
#undef ST_SHARDPROBE_CASE

/// 07 号哨兵：`ST_PARITY_FAIL_07=1` 时故意失败（其余时刻与别的哨兵一样秒过）。
/// 保持在 06 与 08 之间（注册序连续）：TU 装箱下哨兵同 TU 必落同片；它的归属
/// 片由主用例从逐片名单观察（不再从序号取模推导——旧注释的「7%4=3 → 第 4 片」
/// 已随取模调度退役）。
ST_TEST(shardprobe_case_07) {
  if (!parity_probe_enabled()) return;
  if (parity_fail_armed()) {
    ST_FAIL("哨兵故意失败：核对分片不会吞掉/放大失败");
  }
}

// lint-allow: L3 同上：08-11 段的哨兵生成宏
#define ST_SHARDPROBE_CASE(index)                                    \
  ST_TEST(shardprobe_case_##index) {                                 \
    if (!parity_probe_enabled()) return;                             \
  }
ST_SHARDPROBE_CASE(08)
ST_SHARDPROBE_CASE(09)
ST_SHARDPROBE_CASE(10)
ST_SHARDPROBE_CASE(11)
#undef ST_SHARDPROBE_CASE

/// 见文件头。核对选择/执行/失败三个口径下「分片并集 == 全跑」。
/// 软超时放宽到 60s：常态 ~5s（14 次子进程启动），共享机器负载下会膨胀，
/// 默认 10s 会把「机器慢」误报成被测对象问题（软超时语义见 test.hpp）。
ST_TEST_WITH_TIMEOUT(shard_parity_selection_and_results_match_unsharded, 60'000) {
  if (parity_probe_enabled()) return;  // 哨兵运行里空转（防重入：子进程会再跑全套件）

  // —— 第一步：选择口径（`--list`）——
  // 不分片的名单是基准；`--shard i/4` 的名单必须并集为全集且互不重叠
  //（TU 装箱调度：同 TU 不跨片，见下方不变量断言）。
  auto all_run = run_self({"--list", "shardprobe"}, false);
  ST_REQUIRE(all_run.has_value());
  const std::vector<std::string> all_names = extract_probe_names(all_run->stdout_text);
  ST_REQUIRE(all_names.size() == 12);  // 哨兵没被注册全（宏未展开/名字变更）时后续断言全是噪声，必须前置拦住

  // 哨兵的落片必须**观察**得到而不能推导：TU 装箱的落片由贪心装箱的桶负载
  // 决定，不透明但确定。下方逐片 `--list` 后从「哪片含哨兵」观察，供第三步
  // 失败口径使用。
  constexpr std::size_t kShards = 4;

  std::vector<std::vector<std::string>> shard_names(kShards);
  for (std::size_t index = 0; index < kShards; ++index) {
    const std::vector<std::string> arguments = {
        "--list", "shardprobe", "--shard", std::format("{}/{}", index + 1, kShards)};
    auto listing = run_self(arguments, false);
    ST_REQUIRE(listing.has_value());
    shard_names[index] = extract_probe_names(listing->stdout_text);
  }

  // 并集 = 全集、互不重叠（不重不漏）：TU 装箱与旧取模调度共同必须成立的不变量，
  // 与具体分配算法无关。
  {
    std::vector<std::string> union_names;
    for (const auto& names : shard_names) {
      union_names.insert(union_names.end(), names.begin(), names.end());
    }
    std::vector<std::string> union_sorted = union_names;
    std::vector<std::string> all_sorted = all_names;
    std::sort(union_sorted.begin(), union_sorted.end());
    std::sort(all_sorted.begin(), all_sorted.end());
    check_names_equal(union_sorted, all_sorted);
  }
  // **TU 装箱不变量**：哨兵 12 条同 TU（本文件），必须全部落在同一片——
  // 除这一片外任何片都不得含哨兵（同 TU 跨片 = 装箱被破坏，本单根修目标）。
  {
    std::size_t shards_with_sentinels = 0;
    for (const auto& names : shard_names) {
      if (!names.empty()) ++shards_with_sentinels;
    }
    ST_CHECK_EQ(shards_with_sentinels, std::size_t{1});
  }
  // 失败注入片 = 07 的实际归属片（观察得到：唯一含哨兵的那片），不再从序号推导。
  std::size_t failing_shard = kShards;   // 哨兵所在片（0 基）；哨兵必在 → 会被赋值
  for (std::size_t index = 0; index < kShards; ++index) {
    if (!shard_names[index].empty()) failing_shard = index;
  }
  ST_REQUIRE(failing_shard < kShards);

  // —— 第二步：执行口径（过滤运行，全部通过）——
  // 各片执行到的哨兵名单 == 该片 --list 名单；各片退出码 0，之和 == 全跑退出码 0。
  std::vector<std::string> union_names;
  for (std::size_t index = 0; index < kShards; ++index) {
    const std::vector<std::string> arguments = {
        "--shard", std::format("{}/{}", index + 1, kShards), "shardprobe"};
    auto ran = run_self(arguments, false);
    ST_REQUIRE(ran.has_value());
    // 子进程退出码 = 失败用例数（全通过时 0）；stdout 的 PASS 行是执行名单。
    ST_CHECK_EQ(ran->exit_code, 0);
    const std::vector<std::string> executed = extract_probe_names(ran->stdout_text);
    check_names_equal(executed, shard_names[index]);
    union_names.insert(union_names.end(), executed.begin(), executed.end());
  }
  // 并集与全集**作为集合**相等：排序后逐位比对。不排序直接比是错的——
  // 各片名单是注册序的保序子序列，片间拼接天然不是全局注册序，要钉的性质是
  // 「每个用例恰好被选中一次：不多、不少、不重」；片内的保序性由 `case_names`
  // 按注册序遍历的实现语义保证（下一步执行口径比对同一份名单，间接覆盖）。
  std::vector<std::string> union_sorted = union_names;
  std::vector<std::string> all_sorted = all_names;
  std::sort(union_sorted.begin(), union_sorted.end());
  std::sort(all_sorted.begin(), all_sorted.end());
  check_names_equal(union_sorted, all_sorted);

  auto all_executed = run_self({"shardprobe"}, false);
  ST_REQUIRE(all_executed.has_value());
  ST_CHECK_EQ(all_executed->exit_code, 0);
  check_names_equal(extract_probe_names(all_executed->stdout_text), all_names);

  // —— 第三步：失败口径（哨兵 07 故意 FAIL）——
  // 哨兵归属片已在第一步观察（failing_shard）。哨兵同 TU → 全部哨兵与 07 同片
  //（同注册表同算法，子进程与本进程算出同一映射）。失败注入后重跑四片执行：
  // 归属片退出码 1、其余 0，之和 == 全跑退出码 1：失败不被分片吞掉（和为 1）
  // 也不被放大（只有该片为 1）。
  int failed_shard_sum = 0;
  int failing_shard_index = -1;
  for (std::size_t index = 0; index < kShards; ++index) {
    const std::vector<std::string> arguments = {
        "--shard", std::format("{}/{}", index + 1, kShards), "shardprobe"};
    auto ran = run_self(arguments, true);
    ST_REQUIRE(ran.has_value());
    ST_CHECK_EQ(ran->exit_code, index == failing_shard ? 1 : 0);
    failed_shard_sum += ran->exit_code;
    if (ran->exit_code != 0) failing_shard_index = static_cast<int>(index);
  }
  ST_CHECK_EQ(failed_shard_sum, 1);
  ST_CHECK_EQ(failing_shard_index, static_cast<int>(failing_shard));

  auto all_fail_run = run_self({"shardprobe"}, true);
  ST_REQUIRE(all_fail_run.has_value());
  ST_CHECK_EQ(all_fail_run->exit_code, 1);
  // 全跑输出里能看到哨兵 07 的 FAIL 行：失败在两条路径下都「可见」，
  // 不会被「只有分片才看得到」（或反过来）藏住。
  ST_CHECK(all_fail_run->stdout_text.find("shardprobe_case_07") != std::string::npos);
}

/// 空分片是合法形态，不得报「JUnit 报告写入失败」（BACKLOG P2 附带项）。
///
/// 背景：`--test-jobs N` + filter 只命中少量用例时，多数片是 0 用例的空片——
/// `write_junit` 对 0 用例返回 0（「没有结果不如不产出」是它与 merge_junit_files
/// 的共同语义），而旧实现把「返回 0」一律当写失败报 stderr，每个空片一条噪声。
/// 判据：空片以 `ST_JUNIT_XML` 指向合法路径运行，**stderr 里不得出现写入失败**、
/// 退出码 0，且不产出报告文件（0 用例不写文件是语义的一部分，不是「忘了写」）。
ST_TEST(empty_shard_writes_no_junit_and_reports_no_failure) {
  if (parity_probe_enabled()) return;  // 哨兵运行里空转（防重入）

  // 报告路径放临时目录：每次跑都是新路径，不残留也不互踩。
  const std::string report_dir =
      st::fs::join(st::fs::temp_dir(),
                   std::format("st-parity-junit-{}", st::process::current_id()));
  (void)st::fs::remove_all(report_dir);
  ST_REQUIRE(st::fs::create_directories(report_dir).has_value());
  const std::string report_path = st::fs::join(report_dir, "empty-shard.xml");

  // 选一个「filter 只命中主用例名」下的**真空片**：主用例的归属片从**无过滤
  // `--list` 逐片观察**（不硬编码——注册表漂移/装箱变化都会平移归属；更不能
  // 从全局序取模推导：TU 装箱落片不透明，旧注释的「序号%4」推导已随取模调度
  // 退役，2026-10-15 的教训是「期望值只能观察」——现在连片号也不推导）。
  // 避开它取任一其它片：它要么空、要么含别的 TU 用例——filter 后同样空。
  constexpr std::size_t kShards = 4;
  std::size_t main_shard = kShards;   // 0 基；主用例必在 → 会被赋值
  for (std::size_t index = 0; index < kShards; ++index) {
    st::process::Options probe;
    probe.env["ST_PARITY_PROBE"] = "1";
    auto exe = st::process::executable_path();
    ST_REQUIRE(exe.has_value());
    auto listing = st::process::run(
        *exe, {"--list", "shard_parity_selection_and_results_match_unsharded",
               "--shard", std::format("{}/{}", index + 1, kShards)},
        probe);
    ST_REQUIRE(listing.has_value());
    if (listing->stdout_text.find("shard_parity_selection_and_results_match_unsharded")
        != std::string::npos) {
      main_shard = index;
      break;
    }
  }
  ST_REQUIRE(main_shard < kShards);
  const std::size_t empty_shard = (main_shard + 1) % kShards;  // 任一非主用例片

  st::process::Options options;
  options.env["ST_PARITY_PROBE"] = "1";  // 哨兵激活（否则空套件另有含义）
  options.env["ST_JUNIT_XML"] = report_path;
  auto exe = st::process::executable_path();
  ST_REQUIRE(exe.has_value());
  auto ran = st::process::run(*exe, {"--shard", std::format("{}/{}", empty_shard + 1, kShards),
                                    "shard_parity_selection_and_results_match_unsharded"},
                              options);
  ST_REQUIRE(ran.has_value());
  ST_CHECK_EQ(ran->exit_code, 0);
  ST_CHECK(ran->stderr_text.find("JUnit") == std::string::npos);
  ST_CHECK(ran->stderr_text.find("写入失败") == std::string::npos);
  ST_CHECK(!st::fs::is_regular_file(report_path));  // 0 用例不产出报告（共同语义）

  (void)st::fs::remove_all(report_dir);
}
