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

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
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
  /// 本用例所在翻译单元（`ST_TEST` 展开处的 `__FILE__`；手写注册为空）。
  ///
  /// 用途：分片调度按 TU 装箱——同 TU 用例天然同族（共享静态/全局状态、
  /// 同类外部资源如临时目录/git 子进程），集中到单片后同族串行，
  /// 跨进程随机竞争消失（git_service 家族 16 片并行随机误伤的根修，
  /// BACKLOG「git_service 家族分片并行随机误伤」）。为空时退回注册序均分
  /// （与旧调度同一语义，向后兼容）。
  std::string file{};
};

/// 单个用例的运行结果（junit 报告与超时标记用）。
struct CaseResult {
  std::string name{};
  bool passed{false};
  bool timed_out{false};          ///< 超过软超时：用例可能仍在跑，结果标记为失败
  bool skipped{false};            ///< 运行中主动跳过（`ST_SKIP`）：不算通过、不算失败
  double elapsed_ms{0.0};
  std::vector<std::string> failures{};
};

/// 用例注册表与当前用例的失败收集（进程级；测试基础设施的受控例外，见 CONVENTIONS §3.6）。
class Registry {
 public:
  static auto instance() -> Registry&;

  void add(std::string name, std::function<void()> body);
  /// 带来源文件的注册（`__FILE__`；分片按 TU 装箱用，见 `Case::file`）。
  void add(std::string name, std::function<void()> body, std::string file);
  /// 带显式软超时的注册（ms；`0` = 用默认值）。
  void add(std::string name, std::function<void()> body, std::int64_t timeout_ms);
  /// 带来源文件与软超时的注册。
  void add(std::string name, std::function<void()> body, std::string file,
           std::int64_t timeout_ms);
  /// 注册一个**慢**用例（默认不跑，`--slow` 开启）。
  void add_slow(std::string name, std::function<void()> body);
  /// 带来源文件注册慢用例。
  void add_slow(std::string name, std::function<void()> body, std::string file);
  [[nodiscard]] auto cases() -> std::vector<Case>&;

  void record_failure(std::string_view file, int line, std::string message);
  /// 运行中跳过当前用例（`ST_SKIP`）：**不算通过**（防"环境缺失被计成绿灯"），
  /// 汇总行如实报"跳过 N"。跳过与失败同时发生时失败优先（跳过不吞失败）。
  void record_skip(std::string_view reason);
  void clear_failures();
  [[nodiscard]] auto failures() const -> const std::vector<std::string>&;
  [[nodiscard]] auto skip_requested() const noexcept -> bool;
  [[nodiscard]] auto check_count() const noexcept -> std::uint64_t;
  void count_check() noexcept;
};

/// 是否包含慢用例（`--slow` / `ST_TEST_SLOW=1`）。默认 false。
void set_include_slow(bool value);
[[nodiscard]] auto include_slow() -> bool;

/// —— 用例分片（多进程并行执行，见 `--shard`）——
///
/// 全套件单进程跑时**只吃一个核**：819 例串行，CPU 27.5 s / 墙钟 33.6 s，其余核空转。
/// 分片把用例按注册顺序发到 `count` 个进程上，各自跑各自的那一份。
///
/// 为何是**分进程**而不是进程内并行：用例注册在静态初始化期完成、且共用大量进程级状态
/// （控制通道监听端口、字形与排版缓存、主题单例、`Registry` 的失败栈）。同进程并发会让
/// “哪个用例动了共享状态”变成不可诊断的偶发失败；分进程则与串行跑**语义等价**
/// （每片内用例顺序不变，片间不共享任何内存）。
///
/// 分片只解决**内存状态**的隔离；**文件系统**上的共享路径（测试产物）由用例自己按
/// `shard()` 区分目录——否则两片会同时写同一个文件。

/// 解析 `--shard i/n` 的值（`"3/8"` → `{3, 8}`）；非法写法返回 `{0, 0}`。
[[nodiscard]] auto parse_shard(std::string_view text) -> std::pair<std::size_t, std::size_t>;

/// 设置本进程的分片（`index` 从 **1** 起；`index == 0` 或 `count <= 1` = 不分片、跑全部）。
void set_shard(std::size_t index, std::size_t count);

/// 本进程的分片参数；未设置时为 `{0, 0}`。
[[nodiscard]] auto shard() -> std::pair<std::size_t, std::size_t>;

/// 分片后缀（如 `"-shard2of8"`；不分片时为空）。
/// 用途：用例写测试产物时拼进目录名，避免多片争同一个文件。
[[nodiscard]] auto shard_suffix() -> std::string;

