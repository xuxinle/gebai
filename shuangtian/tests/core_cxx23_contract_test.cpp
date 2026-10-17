/// C++23 升级的**契约测试**：语言标准单一真源 + `Result` 门面 + 语言守卫。
///
/// 这组用例守的是"升级后**对外承诺**没变"，而不是"内部换了个实现"：
/// 1. 清单的 `cxx_standard` 是构建标志的唯一来源（含非法值必须报错，不得静默回落）；
/// 2. `Result<T>` 的每一个公开成员在换芯为 `std::expected` 后语义逐条不变，
///    包括**隐式构造通道**（值 → 成功、`Err` → 失败）与移动语义；
/// 3. `Result<void>` 仍是"缺省即成功"；
/// 4. 语言守卫头导出的能力常量与实际包含行为一致（不是随手写死的 true）。
///
/// 为什么这些值得单独测：`Result` 在仓内有 400+ 处使用、`Status`（`Result<void>`）
/// 3000+ 处，任何一条语义漂移都会以"某个角落的调用点行为不同"的形式出现——
/// 那种缺陷不会在编译期暴露，也不会被既有的功能用例覆盖到。

#include "st/test/test.hpp"

#include <concepts>
#include <expected>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "st/core/cxx.hpp"
#include "st/core/error.hpp"
#include "st/pkg/manifest.hpp"

namespace {

/// 构造一个"成功/失败"的最小清单文本（只测 `cxx_standard` 的解析与标志派生）。
[[nodiscard]] auto manifest_with_standard(std::string_view standard_text)
    -> st::Result<st::pkg::Manifest> {
  const std::string text = std::string(R"({"name": "probe", )") +
                           std::string(standard_text) + "}";
  return st::pkg::Manifest::parse_json(st::Json::parse(text), "/tmp/probe");
}

}  // namespace

/// ① 清单写 `cxx_standard: 23` → 标志是 `-std=c++23`（这是构建侧读的那一条）。
ST_TEST(cxx_standard_flag_follows_manifest) {
  auto manifest = manifest_with_standard(R"("cxx_standard": 23)");
  ST_REQUIRE(manifest.has_value());
  ST_CHECK_EQ(manifest->cxx_standard, 23U);
  ST_CHECK_EQ(st::pkg::Manifest::cxx_standard_flag(manifest->cxx_standard), std::string("-std=c++23"));
}

/// ② 清单没写 → 缺省就是 C++23（不是 C++20）。
/// 这条守的是"新工程的缺省口径"：缺省一旦回落，新工程会以旧标准构建而毫无提示。
ST_TEST(cxx_standard_defaults_to_23) {
  auto manifest = manifest_with_standard(R"("version": "0.1.0")");
  ST_REQUIRE(manifest.has_value());
  ST_CHECK_EQ(manifest->cxx_standard, static_cast<std::uint32_t>(st::pkg::Manifest::kDefaultCxxStandard));
  ST_CHECK_EQ(st::pkg::Manifest::kDefaultCxxStandard, 23U);
}

/// ③ 非法年号必须**报错**，不能静默取缺省。
///
/// 静默回落的后果是"清单写了 23、实际用 20 编"，而编译器不会抱怨——
/// 产物能跑，但用了 C++23 的头会以别的形式报错（或行为不同）。
ST_TEST(cxx_standard_rejects_out_of_range) {
  ST_CHECK(!manifest_with_standard(R"("cxx_standard": 17)").has_value());
  ST_CHECK(!manifest_with_standard(R"("cxx_standard": 99)").has_value());
  ST_CHECK(!manifest_with_standard(R"("cxx_standard": -1)").has_value());
  // 边界内的值都必须接受（20 是下限，26 是上限）。
  ST_CHECK(manifest_with_standard(R"("cxx_standard": 20)").has_value());
  ST_CHECK(manifest_with_standard(R"("cxx_standard": 26)").has_value());
}

/// ④ `cxx_standard` 必须**回写**进序列化结果（含等于缺省值时）。
/// 少了它，清单被另一份工具读回来就丢掉了标准语义。
ST_TEST(cxx_standard_round_trips_through_json) {
  auto manifest = manifest_with_standard(R"("cxx_standard": 23)");
  ST_REQUIRE(manifest.has_value());
  const auto json = manifest->to_json();
  ST_CHECK_EQ(st::json_get_i64(json, "cxx_standard", 0), static_cast<std::int64_t>(23));
  auto reparsed = st::pkg::Manifest::parse_json(json, "/tmp/probe");
  ST_REQUIRE(reparsed.has_value());
  ST_CHECK_EQ(reparsed->cxx_standard, 23U);
}

