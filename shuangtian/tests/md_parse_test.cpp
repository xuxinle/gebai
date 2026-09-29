#include "st/test/test.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/string.hpp"
#include "st/md/markdown.hpp"

namespace {

[[nodiscard]] auto txt(std::string_view value) -> std::string { return std::string(value); }

[[nodiscard]] auto kind_of(const st::md::Block& block) -> std::string {
  return std::string(st::md::to_string(block.kind));
}

[[nodiscard]] auto inline_kind_of(const st::md::Inline& node) -> std::string {
  return std::string(st::md::to_string(node.kind));
}

/// 行内序列扁平为纯文本（测试断言用；语义与 UI 的文本提取一致）。
[[nodiscard]] auto plain(const std::vector<st::md::Inline>& inlines) -> std::string {
  std::string out;
  for (const st::md::Inline& node : inlines) {
    if (node.kind == st::md::InlineKind::SoftBreak || node.kind == st::md::InlineKind::HardBreak) {
      out.push_back(' ');
    } else {
      out.append(node.text);
    }
  }
  return out;
}

}  // namespace

// ① 围栏代码块（带语言与内容）
ST_TEST(md_fence_code_block) {
  const std::string_view source =
      "# 标题\n"
      "\n"
      "```python\n"
      "def add(a, b):\n"
      "    return a + b\n"
      "```\n"
      "\n"
      "尾段\n";
  const std::vector<st::md::Block> blocks = st::md::parse(source);
  ST_REQUIRE(blocks.size() == 3);
  ST_CHECK_EQ(kind_of(blocks[0]), txt("heading"));
  ST_CHECK_EQ(blocks[0].level, 1U);
  ST_CHECK_EQ(plain(blocks[0].inlines), txt("标题"));
  ST_CHECK_EQ(kind_of(blocks[1]), txt("code_block"));
  ST_CHECK_EQ(txt(blocks[1].language), txt("python"));
  ST_CHECK_EQ(txt(blocks[1].code), txt("def add(a, b):\n    return a + b\n"));
  ST_CHECK_EQ(kind_of(blocks[2]), txt("paragraph"));
  ST_CHECK_EQ(plain(blocks[2].inlines), txt("尾段"));
  // 行号（0 起）
  ST_CHECK_EQ(blocks[0].start_line, 0ULL);
  ST_CHECK_EQ(blocks[1].start_line, 2ULL);
  ST_CHECK_EQ(blocks[2].start_line, 7ULL);
}

ST_TEST(md_fence_variants_and_unclosed) {
  // ~~~ 围栏 + 信息串首词作语言
  const std::vector<st::md::Block> tilde = st::md::parse("~~~c++ title=demo\nint x = 1;\n~~~\n");
  ST_REQUIRE(tilde.size() == 1);
  ST_CHECK_EQ(kind_of(tilde[0]), txt("code_block"));
  ST_CHECK_EQ(txt(tilde[0].language), txt("c++"));
  ST_CHECK_EQ(txt(tilde[0].code), txt("int x = 1;\n"));

  // 未闭合围栏：内容为已收到部分，不报错
  const std::vector<st::md::Block> open = st::md::parse("```\nline1\nline2");
  ST_REQUIRE(open.size() == 1);
  ST_CHECK_EQ(kind_of(open[0]), txt("code_block"));
  ST_CHECK_EQ(txt(open[0].code), txt("line1\nline2"));

  // 围栏内的 ``` 行不构成块级构造（原样保留）
  const std::vector<st::md::Block> nested = st::md::parse("````\n```js\nx\n```\n````\n");
  ST_REQUIRE(nested.size() == 1);
  ST_CHECK_EQ(txt(nested[0].code), txt("```js\nx\n```\n"));

  // 缩进代码块（无语言）
  const std::vector<st::md::Block> indented = st::md::parse("    code line\n    more\n");
  ST_REQUIRE(indented.size() == 1);
  ST_CHECK_EQ(kind_of(indented[0]), txt("code_block"));
  ST_CHECK_EQ(txt(indented[0].language), txt(""));
  ST_CHECK_EQ(txt(indented[0].code), txt("code line\nmore\n"));
}

