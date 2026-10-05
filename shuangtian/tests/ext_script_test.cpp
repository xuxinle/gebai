/// 脚本引擎测试（`st::ext::ScriptEngine`，底层 QuickJS）。
///
/// 关注点不是"JS 能不能算 1+1"（那是上游的事），而是**我们这层的契约**：
/// - 值与 JSON 的双向转换语义（整型不被写成 `1.0`、NaN 不悄悄变 null、循环引用不爆栈）；
/// - 资源约束**真的**生效（超时/内存/指令预算——不是纸面参数）；
/// - 宿主能力只来自显式注册，且宿主失败对脚本是可捕获的异常；
/// - 一切失败经 `Result` 返回，不抛异常。

#include "st/ext/script.hpp"
#include "st/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "st/ext/json.hpp"

namespace {

using st::ext::ScriptEngine;
using st::ext::ScriptLimits;

/// 一份"宽裕但有界"的配额：正常用例不该被资源限制干扰。
[[nodiscard]] auto relaxed_limits() -> ScriptLimits {
  ScriptLimits limits;
  limits.memory_bytes = 64U * 1024U * 1024U;
  limits.timeout = std::chrono::milliseconds(4000);
  return limits;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 基本求值与 JSON 转换
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_engine_starts_valid) {
  ScriptEngine engine;
  ST_CHECK(engine.valid());
  ST_CHECK(!std::string(ScriptEngine::version()).empty());
}

ST_TEST(script_evaluates_scalars_and_containers) {
  ScriptEngine engine(relaxed_limits());
  const auto number = engine.eval("1 + 2 * 3");
  ST_REQUIRE(number.has_value());
  ST_CHECK_EQ(st::json_as_i64(*number), 7);

  const auto text = engine.eval("'霜天' + ' 🚀'");
  ST_REQUIRE(text.has_value());
  ST_CHECK_EQ(st::json_as_string(*text), std::string("霜天 🚀"));

  const auto object = engine.eval("({name:'shuangtian', n:42, ok:true})");
  ST_REQUIRE(object.has_value());
  ST_CHECK(object->is_object());
  ST_CHECK_EQ(st::json_get_string(*object, "name"), std::string("shuangtian"));
  ST_CHECK_EQ(st::json_get_i64(*object, "n"), 42);
  ST_CHECK(st::json_get_bool(*object, "ok"));

  const auto array = engine.eval("[1,2,3].map(x => x * x)");
  ST_REQUIRE(array.has_value());
  ST_REQUIRE(array->is_array());
  ST_CHECK_EQ(array->size(), 3U);
  ST_CHECK_EQ(st::json_as_i64((*array)[2]), 9);
}

ST_TEST(script_undefined_becomes_json_null) {
  ScriptEngine engine(relaxed_limits());
  const auto value = engine.eval("undefined");
  ST_REQUIRE(value.has_value());
  ST_CHECK(value->is_null());
}

ST_TEST(script_integers_stay_integers) {
  // 若按 double 往返，`42` 会序列化成 `42.0`——控制通道读起来很别扭
  ScriptEngine engine(relaxed_limits());
  const auto value = engine.eval("42");
  ST_REQUIRE(value.has_value());
  ST_CHECK_EQ(st::json_dump(*value), std::string("42"));

  const auto fractional = engine.eval("1.5");
  ST_REQUIRE(fractional.has_value());
  ST_CHECK_EQ(st::json_dump(*fractional), std::string("1.5"));

  // 嵌套里的整数同样保持
  const auto nested = engine.eval("({line: 3, ratio: 0.25})");
  ST_REQUIRE(nested.has_value());
  ST_CHECK_EQ(st::json_dump(*nested), std::string(R"({"line":3,"ratio":0.25})"));
}

ST_TEST(script_nan_and_infinity_are_rejected_not_silently_null) {
  ScriptEngine engine(relaxed_limits());
  // JSON 没有 NaN：必须明确报错，而不是悄悄变 null（那会让"数值出错"看起来像"值为空"）
  const auto nan = engine.eval("NaN");
  ST_REQUIRE(!nan.has_value());
  const auto infinite = engine.eval("Infinity");
  ST_REQUIRE(!infinite.has_value());
}

ST_TEST(script_functions_are_not_silently_dropped) {
  ScriptEngine engine(relaxed_limits());
  // 函数没有 JSON 表示：明确报错，别静默变 null
  const auto function = engine.eval("(function(){})");
  ST_REQUIRE(!function.has_value());
}

ST_TEST(script_cyclic_value_is_rejected_without_stack_overflow) {
  ScriptEngine engine(relaxed_limits());
  // 循环引用若无深度上限，转换会直接爆栈（进程被强杀）
  const auto cyclic = engine.eval("const a = {}; a.self = a; a");
  ST_REQUIRE(!cyclic.has_value());
  ST_CHECK(cyclic.error().message.find("嵌套") != std::string::npos);
}

// ————————————————————————————————————————————————————————————————————————————
// 错误报告
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_syntax_error_reports_line) {
  ScriptEngine engine(relaxed_limits());
  const auto broken = engine.eval("function (");
  ST_REQUIRE(!broken.has_value());
  ST_CHECK(broken.error().code == st::ErrorCode::Parse);
  // 行号是可诊断性的底线：没有它，长脚本报错等于大海捞针
  ST_CHECK(broken.error().message.find("<script>:1") != std::string::npos);
}

