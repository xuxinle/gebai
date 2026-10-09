/// 编辑应用与格式化/重命名语义层测试（LSP 阶段 7）。

#include <string>
#include <vector>

#include "st/ext/json.hpp"
#include "st/lsp/actions.hpp"
#include "st/lsp/edit.hpp"
#include "st/test/test.hpp"

namespace {

[[nodiscard]] auto json_of(std::string_view text) -> st::Json {
  return st::json_parse(text).value();
}

/// 造一条 edit（行列都是 0 基）。
[[nodiscard]] auto make_edit(std::size_t line0, std::size_t char0, std::size_t line1,
                             std::size_t char1, std::string text) -> st::lsp::TextEdit {
  st::lsp::TextEdit edit{};
  edit.range.start = {.line = line0, .character = char0};
  edit.range.end = {.line = line1, .character = char1};
  edit.new_text = std::move(text);
  return edit;
}

}  // namespace

ST_TEST(lsp_edit_applies_back_to_front) {
  // **核心契约**：多条 edit 的区间都基于同一份原始文本——从前往后应用会让
  // 后面的偏移全错。这里用"前一处替换比原文长"来放大错误：若实现从前往后，
  // 第二处会写到错误位置。
  const std::string text = "aaa bbb ccc";
  const std::vector<st::lsp::TextEdit> edits = {
      make_edit(0, 0, 0, 3, "AAAAAAAAAA"),   // "aaa" → 10 字符
      make_edit(0, 8, 0, 11, "C"),           // "ccc" → 1 字符
  };
  std::string error;
  const auto applied = st::lsp::apply_edits(text, edits, &error);
  ST_REQUIRE(applied.has_value());
  ST_CHECK_EQ(*applied, std::string("AAAAAAAAAA bbb C"));
  ST_CHECK(error.empty());

  // 逆序输入也要得到同一结果（本层自己排序，不依赖调用方顺序）。
  const std::vector<st::lsp::TextEdit> reversed = {edits[1], edits[0]};
  const auto applied_reversed = st::lsp::apply_edits(text, reversed, nullptr);
  ST_REQUIRE(applied_reversed.has_value());
  ST_CHECK_EQ(*applied_reversed, *applied);
}

ST_TEST(lsp_edit_reports_overlap_and_bad_range) {
  const std::string text = "0123456789";
  // 重叠：两处区间相交——结果无定义，必须如实报错而不是猜。
  std::string error;
  const auto overlapped = st::lsp::apply_edits(
      text, {make_edit(0, 2, 0, 6, "X"), make_edit(0, 4, 0, 8, "Y")}, &error);
  ST_CHECK(!overlapped.has_value());
  ST_CHECK(error.find("重叠") != std::string::npos);

  // **同起点也算冲突**：`[0,4)` 与 `[0,1)` 是嵌套，两种应用顺序结果不同——
  // 挑一个"看起来合理"的会静默改坏代码，所以如实拒绝。
  const auto same_begin = st::lsp::apply_edits(
      text, {make_edit(0, 0, 0, 4, "wide"), make_edit(0, 0, 0, 1, "n")}, &error);
  ST_CHECK(!same_begin.has_value());
  ST_CHECK(error.find("重叠") != std::string::npos);

  // 紧邻（前一处终点 == 后一处起点）**不是**重叠——这是常见形态（逐行替换）。
  const auto adjacent = st::lsp::apply_edits(
      text, {make_edit(0, 0, 0, 3, "abc"), make_edit(0, 3, 0, 6, "def")}, nullptr);
  ST_REQUIRE(adjacent.has_value());
  ST_CHECK_EQ(*adjacent, std::string("abcdef6789"));

  // 起点 > 终点（协议不该出现）：如实拒绝。
  const auto reversed = st::lsp::apply_edits(
      text, {make_edit(0, 7, 0, 2, "X")}, &error);
  ST_CHECK(!reversed.has_value());
  ST_CHECK(error.find("起点晚于终点") != std::string::npos);

  // 空 edit 列表：返回原文（不是错误）。
  const auto unchanged = st::lsp::apply_edits(text, {}, nullptr);
  ST_REQUIRE(unchanged.has_value());
  ST_CHECK_EQ(*unchanged, text);
}