// ② 嵌套列表（无序 + 有序 + 多段项）
ST_TEST(md_nested_list) {
  const std::string_view source =
      "- 一级 A\n"
      "  - 二级 a\n"
      "  - 二级 b\n"
      "- 一级 B\n";
  const std::vector<st::md::Block> blocks = st::md::parse(source);
  ST_REQUIRE(blocks.size() == 1);
  ST_CHECK_EQ(kind_of(blocks[0]), txt("list"));
  ST_CHECK(!blocks[0].ordered);
  ST_CHECK_EQ(blocks[0].level, 0U);
  ST_REQUIRE(blocks[0].children.size() == 2);
  ST_CHECK_EQ(plain(blocks[0].children[0].inlines), txt("一级 A"));
  ST_CHECK_EQ(plain(blocks[0].children[1].inlines), txt("一级 B"));
  ST_REQUIRE(blocks[0].children[0].children.size() == 1);
  const st::md::Block& nested = blocks[0].children[0].children[0];
  ST_CHECK_EQ(kind_of(nested), txt("list"));
  ST_CHECK_EQ(nested.level, 1U);
  ST_REQUIRE(nested.children.size() == 2);
  ST_CHECK_EQ(plain(nested.children[0].inlines), txt("二级 a"));
  ST_CHECK_EQ(plain(nested.children[1].inlines), txt("二级 b"));

  // 有序列表 + 项内多段（续行缩进 4 格）
  const std::vector<st::md::Block> ordered = st::md::parse("1. first\n2. second\n\n   second 的续段\n");
  ST_REQUIRE(ordered.size() == 1);
  ST_CHECK(ordered[0].ordered);
  ST_REQUIRE(ordered[0].children.size() == 2);
  ST_CHECK_EQ(plain(ordered[0].children[1].inlines), txt("second"));
  ST_REQUIRE(ordered[0].children[1].children.size() == 1);
  ST_CHECK_EQ(kind_of(ordered[0].children[1].children[0]), txt("paragraph"));
  ST_CHECK_EQ(plain(ordered[0].children[1].children[0].inlines), txt("second 的续段"));
}

// ③ 表格（含对齐行）
ST_TEST(md_table_with_alignment) {
  const std::string_view source =
      "| 名称 | 数量 | 说明 |\n"
      "| :--- | ---: | :---: |\n"
      "| 苹果 | 3 | `code` 单元 |\n"
      "| pear | 12 | 脆的 |\n";
  const std::vector<st::md::Block> blocks = st::md::parse(source);
  ST_REQUIRE(blocks.size() == 1);
  ST_CHECK_EQ(kind_of(blocks[0]), txt("table"));
  ST_REQUIRE(blocks[0].header.size() == 3);
  ST_CHECK_EQ(txt(blocks[0].header[0]), txt("名称"));
  ST_CHECK_EQ(txt(blocks[0].header[2]), txt("说明"));
  ST_REQUIRE(blocks[0].rows.size() == 2);
  ST_CHECK_EQ(txt(blocks[0].rows[0][0]), txt("苹果"));
  ST_CHECK_EQ(txt(blocks[0].rows[0][1]), txt("3"));
  ST_CHECK_EQ(txt(blocks[0].rows[0][2]), txt("code 单元"));  // 单元格行内已扁平
  ST_CHECK_EQ(txt(blocks[0].rows[1][1]), txt("12"));
  ST_CHECK_EQ(blocks[0].start_line, 0ULL);

  // 缺列补空、多列截断；对齐行单元数不符则不识别为表格
  const std::vector<st::md::Block> ragged = st::md::parse("| a | b |\n| --- | --- |\n| 1 |\n| 2 | 3 | 4 |\n");
  ST_REQUIRE(ragged.size() == 1);
  ST_REQUIRE(ragged[0].rows.size() == 2);
  ST_CHECK_EQ(txt(ragged[0].rows[0][1]), txt(""));
  ST_CHECK_EQ(txt(ragged[0].rows[1][1]), txt("3"));

  const std::vector<st::md::Block> not_table = st::md::parse("| a | b |\n| --- |\n");
  ST_REQUIRE(not_table.size() == 1);
  ST_CHECK_EQ(kind_of(not_table[0]), txt("paragraph"));
}

// ④ 任务列表
ST_TEST(md_task_list) {
  const std::string_view source =
      "- [x] 已完成\n"
      "- [ ] 未完成\n"
      "- 普通项\n";
  const std::vector<st::md::Block> blocks = st::md::parse(source);
  ST_REQUIRE(blocks.size() == 1);
  ST_REQUIRE(blocks[0].children.size() == 3);
  ST_CHECK(blocks[0].children[0].task_item);
  ST_CHECK(blocks[0].children[0].checked);
  ST_CHECK_EQ(plain(blocks[0].children[0].inlines), txt("已完成"));
  ST_CHECK(blocks[0].children[1].task_item);
  ST_CHECK(!blocks[0].children[1].checked);
  ST_CHECK_EQ(plain(blocks[0].children[1].inlines), txt("未完成"));
  ST_CHECK(!blocks[0].children[2].task_item);
  ST_CHECK_EQ(plain(blocks[0].children[2].inlines), txt("普通项"));
}

