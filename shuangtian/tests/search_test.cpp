// 搜索引擎（枚举 / 过滤 / 内容搜索）的功能与不变量用例。
//
// 分三块：
//   ① 枚举：跳过产物/隐藏/超限/二进制、相对路径与排序稳定；
//   ② 路径过滤：子序列 + 排序（连续段优先、末位越靠后越差）；
//   ③ 内容搜索：与「朴素逐行实现」逐条对齐（正则是自实现子集，必须钉住它的口径），
//      外加大小写/全词/每文件上限/多文件并行稳定顺序。
//
// 逆向验证（`tools/search_reverse_verify.py`）：把 `locate_in_line` 的
// `whole_word` 分支临时短路成 `return at;`，全词用例必须变红。
#include "st/search/search.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/test/test.hpp"

namespace {

[[nodiscard]] auto write_file(const std::string& dir, const std::string& name,
                              std::string_view content) -> bool {
  const std::string path = st::fs::join(dir, name);
  return st::fs::write_text(path, content).has_value();
}

void make_dir(const std::string& path) {
  (void)st::fs::create_directories(path);
}

/// 朴素逐行字面量搜索（区分大小写、不做全词）——作为搜索引擎的语义参照。
[[nodiscard]] auto naive_literal_lines(std::string_view content, std::string_view needle)
    -> std::vector<std::size_t> {
  std::vector<std::size_t> lines;
  std::size_t line_no = 0;
  std::size_t begin = 0;
  while (begin <= content.size()) {
    const std::size_t eol = content.find('\n', begin);
    const std::size_t end = eol == std::string_view::npos ? content.size() : eol;
    std::string_view line = content.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    ++line_no;
    if (line.find(needle) != std::string_view::npos) lines.push_back(line_no);
    if (eol == std::string_view::npos) break;
    begin = eol + 1;
  }
  return lines;
}

}  // namespace

// —————————————————————————————— 枚举 ——————————————————————————————

ST_TEST(search_enumerate_skips_ignored_and_binary) {
  auto dir = st::fs::make_temp_dir("st-search-enum");
  ST_REQUIRE(dir.has_value());
  const std::string root = *dir;
  make_dir(st::fs::join(root, "src"));
  make_dir(st::fs::join(root, "build"));         // 应被默认忽略
  make_dir(st::fs::join(root, ".git"));          // 隐藏 + 默认忽略
  make_dir(st::fs::join(root, "node_modules"));  // 默认忽略
  ST_REQUIRE(write_file(root, "src/a.cpp", "int alpha = 1;\n"));
  ST_REQUIRE(write_file(root, "src/b.hpp", "#pragma once\n"));
  ST_REQUIRE(write_file(root, "README.md", "hello\n"));
  ST_REQUIRE(write_file(st::fs::join(root, "build"), "gen.cpp", "generated\n"));  // 应被跳过
  ST_REQUIRE(write_file(root, ".hidden", "secret\n"));                             // 隐藏
  // 二进制文件（含 NUL）：应被跳过。用显式长度构造，"\x00" 不会截断为 strlen=3。
  const std::string binary("ELF\x00\x01\x02\x03", 7);
  ST_REQUIRE(write_file(root, "blob.bin", binary));

  st::search::EnumerateOptions options;
  options.binary_filter = st::search::BinaryFilter::kByContent;   // 真判二进制（含无扩展名的）
  auto result = st::search::enumerate(root, options);
  ST_REQUIRE(result.has_value());
  std::vector<std::string> relatives;
  for (const auto& entry : result->files) relatives.push_back(entry.relative);
  std::ranges::sort(relatives);
  const std::vector<std::string> expected{"README.md", "src/a.cpp", "src/b.hpp"};
  ST_CHECK(relatives == expected);
  ST_CHECK(result->stats.files_kept == 3);
  ST_CHECK(result->stats.files_binary >= 1);
  ST_CHECK(result->stats.dirs_ignored >= 2);   // build / .git / node_modules
  (void)st::fs::remove_all(root);
}

ST_TEST(search_enumerate_sorted_and_finds_join_paths) {
  auto dir = st::fs::make_temp_dir("st-search-enum-sort");
  ST_REQUIRE(dir.has_value());
  const std::string root = *dir;
  make_dir(st::fs::join(root, "z"));
  make_dir(st::fs::join(root, "a"));
  ST_REQUIRE(write_file(root, "a/x.txt", "x"));
  ST_REQUIRE(write_file(root, "z/y.txt", "y"));
  ST_REQUIRE(write_file(root, "m.txt", "m"));
  auto result = st::search::enumerate(root);
  ST_REQUIRE(result.has_value());
  // 必须按 relative 字典序：a/x.txt < m.txt < z/y.txt
  ST_REQUIRE(result->files.size() == 3);
  ST_CHECK_EQ(result->files[0].relative, std::string("a/x.txt"));
  ST_CHECK_EQ(result->files[1].relative, std::string("m.txt"));
  ST_CHECK_EQ(result->files[2].relative, std::string("z/y.txt"));
  // path 是绝对/可打开的真实路径。
  ST_CHECK(st::fs::is_regular_file(result->files[0].path));
  (void)st::fs::remove_all(root);
}

