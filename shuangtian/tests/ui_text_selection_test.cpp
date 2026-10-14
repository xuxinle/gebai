// 文本选择公共算法（`components_internal.hpp` 的选择段）的单元测试。
//
// 为什么单独测这套算法：三个组件（Terminal/Input/Text）都建在它上面，而它是
// **纯函数**——可以在没有界面、没有文本端口的情况下把边界条件钉死。组件级测试
// 只能覆盖到"某一条路径"，边界（UTF-8 多字节、反向拖选、空区间、行尾裁空白）
// 在这里才测得干净。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/test/test.hpp"

#include "../src/ui/components/components_internal.hpp"

namespace {

using st::ui::components_internal::Caret;
using st::ui::components_internal::compare_caret;
using st::ui::components_internal::nearest_codepoint;
using st::ui::components_internal::normalize_selection;
using st::ui::components_internal::slice_line;
using st::ui::components_internal::word_range_at;

/// 构造一个等宽前缀表：`count` 个码点，每个宽 `advance`。
[[nodiscard]] auto uniform_prefix(std::size_t count, float advance) -> std::vector<float> {
  std::vector<float> prefix(count + 1U, 0.0f);
  for (std::size_t i = 1U; i <= count; ++i) prefix[i] = prefix[i - 1U] + advance;
  return prefix;
}

}  // namespace

ST_TEST(text_selection_compare_and_normalize) {
  // 端点序：先比行、行同比偏移——三方比较（含相等）。
  ST_CHECK(compare_caret(Caret{0, 0}, Caret{0, 5}) < 0);
  ST_CHECK(compare_caret(Caret{0, 5}, Caret{1, 0}) < 0);
  ST_CHECK(compare_caret(Caret{2, 3}, Caret{2, 3}) == 0);
  ST_CHECK(compare_caret(Caret{3, 0}, Caret{1, 9}) > 0);

  // 归一化：反向拖选（focus 在 anchor 之前）得到同一区间。
  const auto forward = normalize_selection(Caret{1, 2}, Caret{4, 6});
  const auto backward = normalize_selection(Caret{4, 6}, Caret{1, 2});
  ST_CHECK(forward.begin == backward.begin);
  ST_CHECK(forward.end == backward.end);
  ST_CHECK(forward.begin == (Caret{1, 2}));
  ST_CHECK(forward.end == (Caret{4, 6}));
  // 同一端点 = 空选择。
  ST_CHECK(normalize_selection(Caret{2, 2}, Caret{2, 2}).empty());
}

ST_TEST(text_selection_range_span_on_line) {
  const auto range = normalize_selection(Caret{1, 3}, Caret{3, 2});
  // 区间之外的行 → 空
  ST_CHECK(range.span_on_line(0, 10U) == std::make_pair(std::size_t{0}, std::size_t{0}));
  ST_CHECK(range.span_on_line(4, 10U) == std::make_pair(std::size_t{0}, std::size_t{0}));
  // 首行：从锚点偏移到行尾
  ST_CHECK(range.span_on_line(1, 10U) == std::make_pair(std::size_t{3}, std::size_t{10}));
  // 中间行：整行
  ST_CHECK(range.span_on_line(2, 7U) == std::make_pair(std::size_t{0}, std::size_t{7}));
  // 末行：从行首到端点偏移
  ST_CHECK(range.span_on_line(3, 9U) == std::make_pair(std::size_t{0}, std::size_t{2}));
  // 单行区间：两端都收在该行内
  const auto single = normalize_selection(Caret{5, 2}, Caret{5, 8});
  ST_CHECK(single.span_on_line(5, 20U) == std::make_pair(std::size_t{2}, std::size_t{8}));
}

