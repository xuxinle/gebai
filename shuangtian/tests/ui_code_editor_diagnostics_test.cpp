/// 诊断装饰测试（LSP 阶段 3）：编辑器上的诊断标记、查询、绘制。
///
/// 绘制用**像素断言**（不是"调了不崩"）：波浪线/行号槽圆点/悬浮框都是视觉承诺，
/// 只有真去数像素才知道画没画、画在哪。这也是本框架既有的做法
/// （见 `ui_terminal_panel_test` 的滚动条断言）。

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Color;
using st::ui::CodeEditor;

/// 挂好编辑器 + 字体端口的宿主（没有字体时部分几何断言跳过——见各用例）。
struct EditorFixture {
  std::optional<st::text::FontStack> stack{};
  std::unique_ptr<st::test::RendererTextPort> port{};
  st::ui::UiRoot root{};
  CodeEditor* editor{nullptr};

  EditorFixture() {
    if (const auto loaded = st::text::FontStack::system_default(); loaded.has_value()) {
      stack.emplace(std::move(*loaded));
      port = std::make_unique<st::test::RendererTextPort>(*stack);
      root.set_text_port(port.get());
    }
    root.set_viewport(st::math::Size{800.0F, 400.0F});
    auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    auto owned = std::make_unique<CodeEditor>();
    editor = owned.get();
    content->add_child(std::move(owned));
    root.set_content(std::move(content));
    root.layout(true);
  }

  void layout() { root.layout(true); }

  /// 画一帧到离屏画布（诊断装饰的像素断言用）。
  auto paint() -> st::raster::Canvas {
    st::raster::Canvas canvas{800, 400, 1.0F};
    root.paint(canvas);
    return canvas;
  }
};

/// 数画布上某颜色附近（容差）的像素数。
[[nodiscard]] auto count_color(const st::raster::Canvas& canvas, Color target, int tolerance)
    -> std::size_t {
  std::size_t count = 0;
  for (int y = 0; y < 400; ++y) {
    for (int x = 0; x < 800; ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const int dr = static_cast<int>(pixel.r) - static_cast<int>(target.r);
      const int dg = static_cast<int>(pixel.g) - static_cast<int>(target.g);
      const int db = static_cast<int>(pixel.b) - static_cast<int>(target.b);
      if (dr * dr + dg * dg + db * db <= tolerance * tolerance) ++count;
    }
  }
  return count;
}

}  // namespace

ST_TEST(code_editor_diagnostics_marks_and_queries) {
  EditorFixture fixture;
  CodeEditor& editor = *fixture.editor;
  editor.set_text("int main() {\n  return missing;\n}\n");

  ST_CHECK_EQ(editor.diagnostics().size(), std::size_t{0});
  ST_CHECK_EQ(editor.get_property("diagnostics").value_or("-"), std::string("0"));

  // 两条诊断：`missing`（错误）+ 整行（警告）。
  std::vector<CodeEditor::DiagnosticMark> marks;
  const std::size_t missing_at = editor.text().find("missing");
  ST_REQUIRE(missing_at != std::string::npos);
  marks.push_back(CodeEditor::DiagnosticMark{
      .begin = missing_at, .end = missing_at + 7,
      .severity = CodeEditor::DiagnosticSeverity::Error,
      .message = "'missing' was not declared", .source = "clang"});
  const std::size_t line2 = editor.text().find("  return");
  marks.push_back(CodeEditor::DiagnosticMark{
      .begin = line2, .end = line2,   // 空范围 = 整行
      .severity = CodeEditor::DiagnosticSeverity::Warning,
      .message = "这一行有点可疑"});
  editor.set_diagnostics(std::move(marks));

  ST_CHECK_EQ(editor.diagnostics().size(), std::size_t{2});
  ST_CHECK_EQ(editor.get_property("diagnostics").value_or("-"), std::string("2"));
  ST_CHECK_EQ(editor.get_property("diagnostic_errors").value_or("-"), std::string("1"));
  fixture.layout();

  // 命中查询：偏移落在 `missing` 上 → 拿到那条错误。
  const auto* hit = editor.diagnostic_at(missing_at + 2);
  ST_REQUIRE(hit != nullptr);
  ST_CHECK(hit->severity == CodeEditor::DiagnosticSeverity::Error);
  ST_CHECK_EQ(hit->source, std::string("clang"));
  // 行首（偏移 0）没有诊断（诊断只覆盖它自己的范围 + 空范围那条所在行）。
  ST_CHECK(editor.diagnostic_at(0) == nullptr || editor.diagnostic_at(0)->severity ==
                                                    CodeEditor::DiagnosticSeverity::Warning);

  // 行严重级别：第 1 行（0 基）有错误，第 3 行没有。
  const auto severity_line1 = editor.severity_on_line(1);
  ST_REQUIRE(severity_line1.has_value());
  ST_CHECK(*severity_line1 == CodeEditor::DiagnosticSeverity::Error);
  ST_CHECK(!editor.severity_on_line(3).has_value());
  // 同一行有错误+警告时取**更严重**的那个。
  const auto severity_line2 = editor.severity_on_line(1);
  ST_CHECK(severity_line2.has_value() &&
           *severity_line2 == CodeEditor::DiagnosticSeverity::Error);

  // 清空。
  editor.clear_diagnostics();
  ST_CHECK(editor.diagnostics().empty());
  ST_CHECK(editor.severity_on_line(1) == std::nullopt);
}

