/// 语法高亮引擎测试：内置语言、自定义语言注册、别名/扩展名解析、边界与不变式。

#include "st/test/test.hpp"
#include "st/text/highlight.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

using st::text::LanguageRegistry;
using st::text::LanguageSpec;
using st::text::Token;
using st::text::TokenKind;

/// 取某类别覆盖的文本片段（便于断言"这段是注释"而不关心具体字节偏移）。
[[nodiscard]] auto spans_of(std::string_view code, std::string_view language, TokenKind kind)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const Token& token : st::text::highlight(code, language)) {
    if (token.kind != kind) continue;
    out.emplace_back(code.substr(token.begin, token.end - token.begin));
  }
  return out;
}

[[nodiscard]] auto contains_kind(std::string_view code, std::string_view language, TokenKind kind)
    -> bool {
  for (const Token& token : st::text::highlight(code, language)) {
    if (token.kind == kind) return true;
  }
  return false;
}

/// 不变式：区间升序、不重叠、全部落在 [0, size]。
void check_invariants(std::string_view code, std::string_view language) {
  const auto tokens = st::text::highlight(code, language);
  std::size_t previous_end = 0;
  for (const Token& token : tokens) {
    ST_CHECK(token.begin <= token.end);
    ST_CHECK(token.end <= code.size());
    ST_CHECK(token.begin >= previous_end);
    previous_end = token.end;
  }
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 内置语言
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(highlight_builtin_languages_cover_mainstream_set) {
  const auto names = st::text::supported_languages();
  ST_CHECK(names.size() > 60U);  // 规范名 + 别名 + 扩展名
  for (const std::string_view expected : {"c", "cpp", "rust", "go", "python", "javascript",
                                          "typescript", "java", "csharp", "kotlin", "swift",
                                          "ruby", "php", "lua", "sql", "html", "xml", "css",
                                          "json", "yaml", "toml", "ini", "shell", "dockerfile",
                                          "makefile", "cmake", "markdown", "diff", "protobuf"}) {
    bool found = false;
    for (const std::string& name : names) {
      if (name == expected) found = true;
    }
    ST_CHECK(found);
  }
}

ST_TEST(highlight_cpp_recognizes_core_constructs) {
  const std::string_view code =
      "#include <vector>\n"
      "// 行注释\n"
      "int main() { /* 块注释 */ auto v = std::vector<int>{}; const char* s = \"文本\"; return 0; }\n";
  check_invariants(code, "cpp");
  ST_CHECK(contains_kind(code, "cpp", TokenKind::Preprocessor));
  ST_CHECK(contains_kind(code, "cpp", TokenKind::Keyword));
  ST_CHECK(contains_kind(code, "cpp", TokenKind::Type));
  ST_CHECK(contains_kind(code, "cpp", TokenKind::String));
  ST_CHECK(contains_kind(code, "cpp", TokenKind::Number));

  bool line_comment = false;
  bool block_comment = false;
  for (const std::string& comment : spans_of(code, "cpp", TokenKind::Comment)) {
    if (comment.find("行注释") != std::string::npos) line_comment = true;
    if (comment.find("块注释") != std::string::npos) block_comment = true;
  }
  ST_CHECK(line_comment);
  ST_CHECK(block_comment);
}

ST_TEST(highlight_block_comment_spans_lines) {
  const std::string_view code = "int a;\n/* 第一行\n第二行\n第三行 */ int b;\n";
  std::size_t comment_bytes = 0;
  for (const Token& token : st::text::highlight(code, "cpp")) {
    if (token.kind == TokenKind::Comment) comment_bytes += token.end - token.begin;
  }
  // 跨行块注释整体成片（含换行）
  ST_CHECK(comment_bytes > 20U);
  // 注释之后的代码仍被识别（`int` 在 C++ 里是类型关键词）
  ST_CHECK(contains_kind(code, "cpp", TokenKind::Type));
}

ST_TEST(highlight_python_triple_quote_and_comment) {
  const std::string_view code =
      "def f(x):\n"
      "    \"\"\"文档\n    跨行\"\"\"\n"
      "    return f'{x}'  # 注释\n";
  check_invariants(code, "python");
  ST_CHECK(contains_kind(code, "python", TokenKind::Comment));
  bool triple = false;
  for (const std::string& text : spans_of(code, "python", TokenKind::String)) {
    if (text.find("文档") != std::string::npos && text.find('\n') != std::string::npos) triple = true;
  }
  ST_CHECK(triple);
}

ST_TEST(highlight_sql_is_case_insensitive) {
  const std::string_view code = "select id from users where name = 'x'";
  const auto keywords = spans_of(code, "sql", TokenKind::Keyword);
  ST_CHECK(keywords.size() >= 3U);  // select / from / where（表名与列名不是关键字）
  ST_CHECK(contains_kind(code, "sql", TokenKind::String));
}

ST_TEST(highlight_html_tags_attributes_and_comments) {
  const std::string_view code =
      "<!-- 注释 -->\n<div class=\"box\" id='app'>文本 <span>嵌套</span></div>";
  check_invariants(code, "html");
  ST_CHECK(contains_kind(code, "html", TokenKind::Tag));
  ST_CHECK(contains_kind(code, "html", TokenKind::Attribute));
  ST_CHECK(contains_kind(code, "html", TokenKind::String));
  bool comment = false;
  for (const std::string& text : spans_of(code, "html", TokenKind::Comment)) {
    if (text.find("注释") != std::string::npos) comment = true;
  }
  ST_CHECK(comment);
}

ST_TEST(highlight_json_keys_and_literals) {
  const std::string_view code = "{\"name\": \"霜天\", \"count\": 12, \"ok\": true}";
  const auto keys = spans_of(code, "json", TokenKind::Key);
  ST_CHECK(keys.size() == 3U);
  ST_CHECK(contains_kind(code, "json", TokenKind::Number));
  ST_CHECK(contains_kind(code, "json", TokenKind::Builtin));
  // JSON 不允许单引号字符串
  ST_CHECK(!contains_kind("'x'", "json", TokenKind::String));
}

ST_TEST(highlight_yaml_keys_comments_and_numbers) {
  const std::string_view code = "name: 霜天\n# 注释\ncount: 3\nnested:\n  key: value\n";
  check_invariants(code, "yaml");
  ST_CHECK(!spans_of(code, "yaml", TokenKind::Key).empty());
  ST_CHECK(contains_kind(code, "yaml", TokenKind::Comment));
  ST_CHECK(contains_kind(code, "yaml", TokenKind::Number));
}

ST_TEST(highlight_markdown_heading_emphasis_code) {
  const std::string_view code = "# 标题\n\n**粗体** 与 `code`\n\n- 列表\n";
  ST_CHECK(contains_kind(code, "markdown", TokenKind::Keyword));   // 标题行
  ST_CHECK(contains_kind(code, "markdown", TokenKind::Type));      // 强调
  ST_CHECK(contains_kind(code, "markdown", TokenKind::String));    // 行内码
}

ST_TEST(highlight_diff_inserted_and_deleted) {
  const std::string_view code = "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-old\n+new\n";
  check_invariants(code, "diff");
  ST_CHECK(contains_kind(code, "diff", TokenKind::Inserted));
  ST_CHECK(contains_kind(code, "diff", TokenKind::Deleted));
  ST_CHECK(contains_kind(code, "diff", TokenKind::Keyword));
}

ST_TEST(highlight_shell_comment_is_hash) {
  const std::string_view code = "#!/bin/bash\nfor f in *.txt; do echo \"$f\"; done  # 注释";
  check_invariants(code, "shell");
  ST_CHECK(contains_kind(code, "shell", TokenKind::Keyword));
  ST_CHECK(contains_kind(code, "shell", TokenKind::Comment));
}

// ————————————————————————————————————————————————————————————————————————————
// 别名与扩展名解析
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(highlight_language_aliases_and_extensions_resolve) {
  // 别名的等价性：同一段代码用别名与规范名应得到相同 token 数
  const std::string_view code = "int main() { return 0; }";
  ST_CHECK_EQ(st::text::highlight(code, "c++").size(), st::text::highlight(code, "cpp").size());
  ST_CHECK_EQ(st::text::highlight(code, "py").size(), st::text::highlight(code, "python").size());
  ST_CHECK_EQ(st::text::highlight(code, "rs").size(), st::text::highlight(code, "rust").size());
  ST_CHECK_EQ(st::text::highlight(code, "ts").size(), st::text::highlight(code, "typescript").size());
  // 扩展名同样可解析
  ST_CHECK_EQ(st::text::highlight(code, "hpp").size(), st::text::highlight(code, "cpp").size());
}

ST_TEST(highlight_language_from_path_detects_common_files) {
  const auto expect = [](std::string_view path, std::string_view language) {
    const auto detected = st::text::language_from_path(path);
    ST_CHECK(detected.has_value());
    if (detected.has_value()) ST_CHECK_EQ(*detected, std::string(language));
  };
  expect("src/main.cpp", "cpp");
  expect("a/b/script.py", "python");
  expect("Makefile", "makefile");
  expect("Dockerfile", "dockerfile");
  expect("CMakeLists.txt", "cmake");
  expect("config.toml", "toml");
  expect("ui/panel.tsx", "typescript");
  expect("styles.scss", "scss");
  ST_CHECK(!st::text::language_from_path("notes.unknownext").has_value());
  ST_CHECK(!st::text::language_from_path("noextension").has_value());
}

// ————————————————————————————————————————————————————————————————————————————
// 边界与不变式
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(highlight_unknown_language_and_empty_input) {
  const auto unknown = st::text::highlight("int main() {}", "definitely-not-a-language");
  ST_REQUIRE(unknown.size() == 1U);
  ST_CHECK(unknown[0].kind == TokenKind::Plain);
  ST_CHECK_EQ(unknown[0].end, std::string_view("int main() {}").size());

  const auto empty = st::text::highlight("", "cpp");
  ST_REQUIRE(empty.size() == 1U);
  ST_CHECK_EQ(empty[0].begin, 0U);
  ST_CHECK_EQ(empty[0].end, 0U);
}

ST_TEST(highlight_unterminated_constructs_do_not_break_invariants) {
  // 未闭合的块注释 / 字符串：吃到文末（编辑器里正是"正在输入"的常见状态）
  for (const std::string_view code : {"/* 未闭合", "\"未闭合字符串", "'''未闭合", "`模板串"}) {
    check_invariants(code, "cpp");
    check_invariants(code, "python");
    check_invariants(code, "javascript");
  }
  const std::string_view open_comment = "int a; /* 后面都算注释";
  std::size_t comment_bytes = 0;
  for (const Token& token : st::text::highlight(open_comment, "cpp")) {
    if (token.kind == TokenKind::Comment) comment_bytes += token.end - token.begin;
  }
  ST_CHECK_EQ(comment_bytes, std::string_view("/* 后面都算注释").size());
}

ST_TEST(highlight_tokens_never_overlap_and_cover_only_their_slice) {
  const std::string_view samples[] = {
      "std::vector<std::string> v;  // 注释\n",
      "def f(): return {'k': 'v'}  # 中文注释\n",
      "<div a=\"1\" b='2'>x</div>\n",
      "SELECT * FROM t WHERE a IN (1,2) -- c\n",
  };
  for (const std::string_view code : samples) {
    for (const std::string_view language : {"cpp", "python", "html", "sql"}) {
      check_invariants(code, language);
    }
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 自定义语言（运行时可注册）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(highlight_custom_language_registration_and_usage) {
  LanguageRegistry registry;  // 独立实例：不污染进程级注册表
  LanguageSpec spec = st::text::make_language("stlog");
  st::text::with_aliases(spec, {"slog", "log"});
  st::text::with_keywords(spec, {"INFO", "WARN", "ERROR", "DEBUG"});
  st::text::with_line_comment(spec, ";");
  ST_CHECK(registry.register_language(spec));

  const std::string_view code = "INFO 服务启动\nERROR 失败 ; 这是注释";
  const auto tokens = registry.find("slog");
  ST_REQUIRE(tokens != nullptr);
  const auto highlighted = st::text::highlight_with(code, *tokens);
  std::size_t keywords = 0;
  std::size_t comments = 0;
  for (const Token& token : highlighted) {
    if (token.kind == TokenKind::Keyword) ++keywords;
    if (token.kind == TokenKind::Comment) ++comments;
  }
  ST_CHECK_EQ(keywords, 2U);
  ST_CHECK_EQ(comments, 1U);

  // 别名同样可查到同一份规则
  ST_CHECK(registry.find("slog") != nullptr);
  ST_CHECK(registry.find("log") != nullptr);
  ST_CHECK(registry.find("LOGGING") == nullptr);
}

ST_TEST(highlight_custom_language_can_override_builtin) {
  LanguageRegistry registry;
  registry.reset_to_builtin();
  const std::size_t before = registry.size();

  LanguageSpec spec = st::text::make_language("json");
  st::text::with_keywords(spec, {"MAGIC"});
  ST_CHECK(!registry.register_language(spec));       // 默认不覆盖
  ST_CHECK_EQ(registry.size(), before);
  ST_CHECK(registry.register_language(spec, true));  // 显式覆盖
  const auto replaced = registry.find("json");
  ST_REQUIRE(replaced != nullptr);
  ST_CHECK(replaced->keywords.size() == 1U);

  // 注销后还原
  ST_CHECK(registry.unregister("json"));
  ST_CHECK(registry.find("json") == nullptr);
  registry.reset_to_builtin();
  ST_CHECK(registry.size() == before);
}

ST_TEST(highlight_custom_language_with_tags_and_strings) {
  LanguageRegistry registry;
  LanguageSpec spec = st::text::make_language("vue");
  st::text::with_tags(spec, "<", ">");
  st::text::with_block_comment(spec, "<!--", "-->");
  st::text::with_keywords(spec, {"template", "script", "style"});
  ST_CHECK(registry.register_language(spec));
  const auto found = registry.find("vue");
  ST_REQUIRE(found != nullptr);

  const std::string_view code = "<template><div class=\"a\">文本</div></template>";
  const auto tokens = st::text::highlight_with(code, *found);
  std::size_t tags = 0;
  std::size_t attributes = 0;
  for (const Token& token : tokens) {
    if (token.kind == TokenKind::Tag) ++tags;
    if (token.kind == TokenKind::Attribute) ++attributes;
  }
  ST_CHECK(tags >= 3U);         // template/div/div/template
  ST_CHECK_EQ(attributes, 1U);
}

ST_TEST(highlight_registry_handles_replacement_and_query) {
  LanguageRegistry registry;
  ST_CHECK_EQ(registry.size(), 0U);
  ST_CHECK(registry.register_language(st::text::make_language("alpha")));
  ST_CHECK(registry.register_language(st::text::make_language("beta")));
  ST_CHECK_EQ(registry.size(), 2U);
  const auto names = registry.names();
  ST_CHECK_EQ(names.size(), 2U);
  ST_CHECK_EQ(names[0], std::string("alpha"));  // 升序
  ST_CHECK(registry.unregister("alpha"));
  ST_CHECK(!registry.unregister("alpha"));      // 重复注销返回 false
  registry.clear();
  ST_CHECK_EQ(registry.size(), 0U);
  registry.reset_to_builtin();
  ST_CHECK(registry.size() > 20U);
}

ST_TEST(highlight_builtin_languages_are_self_consistent) {
  for (const auto& spec : st::text::builtin_languages()) {
    ST_CHECK(!spec.name.empty());
    // 字面量表已规范化为有序去重（注册路径负责，这里验证规范化后不变式）
    LanguageRegistry registry;
    ST_CHECK(registry.register_language(spec));
    const auto stored = registry.find(spec.name);
    ST_REQUIRE(stored != nullptr);
    ST_CHECK(std::is_sorted(stored->keywords.begin(), stored->keywords.end()));
    ST_CHECK(std::is_sorted(stored->types.begin(), stored->types.end()));
    ST_CHECK(std::is_sorted(stored->builtins.begin(), stored->builtins.end()));
    ST_CHECK(std::is_sorted(stored->aliases.begin(), stored->aliases.end()));
    // 块注释定界必须成对
    ST_CHECK(stored->block_comment_begin.empty() == stored->block_comment_end.empty());
    ST_CHECK(stored->tag_begin.empty() == stored->tag_end.empty());
    // 语言名与别名不得重复
    std::vector<std::string> all = {stored->name};
    for (const auto& alias : stored->aliases) all.push_back(alias);
    std::sort(all.begin(), all.end());
    ST_CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
  }
}

ST_TEST(highlight_convenience_builders_compose) {
  LanguageSpec spec = st::text::make_language("demo");
  st::text::with_keywords(spec, {"let", "return"});
  st::text::with_types(spec, {"int", "string"});
  st::text::with_builtins(spec, {"true", "false"});
  st::text::with_definition_keywords(spec, {"fn"});
  st::text::with_aliases(spec, {"demo-lang"});
  st::text::with_c_comments(spec);
  spec.string_prefixes = {"r\""};
  const std::string_view code = "let x: int = 1; // 注释\nfn run() { return r\"raw\"; }";
  const auto tokens = st::text::highlight_with(code, spec);
  std::size_t keywords = 0;
  std::size_t types = 0;
  std::size_t functions = 0;
  for (const Token& token : tokens) {
    if (token.kind == TokenKind::Keyword) ++keywords;
    if (token.kind == TokenKind::Type) ++types;
    if (token.kind == TokenKind::Function) ++functions;
  }
  ST_CHECK(keywords >= 2U);   // let / return
  ST_CHECK(types >= 1U);
  ST_CHECK(functions >= 1U);  // fn 之后的 run（定义关键字驱动）
  // 注册路径会规范化（排序去重），本用例验证"规范化不改语义"
  LanguageRegistry registry;
  ST_CHECK(registry.register_language(spec));
  const auto stored = registry.find("demo");
  ST_REQUIRE(stored != nullptr);
  ST_CHECK(std::is_sorted(stored->keywords.begin(), stored->keywords.end()));
}