// ⑤ 行内：强调 / 行内码 / 链接 / 图片 / 删除线 / 自动链接
ST_TEST(md_inline_emphasis_code_link) {
  const std::string_view source =
      "这是 **加粗** 与 *斜体* 与 `code span` 与 ~~删除~~ 与 [链接](https://example.com \"标题\") 与 ![图](a.png)\n";
  const std::vector<st::md::Block> blocks = st::md::parse(source);
  ST_REQUIRE(blocks.size() == 1);
  const std::vector<st::md::Inline>& inlines = blocks[0].inlines;
  ST_REQUIRE(inlines.size() == 12);
  ST_CHECK_EQ(inline_kind_of(inlines[0]), txt("text"));
  ST_CHECK_EQ(inline_kind_of(inlines[1]), txt("strong"));
  ST_CHECK_EQ(txt(inlines[1].text), txt("加粗"));
  ST_CHECK_EQ(inline_kind_of(inlines[3]), txt("em"));
  ST_CHECK_EQ(txt(inlines[3].text), txt("斜体"));
  ST_CHECK_EQ(inline_kind_of(inlines[5]), txt("code"));
  ST_CHECK_EQ(txt(inlines[5].text), txt("code span"));
  ST_CHECK_EQ(inline_kind_of(inlines[7]), txt("del"));
  ST_CHECK_EQ(txt(inlines[7].text), txt("删除"));
  ST_CHECK_EQ(inline_kind_of(inlines[9]), txt("link"));
  ST_CHECK_EQ(txt(inlines[9].text), txt("链接"));
  ST_CHECK_EQ(txt(inlines[9].url), txt("https://example.com"));
  ST_CHECK_EQ(txt(inlines[9].title), txt("标题"));
  ST_CHECK_EQ(inline_kind_of(inlines[11]), txt("image"));
  ST_CHECK_EQ(txt(inlines[11].text), txt("图"));
  ST_CHECK_EQ(txt(inlines[11].url), txt("a.png"));
}

ST_TEST(md_inline_escapes_breaks_autolinks) {
  // 转义字符：\* 不构成强调；snake_case 中的 _ 不构成强调
  const std::vector<st::md::Block> escaped = st::md::parse("\\*不是强调\\* 与 \\_下划线\\_ 与 snake_case_name\n");
  ST_REQUIRE(escaped.size() == 1);
  ST_REQUIRE(escaped[0].inlines.size() == 1);
  ST_CHECK_EQ(inline_kind_of(escaped[0].inlines[0]), txt("text"));
  ST_CHECK_EQ(plain(escaped[0].inlines), txt("*不是强调* 与 _下划线_ 与 snake_case_name"));

  // 软换行 / 硬换行（行尾两空格）
  const std::vector<st::md::Block> breaks = st::md::parse("第一行\n第二行  \n第三行\n");
  ST_REQUIRE(breaks.size() == 1);
  ST_REQUIRE(breaks[0].inlines.size() == 5);
  ST_CHECK_EQ(inline_kind_of(breaks[0].inlines[1]), txt("soft_break"));
  ST_CHECK_EQ(inline_kind_of(breaks[0].inlines[3]), txt("hard_break"));

  // 自动链接（尖括号 + 裸 URL）
  const std::vector<st::md::Block> auto_links = st::md::parse("见 <https://st.dev/a> 与 https://st.dev/b。\n");
  ST_REQUIRE(auto_links.size() == 1);
  const std::vector<st::md::Inline>& nodes = auto_links[0].inlines;
  ST_REQUIRE(nodes.size() == 5);
  ST_CHECK_EQ(inline_kind_of(nodes[1]), txt("link"));
  ST_CHECK_EQ(txt(nodes[1].url), txt("https://st.dev/a"));
  ST_CHECK_EQ(inline_kind_of(nodes[3]), txt("link"));
  ST_CHECK_EQ(txt(nodes[3].url), txt("https://st.dev/b"));  // 中文句号不入链接
}

// ⑥ 中文与 emoji 的 UTF-8 完整性
ST_TEST(md_utf8_chinese_and_emoji) {
  const std::string_view source =
      "# 中文标题 🎉\n"
      "\n"
      "汉字与 emoji 😀🚀 混排，含全角标点：测试。\n"
      "\n"
      "- 列表项 ✅ 完成\n";
  const std::vector<st::md::Block> blocks = st::md::parse(source);
  ST_REQUIRE(blocks.size() == 3);
  const std::string heading_text = plain(blocks[0].inlines);
  ST_CHECK_EQ(heading_text, txt("中文标题 🎉"));
  ST_CHECK(st::utf8_is_valid(heading_text));
  ST_CHECK_EQ(st::utf8_length(heading_text), std::size_t{6});  // 4 汉字 + 空格 + 1 emoji

  const std::string paragraph_text = plain(blocks[1].inlines);
  ST_CHECK(st::utf8_is_valid(paragraph_text));
  ST_CHECK(paragraph_text.find("😀🚀") != std::string::npos);
  ST_CHECK(paragraph_text.find("测试。") != std::string::npos);

  ST_REQUIRE(blocks[1].children.empty());  const std::vector<st::md::Block> list_only = st::md::parse("- 列表项 ✅ 完成\n");
  ST_REQUIRE(list_only.size() == 1);
  ST_REQUIRE(list_only[0].children.size() == 1);
  const std::string item_text = plain(list_only[0].children[0].inlines);
  ST_CHECK_EQ(item_text, txt("列表项 ✅ 完成"));
  ST_CHECK(st::utf8_is_valid(item_text));
}