/// ⑤ `Result<T>` 的公开面：成功/失败态、取值、取错误、`operator bool`。
ST_TEST(result_keeps_value_and_error_semantics) {
  const auto good = []() -> st::Result<int> { return 7; }();
  ST_CHECK(good.has_value());
  ST_CHECK(static_cast<bool>(good));
  ST_CHECK_EQ(*good, 7);
  ST_CHECK_EQ(good.value(), 7);
  ST_CHECK_EQ(good.value_or(-1), 7);

  const auto bad = []() -> st::Result<int> {
    return st::unexpected(st::ErrorCode::Parse, "坏了");
  }();
  ST_CHECK(!bad.has_value());
  ST_CHECK(!static_cast<bool>(bad));
  ST_CHECK(bad.error().code == st::ErrorCode::Parse);
  ST_CHECK_EQ(bad.error().message, std::string("坏了"));
  ST_CHECK_EQ(bad.value_or(-1), -1);
}

/// ⑥ `Result<T>` 是**可移动**的容器语义：移动后仍能取值，`operator*` 可写。
///
/// 换芯为 `std::expected` 之后这条尤其值得守：移动语义由标准库实现，
/// 而本仓的调用面（`value() &&` / `operator*() &&`）是手写的转发，写错就是悬垂。
ST_TEST(result_move_preserves_value) {
  auto source = []() -> st::Result<std::string> { return std::string("霜天"); }();
  ST_REQUIRE(source.has_value());
  st::Result<std::string> moved = std::move(source);
  ST_CHECK(moved.has_value());
  ST_CHECK_EQ(moved.value(), std::string("霜天"));
  // 非 const 引用通道：`operator*` 必须可写（界面层会就地改容器里的值）。
  *moved = "已改";
  ST_CHECK_EQ(moved.value(), std::string("已改"));
}

/// ⑦ `Err` 是**唯一**的失败构造通道：`std::forward_error` 原样搬运下层错误。
ST_TEST(result_forward_error_preserves_code) {
  const auto inner = []() -> st::Result<int> {
    return st::unexpected(st::ErrorCode::Io, "磁盘满了");
  }();
  const auto outer = [&]() -> st::Result<int> {
    if (!inner) return st::forward_error(inner.error());
    return 1;
  }();
  ST_CHECK(!outer.has_value());
  ST_CHECK(outer.error().code == st::ErrorCode::Io);
  ST_CHECK_EQ(outer.error().message, std::string("磁盘满了"));
}

/// ⑧ `Result<void>`（`Status`）仍是"缺省即成功"，失败时带错误。
ST_TEST(status_default_construction_is_success) {
  const st::Status fresh;
  ST_CHECK(fresh.has_value());
  ST_CHECK(static_cast<bool>(fresh));

  const st::Status failed = st::unexpected(st::ErrorCode::Busy, "忙");
  ST_CHECK(!failed.has_value());
  ST_CHECK(failed.error().code == st::ErrorCode::Busy);
  ST_CHECK_EQ(failed.error().message, std::string("忙"));

  ST_CHECK(st::ok().has_value());
}

