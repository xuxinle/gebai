/// JSON 层测试（`st::ext::json`，底层 nlohmann/json）。
///
/// 覆盖两类关注点：
/// 1. **保真**——这正是替换自研实现的动因（旧实现数字统一存 `double`，2^53+1 与雪花 ID 失真）；
/// 2. **契约**——解析/读取不得抛异常（框架用 `Result`）、深度上限（防栈溢出）、
///    错误信息带位置、键序保持（清单/lock 文件可 diff）。
///
/// 注：不测试 nlohmann 自身（那是上游的事），只测**我们这层适配的语义承诺**。

#include "st/core/fs.hpp"
#include "st/ext/json.hpp"
#include "st/test/test.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using st::Json;

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 保真：整数与浮点
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(json_int64_beyond_double_precision_roundtrips) {
  // 旧实现把 9007199254740993 写成 9007199254740992（2^53 精度墙）
  const auto parsed = st::json_parse("9007199254740993");
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_dump(*parsed), std::string("9007199254740993"));

  // 雪花 ID / 纳秒时间戳这类 19 位整数必须逐位保真
  const auto snowflake = st::json_parse("1234567890123456789");
  ST_REQUIRE(snowflake.has_value());
  ST_CHECK_EQ(st::json_dump(*snowflake), std::string("1234567890123456789"));
}

ST_TEST(json_uint64_max_roundtrips) {
  const std::string maximum = std::to_string(std::numeric_limits<std::uint64_t>::max());
  const auto parsed = st::json_parse(maximum);
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_dump(*parsed), maximum);
}

ST_TEST(json_int64_min_roundtrips) {
  const std::string minimum = std::to_string(std::numeric_limits<std::int64_t>::min());
  const auto parsed = st::json_parse(minimum);
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_dump(*parsed), minimum);
}

ST_TEST(json_float_semantics_survive) {
  // 旧实现把 1.0 吞成 1、1e3 规范化成 1000、-0 变 0——整型/浮点语义不保真
  const auto one_point_zero = st::json_parse("1.0");
  ST_REQUIRE(one_point_zero.has_value());
  ST_CHECK(one_point_zero->is_number_float());
  ST_CHECK_EQ(st::json_dump(*one_point_zero), std::string("1.0"));

  const auto exponent = st::json_parse("1e3");
  ST_REQUIRE(exponent.has_value());
  ST_CHECK(exponent->is_number_float());  // 指数记法仍是浮点
  ST_CHECK_NEAR(st::json_as_double(*exponent), 1000.0, 1e-9);

  const auto half = st::json_parse("0.1");
  ST_REQUIRE(half.has_value());
  ST_CHECK_NEAR(st::json_as_double(*half), 0.1, 1e-12);
}

ST_TEST(json_large_object_id_field_keeps_precision) {
  const auto parsed = st::json_parse(R"({"id":1234567890123456789,"name":"霜天"})");
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_get_i64(*parsed, "id"), static_cast<std::int64_t>(1234567890123456789LL));
  ST_CHECK_EQ(st::json_get_string(*parsed, "name"), std::string("霜天"));
  // 序列化后仍是原值（不是 1.2345678901234568e+18）
  ST_CHECK(st::json_dump(*parsed).find("1234567890123456789") != std::string::npos);
}

// ————————————————————————————————————————————————————————————————————————————
// 保真：文本与转义
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(json_utf8_and_surrogate_pairs_roundtrip) {
  const auto parsed = st::json_parse(R"("\u4E2D\u6587 \uD83D\uDE00")");
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_as_string(*parsed), std::string("中文 😀"));

  // 直接写 UTF-8 也一样
  const auto direct = st::json_parse(R"("中文 😀")");
  ST_REQUIRE(direct.has_value());
  ST_CHECK_EQ(st::json_as_string(*direct), std::string("中文 😀"));
}

ST_TEST(json_control_characters_escape_roundtrip) {
  const auto parsed = st::json_parse(R"("a\nb\tc\"d\\e")");
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_as_string(*parsed), std::string("a\nb\tc\"d\\e"));
  // 再序列化必须重新转义（换行不能原样进 JSON）
  const std::string dumped = st::json_dump(*parsed);
  ST_CHECK(dumped.find('\n') == std::string::npos);
  ST_CHECK(dumped.find("\\n") != std::string::npos);
}

