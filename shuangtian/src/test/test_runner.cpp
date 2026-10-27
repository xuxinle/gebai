#include "st/test/test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"

// lint-allow: L8 测试注册表与当前用例失败栈是测试基础设施的进程级状态（CONVENTIONS §3.6 例外登记）
namespace st::test {
namespace {

std::vector<Case> case_list{};
std::vector<std::string> current_failures{};
std::vector<CaseResult> last_case_results{};
std::uint64_t total_checks{0};

/// 当前用例是否请求跳过（`ST_SKIP`；每条用例开始前清零，见 `clear_failures`）。
/// 进程级标志而非 `Case` 字段：跳过发生在**运行中**（环境探测在用例体内），
/// 框架在循环里只能在用例结束后观察它——与失败收集同一个生命周期。
std::atomic<bool> g_skip_requested{false};

/// 注册表互斥锁（**函数内静态**，不能用命名空间作用域的 `std::mutex`）。
///
/// 为什么要这样：`ST_TEST` 生成的 `Registrar` 是**其他翻译单元的静态对象**，
/// 在各自 TU 的静态初始化阶段就调用 `Registry::add` → 需要这把锁；而命名空间作用域的
/// `std::mutex` 与它们的构造顺序**跨 TU 未定义**——一旦锁未构造就被锁，
/// 在 Windows/`__gthr_win32_mutex_lock` 下是启动即段错误（g++ 按链接顺序踩中，
/// MSVC 侥幸顺序正确而长期掩盖了它）。函数内静态由 C++ 保证**首次使用时构造**，
/// 与调用方的初始化顺序无关。
[[nodiscard]] auto registry_mutex() -> std::mutex& {
  static std::mutex instance;
  return instance;
}

/// 是否包含慢用例（由 `--slow` / `ST_TEST_SLOW` 设定）。
/// 真值源是进程级开关：用例注册是静态初始化，环境变量在 `main` 里读取后才生效，
/// 所以判定放在 `run_all` 的循环里（那时开关已经设好了）。
bool g_include_slow{false};

/// 本进程的分片（`index` 从 1 起；`count <= 1` 或 `index == 0` = 不分片）。
std::size_t g_shard_index{0};
std::size_t g_shard_count{0};

/// TU 装箱后的「用例序号 → 分片」映射（index-0 基；`g_shard_count` 个桶）。
///
/// 为什么能本地算：`ST_TEST` 注册发生在静态初始化，**每个分片进程都持有完整用例
/// 清单**（与单进程同一份注册表）——在首次查询分片归属时把全套件按 TU 装箱一遍，
/// 16 个进程算出**同一份确定性结果**（同清单、同算法、无时间依赖），无需父进程
/// 下发映射。
std::vector<std::size_t> g_shard_assign;

/// 按 TU 贪心装箱（大 TU 优先，尽量均匀）：同 TU 用例不跨片。
///
/// 为何按 TU 装箱（git_service 家族 16 片并行随机误伤的根修，BACKLOG 40 轮立案）：
/// 同 TU 用例天然同族（共享静态/全局状态、同类外部资源如临时目录/git 子进程），
/// 注册序均分会把一族打散到全部 16 片——并行窗口里每片都同时做同类子进程/临时
/// 目录操作，竞争是随机的（失败名单每轮不同）。TU 为最小不可分单元集中到单片
/// 后，同族串行（与单进程同语义），跨进程竞争消失；全部用例仍在默认路径跑
///（不踢隔离集，不重试）。
///
/// 算法：按 TU 聚合 → 大 TU 优先 → 依次放入当前用例数最少的桶（确定性：并列取
/// 序号最小）。**手写注册（`file` 为空）各自独立成组参与装箱**：它们无 TU 信息可
/// 依据，独立成组与旧调度一样均匀，只是具体落桶不同（确定性同样成立）。
/// 用例数（而非耗时）做均衡量：耗时数据上一轮才有、且受环境扰动，用例数是
/// 稳定且对分片并行足够的近似（历史分片也是按条数均分的）。
void assign_shards_by_tu() {
  const auto& cases = case_list;
  if (g_shard_assign.size() == cases.size()) return;   // 惰性：只算一次
  // ① 按 TU 聚合（file 为空的用例各自独立成组）。
  std::map<std::string, std::vector<std::size_t>> groups;
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const std::string& file = cases[index].file;
    groups[file.empty() ? std::format("\x01#{}", index) : file].push_back(index);
  }
  // ② 组按大小降序（并列按组名字典序，确定性）。
  std::vector<const std::vector<std::size_t>*> ordered;
  ordered.reserve(groups.size());
  for (const auto& [name, members] : groups) {
    (void)name;
    ordered.push_back(&members);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const std::vector<std::size_t>* lhs, const std::vector<std::size_t>* rhs) {
              if (lhs->size() != rhs->size()) return lhs->size() > rhs->size();
              return *lhs < *rhs;   // 同大小时按成员序号列表字典序（确定性强序，非指针序）
            });
  // ③ 贪心装箱：大 TU 优先，每次放入当前用例数最少的桶。
  g_shard_assign.assign(cases.size(), 0);
  std::vector<std::size_t> loads(g_shard_count, 0);
  for (const auto* members : ordered) {
    std::size_t target = 0;
    for (std::size_t bucket = 1; bucket < g_shard_count; ++bucket) {
      if (loads[bucket] < loads[target]) target = bucket;
    }
    for (const std::size_t member : *members) g_shard_assign[member] = target;
    loads[target] += members->size();
  }
}