ST_TEST(search_enumerate_skips_large_files) {
  auto dir = st::fs::make_temp_dir("st-search-enum-large");
  ST_REQUIRE(dir.has_value());
  const std::string root = *dir;
  ST_REQUIRE(write_file(root, "small.txt", "ok"));
  ST_REQUIRE(write_file(root, "big.txt", std::string(5000, 'a')));
  st::search::EnumerateOptions options;
  options.max_file_bytes = 100;
  auto result = st::search::enumerate(root, options);
  ST_REQUIRE(result.has_value());
  ST_CHECK_EQ(result->files.size(), std::size_t{1});
  ST_CHECK_EQ(result->files[0].relative, std::string("small.txt"));
  ST_CHECK_EQ(result->stats.files_large, std::uint64_t{1});
  (void)st::fs::remove_all(root);
}

// —————————————————————————————— 路径过滤 ——————————————————————————————

ST_TEST(search_filter_paths_subsequence_and_ordering) {
  std::vector<st::search::FileEntry> files;
  const auto add = [&files](std::string rel) {
    st::search::FileEntry entry;
    entry.relative = rel;
    files.push_back(std::move(entry));
  };
  add("src/ui/window.cpp");
  add("src/ui/widget.cpp");
  add("README.md");
  add("app/main.cpp");

  // "ui" 应命中 src/ui/*（子序列），README 不含 u 后 i 的顺序，app/main 也不含。
  auto hits = st::search::filter_paths(files, "ui", 0);
  ST_CHECK_EQ(hits.size(), std::size_t{2});
  for (const std::size_t index : hits) {
    ST_CHECK(files[index].relative.find("ui") != std::string::npos);
  }

  // "main" 应把 app/main.cpp 排在前（连续段 + 末段命中）。
  auto main_hits = st::search::filter_paths(files, "main", 0);
  ST_REQUIRE(!main_hits.empty());
  ST_CHECK_EQ(files[main_hits[0]].relative, std::string("app/main.cpp"));

  // 空查询返回全部；limit 生效。
  ST_CHECK_EQ(st::search::filter_paths(files, "", 0).size(), files.size());
  ST_CHECK_EQ(st::search::filter_paths(files, "", 2).size(), std::size_t{2});

  // 大小写不敏感："WINDOW" 命中 window.cpp。
  auto ci = st::search::filter_paths(files, "WINDOW", 0);
  ST_REQUIRE(!ci.empty());
  ST_CHECK_EQ(files[ci[0]].relative, std::string("src/ui/window.cpp"));
}

// —————————————————————————————— 内容搜索 ——————————————————————————————

ST_TEST(search_buffer_matches_naive_literal) {
  const std::string content =
      "alpha beta gamma\n"
      "beta line two\n"
      "no match here\n"
      "the beta appears twice: beta\n"
      "\n"
      "trailing beta";
  st::search::SearchOptions options;
  options.case_sensitive = true;
  const auto hits = st::search::search_buffer(content, "beta", options);
  const auto expected_lines = naive_literal_lines(content, "beta");
  std::vector<std::size_t> actual_lines;
  for (const auto& hit : hits) actual_lines.push_back(hit.line);
  ST_CHECK(actual_lines == expected_lines);
  // 列号：第 1 行的 "beta" 在 index 6。
  ST_REQUIRE(!hits.empty());
  ST_CHECK_EQ(hits[0].line, std::size_t{1});
  ST_CHECK_EQ(hits[0].column, std::size_t{6});
  ST_CHECK_EQ(hits[0].line_text, std::string("alpha beta gamma"));
}

ST_TEST(search_buffer_case_insensitive) {
  const std::string content = "Hello World\nhello there\nHELLO again\n";
  st::search::SearchOptions options;   // 默认不区分大小写
  const auto hits = st::search::search_buffer(content, "hello", options);
  ST_CHECK_EQ(hits.size(), std::size_t{3});
  st::search::SearchOptions sensitive;
  sensitive.case_sensitive = true;
  const auto strict = st::search::search_buffer(content, "hello", sensitive);
  ST_CHECK_EQ(strict.size(), std::size_t{1});   // 只有第 2 行小写 hello
  ST_CHECK_EQ(strict[0].line, std::size_t{2});
}

ST_TEST(search_buffer_whole_word) {
  // 每行只取**首个**全词命中（与「搜索结果按行展示」一致）。
  // 逐行设计以验证：行内前面的「包含关系」（catalog / concatenate / cat_scatter）
  // 要被跳过，只命中后面的独立 cat。
  const std::string content =
      "catalog\n"              // 无全词 cat（cat 在 catalog 内）
      "concatenate cat\n"      // 只命中末段独立的 cat（列 12）
      "cat_scatter cat\n";     // 首个 cat_ 不是全词，命中后面的 cat（列 12）
  st::search::SearchOptions options;
  options.case_sensitive = true;
  options.whole_word = true;
  const auto hits = st::search::search_buffer(content, "cat", options);
  ST_CHECK_EQ(hits.size(), std::size_t{2});
  ST_REQUIRE(hits.size() == 2);
  ST_CHECK_EQ(hits[0].line, std::size_t{2});
  ST_CHECK_EQ(hits[0].column, std::size_t{12});
  ST_CHECK_EQ(hits[1].line, std::size_t{3});
  ST_CHECK_EQ(hits[1].column, std::size_t{12});   // 跳过了行首的 cat_scatter
}