ST_TEST(lsp_edit_handles_multiline_and_utf16) {
  // 多行替换（格式化最常见的形态：整段重排）。
  const std::string text = "int  main( ) {\n  return 0 ;\n}\n";
  const auto formatted = st::lsp::apply_edits(
      text, {make_edit(0, 0, 2, 1, "int main() {\n  return 0;\n}")}, nullptr);
  ST_REQUIRE(formatted.has_value());
  ST_CHECK_EQ(*formatted, std::string("int main() {\n  return 0;\n}\n"));

  // UTF-16 列：中文注释后的替换点必须按码元算（不是字节、不是字符数）。
  const std::string cjk = "// 中文注释\nint x = 1;\n";
  // 第 2 行 `int x = 1;` 的 `x` 在第 0 基第 4 列。
  const auto renamed = st::lsp::apply_edits(cjk, {make_edit(1, 4, 1, 5, "value")}, nullptr);
  ST_REQUIRE(renamed.has_value());
  ST_CHECK_EQ(*renamed, std::string("// 中文注释\nint value = 1;\n"));

  // 越界位置夹取到文末（server 手里的版本可能比我们新一帧）。
  const auto clamped = st::lsp::apply_edits("abc", {make_edit(9, 9, 9, 9, "!")}, nullptr);
  ST_REQUIRE(clamped.has_value());
  ST_CHECK_EQ(*clamped, std::string("abc!"));
}

ST_TEST(lsp_edit_in_place_reports_no_change) {
  std::string text = "hello world";
  std::string error;
  // 替换成相同文本：**不改动**（调用方据此不标脏——白标脏会让撤销栈多出无效项）。
  ST_CHECK(!st::lsp::apply_edits_in_place(text, {make_edit(0, 0, 0, 5, "hello")}, &error));
  ST_CHECK_EQ(text, std::string("hello world"));
  // 真变化：写回。
  ST_CHECK(st::lsp::apply_edits_in_place(text, {make_edit(0, 6, 0, 11, "there")}, &error));
  ST_CHECK_EQ(text, std::string("hello there"));
}

ST_TEST(lsp_workspace_edit_parses_both_shapes) {
  // `documentChanges`（现代形态，带版本）。
  const auto modern = st::lsp::parse_workspace_edit(json_of(R"({
    "documentChanges": [
      {"textDocument":{"uri":"file:///a.cpp","version":7},
       "edits":[{"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":5}},
                 "newText":"renamed"}]},
      {"textDocument":{"uri":"file:///b.hpp","version":3},
       "edits":[{"range":{"start":{"line":9,"character":2},"end":{"line":9,"character":7}},
                 "newText":"renamed"}]}
    ]})"));
  ST_CHECK_EQ(modern.files.size(), std::size_t{2});
  ST_CHECK_EQ(modern.files[0].uri, std::string("file:///a.cpp"));
  ST_REQUIRE(modern.files[0].version.has_value());
  ST_CHECK_EQ(*modern.files[0].version, std::int64_t{7});
  ST_CHECK_EQ(modern.files[0].edits.size(), std::size_t{1});
  ST_CHECK_EQ(modern.files[1].edits.front().new_text, std::string("renamed"));

  // `changes`（老形态，无版本）。
  const auto legacy = st::lsp::parse_workspace_edit(json_of(R"({
    "changes": {"file:///c.cpp":[
      {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"z"}]}})"));
  ST_CHECK_EQ(legacy.files.size(), std::size_t{1});
  ST_CHECK_EQ(legacy.files.front().uri, std::string("file:///c.cpp"));
  ST_CHECK(!legacy.files.front().version.has_value());

  // 资源操作：**如实记录并跳过**（静默忽略会让"重命名文件"看起来成功）。
  const auto with_resource = st::lsp::parse_workspace_edit(json_of(R"({
    "documentChanges":[
      {"kind":"create","options":{"uri":"file:///new.cpp"}},
      {"kind":"rename","options":{"oldUri":"file:///old.cpp","newUri":"file:///renamed.cpp"}},
      {"textDocument":{"uri":"file:///a.cpp"},"edits":[]}
    ]})"));
  ST_CHECK_EQ(with_resource.skipped_resource_ops.size(), std::size_t{2});
  ST_CHECK(with_resource.skipped_resource_ops[0].find("create") != std::string::npos);
  ST_CHECK(with_resource.skipped_resource_ops[1].find("old.cpp") != std::string::npos);
  ST_CHECK_EQ(with_resource.files.size(), std::size_t{1});

  // 畸形输入不崩。
  ST_CHECK(st::lsp::parse_workspace_edit(st::Json()).files.empty());
  ST_CHECK(st::lsp::parse_workspace_edit(json_of("null")).files.empty());
}

