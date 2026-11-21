// ============================================================================
// 词法净化：注释与字符串里的名字**不算使用**——回归用例
//
// ## 钉住的是什么缺陷（实测代价）
//
// 决定“要不要给目标加 `ST_NO_THREE` / `ST_NO_SCRIPT` / `ST_NO_WEBVIEW`”的判据是
// “目标源码里有没有那个标识符”。第一版直接在**原文**上找，于是：
//
//   `apps/cefu/src/main.cpp:855`  `// 为何：ThreeView 需要真的三维后端…`
//   `apps/cefu/src/main.cpp:1044` `// （ThreeView 的 tick_animation…`
//
// 两行**注释**让 cefu 被判成“用三维”，1254 KB 的 `three.cjs` 白带了进去。
// 加注释、改注释、留 TODO —— 任何文字工作都会悄悄改变产物体积，
// 而这件事**没有任何编译或链接报错**。
//
// ## 为什么钉 `strip_to_code`
//
// 它是能力判定的输入。只要它把注释与字符串字面量的内容清掉，
// 上层 `mentions_identifier` 就不可能被文字骗到。两个断言方向都要有：
// - 注释/字符串里的名字 ⇒ 剥离后**不出现**；
// - 真代码里的名字 ⇒ 剥离后**仍在**（否则会把真用途也裁掉，那是功能丢失）。
// ============================================================================

#include <string>

#include "st/pkg/build.hpp"
#include "st/pkg/cpp_lex.hpp"
#include "st/test/test.hpp"

namespace {

using st::pkg::lex::strip_to_code;
using st::pkg::mentions_identifier;

/// 便捷判据：净化后是否还“用”了这个标识符。
auto uses_after_strip(const std::string& source, const std::string& ident) -> bool {
  return mentions_identifier(strip_to_code(source), ident);
}

}  // namespace

ST_TEST(cpp_lex_comments_are_not_usage) {
  // 这是缺陷的原始形态。
  const std::string source = R"(// 为何：ThreeView 需要真的三维后端（离屏纹理 + 合成）
void f() { run(); }
/* ThreeView 的 tick_animation 也依赖它 */
)";
  ST_CHECK(!uses_after_strip(source, "ThreeView"));
  // 注释剥离后真代码仍在（别把整个文件清空）。
  ST_CHECK(uses_after_strip(source, "run"));
}

ST_TEST(cpp_lex_string_literals_are_not_usage) {
  // 字符串字面量里提到组件名，也不代表用了它（日志文案、错误消息都是这种）。
  const std::string source = R"(
void f() {
  log("WebView 不支持");
  const char* kName = "ScriptHost";
}
)";
  ST_CHECK(!uses_after_strip(source, "WebView"));
  ST_CHECK(!uses_after_strip(source, "ScriptHost"));
}

ST_TEST(cpp_lex_real_usage_survives) {
  // 真代码必须留下：漏判的代价是“该用的被裁掉”，比多带 1 MB 严重得多。
  const std::string source = R"(
#include "st/ui/components/webview.hpp"
auto make() { return std::make_unique<st::ui::WebView>(); }
)";
  ST_CHECK(uses_after_strip(source, "WebView"));
  ST_CHECK(!uses_after_strip(source, "WebViewPort"));  // 词边界仍然有效
}

ST_TEST(cpp_lex_raw_string_content_is_not_code) {
  // 裸字符串里放着一大段 JS/Python，里面出现什么名字都不算 C++ 侧的使用。
  // 这条同时守住“裸串里的 `//` 不是行注释”（历史坑：那会让状态机卡到文件末尾）。
  const std::string source = R"CPP(
const char* kScript = R"JS(
  // ThreeView 在这里只是 JS 文本
  var x = 1;
)JS";
void after_raw() { ThreeScene scene; }
)CPP";
  ST_CHECK(!uses_after_strip(source, "ThreeView"));
  // 裸串结束后状态必须复位，后面那段真代码要被看到。
  ST_CHECK(uses_after_strip(source, "ThreeScene"));
}

ST_TEST(cpp_lex_string_content_is_blanked_not_deleted) {
  // 字符串**内容**用空格顶位（而不是删掉）：同一行后续字符的位置不会错位。
  // 这是该视图存在的前提（行内判断基于列号）。
  // 但**行注释**是整段丢弃（那是行的剩余部分，后面没有代码了）——两件事不一样，
  // 本条只钉字符串那一半。
  const std::string source = "auto s = \"WebView\";";
  const std::string code = strip_to_code(source);
  ST_CHECK_EQ(code.size(), source.size());
  ST_CHECK(!mentions_identifier(code, "WebView"));
  // 引号本身与赋值语句必须留着（位置对得上）。
  ST_CHECK(code.find("auto s = \"") == 0);
}

ST_TEST(cpp_lex_line_comments_do_not_leak_across_lines) {
  // 行注释只吞到行尾，下一行照常是代码。
  const std::string source = "// WebView\nWebView* p = nullptr;\n";
  ST_CHECK(uses_after_strip(source, "WebView"));
  const std::string only_comment = "int a = 1; // WebView\nint b = 2;\n";
  ST_CHECK(!uses_after_strip(only_comment, "WebView"));
}