ST_TEST(json_serialization_replaces_invalid_utf8_without_throwing) {
  // 序列化路径必须"无论内存里是什么字节都能出结果"——控制通道回包不能被坏串打断
  Json value = Json::object();
  value["bad"] = std::string("ok\xFF\xFE bad");  // 非法 UTF-8 序列
  const std::string dumped = st::json_dump(value);  // 不得抛异常
  ST_CHECK(dumped.find("bad") != std::string::npos);
}

ST_TEST(json_key_order_is_preserved) {
  // 清单/lock 是给人读、被 git diff 的产物：键序稳定比字典序更重要
  const auto parsed = st::json_parse(R"({"z":1,"a":2,"m":3})");
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_dump(*parsed), std::string(R"({"z":1,"a":2,"m":3})"));
}

ST_TEST(json_roundtrip_of_nested_document) {
  const std::string source =
      R"({"name":"shuangtian","targets":{"app":{"kind":"executable","sources":["a.cpp"]}},)"
      R"("nums":[1,2,3],"flags":{"debug":true,"none":null}})";
  const auto parsed = st::json_parse(source);
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_dump(*parsed), source);  // 紧凑输出逐字节相同
}

// ————————————————————————————————————————————————————————————————————————————
// 契约：解析错误与安全上限
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(json_parse_error_reports_position) {
  const auto broken = st::json_parse(R"({"a": })");
  ST_REQUIRE(!broken.has_value());
  ST_CHECK(broken.error().code == st::ErrorCode::Parse);
  // 必须给出可诊断的位置信息（旧实现给字节偏移；现在带字节 + 行列）
  ST_CHECK(broken.error().message.find("字节") != std::string::npos);
}

ST_TEST(json_deep_nesting_is_rejected_not_crashed) {
  // 极深嵌套会让递归下降解析器爆栈——那是"进程被杀"，不是"拒绝畸形输入"，必须前置拦下
  const auto just_over = [](std::size_t depth) {
    std::string text(depth, '[');
    text.append(depth, ']');
    return text;
  };
  const auto accepted = st::json_parse(just_over(st::kJsonMaxDepth));
  ST_CHECK(accepted.has_value());  // 上限内接受

  const auto rejected = st::json_parse(just_over(st::kJsonMaxDepth + 8));
  ST_REQUIRE(!rejected.has_value());
  ST_CHECK(rejected.error().code == st::ErrorCode::Parse);
  ST_CHECK(rejected.error().message.find("嵌套") != std::string::npos);
}

ST_TEST(json_depth_check_ignores_brackets_inside_strings) {
  // 字符串里的方括号不是嵌套——预检必须跳过字符串与转义
  std::string text = "\"";
  text.append(500, '[');
  text.append("\"");
  const auto parsed = st::json_parse(text);
  ST_CHECK(parsed.has_value());
}