/// ⑨ monadic 接口（C++23 新增能力）：`and_then` 必须是「平链」，且失败态短路。
///
/// ⚠ 三条写法纪律（都是逆向验证时实测踩出来的，逐条都有代价）：
/// 1. **只观察副作用没有分辨力**：`transform` 与 `and_then` 在成功态**都**调回调、
///    在失败态**都**短路——把 `and_then` 误实现成 `transform` 时，
///    “回调被调了/没被调”两条断言**全绿**（实测就是这样假绿的）。
/// 2. 真正的区别在**返回类型**：`and_then` 是平链（`expected<int>`），
///    `transform` 会包成双层（`expected<expected<int>, E>`）。所以主要判据是类型。
/// 3. 但**不能**把 `expected` 本身交给 `ST_CHECK_EQ`——验收宏会用 `std::format` 拼
///    失败信息，而 libstdc++ 没有 `expected` 的 `formatter` 特化 ⇒ 整个单元编不过，
///    “回退变红”会红在**构建阶段**（看上去测到了，其实没跑到断言）。
///    因此类型判断用 `requires`/`same_as` 转成一个 `bool` 再断言。
ST_TEST(result_monadic_short_circuits_on_error) {
  const auto ok = []() -> st::Result<int> { return 20; }();
  // 主要的判据：`and_then` 的返回类型必须**不是**双层的（即不是 `transform` 的语义）。
  constexpr bool flat_chain = std::same_as<
      decltype(ok.and_then([](int value) -> std::expected<int, st::Error> { return value; })),
      std::expected<int, st::Error>>;
  ST_CHECK(flat_chain);

  int received = 0;
  const auto chained = ok.and_then([&received](int value) -> std::expected<int, st::Error> {
    received = value;
    return value * 2;
  });
  ST_CHECK_EQ(received, 20);  // 成功态必须把值交给回调
  // 值检查只在**平链**分支里做：回退成 `transform` 时这里返回双层 `expected`，
  // 把它交给验收宏会让整个单元编不过（见纪律 3）——而那种情况下上面那条
  // `flat_chain` 已经把它抓住了。
  if constexpr (flat_chain) {
    ST_CHECK_EQ(chained.value_or(-1), 40);
  }

  const auto failed = []() -> st::Result<int> {
    return st::unexpected(st::ErrorCode::Timeout, "超时");
  }();
  bool tail_called = false;
  const auto skipped = failed.and_then([&tail_called](int value) -> std::expected<int, st::Error> {
    tail_called = true;
    return value;
  });
  ST_CHECK(!tail_called);  // 失败态必须短路（回调不得被调）
  ST_CHECK(!skipped.has_value());
  ST_CHECK(skipped.error().code == st::ErrorCode::Timeout);

  // `transform` 的失败态短路。
  bool transform_called = false;
  const auto transformed = failed.transform([&transform_called](int value) {
    transform_called = true;
    return value + 1;
  });
  ST_CHECK(!transform_called);
  ST_CHECK(!transformed.has_value());
  ST_CHECK(transformed.error().code == st::ErrorCode::Timeout);
}

/// ⑩ `Status` 的 monadic 链：成功继续、失败短路（与主模板同口径）。
ST_TEST(status_monadic_and_then) {
  bool chained = false;
  const st::Status step = []() -> st::Status { return st::ok(); }();
  const auto next = step.and_then([&chained]() -> std::expected<void, st::Error> {
    chained = true;
    return {};
  });
  ST_CHECK(chained);
  ST_CHECK(next.has_value());

  bool failed_tail = false;
  const st::Status bad = st::unexpected(st::ErrorCode::Cancelled, "已取消");
  const auto skipped = bad.and_then([&failed_tail]() -> std::expected<void, st::Error> {
    failed_tail = true;
    return {};
  });
  ST_CHECK(!failed_tail);
  ST_CHECK(!skipped.has_value());
}

/// ⑪ 语言守卫：本翻译单元编译到这里，就证明 `-std=c++23` 生效。
///
/// 这条用例同时是**守卫本身**的证据：把 `st.pkg` 的 `cxx_standard` 改回 20，
/// 编译期会在 `st/core/cxx.hpp` 直接 `#error`，而不会悄悄用旧标准编出一份"看着正常"的产物。
ST_TEST(language_guard_reports_cxx23) {
  ST_CHECK_EQ(st::cxx_standard_in_use(), 23L);
  ST_CHECK_EQ(st::cxx_standard(), 23L);
  // 能力探针必须与实际可用性一致：`<expected>` 是本仓 Result 的内芯，不可缺。
  ST_CHECK(st::has_lib_expected());
}

/// ⑫ 能力探测常量与**实际 include 行为**一致（而不是随手写死）。
///
/// 报告的三个"能编但不该依赖"的设施，判据与 `st/core/cxx.hpp` 的注释同源：
/// - `<print>`：宏在 → 常量真，但 MinGW 链不上（所以本仓不用它）；
/// - `<generator>`：clang 目标下宏不存在 → 常量假；
/// - `<stacktrace>`：宏在但链接要额外库 → 常量真、本仓不用。
ST_TEST(capability_flags_match_macros) {
#if defined(__cpp_lib_print)
  ST_CHECK(st::has_lib_print());
#else
  ST_CHECK(!st::has_lib_print());
#endif
#if defined(__cpp_lib_generator)
  ST_CHECK(st::has_lib_generator());
#else
  ST_CHECK(!st::has_lib_generator());
#endif
#if defined(__cpp_lib_stacktrace)
  ST_CHECK(st::has_lib_stacktrace());
#else
  ST_CHECK(!st::has_lib_stacktrace());
#endif
}
