/// 代码编辑器**行距倍数**的回归测试。
///
/// 需求来源（用户）：「代码编辑器组件的行间距加大点」。
///
/// 行高来自字体度量（`ascender - descender + line_gap`），没有系数可调；因此组件
/// 提供 `set_line_spacing(multiple)`——**行距倍数**，默认 `1.15`（代码行比正文更需要
/// 透气：注释/字符串/嵌套在密排下容易糊成一片）。
///
/// 本组用例只走**公开面**（`line_spacing()` / `layout_dirty()` / 滚动极限），
/// 钉住四件事：
/// ① 默认档位**确实大于 1.0**——"加大行距"是需求本身，把它钉住，
///    否则后人把默认改回 1.0 不会有任何测试变红；
/// ② 改行距**必须标布局脏**：行高是滚动极限/命中测试/内容高度的共同输入，
///    不标脏就会画出错位的行、或滚不到底（同 `set_font_scale` 的既有约束）；
/// ③ 非法值（0/负/NaN）被忽略而非静默接受——静默接受会让整屏文字叠成一行；
/// ④ **行距增量两侧分摊**（见 `line_block_offset`）：行距只该改变**行间**空隙，
///    不该把字形在自己的行盒里往下推——用户报的"行间距怎么全在行下方"就是这条。
#include "st/test/test.hpp"

#include <cmath>
#include <limits>
#include <string>

#include "st/app/text_port.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::CodeEditor;
using st::ui::RenderContext;
using st::ui::Theme;

/// 排一次以填充几何缓存（`line_height`/滚动极限都在这条路径上算出来）。
void layout_once(CodeEditor& editor, const Theme& theme) {
  editor.arrange(RenderContext{theme, nullptr, 0.0},
                 st::math::Rect{0.0F, 0.0F, 400.0F, 200.0F});
}

}  // namespace

ST_TEST(code_editor_line_spacing_default_widens_beyond_natural_leading) {
  CodeEditor editor;
  editor.apply_theme(Theme::light());
  // 需求就是"加大行距"：默认必须**大于**自然行高（= 1.0）。
  // 只断言"等于某常量"是不够的——那个常量被改成 1.0 也照样绿。
  ST_CHECK(editor.line_spacing() > 1.0F);
  ST_CHECK(std::abs(editor.line_spacing() - CodeEditor::kDefaultLineSpacing) < 0.001F);
}

ST_TEST(code_editor_line_spacing_change_marks_layout_dirty) {
  // 行距是**排版输入**：改了它，行高/滚动极限/内容高度全部要重算。
  // 本组件特意让布局脏不冒泡（否则每次编辑都拖出整帧重绘），
  // 所以"有没有标脏"得读它自己的 `layout_dirty()`。
  CodeEditor editor;
  const Theme theme = Theme::light();
  editor.apply_theme(theme);
  editor.set_text("a\nb\nc\nd\ne\nf\ng\nh");
  layout_once(editor, theme);
  editor.clear_dirty();
  ST_CHECK(!editor.layout_dirty());

  editor.set_line_spacing(1.4F);
  ST_CHECK(editor.layout_dirty());
  ST_CHECK(std::abs(editor.line_spacing() - 1.4F) < 0.001F);
}

ST_TEST(code_editor_line_spacing_accepted_value_survives_relayout) {
  // 行距是"持久配置"（宿主设置项），排一次不能被抹回默认——
  // 与 `font_scale`/`read_only` 同一类约束（实测同类缺陷：每帧无条件写会把
  // 控制通道设的值当场抹掉）。
  CodeEditor editor;
  const Theme theme = Theme::light();
  editor.apply_theme(theme);
  editor.set_line_spacing(1.5F);
  layout_once(editor, theme);
  ST_CHECK(std::abs(editor.line_spacing() - 1.5F) < 0.001F);
  layout_once(editor, theme);
  ST_CHECK(std::abs(editor.line_spacing() - 1.5F) < 0.001F);
}

ST_TEST(code_editor_line_spacing_rejects_invalid_values) {
  CodeEditor editor;
  editor.apply_theme(Theme::light());
  const float baseline = editor.line_spacing();

  editor.set_line_spacing(0.0F);
  ST_CHECK(std::abs(editor.line_spacing() - baseline) < 0.001F);
  editor.set_line_spacing(-2.0F);
  ST_CHECK(std::abs(editor.line_spacing() - baseline) < 0.001F);
  editor.set_line_spacing(std::numeric_limits<float>::quiet_NaN());
  ST_CHECK(std::abs(editor.line_spacing() - baseline) < 0.001F);
  editor.set_line_spacing(std::numeric_limits<float>::infinity());
  ST_CHECK(std::abs(editor.line_spacing() - baseline) < 0.001F);
  // 合法值仍然接受
  editor.set_line_spacing(1.25F);
  ST_CHECK(std::abs(editor.line_spacing() - 1.25F) < 0.001F);
}

ST_TEST(code_editor_line_spacing_does_not_touch_font_size) {
  // 两个旋钮正交：改行距不该动字号（反之亦然）。捆在一起会让"设置面板里
  // 只调行距"变成"字号也变了"，用户无从察觉原因。
  CodeEditor editor;
  const Theme theme = Theme::light();
  editor.apply_theme(theme);
  const float size_before = editor.font_size();
  editor.set_line_spacing(1.6F);
  ST_CHECK(std::abs(editor.font_size() - size_before) < 0.001F);
  editor.set_font_scale(1.3F);
  ST_CHECK(std::abs(editor.line_spacing() - 1.6F) < 0.001F);
}