ST_TEST(json_malformed_inputs_never_throw) {
  for (const std::string_view broken : {"", "{", "[", "{\"a\"}", "tru", "01", "\"unterminated",
                                        "{'single':1}", "[1,2,]", "{\"a\":1,}", "nan"}) {
    const auto parsed = st::json_parse(broken);
    ST_CHECK(!parsed.has_value());  // 一律以错误返回，绝不抛异常
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 契约：安全读取（类型不符退化为默认值，不抛）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(json_accessors_degrade_to_defaults) {
  const auto parsed = st::json_parse(R"({"num":42,"text":"hi","flag":true,"list":["a","b","c"]})");
  ST_REQUIRE(parsed.has_value());
  const Json& doc = *parsed;

  ST_CHECK_EQ(st::json_get_i64(doc, "num"), 42);
  ST_CHECK_EQ(st::json_get_string(doc, "text"), std::string("hi"));
  ST_CHECK(st::json_get_bool(doc, "flag"));

  // 键缺失
  ST_CHECK_EQ(st::json_get_i64(doc, "missing", -7), -7);
  ST_CHECK_EQ(st::json_get_string(doc, "missing", "fallback"), std::string("fallback"));
  ST_CHECK(!st::json_get_bool(doc, "missing"));

  // 类型不符：不得抛异常，取默认值
  ST_CHECK_EQ(st::json_get_i64(doc, "text", -1), -1);      // 字符串不是整数
  ST_CHECK_EQ(st::json_get_string(doc, "num", "d"), std::string("42"));  // 数字可按文本读
  ST_CHECK_EQ(st::json_get_i64(doc, "list", -1), -1);      // 数组不是整数

  // 非对象节点上读取
  ST_CHECK_EQ(st::json_get_i64(*parsed, "num"), 42);
  const auto scalar = st::json_parse("7");
  ST_REQUIRE(scalar.has_value());
  ST_CHECK_EQ(st::json_get_i64(*scalar, "anything", -1), -1);

  // 指针比较走布尔断言：`ST_CHECK_EQ` 需要可格式化类型，裸指针进不了 std::format
  ST_CHECK(st::json_find(doc, "num") == &st::json_at(doc, "num"));
  ST_CHECK(st::json_find(doc, "missing") == nullptr);
  ST_CHECK(st::json_at(doc, "missing").is_null());
}

ST_TEST(json_string_array_extraction_filters_non_strings) {
  const auto parsed = st::json_parse(R"({"a":["x","y"],"mixed":["ok",1,null,"fine"],"scalar":"z"})");
  ST_REQUIRE(parsed.has_value());
  const auto plain = st::json_get_string_array(*parsed, "a");
  ST_CHECK_EQ(plain.size(), 2U);
  const auto mixed = st::json_get_string_array(*parsed, "mixed");
  ST_CHECK_EQ(mixed.size(), 2U);  // 只取字符串元素
  ST_CHECK_EQ(mixed[1], std::string("fine"));
  ST_CHECK(st::json_get_string_array(*parsed, "scalar").empty());  // 非数组 → 空
  ST_CHECK(st::json_get_string_array(*parsed, "missing").empty());
}

ST_TEST(json_path_navigation) {
  const auto parsed = st::json_parse(R"({"spec":{"source":{"kind":"git","location":"u"}}})");
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_as_string(st::json_path(*parsed, "spec.source.kind")), std::string("git"));
  // 任一层缺失 → 返回 null 节点，不抛
  ST_CHECK(st::json_path(*parsed, "spec.missing.kind").is_null());
  ST_CHECK(st::json_path(*parsed, "spec.source.location.deep").is_null());
}

ST_TEST(json_float_to_int_conversion_refuses_silent_truncation) {
  const auto parsed = st::json_parse(R"({"whole":7.0,"fraction":7.5})");
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(st::json_get_i64(*parsed, "whole"), 7);       // 无小数部分 → 可转
  ST_CHECK_EQ(st::json_get_i64(*parsed, "fraction", -1), -1);  // 有小数 → 拒绝，别静默截断
}

// ————————————————————————————————————————————————————————————————————————————
// 契约：构造与文件读写
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(json_construction_is_object_by_default_usage) {
  // nlohmann 的花括号是"造数组"信号：`Json x{Json::object()}` 会得到 [{}]。
  // 这条用例锁住正确写法，防止后来者踩坑（框架内已因它出过一次 type_error.305）。
  Json as_copy = Json::object();
  ST_CHECK(as_copy.is_object());

  const Json* found = st::json_find(as_copy, "anything");
  ST_CHECK(found == nullptr);
  as_copy["k"] = 1;
  ST_CHECK(as_copy.is_object());
  ST_CHECK_EQ(st::json_get_i64(as_copy, "k"), 1);
}

ST_TEST(json_file_roundtrip) {
  const std::string path = "ext_json_test_tmp.json";
  Json document = Json::object();
  document["name"] = "霜天";
  document["id"] = static_cast<std::int64_t>(9007199254740993LL);
  document["list"] = Json::array({1, 2, 3});

  ST_CHECK(st::json_write_file(path, document, true).has_value());
  const auto loaded = st::json_parse_file(path);
  ST_REQUIRE(loaded.has_value());
  ST_CHECK_EQ(st::json_get_string(*loaded, "name"), std::string("霜天"));
  ST_CHECK_EQ(st::json_get_i64(*loaded, "id"), 9007199254740993LL);
  ST_CHECK_EQ(st::json_get_string(*loaded, "list").empty(), true);  // 不是字符串
  ST_CHECK(loaded->contains("list"));

  const auto missing = st::json_parse_file("definitely_missing_file.json");
  ST_REQUIRE(!missing.has_value());
  ST_CHECK(missing.error().code == st::ErrorCode::NotFound);

  // 测试不留垃圾：临时文件必须自己收拾（否则会被 `git add -A` 收进仓库）
  ST_CHECK(st::fs::remove_file(path).has_value());
}