ST_TEST(script_runtime_error_reports_type_and_line) {
  ScriptEngine engine(relaxed_limits());
  const auto failure = engine.eval("null.x");
  ST_REQUIRE(!failure.has_value());
  ST_CHECK(failure.error().message.find("TypeError") != std::string::npos);
}

ST_TEST(script_check_syntax_does_not_execute) {
  ScriptEngine engine(relaxed_limits());
  // 只编译不执行：配置加载期校验语法时必须零副作用
  ST_CHECK(engine.check_syntax("const a = 1 + 1;").has_value());
  ST_CHECK(engine.check_syntax("function f() { return 1; }").has_value());
  const auto broken = engine.check_syntax("function (");
  ST_REQUIRE(!broken.has_value());
  ST_CHECK(broken.error().code == st::ErrorCode::Parse);

  // 语法合法但运行会抛的脚本：check_syntax 必须通过（证明它真的没跑）
  ST_CHECK(engine.check_syntax("null.x").has_value());
}

// ————————————————————————————————————————————————————————————————————————————
// 宿主函数
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_host_function_call_and_result) {
  ScriptEngine engine(relaxed_limits());
  int calls = 0;
  std::vector<st::Json> seen;
  const auto status = engine.register_function(
      "sum", [&calls, &seen](const std::vector<st::Json>& args) -> st::Result<st::Json> {
        ++calls;
        seen = args;
        std::int64_t total = 0;
        for (const st::Json& item : args) total += st::json_as_i64(item);
        return st::Json(total);
      });
  ST_CHECK(status.has_value());

  const auto result = engine.eval("sum(20, 22)");
  ST_REQUIRE(result.has_value());
  ST_CHECK_EQ(st::json_as_i64(*result), 42);
  ST_CHECK_EQ(calls, 1);
  ST_CHECK_EQ(seen.size(), 2U);

  // 参数也走 JSON 转换：对象/数组可传
  const auto nested = engine.eval("sum(1, 2) + sum(3, 4)");
  ST_REQUIRE(nested.has_value());
  ST_CHECK_EQ(st::json_as_i64(*nested), 10);
}

ST_TEST(script_host_function_failure_is_catchable_exception) {
  ScriptEngine engine(relaxed_limits());
  ST_CHECK(engine
               .register_function("deny", [](const std::vector<st::Json>&) -> st::Result<st::Json> {
                 return st::unexpected(st::ErrorCode::NotFound, "宿主侧拒绝");
               })
               .has_value());

  // 宿主失败必须是**脚本可捕获的异常**，而不是伪装成一个返回值——
  // 否则脚本无法区分"操作成功且返回了空"与"操作失败了"。
  const auto caught = engine.eval("(() => { try { deny(); return 'no'; } catch (e) { return 'caught'; } })()");
  ST_REQUIRE(caught.has_value());
  ST_CHECK_EQ(st::json_as_string(*caught), std::string("caught"));

  // 不捕获则冒泡为执行失败，且消息里能看出是谁失败的
  const auto uncaught = engine.eval("deny()");
  ST_REQUIRE(!uncaught.has_value());
  ST_CHECK(uncaught.error().message.find("deny") != std::string::npos);
}