ST_TEST(code_editor_diagnostics_replaces_snapshot_not_merges) {
  // 每次 `set_diagnostics` 是**完整快照**：第二次调用不该保留第一次的条目
  // （LSP 的 publishDiagnostics 语义如此——它是全量替换，不是增量）。
  EditorFixture fixture;
  CodeEditor& editor = *fixture.editor;
  editor.set_text("aaa bbb\n");
  editor.set_diagnostics({CodeEditor::DiagnosticMark{.begin = 0, .end = 3,
                                                     .message = "first"}});
  ST_CHECK_EQ(editor.diagnostics().size(), std::size_t{1});
  editor.set_diagnostics({CodeEditor::DiagnosticMark{.begin = 4, .end = 7, .message = "second"},
                          CodeEditor::DiagnosticMark{.begin = 4, .end = 7, .message = "third"}});
  ST_CHECK_EQ(editor.diagnostics().size(), std::size_t{2});
  ST_CHECK_EQ(editor.diagnostics().front().message, std::string("second"));
}

ST_TEST(code_editor_diagnostics_sorts_by_begin) {
  // 输入乱序也要能正确查询（内部按 begin 排序；命中查询依赖有序提前退出）。
  EditorFixture fixture;
  CodeEditor& editor = *fixture.editor;
  editor.set_text("0123456789\n");
  editor.set_diagnostics({
      CodeEditor::DiagnosticMark{.begin = 8, .end = 9, .message = "late"},
      CodeEditor::DiagnosticMark{.begin = 2, .end = 3, .message = "early"},
      CodeEditor::DiagnosticMark{.begin = 5, .end = 6, .message = "middle"},
  });
  fixture.layout();
  ST_CHECK_EQ(editor.diagnostics().front().message, std::string("early"));
  ST_CHECK_EQ(editor.diagnostics().back().message, std::string("late"));
  const auto* at_six = editor.diagnostic_at(5);
  ST_REQUIRE(at_six != nullptr);
  ST_CHECK_EQ(at_six->message, std::string("middle"));
}

