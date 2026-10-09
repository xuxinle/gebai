/// 导航语义层测试（LSP 阶段 5）：位置 / 悬停 / 符号解析 + 标识符取词。

#include <iostream>
#include <string>
#include <vector>

#include "st/ext/json.hpp"
#include "st/lsp/navigation.hpp"
#include "st/test/test.hpp"

namespace {

[[nodiscard]] auto json_of(std::string_view text) -> st::Json {
  return st::json_parse(text).value();
}

}  // namespace

ST_TEST(lsp_navigation_parses_three_response_shapes) {
  // ① 单个 `Location`（对象）。
  const auto single = st::lsp::parse_locations(
      json_of(R"({"uri":"file:///a.cpp","range":{"start":{"line":3,"character":2},
                "end":{"line":3,"character":8}}})"));
  ST_CHECK_EQ(single.size(), std::size_t{1});
  ST_CHECK_EQ(single.front().uri, std::string("file:///a.cpp"));
  ST_CHECK_EQ(single.front().range.start.line, std::size_t{3});
  ST_CHECK(!single.front().selection.has_value());   // 普通 Location 没有 selection

  // ② `Location[]`（数组）。
  const auto many = st::lsp::parse_locations(json_of(
      R"([{"uri":"file:///a.cpp","range":{"start":{"line":1,"character":0},"end":{"line":1,"character":3}}},
          {"uri":"file:///b.hpp","range":{"start":{"line":9,"character":4},"end":{"line":9,"character":9}}}])"));
  ST_CHECK_EQ(many.size(), std::size_t{2});
  ST_CHECK_EQ(many[1].uri, std::string("file:///b.hpp"));

  // ③ `LocationLink[]`：必须优先取 `targetSelectionRange`（标识符本身），
  //    而不是 `targetRange`（整个声明）。
  const auto links = st::lsp::parse_locations(json_of(
      R"([{"originSelectionRange":{"start":{"line":0,"character":0},"end":{"line":0,"character":3}},
           "targetUri":"file:///c.cpp",
           "targetRange":{"start":{"line":10,"character":0},"end":{"line":40,"character":1}},
           "targetSelectionRange":{"start":{"line":12,"character":5},"end":{"line":12,"character":11}}}])"));
  ST_CHECK_EQ(links.size(), std::size_t{1});
  ST_CHECK_EQ(links.front().uri, std::string("file:///c.cpp"));
  ST_REQUIRE(links.front().selection.has_value());
  ST_CHECK_EQ(links.front().selection->start.line, std::size_t{12});
  ST_CHECK_EQ(links.front().selection->start.character, std::size_t{5});
  // `range` 保留 targetRange（调用方若想框整个声明仍有数据）。
  ST_CHECK_EQ(links.front().range.start.line, std::size_t{10});

  // `null`（找不到定义）→ 空列表，不崩。
  ST_CHECK(st::lsp::parse_locations(st::Json()).empty());
  ST_CHECK(st::lsp::parse_locations(json_of("null")).empty());
  ST_CHECK(st::lsp::parse_locations(json_of("[]")).empty());
}

ST_TEST(lsp_navigation_parses_hover_shapes) {
  // MarkupContent。
  const auto markup = st::lsp::parse_hover(json_of(
      R"({"contents":{"kind":"markdown","value":"**int** width"},"range":{"start":{"line":1,"character":2},"end":{"line":1,"character":7}}})"));
  ST_REQUIRE(markup.has_value());
  ST_CHECK_EQ(markup->text, std::string("**int** width"));
  ST_REQUIRE(markup->range.has_value());
  ST_CHECK_EQ(markup->range->start.character, std::size_t{2});

  // 纯字符串。
  const auto plain = st::lsp::parse_hover(json_of(R"({"contents":"int width"})"));
  ST_REQUIRE(plain.has_value());
  ST_CHECK_EQ(plain->text, std::string("int width"));
  ST_CHECK(!plain->range.has_value());

  // `MarkedString[]`（字符串 + `{language,value}` 混合）。
  const auto marked = st::lsp::parse_hover(json_of(
      R"({"contents":["第一段",{"language":"cpp","value":"int x;"}]})"));
  ST_REQUIRE(marked.has_value());
  ST_CHECK(marked->text.find("第一段") != std::string::npos);
  ST_CHECK(marked->text.find("int x;") != std::string::npos);

  // `null` / 空内容 → `nullopt`（弹空框不如不弹）。
  ST_CHECK(!st::lsp::parse_hover(json_of("null")).has_value());
  ST_CHECK(!st::lsp::parse_hover(json_of(R"({"contents":""})")).has_value());
  ST_CHECK(!st::lsp::parse_hover(json_of(R"({"contents":{"kind":"markdown","value":""}})")).has_value());
}

