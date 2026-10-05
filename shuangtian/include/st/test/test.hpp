#pragma once

/// 霜天自研测试框架（零依赖）：注册制用例 + 断言宏 + 汇总报告。
/// 用法：
/// ```cpp
/// #include "st/test/test.hpp"
/// ST_TEST(json_parse_basic) {
///   auto value = st::json_parse(R"({"a":1})");
///   ST_REQUIRE(value.has_value());
///   ST_CHECK_EQ(st::json_get_i64(*value, "a"), 1);
/// }
/// ```
/// 约定：本文件是 `CONVENTIONS.md` §3.6 登记的唯一函数式宏例外——断言必须捕获 `__FILE__`/`__LINE__`。

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace st::test {

struct Case {
  std::string name{};
  std::function<void()> body{};
  /// 本用例的软超时上限（ms）；`0` = 用默认值。
  ///
  /// 为何需要逐用例覆盖：默认值（10 s）是按“单元级用例”标的（最慢的动画类 ~100 ms），
  /// 而**集成级**用例要真编译、真跑产物，在 debug 档下 10 s 不够（实测 18 s）——
  /// 它会在测试框架里报软超时失败，而那不是被测对象的问题。
  std::int64_t timeout_ms{0};
  /// 慢/环境敏感用例：默认**不跑**（`--slow` 显式开启，`set_include_slow`）。
  ///
  /// 适用对象是**量机器性能**的用例（帧耗时/吞吐阈值）与需要真编译的集成用例：
  /// 它们耗时由环境决定，在共享机器上还会被邻居抬高而**偶发红灯**。
  /// 把“代码对不对”与“这台机器此刻快不快”分开报，是它们不阻塞内循环的前提。
  bool slow{false};
};

/// 单个用例的运行结果（junit 报告与超时标记用）。
struct CaseResult {
  std::string name{};
  bool passed{false};
  bool timed_out{false};          ///< 超过软超时：用例可能仍在跑，结果标记为失败
  double elapsed_ms{0.0};
  std::vector<std::string> failures{};
};

/// 用例注册表与当前用例的失败收集（进程级；测试基础设施的受控例外，见 CONVENTIONS §3.6）。
class Registry {
 public:
  static auto instance() -> Registry&;

  void add(std::string name, std::function<void()> body);
  /// 带显式软超时的注册（ms；`0` = 用默认值）。
  void add(std::string name, std::function<void()> body, std::int64_t timeout_ms);
  /// 注册一个**慢**用例（默认不跑，`--slow` 开启）。
  void add_slow(std::string name, std::function<void()> body);
  [[nodiscard]] auto cases() -> std::vector<Case>&;

  void record_failure(std::string_view file, int line, std::string message);
  void clear_failures();
  [[nodiscard]] auto failures() const -> const std::vector<std::string>&;
  [[nodiscard]] auto check_count() const noexcept -> std::uint64_t;
  void count_check() noexcept;
};

/// 是否包含慢用例（`--slow` / `ST_TEST_SLOW=1`）。默认 false。
void set_include_slow(bool value);
[[nodiscard]] auto include_slow() -> bool;

/// 运行全部用例（`filter` 非空时按名称子串过滤）；返回失败用例数。
/// 单用例软超时（默认 10s，`ST_TEST_TIMEOUT_MS` 覆盖）：超时标记 FAIL 但**不硬杀**——
/// 观察线程只标记，用例线程继续跑完（硬杀会撕裂静态状态；软超时保证单个死循环
/// 用例不会无声挂住整个测试进程，后续用例仍能拿到结果）。
auto run_all(std::string_view filter) -> int;

/// 列出全部用例名（`--list`；每行一个，含 filter 语义）。
auto list_cases(std::string_view filter) -> int;

/// 把上一轮 `run_all` 的逐用例结果写成 JUnit XML（CI 消费）。
/// 返回写入的字节数；路径为空或上次无结果时返回 0。
[[nodiscard]] auto write_junit(std::string_view path) -> std::size_t;

/// 上一轮运行的逐用例结果（`write_junit` 的数据源）。
[[nodiscard]] auto last_results() -> const std::vector<CaseResult>&;

/// 用例注册器（`ST_TEST` 生成的静态对象调用）。
struct Registrar {
  Registrar(std::string_view name, std::function<void()> body) {
    Registry::instance().add(std::string(name), std::move(body));
  }
  /// 带显式软超时的注册（`ST_TEST_WITH_TIMEOUT` 用）。
  Registrar(std::string_view name, std::function<void()> body, std::int64_t timeout_ms) {
    Registry::instance().add(std::string(name), std::move(body), timeout_ms);
  }
};

/// 慢用例注册器（`ST_TEST_SLOW` 生成的静态对象调用）。
struct SlowRegistrar {
  SlowRegistrar(std::string_view name, std::function<void()> body) {
    Registry::instance().add_slow(std::string(name), std::move(body));
  }
};

}  // namespace st::test