/// 本用例是否属于当前分片（TU 装箱后的查表；`count <= 1` 时恒真）。
[[nodiscard]] auto in_shard(std::size_t ordinal) -> bool {
  if (g_shard_count <= 1 || g_shard_index == 0) return true;
  if (g_shard_assign.empty()) assign_shards_by_tu();
  if (ordinal >= g_shard_assign.size()) return false;
  return g_shard_assign[ordinal] == g_shard_index - 1;
}

/// 名下的用例是否属于当前分片（供 `case_names` 与 `run_all` 共用同一口径）。
[[nodiscard]] auto selected_ordinal(const Case& item, std::size_t ordinal,
                                    std::string_view filter) -> bool {
  if (!filter.empty() && item.name.find(filter) == std::string::npos) return false;
  return in_shard(ordinal);
}

}  // namespace

auto Registry::instance() -> Registry& {
  // lint-allow: L8 测试注册表是测试基础设施的进程级单例（CONVENTIONS §3.6 例外登记；行内豁免须随违规行走）
  static Registry registry;
  return registry;
}

void Registry::add(std::string name, std::function<void()> body) {
  const std::scoped_lock lock(registry_mutex());
  case_list.push_back(Case{std::move(name), std::move(body), 0, false, {}});
}

void Registry::add(std::string name, std::function<void()> body, std::string file) {
  const std::scoped_lock lock(registry_mutex());
  case_list.push_back(Case{std::move(name), std::move(body), 0, false, std::move(file)});
}

void Registry::add(std::string name, std::function<void()> body, std::int64_t timeout_ms) {
  const std::scoped_lock lock(registry_mutex());
  case_list.push_back(Case{std::move(name), std::move(body), timeout_ms, false, {}});
}

void Registry::add(std::string name, std::function<void()> body, std::string file,
                    std::int64_t timeout_ms) {
  const std::scoped_lock lock(registry_mutex());
  case_list.push_back(
      Case{std::move(name), std::move(body), timeout_ms, false, std::move(file)});
}

void Registry::add_slow(std::string name, std::function<void()> body) {
  const std::scoped_lock lock(registry_mutex());
  case_list.push_back(Case{std::move(name), std::move(body), 0, true, {}});
}

void Registry::add_slow(std::string name, std::function<void()> body, std::string file) {
  const std::scoped_lock lock(registry_mutex());
  case_list.push_back(Case{std::move(name), std::move(body), 0, true, std::move(file)});
}

auto Registry::cases() -> std::vector<Case>& { return case_list; }

void set_include_slow(bool value) { g_include_slow = value; }

auto include_slow() -> bool { return g_include_slow; }

auto parse_shard(std::string_view text) -> std::pair<std::size_t, std::size_t> {
  // 两个数字分别取出再转换：不能直接 `return {expr, expr}`——`std::optional<uint64_t>`
  // 到 `size_t` 的窄化转换在**花括号初始化列表**里是非法的（列表初始化禁窄化），
  // 而 `static_cast` 写在列表里也仍被判为窄化（GCC 实测：`could not convert`）。
  const std::size_t slash = text.find('/');
  if (slash == std::string_view::npos) return std::make_pair<std::size_t, std::size_t>(0, 0);
  const auto index = st::parse_u64(text.substr(0, slash));
  const auto count = st::parse_u64(text.substr(slash + 1));
  if (!index.has_value() || !count.has_value()) return std::make_pair<std::size_t, std::size_t>(0, 0);
  // 单片/越界的写法都当"不分片"处理（调用方对 0 报错——只有 `--shard i/n` 里
  // **显式给错**才算错误，`n == 1` 是合法的"单片"表达）。
  if (*count <= 1 || *index == 0 || *index > *count) {
    return std::make_pair<std::size_t, std::size_t>(0, 0);
  }
  const std::size_t index_out = static_cast<std::size_t>(*index);
  const std::size_t count_out = static_cast<std::size_t>(*count);
  return std::make_pair(index_out, count_out);
}

