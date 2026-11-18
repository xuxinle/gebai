/// `MarkdownView` 含**代码块**时也要能收缩到内容宽。
///
/// ## 缺陷现场
///
/// `intrinsic_content_width()` 旧实现对 `CodeLine` / `TableRow` **直接 `return 0`**
/// （理由：代码底板会铺满可用宽，用它算“内容宽”恒等于上限）。
/// 后果是**任何含代码块的内容都放弃收缩**：
/// 工具卡片里一条 `echo hello` 的输出（23 字符）也会把卡片撑到整行宽
/// （实测：1106px），用户看到一只巨大的空卡片包着三行小字。
///
/// ## 修法
///
/// 代码/表格行的**文字**仍有自然宽：底板虽然铺满，但文字只占左边一截。
/// 于是照常累加 `text_x + 各 span 宽`，再补一份与 `text_x` 同量的右侧留白
/// （底板左右对称，否则文字会贴着右边缘）。
///
/// 保留的护栏：**只要有任何一行是自动折行，仍然放弃收缩**——
/// 折行后的行碎片算不出“自然宽”，硬算会把内容压成细长条（那是另一个已修缺陷）。
#include <string>

#include "st/test/test.hpp"

#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::ui::Constraints;
using st::ui::MarkdownView;
using st::ui::RenderContext;
using st::ui::Theme;

constexpr float kAvailableWidth = 900.0f;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};
  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

[[nodiscard]] auto available_constraints() -> Constraints {
  Constraints constraints;
  constraints.max_width = kAvailableWidth;
  constraints.available_width = kAvailableWidth;
  return constraints;
}

}  // namespace

ST_TEST(md_view_code_block_shrinks_to_content) {
  // **核心回归**：短代码块必须收缩（不再撑满可用宽）。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(12.0f);
  view.set_markdown("```\nhello\n[exit 0]\n```");

  view.measure(fixture.context(), available_constraints());
  const float width = view.measured_size().width;

  ST_CHECK(width > 0.0f);
  // 8 字符 × 8px + 两侧留白，远小于 900。
  ST_CHECK(width < kAvailableWidth * 0.5f);
}

ST_TEST(md_view_code_block_still_expands_when_long) {
  // **单行超宽**的代码必须占满可用宽——不能因为“能收缩”就把长代码压细。
  //
  // ⚠ 关键是**单行**超宽：多行短代码各占一行时，最长那行仍可能装得下，
  // 那时收缩是正确行为（实测踩过：写 40 行中等长度代码，每行都不超宽，
  // 断言却期望“铺满”，测试自己就是错的）。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(12.0f);
  std::string code = "```\n";
  code += std::string(300, 'x');   // 一行 300 字符，远超 900px 可用宽
  code += "\n```";
  view.set_markdown(code);

  view.measure(fixture.context(), available_constraints());
  ST_CHECK(view.measured_size().width >= kAvailableWidth - 1.0f);
}

ST_TEST(md_view_code_block_width_grows_with_longest_line) {
  // 更长的代码行要得到更宽的气泡（宽度跟着内容走，不是固定值）。
  Fixture short_code;
  MarkdownView narrow;
  narrow.set_shrink_to_fit(true);
  narrow.set_base_font_size(12.0f);
  narrow.set_markdown("```\nab\n```");
  narrow.measure(short_code.context(), available_constraints());
  const float narrow_width = narrow.measured_size().width;

  Fixture long_code;
  MarkdownView wide;
  wide.set_shrink_to_fit(true);
  wide.set_base_font_size(12.0f);
  wide.set_markdown("```\nabcdefghijklmnopqrstuvwxyz\n```");
  wide.measure(long_code.context(), available_constraints());
  const float wide_width = wide.measured_size().width;

  ST_CHECK(narrow_width > 0.0f);
  ST_CHECK(wide_width > narrow_width + 8.0f);
  ST_CHECK(wide_width < kAvailableWidth * 0.5f);
}
