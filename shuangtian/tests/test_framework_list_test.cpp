/// 测试框架 ST_CHECK_LIST 的回归用例：vector<string> 相等/不等断言，
/// 不等时失败消息能看清差异（首个不符下标 + 双方完整列表）。
///
/// 消息内容本身也在断言面内：格式化与差异定位提成 `st::test::format_list` /
/// `list_diff` 两个函数，就是为了这里能**不弄红套件**直接断言不等路径的消息形态
/// （宏内部只做编排——`record_failure` 一旦触发就是真失败，测消息得走函数）。
/// 手法与 ui_terminal_palette_test「直接用 record_failure 拼消息」同一思路。

#include <format>
#include <string>
#include <vector>

#include "st/test/test.hpp"

namespace {

using StringList = std::vector<std::string>;

}  // namespace

ST_TEST(list_format_joins_items_with_brackets) {
  ST_CHECK_EQ(st::test::format_list(StringList{}), std::string("[]"));
  ST_CHECK_EQ(st::test::format_list(StringList{"a"}), std::string("[a]"));
  ST_CHECK_EQ(st::test::format_list(StringList{"a", "b", "c"}), std::string("[a, b, c]"));
  // 元素本身可 format 即可（不限 string）：整型列表同一形态。
  ST_CHECK_EQ(st::test::format_list(std::vector<int>{1, 2, 3}), std::string("[1, 2, 3]"));
}

ST_TEST(list_diff_locates_first_mismatch) {
  // 首个不符项带下标 + 双方长度。
  ST_CHECK_EQ(st::test::list_diff(StringList{"a", "X", "c"}, StringList{"a", "b", "c"}),
              std::string("首个不符在 [1]：X != b（actual 共 3 项，expected 共 3 项）"));
  // 长度不同、公共前缀一致 → 报长度差异。
  ST_CHECK_EQ(st::test::list_diff(StringList{"a", "b"}, StringList{"a", "b", "c"}),
              std::string("长度不同：actual 2 项，expected 3 项；公共前缀一致"));
  // 完全一致 → 空串。
  ST_CHECK_EQ(st::test::list_diff(StringList{"a", "b"}, StringList{"a", "b"}), std::string());
  // 双空 → 空串（空列表是合法且常见的断言目标）。
  ST_CHECK_EQ(st::test::list_diff(StringList{}, StringList{}), std::string());
}

ST_TEST(check_list_passes_on_equal_lists) {
  const StringList recent = {"c:/work/a.cpp", "c:/work/中文 名.md"};
  ST_CHECK_LIST(recent, (StringList{"c:/work/a.cpp", "c:/work/中文 名.md"}));
  // 空列表相等（fail-safe 语义的常用断言面）。
  const StringList empty{};
  ST_CHECK_LIST(empty, StringList{});
}

ST_TEST(check_list_failure_message_is_assertable) {
  // 不等路径的消息形态：断言宏本身会弄红套件，所以这里只验「拼出来的消息」
  // 与 ST_CHECK_EQ 的消息格式一致（期望 X == Y，实际 A != B；差异定位）。
  const StringList actual = {"a", "X", "c"};
  const StringList expected = {"a", "b", "c"};
  ST_CHECK_EQ(std::format("期望 {} == {}，实际 {} != {}；{}", "actual", "expected",
                          st::test::format_list(actual), st::test::format_list(expected),
                          st::test::list_diff(actual, expected)),
              std::string("期望 actual == expected，实际 [a, X, c] != [a, b, c]；"
                          "首个不符在 [1]：X != b（actual 共 3 项，expected 共 3 项）"));
}

ST_TEST(check_list_accepts_braced_init_list_via_parens) {
  // ST_CHECK_LIST(v, (std::vector<std::string>{"c", "b", "a"})) 的带括号写法
  // 是推荐用法（宏实参里的花括号会被当逗号分隔的多实参）；这里钉住它可用。
  const StringList recents = {"c", "b", "a"};
  ST_CHECK_LIST(recents, (std::vector<std::string>{"c", "b", "a"}));
}

ST_TEST(check_list_captures_by_value_for_temporaries) {
  // 与 ST_CHECK_EQ 同一坑：宏体多语句，引用绑定会让实参里的临时对象在首条
  // 语句后析构（框架头注释里 ASan 实测的 stack-use-after-scope）。这里用
  // 返回临时的函数实参走一遍，断言按值落地后比较/格式化仍读得到内容。
  const auto make = [] { return StringList{"x", "y"}; };
  ST_CHECK_LIST(make(), (StringList{"x", "y"}));
}
