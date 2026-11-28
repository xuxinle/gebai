/// 内置字体**按需裁剪**判据的回归测试。
///
/// 需求来源（用户）：内置字体（Fira Code / Inter）**只有用到它的应用才链接进产物**。
///
/// 判据是“目标自己的源里有没有那些会按等宽/比例字体绘制的组件”（见 `build.cpp` 的
/// `target_uses_mono_font` / `target_uses_text_ui`）。这里钉的是判据的**词边界语义**——
/// 它决定了“哪些应用会被判成用到”，写错的两个方向都有代价：
/// - **漏判**：应用里代码区退化成系统等宽字体（不报错，只是外观不统一）；
/// - **误判**：白背 289 KB（甚至 830 KB）的字体数据。
///
/// 实测过的坑：注释里出现 `ThreeView` 曾把 1254 KB 的 three.js 骗进 `cefu`——
/// 所以判据必须只看**净化后**的代码文本（`lex::strip_to_code`），不是原始源码。
/// 本文件直接测 `mentions_identifier` 的词边界部分（净化在上层完成，不在此重复）。

#include "st/test/test.hpp"

#include <string>

#include "st/pkg/build.hpp"

ST_TEST(mono_judgement_matches_component_type_names) {
  using st::pkg::mentions_identifier;
  // 真实用法：类型名/构造/模板实参——都该命中。
  for (const std::string_view text : {
           "auto* ed = new CodeEditor();",
           "st::ui::CodeEditor* editor{nullptr};",
           "auto view = std::make_unique<DiffView>();",
           "Terminal* terminal = nullptr;",
           "MergeView merge;",
           "MarkdownView md;",
           "HtmlView html;",
       }) {
    const bool hit = mentions_identifier(text, "CodeEditor") ||
                     mentions_identifier(text, "Terminal") || mentions_identifier(text, "DiffView") ||
                     mentions_identifier(text, "MergeView") ||
                     mentions_identifier(text, "MarkdownView") ||
                     mentions_identifier(text, "HtmlView");
    ST_CHECK(hit);
  }
}

ST_TEST(mono_judgement_ignores_longer_identifiers) {
  using st::pkg::mentions_identifier;
  // **词边界**：更长的标识符含子串但**不是同一个东西**，不该命中。
  // 这条是误判防线：`CodeEditorPage` 是 gbcode 的页面类（它确实用编辑器，
  // 但判据不该靠“名字里含 CodeEditor”来撞对——那种巧合会在改名后静默失效）。
  ST_CHECK(!mentions_identifier("struct CodeEditorPage;", "CodeEditor"));
  ST_CHECK(!mentions_identifier("class TerminalEmulator;", "Terminal"));
  ST_CHECK(!mentions_identifier("int my_MarkdownViewCount = 0;", "MarkdownView"));
  ST_CHECK(!mentions_identifier("void DiffViewer();", "DiffView"));
  // 但独立出现时仍必须命中（前一条是“含子串”，这一条是“整词”）。
  ST_CHECK(mentions_identifier("CodeEditor* p;", "CodeEditor"));
}

ST_TEST(mono_judgement_handles_empty_and_short_text) {
  using st::pkg::mentions_identifier;
  ST_CHECK(!mentions_identifier("", "CodeEditor"));
  ST_CHECK(!mentions_identifier("int x;", "CodeEditor"));
  ST_CHECK(!mentions_identifier("Terminal", ""));   // 空标识符：永不命中（不是“什么都命中”）
}