/// 按当前分片过滤后的用例名清单（保持注册顺序，含 `slow` 用例——是否跑由 `run_all` 决定）。
/// `st test --jobs N` 用它**先算好分片大小**再启动子进程。
[[nodiscard]] auto case_names(std::string_view filter) -> std::vector<std::string>;

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
  /// 带来源文件的注册（`ST_TEST` 宏展开处自动传 `__FILE__`；分片按 TU 装箱用，
  /// 见 `Case::file`）。旧形态（无文件）仍可用——手写注册或第三方接入不受影响。
  Registrar(std::string_view name, std::function<void()> body, std::string_view file) {
    Registry::instance().add(std::string(name), std::move(body), std::string(file));
  }
  /// 带显式软超时的注册（`ST_TEST_WITH_TIMEOUT` 用）。
  Registrar(std::string_view name, std::function<void()> body, std::int64_t timeout_ms) {
    Registry::instance().add(std::string(name), std::move(body), timeout_ms);
  }
  /// 带来源文件与软超时的注册（`ST_TEST_WITH_TIMEOUT` 宏展开自动传 `__FILE__`）。
  Registrar(std::string_view name, std::function<void()> body, std::string_view file,
            std::int64_t timeout_ms) {
    Registry::instance().add(std::string(name), std::move(body), std::string(file), timeout_ms);
  }
};

/// 慢用例注册器（`ST_TEST_SLOW` 生成的静态对象调用）。
struct SlowRegistrar {
  SlowRegistrar(std::string_view name, std::function<void()> body) {
    Registry::instance().add_slow(std::string(name), std::move(body));
  }
  /// 带来源文件的注册（`ST_TEST_SLOW` 宏展开自动传 `__FILE__`）。
  SlowRegistrar(std::string_view name, std::function<void()> body, std::string_view file) {
    Registry::instance().add_slow(std::string(name), std::move(body), std::string(file));
  }
};

}  // namespace st::test

/// 定义并注册一个测试用例（函数体即用例）。
#define ST_TEST(test_name)                                                        \
  static void st_test_case_##test_name();                                         \
  namespace {                                                                     \
  const ::st::test::Registrar st_test_registrar_##test_name{                      \
      #test_name, &st_test_case_##test_name, __FILE__};                           \
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
      #test_name, &st_test_case_##test_name, __FILE__};                            \
  }                                                                               \
  static void st_test_case_##test_name()