void set_shard(std::size_t index, std::size_t count) {
  if (count <= 1 || index == 0 || index > count) {
    g_shard_index = 0;
    g_shard_count = 0;
    g_shard_assign.clear();
    return;
  }
  g_shard_index = index;
  g_shard_count = count;
  // 装箱映射按 (清单, 片数) 缓存：片数变了必须重建，否则查的还是旧片数的桶
  //（生产路径每子进程单一 (index, count)、只设一次，此句零影响；进程内切换
  // 片数的自检用例——`shard_partitions_cover_every_case_exactly_once` 等——
  // 依赖它拿到新片数的正确划分）。
  g_shard_assign.clear();
}

auto shard() -> std::pair<std::size_t, std::size_t> { return {g_shard_index, g_shard_count}; }

auto shard_suffix() -> std::string {
  if (g_shard_count <= 1 || g_shard_index == 0) return {};
  return std::format("-shard{}of{}", g_shard_index, g_shard_count);
}


auto case_names(std::string_view filter) -> std::vector<std::string> {
  std::vector<std::string> names;
  std::size_t ordinal = 0;
  for (const auto& item : Registry::instance().cases()) {
    if (!selected_ordinal(item, ordinal, filter)) {
      ++ordinal;
      continue;
    }
    names.push_back(item.name);
    ++ordinal;
  }
  return names;
}

void Registry::record_failure(std::string_view file, int line, std::string message) {
  const std::scoped_lock lock(registry_mutex());
  current_failures.push_back(std::format("{}:{}: {}", file, line, message));
}

void Registry::record_skip(std::string_view reason) {
  // 跳过理由必须进输出（否则只是把"静默计成 PASS"换成"静默不计"，可见性问题没解决）。
  // 不入失败栈：跳过与失败同时发生时，run_all 的判定序是「失败优先」——失败栈里
  // 混入跳过记录反而会把纯跳过用例误判成失败。
  st::print("  \x1b[33mSKIP\x1b[0m {}\n", std::string(reason).substr(0, 200));
  g_skip_requested.store(true);
}

auto Registry::skip_requested() const noexcept -> bool { return g_skip_requested.load(); }

void Registry::clear_failures() {
  const std::scoped_lock lock(registry_mutex());
  current_failures.clear();
  g_skip_requested.store(false);
}

auto Registry::failures() const -> const std::vector<std::string>& { return current_failures; }

auto Registry::check_count() const noexcept -> std::uint64_t { return total_checks; }

void Registry::count_check() noexcept { ++total_checks; }