// 其它块级：引用（嵌套）、分隔线、HTML 块、CRLF、超长行
ST_TEST(md_blocks_quote_divider_html_crlf) {
  const std::vector<st::md::Block> quote = st::md::parse("> 引文\n> \n> - 项\n");
  ST_REQUIRE(quote.size() == 1);
  ST_CHECK_EQ(kind_of(quote[0]), txt("quote"));
  ST_REQUIRE(quote[0].children.size() == 2);
  ST_CHECK_EQ(kind_of(quote[0].children[0]), txt("paragraph"));
  ST_CHECK_EQ(kind_of(quote[0].children[1]), txt("list"));

  const std::vector<st::md::Block> nested_quote = st::md::parse("> 外层\n> > 内层\n");
  ST_REQUIRE(nested_quote.size() == 1);
  ST_REQUIRE(nested_quote[0].children.size() == 2);
  ST_CHECK_EQ(kind_of(nested_quote[0].children[1]), txt("quote"));

  const std::vector<st::md::Block> dividers = st::md::parse("---\n\n***\n\n- - -\n");
  ST_REQUIRE(dividers.size() == 3);
  ST_CHECK_EQ(kind_of(dividers[0]), txt("divider"));
  ST_CHECK_EQ(kind_of(dividers[1]), txt("divider"));
  ST_CHECK_EQ(kind_of(dividers[2]), txt("divider"));

  const std::vector<st::md::Block> html = st::md::parse("<div class=\"x\">\nhello\n</div>\n");
  ST_REQUIRE(html.size() == 1);
  ST_CHECK_EQ(kind_of(html[0]), txt("html_block"));
  ST_CHECK_EQ(txt(html[0].code), txt("<div class=\"x\">\nhello\n</div>\n"));

  // CRLF：`\r` 不得进入文本
  const std::vector<st::md::Block> crlf = st::md::parse("第一行\r\n第二行\r\n\r\n# 标题\r\n");
  ST_REQUIRE(crlf.size() == 2);
  ST_CHECK_EQ(kind_of(crlf[0]), txt("paragraph"));
  ST_CHECK_EQ(plain(crlf[0].inlines), txt("第一行 第二行"));
  ST_CHECK_EQ(kind_of(crlf[1]), txt("heading"));

  // 超长行（8 KiB）不丢失、不错位
  std::string long_line;
  long_line.reserve(9000);
  for (std::size_t index = 0; index < 2048; ++index) long_line.append("汉字");
  const std::vector<st::md::Block> long_blocks = st::md::parse(long_line);
  ST_REQUIRE(long_blocks.size() == 1);
  ST_CHECK_EQ(plain(long_blocks[0].inlines), long_line);
  ST_CHECK(st::utf8_is_valid(plain(long_blocks[0].inlines)));

  // 空输入与纯空白
  ST_CHECK(st::md::parse("").empty());
  ST_CHECK(st::md::parse("\n\n   \n").empty());
}

ST_TEST(md_inline_unclosed_fallbacks) {
  // 未闭合强调/行内码/链接退化为字面文本，不产生半个节点
  const std::vector<st::md::Block> blocks = st::md::parse("**未闭合 与 `未闭合 与 [未闭合]( 与 ~~未闭合\n");
  ST_REQUIRE(blocks.size() == 1);
  for (const st::md::Inline& node : blocks[0].inlines) {
    ST_CHECK_EQ(inline_kind_of(node), txt("text"));
  }
  ST_CHECK_EQ(plain(blocks[0].inlines), txt("**未闭合 与 `未闭合 与 [未闭合]( 与 ~~未闭合"));

  // 强调内层嵌套行内码：扁平化（行内模型为平铺序列）
  const std::vector<st::md::Block> nested = st::md::parse("**加粗含 `code`**\n");
  ST_REQUIRE(nested.size() == 1);
  ST_REQUIRE(nested[0].inlines.size() == 1);
  ST_CHECK_EQ(inline_kind_of(nested[0].inlines[0]), txt("strong"));
  ST_CHECK_EQ(txt(nested[0].inlines[0].text), txt("加粗含 code"));
}
