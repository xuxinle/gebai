/// 小白内置工具的**判定逻辑**回归（`apps/xiaobai/tool_support.hpp`）。
///
/// ## 为什么这些函数值得单测
///
/// 工具是模型唯一能改变世界的通道，而它的失败模式与普通函数不同——**错得静默**：
/// `edit` 没找到原文却写回一份未改动的文件，模型会以为改成功了；
/// `read` 把二进制当文本吐出来，模型会据此编造"文件内容"。这类错误比崩溃贵得多。
///
/// ## 为什么直接 include 应用的头文件
///
/// 旧测试（`xiaobai_tools_test.cpp` 的开头）是**复制一份同语义实现**来断言——
/// 那测的是"复制品对不对"，实现改坏了它照样绿，等于没有护栏。
/// 这些函数抽进 `tool_support.hpp` 后，应用与测试共用同一份代码。
#include <string>
#include <utility>
#include <vector>

#include "apps/xiaobai/tool_support.hpp"
#include "st/test/test.hpp"

namespace {

namespace tool = xiaobai::tool;

}  // namespace

// ── looks_binary ──────────────────────────────────────────────────────────

ST_TEST(xiaobai_read_rejects_binary_by_nul_probe) {
  // 二进制：含裸 NUL（几乎所有二进制格式都有）
  ST_CHECK(tool::looks_binary(std::string("PNG\x89\0\x1a\n", 9)));
  ST_CHECK(tool::looks_binary(std::string{'a', 'b', '\0', 'c'}));

  // 文本：UTF-8 中文、GBK 字节序列、UTF-16 都不含**裸** NUL 的文本形态
  ST_CHECK(!tool::looks_binary("hello world\n"));
  ST_CHECK(!tool::looks_binary("中文测试：你好世界"));
  ST_CHECK(!tool::looks_binary(""));   // 空文件不是二进制

  // 只看前 8KB：超长文本后面出现 NUL 不算（避免把整个大文件读进来扫）
  std::string late_nul(9000, 'x');
  late_nul[8500] = '\0';
  ST_CHECK(!tool::looks_binary(late_nul));
}

// ── normalize_path / path_escapes_root ────────────────────────────────────

ST_TEST(xiaobai_path_escape_detects_parent_traversal) {
  const std::string root = "/work/project";
  // 正常：根内、子目录、带 `./`
  ST_CHECK(!tool::path_escapes_root(root, "/work/project/a.txt"));
  ST_CHECK(!tool::path_escapes_root(root, "/work/project/sub/a.txt"));
  ST_CHECK(!tool::path_escapes_root(root, "/work/project/./a.txt"));
  // 回退再进来仍然在根内（归一化后）
  ST_CHECK(!tool::path_escapes_root(root, "/work/project/sub/../a.txt"));

  // 越界：`..` 真上溯出去、兄弟目录、绝对路径在外部
  ST_CHECK(tool::path_escapes_root(root, "/work/project/../secret.txt"));
  ST_CHECK(tool::path_escapes_root(root, "/work/other/a.txt"));
  ST_CHECK(tool::path_escapes_root(root, "/etc/passwd"));

  // **前缀不能当包含**：`/work/project2` 不在 `/work/project` 之内
  ST_CHECK(tool::path_escapes_root(root, "/work/project2/a.txt"));
}

ST_TEST(xiaobai_path_escape_is_case_insensitive_on_windows) {
  // Windows 盘符与路径大小写不敏感：`D:\Work` 与 `d:\work` 是同一处
  ST_CHECK(!tool::path_escapes_root("D:\\Work\\Proj", "d:\\work\\proj\\a.txt"));
  ST_CHECK(!tool::path_escapes_root("d:\\work\\proj", "D:\\Work\\Proj\\a.txt"));
  // 但真在外部仍然要报
  ST_CHECK(tool::path_escapes_root("D:\\Work\\Proj", "D:\\Work\\Other\\a.txt"));
}

ST_TEST(xiaobai_path_escape_handles_mixed_separators) {
  // Windows 上 `/` 与 `\` 都合法，混用时归一化必须认得
  ST_CHECK(!tool::path_escapes_root("C:\\proj", "C:/proj/sub/a.txt"));
  ST_CHECK(!tool::path_escapes_root("C:/proj", "C:\\proj\\sub\\a.txt"));
  ST_CHECK(tool::path_escapes_root("C:/proj", "C:\\other\\a.txt"));
}

ST_TEST(xiaobai_path_escape_ignores_empty_root) {
  // 根目录未设（应用启动早期的窗口）：不做任何判定，免得误报
  ST_CHECK(!tool::path_escapes_root("", "/anywhere/a.txt"));
}

// ── similar_lines（edit 命不中时的定位线索）────────────────────────────────

ST_TEST(xiaobai_edit_hint_finds_lines_ignoring_indent) {
  const std::string content =
      "int main() {\n"
      "    return 0;\n"
      "}\n"
      "    if (x) { foo(); }\n";
  // 模型给的是**无缩进**的版本，文件里是四空格缩进 —— 这正是最常见的失配
  const auto hits = tool::similar_lines(content, "if (x) { foo(); }");
  ST_REQUIRE(hits.size() == 1);
  ST_CHECK_EQ(hits[0].first, std::size_t{4});            // 第 4 行
  ST_CHECK(hits[0].second.find("if (x)") != std::string::npos);
}

ST_TEST(xiaobai_edit_hint_caps_at_three_lines) {
  std::string content;
  for (int i = 0; i < 10; ++i) content += "    target();\n";
  const auto hits = tool::similar_lines(content, "target();", 3);
  ST_CHECK_EQ(hits.size(), std::size_t{3});   // 只给前 3 条，不刷屏
  ST_CHECK_EQ(hits[0].first, std::size_t{1});
  ST_CHECK_EQ(hits[1].first, std::size_t{2});
}

ST_TEST(xiaobai_edit_hint_empty_when_nothing_similar) {
  const auto hits = tool::similar_lines("alpha\nbeta\n", "totally different");
  ST_CHECK(hits.empty());   // 没线索就别瞎给（宁缺毋滥）
}

ST_TEST(xiaobai_edit_hint_ignores_blank_needle) {
  // 空白 needle 会让 `find("")` 恒真 —— 必须早退，否则给出满屏无意义的“线索”。
  ST_CHECK(tool::similar_lines("a\nb\nc\n", "").empty());

  // 关键用例：**只含空白的 needle 不得匹配到含空白的行**。
  //
  // 若实现忘了先 trim（直接用原文本去找），`"   "`（三个空格）会命中
  // 文件里的 `"a   b"`——那是一条**毫无意义的“线索”**：模型拿到它会以为
  // 要匹配的文本就在那行，实际只是空白巧合。
  const auto hits = tool::similar_lines("alpha   beta\ngamma   delta\n", "   ");
  ST_CHECK(hits.empty());
}

// ── trim_ws ───────────────────────────────────────────────────────────────

ST_TEST(xiaobai_trim_ws_basic) {
  ST_CHECK_EQ(tool::trim_ws("  abc  "), std::string("abc"));
  ST_CHECK_EQ(tool::trim_ws("\t\na\r\n"), std::string("a"));
  ST_CHECK_EQ(tool::trim_ws("   "), std::string(""));
  ST_CHECK_EQ(tool::trim_ws("abc"), std::string("abc"));
  // 只去首尾，不碰中间
  ST_CHECK_EQ(tool::trim_ws("  a b  "), std::string("a b"));
}