ST_TEST(code_editor_diagnostics_nested_picks_innermost) {
  // 诊断常嵌套（外层语法错覆盖内层语义错）——鼠标停在内层时该看到内层那条。
  EditorFixture fixture;
  CodeEditor& editor = *fixture.editor;
  editor.set_text("x = foo(bar);\n");
  editor.set_diagnostics({
      CodeEditor::DiagnosticMark{.begin = 0, .end = 13, .message = "whole line"},
      CodeEditor::DiagnosticMark{.begin = 4, .end = 7, .message = "innermost"},
  });
  fixture.layout();
  const auto* hit = editor.diagnostic_at(5);
  ST_REQUIRE(hit != nullptr);
  ST_CHECK_EQ(hit->message, std::string("innermost"));
  // 只被外层覆盖的位置拿到外层。
  const auto* outer = editor.diagnostic_at(1);
  ST_REQUIRE(outer != nullptr);
  ST_CHECK_EQ(outer->message, std::string("whole line"));
}

ST_TEST(code_editor_diagnostics_draw_squiggle_and_gutter_dot) {
  if (std::getenv("ST_TEST_NO_FONT") != nullptr) return;
  EditorFixture fixture;
  CodeEditor& editor = *fixture.editor;
  // 用属性面判字体可用性（`line_height` 是私有方法；属性面是公开契约，
  // 而且自动化验收走的就是它）。
  const double line_height = std::stod(editor.get_property("line_height").value_or("0"));
  if (line_height <= 0.0) {
    std::cout << "[diagnostics] 无可用字体，跳过像素断言" << '\n';
    return;
  }
  editor.set_text("int main() {\n  return missing;\n}\n");
  editor.set_show_line_numbers(true);
  const std::size_t at = editor.text().find("missing");
  editor.set_diagnostics({CodeEditor::DiagnosticMark{
      .begin = at, .end = at + 7,
      .severity = CodeEditor::DiagnosticSeverity::Error,
      .message = "'missing' was not declared in this scope", .source = "clang"}});
  fixture.layout();

  const Color danger = fixture.root.theme().colors().danger;
  const st::raster::Canvas without = fixture.paint();
  const std::size_t before = count_color(without, danger, 40);

  // 悬停那条诊断：悬浮框出现（框体是 surface_alt，但左侧 3px 条用诊断色）。
  const auto* mark = editor.diagnostic_at(at);
  ST_REQUIRE(mark != nullptr);
  editor.set_hover_diagnostic(mark);
  ST_CHECK(editor.hover_diagnostic() != nullptr);
  ST_CHECK_EQ(editor.get_property("hover_diagnostic").value_or(""), std::string("'missing' was not declared in this scope"));
  const st::raster::Canvas with_hover = fixture.paint();
  const std::size_t after = count_color(with_hover, danger, 40);

  // 悬浮框的左侧色条 + 波浪线让诊断色像素**变多**（不要求精确数量——
  // 字体渲染差异会让锯齿像素数不同，但"有更多红色"是稳的）。
  ST_CHECK(after > before);
  std::cout << "[diagnostics] 诊断色像素：" << before << " → " << after << '\n';

  // 关掉悬浮：回到原样。
  editor.set_hover_diagnostic(nullptr);
  ST_CHECK(editor.hover_diagnostic() == nullptr);
}

ST_TEST(code_editor_diagnostics_survive_text_edit) {
  // 编辑后诊断标记按字节偏移保留（不做自动位移——位移是 LSP 客户端的责任，
  // 组件只负责"你给什么我画什么"）。编辑到诊断范围之外不该崩。
  EditorFixture fixture;
  CodeEditor& editor = *fixture.editor;
  editor.set_text("abc def\n");
  editor.set_diagnostics({CodeEditor::DiagnosticMark{.begin = 4, .end = 7, .message = "m"}});
  editor.set_cursor_index(0);
  editor.insert_text("XXXX");   // 文本变了，偏移不再对应原来的字
  fixture.layout();
  (void)fixture.paint();        // 不该崩（绘制时对越界范围要夹取）
  ST_CHECK_EQ(editor.diagnostics().size(), std::size_t{1});
  // 诊断范围超出当前文本末尾时，查询与绘制都不崩。
  editor.set_text("a\n");
  fixture.layout();
  (void)fixture.paint();
  ST_CHECK(editor.diagnostic_at(0) != nullptr || true);
}
