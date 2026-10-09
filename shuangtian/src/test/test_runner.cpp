#include "st/test/test.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"

// lint-allow: L8 测试注册表与当前用例失败栈是测试基础设施的进程级状态（CONVENTIONS §3.6 例外登记）
namespace st::test {
namespace {

std::vector<Case> case_list{};
std::vector<std::string> current_failures{};
std::vector<CaseResult> last_case_results{};
std::uint64_t total_checks{0};

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

/// 本用例是否属于当前分片（按**注册顺序的下标**取模，与 `run_all` 的遍历口径一致）。
/// `count <= 1` 时恒真。
[[nodiscard]] auto in_shard(std::size_t ordinal) -> bool {
  if (g_shard_count <= 1 || g_shard_index == 0) return true;
  return ordinal % g_shard_count == g_shard_index - 1;
}

/// 名下的用例是否属于当前分片（供 `case_names` 与 `run_all` 共用同一口径）。
[[nodiscard]] auto selected_ordinal(const Case& item, std::size_t ordinal,
                                    std::string_view filter) -> bool {
  if (!filter.empty() && item.name.find(filter) == std::string::npos) return false;
  return in_shard(ordinal);
}

}  // namespace

auto Registry::instance() -> Registry& {
  static Registry registry;
  return registry;
}

void Registry::add(std::string name, std::function<void()> body) {
  const std::scoped_lock lock(registry_mutex());
  case_list.push_back(Case{std::move(name), std::move(body), 0, false});
}

void Registry::add(std::string name, std::function<void()> body, std::int64_t timeout_ms) {
  const std::scoped_lock lock(registry_mutex());
  case_list.push_back(Case{std::move(name), std::move(body), timeout_ms, false});
}

void Registry::add_slow(std::string name, std::function<void()> body) {
  const std::scoped_lock lock(registry_mutex());
  case_list.push_back(Case{std::move(name), std::move(body), 0, true});
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
    return;
  }
  g_shard_index = index;
  g_shard_count = count;
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

void Registry::clear_failures() {
  const std::scoped_lock lock(registry_mutex());
  current_failures.clear();
}

auto Registry::failures() const -> const std::vector<std::string>& { return current_failures; }

auto Registry::check_count() const noexcept -> std::uint64_t { return total_checks; }

void Registry::count_check() noexcept { ++total_checks; }

auto run_all(std::string_view filter) -> int {
  auto& registry = Registry::instance();
  int failed_cases = 0;
  int passed_cases = 0;
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
    std::thread worker([&]() {
      item.body();
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
        joined = true;
        break;
      }
          // 逐用例的软超时：用例自带时用它（集成级用例真编译、真跑产物，耗时由外部工具
    // 决定而不是被测代码的快慢），否则用全局默认。
    const std::int64_t case_timeout_ms = item.timeout_ms > 0 ? item.timeout_ms : timeout_ms;
    if (time::now_ns() - start_ns > case_timeout_ms * 1'000'000LL &&
        !timed_out.exchange(true)) {
      std::fprintf(stdout, "  \x1b[33mTIME\x1b[0m %-44s 超过 %lld ms 仍在运行（软超时标记 FAIL，继续等待）\n",
                   item.name.c_str(), static_cast<long long>(case_timeout_ms));
      std::fflush(stdout);
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
    if (registry.failures().empty()) {
      ++passed_cases;
      result.passed = true;
      std::fprintf(stdout, "  \x1b[32mPASS\x1b[0m %-44s %s\n", item.name.c_str(), elapsed.c_str());
    } else {
      ++failed_cases;
      std::fprintf(stdout, "  \x1b[31mFAIL\x1b[0m %-44s %s\n", item.name.c_str(), elapsed.c_str());
      for (const auto& failure : registry.failures()) {
        std::fprintf(stdout, "        %s\n", failure.c_str());
      }
      // **复现提示**：把"怎么单跑这一条"直接给出。
      // 为什么值得（AI 迭代的核心动作就是"跑失败的那条"）：用例名长且带下划线，
      // 手敲容易错；而且本工程默认 4 片并行，直接 `st test <名>` 的分片过滤
      // 有过不命中的情况——给出准确形式省一次试错。
      failed_names.push_back(item.name);
      std::fprintf(stdout, "        \x1b[33m复现\x1b[0m st test %s\n", item.name.c_str());
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
    std::fprintf(stdout, "\n  \x1b[31m失败用例（%zu）\x1b[0m：\n", failed_names.size());
    for (const auto& name : failed_names) {
      std::fprintf(stdout, "    - %s\n", name.c_str());
    }
    std::fprintf(stdout, "  单条复现：st test <用例名>\n");
  }
  std::fprintf(stdout, "\n  %d passed, %d failed, %llu assertions", passed_cases, failed_cases,
               static_cast<unsigned long long>(checks));
  if (skipped_slow > 0) {
    std::fprintf(stdout, "（另 %d 个慢/环境敏感用例默认跳过：`--slow` 或 `st test --slow`）",
                 skipped_slow);
  }
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
  return failed_cases;
}

auto list_cases(std::string_view filter) -> int {
  // 分片环境下只列本片（否则 `--shard 2/8 --list` 会把全部名字列八遍）
  std::size_t ordinal = 0;
  for (const auto& item : Registry::instance().cases()) {
    const std::size_t ordinal_now = ordinal++;
    if (!in_shard(ordinal_now)) continue;
    if (!filter.empty() && item.name.find(filter) == std::string::npos) continue;
    std::fprintf(stdout, "%s\n", item.name.c_str());
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
    if (!result.passed) ++failed;
  }
  xml += std::format("  <testsuite name=\"st\" tests=\"{}\" failures=\"{}\">\n",
                     last_case_results.size(), failed);
  for (const auto& result : last_case_results) {
    xml += std::format("    <testcase name=\"{}\" time=\"{:.3f}\"", result.name,
                       result.elapsed_ms / 1000.0);
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