/// 定义并注册一个测试用例（函数体即用例）。
#define ST_TEST(test_name)                                                        \
  static void st_test_case_##test_name();                                         \
  namespace {                                                                     \
  const ::st::test::Registrar st_test_registrar_##test_name{                      \
      #test_name, &st_test_case_##test_name};                                     \
  }                                                                               \
  static void st_test_case_##test_name()

/// 注册一个**慢/环境敏感**用例（默认不跑，`--slow` 开启）。
///
/// 列入这里的理由必须是“耗时或成败取决于环境，而非代码正确性”：
/// 量帧耗时/吞吐阈值的性能门禁、需要真编译真跑进程的集成用例。
/// 组件功能与不变量用例**不属于**这里——它们快且确定，必须留在默认路径上。
#define ST_TEST_SLOW(test_name)                                                    \
  static void st_test_case_##test_name();                                          \
  namespace {                                                                      \
  const ::st::test::SlowRegistrar st_test_registrar_##test_name{                   \
      #test_name, &st_test_case_##test_name};                                      \
  }                                                                                \
  static void st_test_case_##test_name()

/// 同 `ST_TEST`，但指定本用例的**软超时上限**（ms）。
///
/// 用途：**集成级**用例（真编译一个工程、真跑一个进程）——它们的耗时由外部工具决定
/// （debug 档实测 18 s），而不是被测代码的快慢。不覆盖的话它们会在测试框架里
/// 报软超时失败，把一个“框架默认值不适合这类用例”的问题误报成“被测对象有问题”。
#define ST_TEST_WITH_TIMEOUT(test_name, timeout_ms)                                \
  static void st_test_case_##test_name();                                         \
  namespace {                                                                     \
  const ::st::test::Registrar st_test_registrar_##test_name{                      \
      #test_name, &st_test_case_##test_name, (timeout_ms)};                        \
  }                                                                               \
  static void st_test_case_##test_name()

/// 记录失败但继续执行。
#define ST_CHECK(expr)                                                             \
  do {                                                                             \
    ::st::test::Registry::instance().count_check();                                 \
    if (!(expr)) {                                                                  \
      ::st::test::Registry::instance().record_failure(                              \
          __FILE__, __LINE__, std::format("断言失败: {}", #expr));                   \
    }                                                                              \
  } while (false)

/// 前置断言：失败即结束当前用例。
#define ST_REQUIRE(expr)                                                            \
  do {                                                                              \
    ::st::test::Registry::instance().count_check();                                  \
    if (!(expr)) {                                                                   \
      ::st::test::Registry::instance().record_failure(                               \
          __FILE__, __LINE__, std::format("前置断言失败: {}", #expr));                \
      return;                                                                        \
    }                                                                                \
  } while (false)

/// 相等断言（要求两侧可 `std::format`）。
///
/// **按值落地两侧**（`auto` 而非 `const auto&`）：宏体是多条语句，若用引用绑定，
/// 实参里的临时对象（如 `*view.get_property(...)` 返回的 `optional<string>`）会在第一条
/// 语句结束时就析构，后续比较与格式化读到的是已销毁对象——ASan 实测为 stack-use-after-scope。
#define ST_CHECK_EQ(actual, expected)                                               \
  do {                                                                              \
    ::st::test::Registry::instance().count_check();                                  \
    const auto st_actual = (actual);                                                 \
    const auto st_expected = (expected);                                             \
    if (!(st_actual == st_expected)) {                                               \
      ::st::test::Registry::instance().record_failure(                               \
          __FILE__, __LINE__,                                                        \
          std::format("期望 {} == {}，实际 {} != {}", #actual, #expected,             \
                      st_actual, st_expected));                                       \
    }                                                                                \
  } while (false)

/// 浮点近似断言。
#define ST_CHECK_NEAR(actual, expected, epsilon)                                     \
  do {                                                                               \
    ::st::test::Registry::instance().count_check();                                   \
    const double st_delta = static_cast<double>(actual) - static_cast<double>(expected); \
    const double st_eps = static_cast<double>(epsilon);                                \
    if (!(st_delta <= st_eps && st_delta >= -st_eps)) {                                \
      ::st::test::Registry::instance().record_failure(                                 \
          __FILE__, __LINE__,                                                          \
          std::format("期望 {} ≈ {}（容差 {}），差值 {}", #actual, #expected, #epsilon,  \
                      st_delta));                                                      \
    }                                                                                  \
  } while (false)

/// 直接失败。
#define ST_FAIL(message)                                                            \
  do {                                                                              \
    ::st::test::Registry::instance().record_failure(                                \
        __FILE__, __LINE__, std::format("显式失败: {}", (message)));                  \
  } while (false)
