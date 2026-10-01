#include "st/test/test.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "st/core/fs.hpp"
#include "st/core/time.hpp"

// lint-allow: L8 测试注册表与当前用例失败栈是测试基础设施的进程级状态（CONVENTIONS §3.6 例外登记）
namespace st::test {
namespace {

std::vector<Case> case_list{};
std::vector<std::string> current_failures{};
std::vector<CaseResult> last_case_results{};
std::uint64_t total_checks{0};
std::mutex registry_mutex{};

}  // namespace

auto Registry::instance() -> Registry& {
  static Registry registry;
  return registry;
}

void Registry::add(std::string name, std::function<void()> body) {
  const std::scoped_lock lock(registry_mutex);
  case_list.push_back(Case{std::move(name), std::move(body)});
}

auto Registry::cases() -> std::vector<Case>& { return case_list; }

void Registry::record_failure(std::string_view file, int line, std::string message) {
  const std::scoped_lock lock(registry_mutex);
  current_failures.push_back(std::format("{}:{}: {}", file, line, message));
}

void Registry::clear_failures() {
  const std::scoped_lock lock(registry_mutex);
  current_failures.clear();
}

auto Registry::failures() const -> const std::vector<std::string>& { return current_failures; }

auto Registry::check_count() const noexcept -> std::uint64_t { return total_checks; }

void Registry::count_check() noexcept { ++total_checks; }

auto run_all(std::string_view filter) -> int {
  auto& registry = Registry::instance();
  int failed_cases = 0;
  int passed_cases = 0;
  std::uint64_t checks_before = registry.check_count();
  last_case_results.clear();

  // 软超时上限：默认 10s。挑得宽（最慢的合法用例 ui_* 动画类 ~100ms 量级），
  // 只拦「死循环/挂死」级别的异常——这是此前整个测试进程会无声挂住的那类故障。
  std::int64_t timeout_ms = 10'000;
  if (const auto env = fs::read_env("ST_TEST_TIMEOUT_MS"); env.has_value() && !env->empty()) {
    const auto parsed = std::atoll(env->c_str());
    if (parsed > 0) timeout_ms = parsed;
  }

  for (auto& item : registry.cases()) {
    if (!filter.empty() && item.name.find(filter) == std::string::npos) continue;
    registry.clear_failures();
    const std::int64_t start_ns = time::now_ns();

    // 用例在独立线程上跑，主线程带软超时探测：超时记 FAIL 并**继续等它跑完**
    // （不 detach、不杀——静态状态不允许）。观察者只做标记，主线程最多等
    // timeout_ms + 一个宽限窗，之后按「疑似挂死」打印诊断并继续下一个用例。
    std::atomic<bool> done{false};
    std::atomic<bool> timed_out{false};
    std::thread worker([&]() {
      item.body();
      done.store(true);
    });
    bool joined = false;
    while (!joined) {
      if (worker.joinable() && done.load()) {
        worker.join();
        joined = true;
        break;
      }
      if (!done.load() && time::now_ns() - start_ns > timeout_ms * 1'000'000LL &&
          !timed_out.exchange(true)) {
        std::fprintf(stdout, "  \x1b[33mTIME\x1b[0m %-44s 超过 %lld ms 仍在运行（软超时标记 FAIL，继续等待）\n",
                     item.name.c_str(), static_cast<long long>(timeout_ms));
        std::fflush(stdout);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
    }
    {
      const std::scoped_lock lock(registry_mutex);
      last_case_results.push_back(std::move(result));
    }
    std::fflush(stdout);
  }

  const std::uint64_t checks = registry.check_count() - checks_before;
  std::fprintf(stdout, "\n  %d passed, %d failed, %llu assertions\n", passed_cases, failed_cases,
               static_cast<unsigned long long>(checks));
  std::fflush(stdout);
  return failed_cases;
}

auto list_cases(std::string_view filter) -> int {
  for (const auto& item : Registry::instance().cases()) {
    if (!filter.empty() && item.name.find(filter) == std::string::npos) continue;
    std::fprintf(stdout, "%s\n", item.name.c_str());
  }
  return 0;
}

auto last_results() -> const std::vector<CaseResult>& { return last_case_results; }

auto write_junit(std::string_view path) -> std::size_t {
  if (path.empty() || last_case_results.empty()) return 0;
  const std::scoped_lock lock(registry_mutex);
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
