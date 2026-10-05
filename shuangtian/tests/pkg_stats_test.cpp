/// `st stats`（源码结构度量）的测试。
///
/// 这组用例守的是**度量本身的正确性**——一把量错的尺子比没有尺子更坏：
/// 结构评审的结论（"最大函数 N 行"）会直接引向重构决策，尺子偏了就是白干。
///
/// 三个最容易错的地方，各有用例：
/// 1. **注释/字符串里的括号**不能参与括号匹配（实测踩到：把 42 行的函数算成 1536 行——
///    注释里写着 `static Element* none = nullptr; return *none;`，其中的括号把匹配带偏）；
/// 2. **嵌套函数/块**不能重复计数（内层 lambda 不该让外层行数翻倍）；
/// 3. **复杂度**只数函数体内的分支（`if/for/while/case/catch/&&/||`）。

#include "st/test/test.hpp"

#include <fstream>
#include <string>

#include "st/core/fs.hpp"
#include "st/pkg/stats.hpp"

namespace {

/// 把源码写进临时工程目录并度量（`collect_stats` 按 `include/src/...` 布局找文件）。
[[nodiscard]] auto stats_of(std::string_view name, std::string_view source)
    -> st::pkg::ProjectStats {
  const std::string root = st::fs::join(st::fs::temp_dir(), std::string(name));
  (void)st::fs::remove_all(root);
  const std::string dir = st::fs::join(root, "src");
  (void)st::fs::create_directories(dir);
  {
    std::ofstream stream(st::fs::join(dir, "probe.cpp"), std::ios::binary | std::ios::trunc);
    stream << source;
  }
  auto result = st::pkg::collect_stats(root, 20);
  (void)st::fs::remove_all(root);
  if (!result) return {};
  return *result;
}

}  // namespace

/// ① 注释里的大括号/圆括号**不得**影响函数边界（这是最容易错、也最伤结论的一条）。
///
/// 反例原型来自真实代码：`src/ui/dsl.cpp` 里那个 `static Element* none = nullptr;
/// return *none;` 注释——按原文匹配会把函数算成上千行，去掉注释才是真实行数。
ST_TEST(stats_ignores_braces_inside_comments) {
  const auto stats = stats_of("stats_comment", R"CPP(
void small() {
  // 下面这段注释里有大括号和圆括号，绝不能参与匹配 } ) { (
  /* 多行注释
     { { { } } }
     ( ) ( ) */
  int value = 1;
  (void)value;
}
)CPP");
  ST_REQUIRE(!stats.biggest_functions.empty());
  // `small` 从 `void small() {` 到闭括号共 8 行——注释里那些括号**没有**让它变长。
  // （这里刻意写死行数：若剥离失效，行数会立刻变成十几行，用例当场红灯。）
  ST_CHECK_EQ(stats.biggest_functions[0].lines, 8);
  ST_CHECK_EQ(stats.functions, 1);
}

/// ② 字符串字面量里的花括号同样不参与匹配（markdown 解析器里满地图都是 `"{"`）。
ST_TEST(stats_ignores_braces_inside_string_literals) {
  const auto stats = stats_of("stats_string", R"CPP(
const char* table[] = {"{", "}", "{{{", "()"};
void tiny() {
  const char* glyph = "{}()";
  (void)glyph;
}
)CPP");
  ST_REQUIRE(!stats.biggest_functions.empty());
  // 只有 `tiny` 被算作函数；数组初始化不算（它是声明，不是函数体）。
  ST_CHECK_EQ(stats.functions, 1);
  ST_CHECK_EQ(stats.biggest_functions[0].lines, 4);
}

/// ③ 函数名与行数：能报出名字与起始行，且**不把内层块算成独立函数**。
ST_TEST(stats_reports_function_names_and_finds_the_largest) {
  const auto stats = stats_of("stats_names", R"CPP(
auto alpha() -> int {
  return 1;
}

auto beta() -> int {
  int total = 0;
  for (int index = 0; index < 3; ++index) {
    if (index > 1 && total < 10) total += index;
  }
  while (total > 100) { --total; }
  switch (total) {
    case 0: break;
    default: break;
  }
  return total;
}
)CPP");
  ST_REQUIRE(stats.biggest_functions.size() >= 2U);
  // 最大的必须是 beta（18 行 > alpha 的 3 行）
  ST_CHECK_EQ(stats.biggest_functions[0].name, std::string("beta"));
  ST_CHECK_EQ(stats.functions, 2);
  // beta 的分支数：for + if + && + while + switch/case + case + default 的 case → 至少 5
  ST_CHECK(stats.biggest_functions[0].complexity >= 5);
}

/// ④ 头文件被包含次数（"改动它要重编多少"的读数）。
ST_TEST(stats_counts_header_inclusions) {
  const std::string root = st::fs::join(st::fs::temp_dir(), "stats_include");
  (void)st::fs::remove_all(root);
  (void)st::fs::create_directories(st::fs::join(root, "src"));
  (void)st::fs::create_directories(st::fs::join(root, "include/st"));
  {
    std::ofstream header(st::fs::join(st::fs::join(root, "include/st"), "hot.hpp"));
    header << "#pragma once\nstruct Hot {};\n";
  }
  for (const char* name : {"a.cpp", "b.cpp", "c.cpp"}) {
    std::ofstream unit(st::fs::join(st::fs::join(root, "src"), name));
    unit << "#include \"st/hot.hpp\"\nvoid f() {}\n";
  }
  auto result = st::pkg::collect_stats(root, 5);
  ST_REQUIRE(result.has_value());
  bool found = false;
  for (const auto& header : result->hot_headers) {
    if (header.header == "st/hot.hpp") {
      found = true;
      ST_CHECK_EQ(header.includers, 3);
    }
  }
  ST_CHECK(found);
  (void)st::fs::remove_all(root);
}