ST_TEST(lsp_navigation_parses_document_symbols_hierarchical_and_flat) {
  // 层级形态（`DocumentSymbol[]`）：父在子前，depth 正确。
  const auto hierarchical = st::lsp::parse_document_symbols(json_of(
      R"([{"name":"Widget","kind":5,"range":{"start":{"line":0,"character":0},"end":{"line":10,"character":1}},
           "children":[
             {"name":"width","kind":8,"range":{"start":{"line":2,"character":2},"end":{"line":2,"character":7}},"detail":"int"},
             {"name":"resize","kind":6,"range":{"start":{"line":4,"character":2},"end":{"line":7,"character":3}},
              "children":[{"name":"height","kind":8,"range":{"start":{"line":5,"character":4},"end":{"line":5,"character":10}}}]}
           ]}])"));
  ST_CHECK_EQ(hierarchical.size(), std::size_t{4});
  ST_CHECK_EQ(hierarchical[0].name, std::string("Widget"));
  ST_CHECK_EQ(hierarchical[0].depth, std::size_t{0});
  ST_CHECK_EQ(hierarchical[1].name, std::string("width"));
  ST_CHECK_EQ(hierarchical[1].depth, std::size_t{1});
  ST_CHECK_EQ(hierarchical[1].detail, std::string("int"));
  ST_CHECK_EQ(hierarchical[2].depth, std::size_t{1});   // resize
  ST_CHECK_EQ(hierarchical[3].name, std::string("height"));
  ST_CHECK_EQ(hierarchical[3].depth, std::size_t{2});   // 孙
  // 大纲顺序：父 → 子 → 兄弟 → 侄（深度优先，正是列表要的顺序）。

  // 扁平形态（`SymbolInformation[]`）：`location.range` 也要认。
  const auto flat = st::lsp::parse_document_symbols(json_of(
      R"([{"name":"main","kind":12,"location":{"uri":"file:///m.cpp",
           "range":{"start":{"line":20,"character":0},"end":{"line":22,"character":1}}}}])"));
  ST_CHECK_EQ(flat.size(), std::size_t{1});
  ST_CHECK_EQ(flat.front().name, std::string("main"));
  ST_CHECK_EQ(flat.front().range.start.line, std::size_t{20});
  ST_CHECK_EQ(flat.front().depth, std::size_t{0});

  ST_CHECK(st::lsp::parse_document_symbols(json_of("null")).empty());
  ST_CHECK(st::lsp::parse_document_symbols(json_of("{}")).empty());
}

ST_TEST(lsp_navigation_parses_workspace_symbols) {
  const auto symbols = st::lsp::parse_workspace_symbols(json_of(
      R"([{"name":"compute_area","kind":12,"containerName":"geometry",
           "location":{"uri":"file:///geo.cpp","range":{"start":{"line":5,"character":0},"end":{"line":5,"character":12}}}},
          {"name":"Widget","kind":5,"location":{"uri":"file:///w.hpp","range":{"start":{"line":1,"character":0},"end":{"line":1,"character":6}}}}])"));
  ST_CHECK_EQ(symbols.size(), std::size_t{2});
  ST_CHECK_EQ(symbols[0].name, std::string("compute_area"));
  ST_CHECK_EQ(symbols[0].container, std::string("geometry"));
  ST_CHECK_EQ(symbols[1].container, std::string{});   // 缺 containerName 不崩
  ST_CHECK_EQ(symbols[1].uri, std::string("file:///w.hpp"));
  // `{symbols:[...]}` 形态。
  const auto wrapped = st::lsp::parse_workspace_symbols(json_of(
      R"({"symbols":[{"name":"x","kind":13,"location":{"uri":"file:///a","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}}}]})"));
  ST_CHECK_EQ(wrapped.size(), std::size_t{1});
  // 无名条目被跳过（没有名字的符号没法显示也没法跳）。
  const auto unnamed = st::lsp::parse_workspace_symbols(json_of(
      R"([{"kind":12,"location":{"uri":"file:///a","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}}}])"));
  ST_CHECK(unnamed.empty());
}

ST_TEST(lsp_navigation_symbol_badges) {
  ST_CHECK_EQ(st::lsp::symbol_badge(12), std::string_view("ƒ"));   // Function
  ST_CHECK_EQ(st::lsp::symbol_badge(6), std::string_view("m"));    // Method
  ST_CHECK_EQ(st::lsp::symbol_badge(5), std::string_view("C"));    // Class
  ST_CHECK_EQ(st::lsp::symbol_badge(8), std::string_view("·"));    // Field
  ST_CHECK_EQ(st::lsp::symbol_badge(999), std::string_view("•"));  // 未知：给个通用点
  ST_CHECK(!st::lsp::symbol_badge(0).empty());                      // 永不返回空（列表对齐需要）
}

ST_TEST(lsp_navigation_identifier_at_cursor) {
  const std::string text = "int area = compute(width);";
  // 光标在 `compute` 中间。
  const std::size_t inside = text.find("compute") + 3;
  const auto [b1, e1] = st::lsp::identifier_at(text, inside);
  ST_CHECK_EQ(text.substr(b1, e1 - b1), std::string("compute"));
  // 光标恰在词**末尾之后**（词与 `(` 之间）——仍应拿到这个词。
  const std::size_t after = text.find("compute") + 7;
  const auto [b2, e2] = st::lsp::identifier_at(text, after);
  ST_CHECK_EQ(text.substr(b2, e2 - b2), std::string("compute"));
  // 光标在标点上：返回空区间（调用方原样发光标位置）。
  const std::size_t on_semicolon = text.size() - 1;
  const auto [b3, e3] = st::lsp::identifier_at(text, on_semicolon);
  ST_CHECK_EQ(b3, e3);
  // 中文标识符不被切碎。
  const std::string cjk = "auto 变量名 = 1;";
  const std::size_t cjk_at = cjk.find("变量名") + 1;
  const auto [b4, e4] = st::lsp::identifier_at(cjk, cjk_at);
  ST_CHECK_EQ(cjk.substr(b4, e4 - b4), std::string("变量名"));
  // 空文本 / 越界光标不崩。
  ST_CHECK_EQ(st::lsp::identifier_at("", 5).first, std::size_t{0});
  const auto [b5, e5] = st::lsp::identifier_at("abc", 99);
  ST_CHECK_EQ(b5, std::size_t{0});
  ST_CHECK_EQ(e5, std::size_t{3});
}