ST_TEST(text_selection_nearest_codepoint) {
  const std::vector<float> prefix = uniform_prefix(4U, 10.0f);   // 边界 0,10,20,30,40
  ST_CHECK_EQ(nearest_codepoint(prefix, -5.0f), 0U);    // 左侧越界 → 行首
  ST_CHECK_EQ(nearest_codepoint(prefix, 0.0f), 0U);     // 正落在边界上
  ST_CHECK_EQ(nearest_codepoint(prefix, 4.0f), 0U);     // 靠左端更近
  ST_CHECK_EQ(nearest_codepoint(prefix, 6.0f), 1U);     // 靠右端更近
  ST_CHECK_EQ(nearest_codepoint(prefix, 9.0f), 1U);   // 偏左（边界 10）
  ST_CHECK_EQ(nearest_codepoint(prefix, 14.0f), 1U);  // 偏左
  ST_CHECK_EQ(nearest_codepoint(prefix, 15.0f), 1U);  // 等距：约定取**左**（与实现一致）
  ST_CHECK_EQ(nearest_codepoint(prefix, 16.0f), 2U);  // 偏右
  ST_CHECK_EQ(nearest_codepoint(prefix, 40.0f), 4U);    // 行尾
  ST_CHECK_EQ(nearest_codepoint(prefix, 999.0f), 4U);   // 右侧越界 → 行尾
  // 空前缀表（无文本行）→ 0，不崩。
  ST_CHECK_EQ(nearest_codepoint({}, 12.0f), 0U);
  ST_CHECK_EQ(nearest_codepoint({0.0f}, 12.0f), 0U);
}

ST_TEST(text_selection_word_range_ascii) {
  const std::string text = "foo bar_baz 42 qux";
  // 词中 → 取整词
  auto range = word_range_at(text, 1U);             // 在 "foo" 内
  ST_CHECK_EQ(text.substr(range.first, range.second - range.first), std::string("foo"));
  // 下划线与数字都算词字符
  range = word_range_at(text, 6U);                  // 在 "bar_baz" 内
  ST_CHECK_EQ(text.substr(range.first, range.second - range.first), std::string("bar_baz"));
  range = word_range_at(text, 12U);                 // 在 "42" 内（下标 12/13）
  ST_CHECK_EQ(text.substr(range.first, range.second - range.first), std::string("42"));
  // 落在空格上（下标 14）→ 跳到右词（与浏览器行为一致）
  range = word_range_at(text, 14U);
  ST_CHECK_EQ(text.substr(range.first, range.second - range.first), std::string("qux"));
  // 末尾词
  range = word_range_at(text, 17U);
  ST_CHECK_EQ(text.substr(range.first, range.second - range.first), std::string("qux"));
}

ST_TEST(text_selection_word_range_utf8) {
  // 中文连跑取整段；UTF-8 多字节不能切在码点中间。
  const std::string text = "前缀 汉字词组 后缀";
  const std::size_t han = text.find("汉字词组");
  ST_REQUIRE(han != std::string::npos);
  const auto range = word_range_at(text, han + 1U);   // 落在词中间（可能非码点边界）
  ST_CHECK_EQ(text.substr(range.first, range.second - range.first), std::string("汉字词组"));
  // 结果必须落在码点边界上（否则会切出半个汉字）。
  ST_CHECK(st::utf8_is_valid(text.substr(range.first, range.second - range.first)));
  // 日文/emoji 之类的多字节也应被当作词字符（非 ASCII 归为一类）。
  const std::string mixed = "abc 日本語 xyz";
  const std::size_t jp = mixed.find("日本語");
  const auto jp_range = word_range_at(mixed, jp);
  ST_CHECK_EQ(mixed.substr(jp_range.first, jp_range.second - jp_range.first),
              std::string("日本語"));
}

ST_TEST(text_selection_slice_line) {
  const std::string line = "hello world   ";
  // 不裁空白：原样切片（下标 6 才是 'w'）
  ST_CHECK_EQ(slice_line(line, 6U, 11U), std::string("world"));
  // 裁**行尾**空白（供等宽网格复制用：终端行尾大量填充空格）
  ST_CHECK_EQ(slice_line(line, 6U, line.size(), true), std::string("world"));
  // 行首空白**不裁**（它是内容的一部分；终端里缩进有意义）
  ST_CHECK_EQ(slice_line(line, 5U, 11U, true), std::string(" world"));
  // 越界安全（from/to 超出长度）
  ST_CHECK_EQ(slice_line(line, 900U, 999U), std::string{});
  ST_CHECK_EQ(slice_line(line, 3U, 1U), std::string{});   // 反向 → 空（调用方应已归一化）
  ST_CHECK_EQ(slice_line(line, 0U, line.size()), line);
}