auto run_all(std::string_view filter) -> int {
  auto& registry = Registry::instance();
  int failed_cases = 0;
  int passed_cases = 0;
  int skipped_env = 0;   // 运行中跳过（`ST_SKIP`）：环境前置不满足等，汇总行如实报数
  std::vector<std::string> failed_names;   // 失败用例名（结尾汇总 + 复现提示）
  int skipped_slow = 0;
  std::size_t ordinal = 0;   ///< 注册顺序下标（分片取模的口径，与 filter 无关）
  std::uint64_t checks_before = registry.check_count();
  // 每次运行都清空：这份结果只描述**本次**跑过的用例（见 `write_junit`）
  last_case_results.clear();

  // 软超时上限：默认 10s。挑得宽（最慢的合法用例 ui_* 动画类 ~100ms 量级），
  // 只拦「死循环/挂死」级别的异常——这是此前整个测试进程会无声挂住的那类故障。
  std::int64_t timeout_ms = 10'000;
  if (const auto env = fs::read_env("ST_TEST_TIMEOUT_MS"); env.has_value() && !env->empty()) {
    const auto parsed = std::atoll(env->c_str());
    if (parsed > 0) timeout_ms = parsed;
  }

  // `ordinal_now` 是**注册顺序下标**（分片的取模口径），与 filter 无关——
  // `st test --jobs 8 foo` 与串行跑的候选集因此完全一致。
  for (auto& item : registry.cases()) {
    const std::size_t ordinal_now = ordinal++;
    if (!in_shard(ordinal_now)) continue;
    if (!filter.empty() && item.name.find(filter) == std::string::npos) continue;
    // 慢/环境敏感用例：默认跳过（见 `Case::slow` 的注释）。
    // **显式指名时仍然跑**——`st test frame_cost` 这种用法意图明确，不该被门挡掉。
    if (item.slow && !include_slow() && filter.empty()) {
      ++skipped_slow;
      continue;
    }
    registry.clear_failures();
    const std::int64_t start_ns = time::now_ns();

    // 用例在独立线程上跑，主线程带软超时探测：超时记 FAIL 并**继续等它跑完**
    // （不 detach、不杀——静态状态不允许）。观察者只做标记，主线程最多等
    // timeout_ms + 一个宽限窗，之后按「疑似挂死」打印诊断并继续下一个用例。
    //
    // **唤醒方式是条件变量而非 5ms 轮询**：549 个用例 × 每例固定等一轮 5ms ≈ 2.7s
    // 的纯轮询税（用例本身再快也要付），`wait_for` 到点即醒——快用例（<1ms）的
    // 观测开销从恒定 5ms 降到微秒级。
    std::mutex done_mutex;
    std::condition_variable done_signal;
    bool done_flag = false;
    std::atomic<bool> timed_out{false};
    // ⚠ **用例体必须被异常边界包住**：用例里任何未捕获的异常（`Json::get<T>()` 对
    // null 会抛 `type_error`、容器越界、stoi 失败…）都会一路逃到线程入口，
    // 而线程函数抛出 = `std::terminate` ⇒ **整个测试进程消失**，
    // 前面的 PASS 全部作废，且报告里只留一句 `terminate called after throwing …`
    //（实测：`script_bridge_run_inline_source` 里的 `eval_ok` 返回 null 后
    // `get<int>()` 抛出，把 1268 条用例的整套运行在第 800 条处带走）。
    //
    // 记成 FAIL（而不是吞掉）是关键：**异常是用例失败的另一种形态**，
    // 它必须出现在结果里，否则就成了"静默消失的用例"。
    std::string uncaught;
    std::thread worker([&]() {
      try {
        item.body();
      } catch (const std::exception& error) {
        uncaught = std::string("未捕获异常: ") + error.what();
      } catch (...) {
        uncaught = "未捕获异常（非 std::exception）";
      }
      {
        const std::scoped_lock lock(done_mutex);
        done_flag = true;
      }
      done_signal.notify_all();
    });
    bool joined = false;
    while (!joined) {
      {
        std::unique_lock<std::mutex> lock(done_mutex);
        done_signal.wait_for(lock, std::chrono::milliseconds(50), [&] { return done_flag; });
      }
      if (done_flag) {
        worker.join();
        if (!uncaught.empty()) {
          // 报在**本文件本行**（框架侧）而不是用例的声明处：`Case` 不带 file/line
          // （用例自己报错时才由宏提供），而异常已经带上 `what()`——信息够定位。
          ::st::test::Registry::instance().record_failure(__FILE__, __LINE__,
                                                          item.name + "：" + uncaught);
        }
        joined = true;
        break;
      }
          // 逐用例的软超时：用例自带时用它（集成级用例真编译、真跑产物，耗时由外部工具
    // 决定而不是被测代码的快慢），否则用全局默认。
    const std::int64_t case_timeout_ms = item.timeout_ms > 0 ? item.timeout_ms : timeout_ms;
    if (time::now_ns() - start_ns > case_timeout_ms * 1'000'000LL &&
        !timed_out.exchange(true)) {
      st::print("  \x1b[33mTIME\x1b[0m {:<44} 超过 {} ms 仍在运行（软超时标记 FAIL，继续等待）\n",
                item.name, case_timeout_ms);
    }
    }
    // 走到这说明用例已结束（join 完成）。超时但最终结束的用例按失败计。
    if (timed_out.load()) registry.record_failure("timeout", 0, "用例超过软超时上限");

    const std::string elapsed = time::format_duration_ns(time::now_ns() - start_ns);
    CaseResult result;
    result.name = item.name;
    result.elapsed_ms = static_cast<double>(time::now_ns() - start_ns) / 1e6;
    result.failures = registry.failures();
    result.timed_out = timed_out.load();
    if (!registry.failures().empty()) {
      ++failed_cases;
      st::print("  \x1b[31mFAIL\x1b[0m {:<44} {}\n", item.name, elapsed);
      for (const auto& failure : registry.failures()) {
        st::print("        {}\n", failure);
      }
      // **复现提示**：把"怎么单跑这一条"直接给出。
      // 为什么值得（AI 迭代的核心动作就是"跑失败的那条"）：用例名长且带下划线，
      // 手敲容易错；而且本工程默认 4 片并行，直接 `st test <名>` 的分片过滤
      // 有过不命中的情况——给出准确形式省一次试错。
      failed_names.push_back(item.name);
      st::print("        \x1b[33m复现\x1b[0m st test {}\n", item.name);
    } else if (registry.skip_requested()) {
      // 运行中跳过（`ST_SKIP`）：不算通过（否则环境缺失被计成绿灯）、不算失败
      // （那会把"框架根未注入"误报成被测对象的缺陷）。理由已由 `record_skip` 打出。
      ++skipped_env;
      result.skipped = true;
      st::print("  \x1b[33mSKIP\x1b[0m {:<44} {}\n", item.name, elapsed);
    } else {
      ++passed_cases;
      result.passed = true;
      st::print("  \x1b[32mPASS\x1b[0m {:<44} {}\n", item.name, elapsed);
    }
    {
      const std::scoped_lock lock(registry_mutex());
      last_case_results.push_back(std::move(result));
    }
    std::fflush(stdout);
  }

  const std::uint64_t checks = registry.check_count() - checks_before;
  // **失败汇总**：跑了几百条时，失败散布在输出里（尤其分片并行时各片交错），
  // 结尾集中列一遍，省得往上翻。
  if (!failed_names.empty()) {
    st::print("\n  \x1b[31m失败用例（{}）\x1b[0m：\n", failed_names.size());
    for (const auto& name : failed_names) {
      st::print("    - {}\n", name);
    }
    st::print("  单条复现：st test <用例名>\n");
  }
  st::print("\n  {} passed, {} failed, {} assertions", passed_cases, failed_cases, checks);
  if (skipped_env > 0) {
    // 运行中跳过必须在汇总里可见：CI 门禁看到"跳过 N"才能发现"环境没配好，
    // 这一条其实没验"（静默时它藏在 PASS 计数里，BACKLOG 3903）。
    st::print(", {} skipped（环境前置不满足，ST_SKIP）", skipped_env);
  }
  if (skipped_slow > 0) {
    st::print("（另 {} 个慢/环境敏感用例默认跳过：`--slow` 或 `st test --slow`）", skipped_slow);
  }
  st::print("\n");
  return failed_cases;
}