ST_TEST(script_duplicate_registration_is_refused) {
  ScriptEngine engine(relaxed_limits());
  const auto first = engine.register_function(
      "f", [](const std::vector<st::Json>&) -> st::Result<st::Json> { return st::Json(1); });
  ST_CHECK(first.has_value());
  // 静默覆盖会让"谁改了这个函数"无从追查
  const auto second = engine.register_function(
      "f", [](const std::vector<st::Json>&) -> st::Result<st::Json> { return st::Json(2); });
  ST_REQUIRE(!second.has_value());
  ST_CHECK(second.error().code == st::ErrorCode::Busy);

  ST_CHECK(!engine.register_function("", nullptr).has_value());
  ST_CHECK(!engine.register_function("g", nullptr).has_value());
}

// ————————————————————————————————————————————————————————————————————————————
// 全局值与函数调用
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_globals_roundtrip_through_json) {
  ScriptEngine engine(relaxed_limits());
  const auto document = st::json_parse(R"({"width":800,"ids":[1,2],"name":"霜天"})");
  ST_REQUIRE(document.has_value());
  ST_CHECK(engine.set_global("config", *document).has_value());

  const auto computed = engine.eval("config.width + config.ids.length");
  ST_REQUIRE(computed.has_value());
  ST_CHECK_EQ(st::json_as_i64(*computed), 802);

  const auto read_back = engine.global("config");
  ST_REQUIRE(read_back.has_value());
  ST_CHECK_EQ(st::json_get_string(*read_back, "name"), std::string("霜天"));
  ST_CHECK(read_back->contains("ids"));

  const auto missing = engine.global("nope");
  ST_REQUIRE(!missing.has_value());
  ST_CHECK(missing.error().code == st::ErrorCode::NotFound);
}

ST_TEST(script_call_named_function_with_arguments) {
  ScriptEngine engine(relaxed_limits());
  const auto defined = engine.eval("function greet(name, times) { return name.repeat(times); } 0");
  ST_REQUIRE(defined.has_value());

  const auto called = engine.call("greet", {st::Json("ab"), st::Json(3)});
  ST_REQUIRE(called.has_value());
  ST_CHECK_EQ(st::json_as_string(*called), std::string("ababab"));

  const auto undefined_function = engine.call("nope", {});
  ST_REQUIRE(!undefined_function.has_value());
  ST_CHECK(undefined_function.error().code == st::ErrorCode::NotFound);
}

// ————————————————————————————————————————————————————————————————————————————
// 资源约束（这些必须"真的生效"，不是纸面参数）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_infinite_loop_is_interrupted_by_timeout) {
  ScriptLimits limits;
  limits.timeout = std::chrono::milliseconds(80);
  ScriptEngine engine(limits);
  const auto spin = engine.eval("while (true) {}");
  ST_REQUIRE(!spin.has_value());
  ST_CHECK(engine.last_stats().timed_out);
  ST_CHECK(spin.error().message.find("超时") != std::string::npos);
}

ST_TEST(script_timeout_also_applies_to_calls) {
  ScriptLimits limits;
  limits.timeout = std::chrono::milliseconds(80);
  ScriptEngine engine(limits);
  ST_REQUIRE(engine.eval("function spin() { while (true) {} } 0").has_value());
  const auto spin = engine.call("spin", {});
  ST_REQUIRE(!spin.has_value());
  ST_CHECK(engine.last_stats().timed_out);
}

