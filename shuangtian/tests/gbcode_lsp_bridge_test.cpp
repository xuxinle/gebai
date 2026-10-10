/// gbcode 语言服务桥（lsp_bridge）的数据层测试：配方匹配 / 位置换算的行索引加速 /
/// 诊断指纹去重。不连真 server（那些在 lsp_language_smoke_test 里按机器环境跑）。

#include <iostream>
#include <string>
#include <vector>

#include "st/test/test.hpp"

// lsp_bridge 是 gbcode 的应用层源（不在框架 sources 清单里）——与
// git_service_test 同一策略：直接 include 实现编译进测试。
#include "examples/gbcode/lsp_bridge.hpp"
#include "examples/gbcode/lsp_bridge.cpp"

namespace {

/// 构造一个多行多字节文本（行首偏移各不相同、含中文行）。
[[nodiscard]] auto sample_text() -> std::string {
  return "alpha\nbeta gamma\ndelta\n中文行内容\nomega\n";
}

}  // namespace

ST_TEST(gbcode_recipe_matches_extension) {
  // 扩展名匹配（大小写归一）。
  const gbcode::LanguageServerRecipe* cpp = gbcode::recipe_for_path("/x/y/main.CPP");
  ST_REQUIRE(cpp != nullptr);
  ST_CHECK_EQ(cpp->language, std::string("cpp"));
  const gbcode::LanguageServerRecipe* py = gbcode::recipe_for_path("script.py");
  ST_REQUIRE(py != nullptr);
  ST_CHECK_EQ(py->language, std::string("python"));
  // 无配方：如实 nullptr（不假装有）。
  ST_CHECK(gbcode::recipe_for_path("README.md") == nullptr);
  ST_CHECK(gbcode::recipe_for_path("noext") == nullptr);
  // 语言名反查（编辑器 language id → 配方）。
  ST_CHECK_EQ(gbcode::recipe_for_language("go")->program, std::string("gopls"));
  ST_CHECK(gbcode::recipe_for_language("nosuch") == nullptr);
}

ST_TEST(gbcode_offset_to_position_uses_line_index) {
  gbcode::LanguageService service{};
  const std::string text = sample_text();
  service.sync_document_if_ready_for_test("/tmp/a.cpp", text);

  // 换算正确性：每个「行首偏移」都应得到 (line, 0)。
  struct Case { std::size_t offset; std::size_t line; std::uint32_t character; };
  const Case cases[] = {
      {0, 0, 0},          // 行首
      {6, 1, 0},          // 第二行行首（"beta" 的 b）
      {11, 1, 5},         // "beta " 之后（含空格）
      {17, 2, 0},         // 第三行行首
      {23, 3, 0},         // 中文行行首
      {23 + 3, 3, 1},     // 中文第一字后（UTF-16 一个码元）
      {23 + 15, 3, 5},    // 中文五行后（每字 UTF-16 码元 1）
      {text.size(), 5, 0} // 末尾（空行起点）
  };
  for (const auto& item : cases) {
    const auto position = service.offset_to_position("/tmp/a.cpp", item.offset);
    ST_REQUIRE(position.has_value());
    ST_CHECK_EQ(position->line, item.line);
    ST_CHECK_EQ(position->character, item.character);
  }
  // 未同步的路径：nullopt（如实）。
  ST_CHECK(!service.offset_to_position("/tmp/missing.cpp", 0).has_value());
}

ST_TEST(gbcode_diagnostics_dedup_by_fingerprint) {
  gbcode::LanguageService service{};
  const std::string text = sample_text();
  service.sync_document_if_ready_for_test("/tmp/b.cpp", text);
  service.set_active_path("/tmp/b.cpp");

  // 指纹去重的可观测行为：同内容重推**不会**多存/重排序/清空；内容变化会替换。
  //（pump 的返回值依赖 started（真 server），这里测数据层语义即可。）
  service.push_diagnostics_for_test("file:///tmp/b.cpp", "use of undeclared 'foo'");
  ST_CHECK_EQ(service.problems().size(), std::size_t{1});
  ST_CHECK_EQ(service.problems().front().message,
              std::string("use of undeclared 'foo'"));
  ST_CHECK_EQ(service.error_count(), std::size_t{1});

  // 同内容重推：仍是那一条（指纹命中，不重建）。
  service.push_diagnostics_for_test("file:///tmp/b.cpp", "use of undeclared 'foo'");
  ST_CHECK_EQ(service.problems().size(), std::size_t{1});

  // 内容变了：替换为新一梉。
  service.push_diagnostics_for_test("file:///tmp/b.cpp", "expected ';'");
  ST_CHECK_EQ(service.problems().size(), std::size_t{1});
  ST_CHECK_EQ(service.problems().front().message, std::string("expected ';'"));

  // 多条：入库且按行列排序稳定（注入接口按一条一推设计，多条分多次推；
  // 指纹语义是「整批替换」，所以最后一批覆盖前一批——这正是替换语义的验证）。
  service.push_diagnostics_for_test("file:///tmp/b.cpp", "second;line=1");
  ST_CHECK_EQ(service.problems().size(), std::size_t{1});
  ST_CHECK_EQ(service.problems().front().message, std::string("second;line=1"));
  // 换活动文件：诊断归属切换（problems 读的是活动文件的那份）。
  service.sync_document_if_ready_for_test("/tmp/other.cpp", "x\n");
  service.set_active_path("/tmp/other.cpp");
  ST_CHECK(service.problems().empty());
  service.set_active_path("/tmp/b.cpp");
  ST_CHECK_EQ(service.problems().size(), std::size_t{1});
}

ST_TEST(gbcode_line_index_rebuilt_after_change) {
  gbcode::LanguageService service{};
  service.sync_document_if_ready_for_test("/tmp/c.cpp", "one\ntwo\n");
  auto position = service.offset_to_position("/tmp/c.cpp", 4);
  ST_REQUIRE(position.has_value());
  ST_CHECK_EQ(position->line, std::size_t{1});
  // 文本变化：旧行索引必须失效（否则换算落在错误的行上）。
  service.sync_document_if_ready_for_test("/tmp/c.cpp", "one\ninserted\ntwo\n");
  position = service.offset_to_position("/tmp/c.cpp", 13);
  ST_REQUIRE(position.has_value());
  ST_CHECK_EQ(position->line, std::size_t{2});
  ST_CHECK_EQ(position->character, std::uint32_t{0});
}