auto list_cases(std::string_view filter) -> int {
  // 分片环境下只列本片（否则 `--shard 2/8 --list` 会把全部名字列八遍）
  std::size_t ordinal = 0;
  for (const auto& item : Registry::instance().cases()) {
    const std::size_t ordinal_now = ordinal++;
    if (!in_shard(ordinal_now)) continue;
    if (!filter.empty() && item.name.find(filter) == std::string::npos) continue;
    st::print("{}\n", item.name);
  }
  return 0;
}

auto last_results() -> const std::vector<CaseResult>& { return last_case_results; }

auto write_junit(std::string_view path) -> std::size_t {
  if (path.empty() || last_case_results.empty()) return 0;
  const std::scoped_lock lock(registry_mutex());
  std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites>\n";
  std::size_t failed = 0;
  for (const auto& result : last_case_results) {
    // 跳过不算失败（`ST_SKIP`：环境前置不满足）——否则单进程报告的 failures 属性虚高，
    // CI 会把"环境没配好"误判成"被测对象坏了"。分片合并侧同语义（只认 <failure> 标签）。
    if (!result.passed && !result.skipped) ++failed;
  }
  xml += std::format("  <testsuite name=\"st\" tests=\"{}\" failures=\"{}\">\n",
                     last_case_results.size(), failed);
  for (const auto& result : last_case_results) {
    xml += std::format("    <testcase name=\"{}\" time=\"{:.3f}\"", result.name,
                       result.elapsed_ms / 1000.0);
    if (result.skipped) {
      // 跳过也进 JUnit：CI 才能发现"这条其实没验"（`ST_SKIP`，与汇总行同一信息）。
      xml += "><skipped/></testcase>\n";
      continue;
    }
    if (result.passed) {
      xml += "/>\n";
      continue;
    }
    xml += ">\n      <failure><![CDATA[";
    for (const auto& failure : result.failures) {
      xml += failure;
      xml += "\\n";
    }
    xml += "]]></failure>\n    </testcase>\n";
  }
  xml += "  </testsuite>\n</testsuites>\n";
  if (auto status = fs::write_text(path, xml); !status) return 0;
  return xml.size();
}

}  // namespace st::test
