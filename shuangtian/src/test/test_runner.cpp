#include "st/test/test.hpp"

#include <cstdio>
#include <mutex>

#include "st/core/time.hpp"

// lint-allow: L8 测试注册表与当前用例失败栈是测试基础设施的进程级状态（CONVENTIONS §3.6 例外登记）
namespace st::test {
namespace {

std::vector<Case> case_list{};
std::vector<std::string> current_failures{};
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

  for (auto& item : registry.cases()) {
    if (!filter.empty() && item.name.find(filter) == std::string::npos) continue;
    registry.clear_failures();
    const std::int64_t start_ns = time::now_ns();
    item.body();
    const std::string elapsed = time::format_duration_ns(time::now_ns() - start_ns);
    if (registry.failures().empty()) {
      ++passed_cases;
      std::fprintf(stdout, "  \x1b[32mPASS\x1b[0m %-44s %s\n", item.name.c_str(), elapsed.c_str());
    } else {
      ++failed_cases;
      std::fprintf(stdout, "  \x1b[31mFAIL\x1b[0m %-44s %s\n", item.name.c_str(), elapsed.c_str());
      for (const auto& failure : registry.failures()) {
        std::fprintf(stdout, "        %s\n", failure.c_str());
      }
    }
    std::fflush(stdout);
  }

  const std::uint64_t checks = registry.check_count() - checks_before;
  std::fprintf(stdout, "\n  %d passed, %d failed, %llu assertions\n", passed_cases, failed_cases,
               static_cast<unsigned long long>(checks));
  std::fflush(stdout);
  return failed_cases;
}

}  // namespace st::test