/// 同 `ST_TEST`，但指定本用例的**软超时上限**（ms）。
///
/// 用途：**集成级**用例（真编译一个工程、真跑一个进程）——它们的耗时由外部工具决定
/// （debug 档实测 18 s），而不是被测代码的快慢。不覆盖的话它们会在测试框架里
/// 报软超时失败，把一个“框架默认值不适合这类用例”的问题误报成“被测对象有问题”。
#define ST_TEST_WITH_TIMEOUT(test_name, timeout_ms)                                \
  static void st_test_case_##test_name();                                         \
  namespace {                                                                     \
    const ::st::test::Registrar st_test_registrar_##test_name{                       \
      #test_name, &st_test_case_##test_name, __FILE__, (timeout_ms)};              \
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
///
/// **仅限 void 函数体**（用例体或 void helper）：宏体是裸 `return;`，在值返回
/// 函数里直接编译失败（`-fpermissive` 也救不了）——这是刻意的护栏而不是缺陷：
/// 值返回 helper 里的早退只退出 helper 自己，不弃跑用例，若让它编译通过反而
/// 把「半途回值」伪装成「用例结束」。需要该形态时用 `ST_REQUIRE_OR(expr, fallback)`。
#define ST_REQUIRE(expr)                                                            \
  do {                                                                              \
    ::st::test::Registry::instance().count_check();                                  \
    if (!(expr)) {                                                                   \
      ::st::test::Registry::instance().record_failure(                               \
          __FILE__, __LINE__, std::format("前置断言失败: {}", #expr));                \
      return;                                                                        \
    }                                                                                \
  } while (false)

/// 同 `ST_REQUIRE`，但用于**值返回函数**（helper/lambda）：失败时记录失败并
/// `return (fallback)`。语义：早退当前函数并由调用方给回什么——不会、也不能
/// 替用例决定弃跑（helper 的 return 只退出 helper 自己）。
///
/// 为何不回默认值而要显式 fallback：回什么由调用方定才不会把语义藏进宏里
/// （`std::string_view{}` vs `"<failed>"` vs `false` 都是合法选择，取决于
/// 调用方后续怎么消费）。
///
/// 护栏：在 void 函数里误用会直接编译失败（`return (fallback)` 在 void 里非法），
/// 与 `ST_REQUIRE` 在非 void 里的硬错互为镜像。
#define ST_REQUIRE_OR(expr, fallback)                                               \
  do {                                                                              \
    ::st::test::Registry::instance().count_check();                                  \
    if (!(expr)) {                                                                   \
      ::st::test::Registry::instance().record_failure(                               \
          __FILE__, __LINE__, std::format("前置断言失败: {}", #expr));                \
      return (fallback);                                                             \
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
///
/// **按值落地两侧**的理由同 ST_CHECK_EQ（临时对象生存期，见其注释）。
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
  do {                                                                               \
    ::st::test::Registry::instance().record_failure(                                 \
        __FILE__, __LINE__, std::format("显式失败: {}", (message)));                 \
  } while (false)

/// 跳过当前用例（不算通过、不算失败；汇总行如实报数）。**从用例内 return 不了的地方**
/// 也能用，但注意它只标记，不转移控制流——直接 `return` 跳过时也建议先调它。
/// 典型场景：环境前置不满足（框架根缺失、ffmpeg/git 未安装）——"静默跳过 + 计成
/// PASS"会把环境缺失伪装成绿灯（`ST_TEST_FRAMEWORK_ROOT` 缺失时的
/// `independent_project_builds_links_and_runs` 曾如此，BACKLOG 3903）。
#define ST_SKIP(reason)                                                              \
  do {                                                                               \
    ::st::test::Registry::instance().record_skip(reason);                            \
  } while (false)

// —— 列表断言（vector<string> 等 std::format 无 formatter 的序列）——————
//
// 为何不走「特化 std::formatter<std::vector<std::string>>」：std 命名空间模板的
// 显式特化必须依赖程序定义类型（[namespace.std]），对标准库类型特化是 UB。
// 故另立宏，格式化与差异定位提成函数（可在不弄红套件的前提下直接单测消息内容）。

namespace st::test {

/// 序列格式化成单行 `[a, b, c]`（消息里与行内差异描述统一用同一形态）。
/// 泛型版：任意元素可 format 的序列都是同一段代码；vector<string> 是最常用特例。
template <typename Sequence>
[[nodiscard]] auto format_list(const Sequence& items) -> std::string {
  std::string out{"["};
  for (const auto& item : items) {
    if (out.size() != 1) out += ", ";
    out += std::format("{}", item);
  }
  out += "]";
  return out;
}

/// 两个序列的逐元素差异描述（不含前缀）：列出首个不符项及下标，
/// 长度不同时列出双方长度；无差异返回空串。
template <typename Sequence>
[[nodiscard]] auto list_diff(const Sequence& actual, const Sequence& expected) -> std::string {
  const std::size_t common = actual.size() < expected.size() ? actual.size() : expected.size();
  for (std::size_t index = 0; index < common; ++index) {
    if (!(actual[index] == expected[index])) {
      return std::format("首个不符在 [{}]：{} != {}（actual 共 {} 项，expected 共 {} 项）",
                         index, actual[index], expected[index], actual.size(), expected.size());
    }
  }
  if (actual.size() != expected.size()) {
    return std::format("长度不同：actual {} 项，expected {} 项；公共前缀一致",
                       actual.size(), expected.size());
  }
  return {};
}

}  // namespace st::test

/// 序列相等断言（ST_CHECK_EQ 的序列版：元素级差异定位，不等时能看清差在哪）。
/// 元素类型需可 `==` 且可 `std::format`；ST_CHECK_LIST(recents, (std::vector<std::string>{"c", "b", "a"}))
/// 这种带括号的 initializer_list 实参也接（按值落地，与 ST_CHECK_EQ 同一理由）。
#define ST_CHECK_LIST(actual, expected)                                              \
  do {                                                                               \
    ::st::test::Registry::instance().count_check();                                   \
    const auto st_actual = (actual);                                                  \
    const auto st_expected = (expected);                                              \
    if (!(st_actual == st_expected)) {                                                \
      ::st::test::Registry::instance().record_failure(                                \
          __FILE__, __LINE__,                                                         \
          std::format("期望 {} == {}，实际 {} != {}；{}", #actual, #expected,           \
                      ::st::test::format_list(st_actual),                             \
                      ::st::test::format_list(st_expected),                            \
                      ::st::test::list_diff(st_actual, st_expected)));                 \
    }                                                                                \
  } while (false)