ST_TEST(search_buffer_regex_subset) {
  const std::string content = "id_123 value\na7b\nowner-42\n";
  st::search::SearchOptions options;
  options.case_sensitive = true;
  options.regex = true;
  // \d+ 等价于「连续数字」：用 \d\d 找两位数字。
  const auto two_digits = st::search::search_buffer(content, "\\d\\d", options);
  ST_CHECK(!two_digits.empty());
  // "id_\d\d" 锚定前缀：只应命中第 1 行。
  const auto prefixed = st::search::search_buffer(content, "id_\\d\\d", options);
  ST_REQUIRE(prefixed.size() == 1);
  ST_CHECK_EQ(prefixed[0].line, std::size_t{1});
  ST_CHECK_EQ(prefixed[0].column, std::size_t{0});
}

ST_TEST(search_buffer_caps_hits_per_file) {
  std::string content;
  for (int i = 0; i < 50; ++i) content += "needle line\n";
  st::search::SearchOptions options;
  options.case_sensitive = true;
  options.max_hits_per_file = 10;
  const auto hits = st::search::search_buffer(content, "needle", options);
  ST_CHECK_EQ(hits.size(), std::size_t{10});
}

ST_TEST(search_file_uses_mmap_and_returns_path) {
  auto dir = st::fs::make_temp_dir("st-search-file");
  ST_REQUIRE(dir.has_value());
  const std::string path = st::fs::join(*dir, "sample.txt");
  ST_REQUIRE(st::fs::write_text(path, "first line\nsecond target here\nthird\n").has_value());
  st::search::SearchOptions options;
  options.case_sensitive = true;
  auto result = st::search::search_file(path, "target", options, "sample.txt");
  ST_REQUIRE(result.has_value());
  ST_REQUIRE(result->size() == 1);
  ST_CHECK_EQ((*result)[0].line, std::size_t{2});
  ST_CHECK_EQ((*result)[0].path, path);
  ST_CHECK_EQ((*result)[0].relative, std::string("sample.txt"));
  (void)st::fs::remove_all(*dir);
}

ST_TEST(search_paths_parallel_stable_order) {
  auto dir = st::fs::make_temp_dir("st-search-paths");
  ST_REQUIRE(dir.has_value());
  const std::string root = *dir;
  // 造 20 个文件，命中数各不相同：验证并行后结果仍按输入顺序、文件内按行升序。
  std::vector<st::search::FileEntry> files;
  for (int i = 0; i < 20; ++i) {
    const std::string name = std::format("file{:02}.txt", i);
    std::string content;
    for (int line = 0; line < i; ++line) content += "hit line\n";
    content += "tail\n";
    ST_REQUIRE(write_file(root, name, content));
    st::search::FileEntry entry;
    entry.path = st::fs::join(root, name);
    entry.relative = name;
    entry.name = name;
    entry.size = content.size();
    files.push_back(std::move(entry));
  }
  st::search::SearchOptions options;
  options.case_sensitive = true;
  std::size_t progress_last = 0;
  const auto result = st::search::search_paths(
      files, "hit", options, [&](std::size_t done, std::size_t total) {
        ST_CHECK(done <= total);
        progress_last = done;
      });
  ST_CHECK_EQ(result.files_with_hits, std::size_t{19});   // file00 无命中
  ST_CHECK_EQ(progress_last, files.size());
  // 结果按输入顺序：file01 的命中在 file02 之前；每文件内行号 1..i 升序。
  std::vector<std::string> seen_order;
  std::string previous;
  for (const auto& hit : result.hits) {
    if (hit.relative != previous) {
      seen_order.push_back(hit.relative);
      previous = hit.relative;
    }
  }
  // 出现的文件相对路径必须严格递增（输入已按 fileXX 命名排序）。
  ST_CHECK(std::is_sorted(seen_order.begin(), seen_order.end()));
  (void)st::fs::remove_all(root);
}

ST_TEST(search_looks_binary_and_word_char) {
  ST_CHECK(!st::search::looks_binary(std::string_view("plain text\n")));
  ST_CHECK(st::search::looks_binary(std::string_view("\x00binary", 7)));
  ST_CHECK(st::search::looks_binary(std::string_view("\x01\x02\x03\x04", 4)));
  ST_CHECK(st::search::is_word_char('a'));
  ST_CHECK(st::search::is_word_char('0'));
  ST_CHECK(st::search::is_word_char('_'));
  ST_CHECK(!st::search::is_word_char('-'));
  ST_CHECK(!st::search::is_word_char(' '));
}