ST_TEST(lsp_formatting_options_json) {
  st::lsp::FormattingOptions options{};
  options.tab_size = 4;
  options.insert_spaces = true;
  const auto basic = st::lsp::formatting_options_json(options);
  ST_CHECK_EQ(st::json_get_i64(basic, "tabSize"), std::int64_t{4});
  ST_CHECK(st::json_get_bool(basic, "insertSpaces"));
  // 可选字段：**给了才带**（带 false 与不带语义不同——前者是强制"不要做"）。
  ST_CHECK(!basic.contains("trimTrailingWhitespace"));
  options.trim_trailing_whitespace = true;
  options.insert_final_newline = true;
  const auto richer = st::lsp::formatting_options_json(options);
  ST_CHECK(st::json_get_bool(richer, "trimTrailingWhitespace"));
  ST_CHECK(st::json_get_bool(richer, "insertFinalNewline"));

  // 额外选项合入，但**不覆盖核心字段**（那是协议定义的，不该被 extras 改掉）。
  options.extras = json_of(R"({"tabSize":99,"clangdCustom":true})");
  const auto merged = st::lsp::formatting_options_json(options);
  ST_CHECK_EQ(st::json_get_i64(merged, "tabSize"), std::int64_t{4});
  ST_CHECK(st::json_get_bool(merged, "clangdCustom"));
}

ST_TEST(lsp_parse_formatting_edits_and_prepare_rename) {
  const auto edits = st::lsp::parse_formatting_edits(json_of(
      R"([{"range":{"start":{"line":0,"character":0},"end":{"line":2,"character":1}},"newText":"formatted"}])"));
  ST_CHECK_EQ(edits.size(), std::size_t{1});
  ST_CHECK_EQ(edits.front().new_text, std::string("formatted"));
  ST_CHECK(st::lsp::parse_formatting_edits(json_of("null")).empty());

  // `{range, placeholder}`：能拿到当前名字（预填输入框）。
  const auto titled = st::lsp::parse_prepare_rename(json_of(
      R"({"range":{"start":{"line":3,"character":4},"end":{"line":3,"character":9}},
          "placeholder":"width"})"));
  ST_REQUIRE(titled.has_value());
  ST_CHECK_EQ(titled->placeholder, std::string("width"));
  ST_CHECK_EQ(titled->range.start.line, std::size_t{3});

  // `{defaultBehavior:true}`：可以试，但没有预填（**不等于**"不可重命名"）。
  const auto defaulted = st::lsp::parse_prepare_rename(json_of(R"({"defaultBehavior":true})"));
  ST_REQUIRE(defaulted.has_value());
  ST_CHECK(defaulted->placeholder.empty());

  // **裸 `Range`**：clangd 回的就是这个形态（既无 `range` 包裹也无 `placeholder`）。
  // 漏掉它会表现为"clangd 下重命名永远说此处没有可重命名的符号"——实测踩到。
  const auto bare = st::lsp::parse_prepare_rename(json_of(
      R"({"start":{"line":7,"character":9},"end":{"line":7,"character":21}})"));
  ST_REQUIRE(bare.has_value());
  ST_CHECK_EQ(bare->range.start.line, std::size_t{7});
  ST_CHECK_EQ(bare->range.start.character, std::size_t{9});
  ST_CHECK_EQ(bare->range.end.character, std::size_t{21});
  ST_CHECK(bare->placeholder.empty());   // 没给名字：调用方留空输入框

  // `null`：明确不可重命名。
  ST_CHECK(!st::lsp::parse_prepare_rename(json_of("null")).has_value());
  ST_CHECK(!st::lsp::parse_prepare_rename(json_of("{}")).has_value());
}