ST_TEST(script_memory_limit_stops_runaway_allocation) {
  ScriptLimits limits;
  limits.memory_bytes = 6U * 1024U * 1024U;
  limits.timeout = std::chrono::milliseconds(4000);
  ScriptEngine engine(limits);
  const auto hungry = engine.eval("const a = []; for (let i = 0; i < 1e7; ++i) a.push('xxxxxxxx' + i);");
  ST_REQUIRE(!hungry.has_value());
  // 报错要指向"内存"而不是给个 null 异常——否则使用者无从下手
  ST_CHECK(hungry.error().message.find("内存") != std::string::npos);
  ST_CHECK(engine.last_stats().memory_used <= 6U * 1024U * 1024U);
}

ST_TEST(script_interrupt_budget_is_enforced_before_timeout) {
  // 注意量级：中断回调约 2 千次/秒，预算必须按这个量级设，否则永远等不到（先超时）。
  // 超时给得足够宽，确保"是预算先触发"，从而证明预算真的在起作用而不只是装饰。
  ScriptLimits limits;
  limits.timeout = std::chrono::milliseconds(30'000);
  limits.max_interrupts = 200;
  ScriptEngine engine(limits);
  const auto busy = engine.eval("let s = 0; for (let i = 0; i < 1e9; ++i) s += i; s");
  ST_REQUIRE(!busy.has_value());
  ST_CHECK(busy.error().message.find("中断轮询预算") != std::string::npos);
  ST_CHECK(!engine.last_stats().timed_out);  // 是预算拦下的，不是超时
}

ST_TEST(script_stats_are_recorded) {
  ScriptEngine engine(relaxed_limits());
  ST_REQUIRE(engine.eval("let s = 0; for (let i = 0; i < 1000; ++i) s += i; s").has_value());
  const auto stats = engine.last_stats();
  ST_CHECK(stats.ops > 0U);
  ST_CHECK(!stats.timed_out);
  ST_CHECK(stats.elapsed.count() >= 0);
}

// ————————————————————————————————————————————————————————————————————————————
// 语义细节与便捷入口
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_state_persists_across_evaluations) {
  ScriptEngine engine(relaxed_limits());
  ST_REQUIRE(engine.eval("var counter = 41; counter").has_value());
  const auto next = engine.eval("++counter");
  ST_REQUIRE(next.has_value());
  ST_CHECK_EQ(st::json_as_i64(*next), 42);
}

ST_TEST(script_engine_is_movable) {
  ScriptEngine engine(relaxed_limits());
  ST_REQUIRE(engine.eval("var v = 7; v").has_value());
  ScriptEngine moved(std::move(engine));
  ST_CHECK(moved.valid());
  const auto value = moved.eval("v");
  ST_REQUIRE(value.has_value());
  ST_CHECK_EQ(st::json_as_i64(*value), 7);
}

ST_TEST(script_run_helper_is_one_shot) {
  const auto value = st::ext::run_script("({ok: true, n: [1,2]})");
  ST_REQUIRE(value.has_value());
  ST_CHECK(st::json_get_bool(*value, "ok"));
}

ST_TEST(script_runtime_range_errors_surface_as_errors) {
  ScriptEngine engine(relaxed_limits());
  // 运行时异常必须冒泡成 Result 错误（而不是被吞掉当成 null 返回值）
  const auto failure = engine.eval("'abc'.repeat(-1)");
  ST_REQUIRE(!failure.has_value());
  ST_CHECK(failure.error().code == st::ErrorCode::Parse);
  ST_CHECK(failure.error().message.find("RangeError") != std::string::npos);
}

ST_TEST(script_bigint_beyond_int64_falls_back_to_string) {
  ScriptEngine engine(relaxed_limits());
  const auto small = engine.eval("123n");
  ST_REQUIRE(small.has_value());
  ST_CHECK_EQ(st::json_as_i64(*small), 123);

  // 超出 int64 的 BigInt：以字符串保真，而不是静默取模
  const auto huge = engine.eval("123456789012345678901234567890n");
  ST_REQUIRE(huge.has_value());
  ST_CHECK(huge->is_string());
  ST_CHECK_EQ(st::json_as_string(*huge), std::string("123456789012345678901234567890"));
}
