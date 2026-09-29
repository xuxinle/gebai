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
};

/// 用例注册表与当前用例的失败收集（进程级；测试基础设施的受控例外，见 CONVENTIONS §3.6）。
class Registry {
 public:
  static auto instance() -> Registry&;

  void add(std::string name, std::function<void()> body);
  [[nodiscard]] auto cases() -> std::vector<Case>&;

  void record_failure(std::string_view file, int line, std::string message);
  void clear_failures();
  [[nodiscard]] auto failures() const -> const std::vector<std::string>&;
  [[nodiscard]] auto check_count() const noexcept -> std::uint64_t;
  void count_check() noexcept;
};

/// 运行全部用例（`filter` 非空时按名称子串过滤）；返回失败用例数。
auto run_all(std::string_view filter) -> int;

/// 用例注册器（`ST_TEST` 生成的静态对象调用）。
struct Registrar {
  Registrar(std::string_view name, std::function<void()> body) {
    Registry::instance().add(std::string(name), std::move(body));
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
