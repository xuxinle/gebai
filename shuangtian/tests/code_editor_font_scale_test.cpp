/// 代码编辑器**字号跟随主题**的回归测试。
///
/// 需求来源（用户）：「代码编辑组件默认字体调大一号」。
///
/// 调字号本身是一行常量，但排查时发现底下的**结构性缺陷**：本组件曾经把字号
/// 硬写成绝对像素（`style_.font_size = 13.0f`），而主题才是缩放的真值源
/// （`--ui-font-scale` → `Metrics::scale_fonts`）。后果：用户把 UI 缩放调大后，
/// 普通文字跟着放大、**编辑器纹丝不动**（实测 `--ui-font-scale 1.5` 时
/// UI 文字 15→22.5，编辑器恒为 13.5）。
///
/// 因此本组用例钉的是**关系**而不是数值：
/// ① 默认档位 = 1.0 → 实际字号 == 主题 `font_base`；
/// ② 主题基准字号变化（模拟缩放）→ 实际字号**按同一比例**跟上去；
/// ③ 档位与显式像素互斥：设了显式像素就忽略档位，复位后又能跟主题；
/// ④ 档位会被夹取到可读区间（0 会让整屏文字消失）。

#include "st/test/test.hpp"

#include <cmath>
#include <memory>

#include "st/text/text.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::CodeEditor;
using st::ui::Theme;

/// 造一个可施加“缩放”的主题（`--ui-font-scale` 走的就是 `Metrics::scale_fonts`）。
[[nodiscard]] auto theme_with_scale(float factor) -> Theme {
  Theme theme = Theme::light();
  theme.metrics().scale_fonts(factor);
  return theme;
}

}  // namespace

ST_TEST(code_editor_font_scale_defaults_to_theme_base) {
  CodeEditor editor;
  const Theme theme = Theme::light();
  editor.apply_theme(theme);
  // 默认档位 1.0：实际字号就是主题正文大小（与 `Text` 的缺省一致）
  ST_CHECK(std::abs(editor.font_scale() - 1.0F) < 0.001F);
  ST_CHECK(std::abs(editor.font_size() - theme.metrics().font_base) < 0.001F);
}

ST_TEST(code_editor_font_follows_theme_scaling) {
  // **核心回归**：主题缩放后编辑器必须跟着放大，而不是停在旧像素上。
  CodeEditor editor;
  const Theme base_theme = Theme::light();
  editor.apply_theme(base_theme);
  const float before = editor.font_size();

  const Theme scaled_theme = theme_with_scale(1.5F);
  editor.apply_theme(scaled_theme);
  const float after = editor.font_size();

  ST_CHECK(scaled_theme.metrics().font_base > base_theme.metrics().font_base);
  ST_CHECK(after > before);
  // 与主题同比例（不是“变大了就算对”）
  ST_CHECK(std::abs(after / before - scaled_theme.metrics().font_base /
                                        base_theme.metrics().font_base) < 0.001F);
}

ST_TEST(code_editor_font_scale_multiplies_the_theme_base) {
  // 档位是**倍数**：1.15 档必须比正文大，且严格等于 base × 1.15。
  CodeEditor editor;
  const Theme theme = Theme::light();
  editor.set_font_scale(1.15F);
  editor.apply_theme(theme);
  const float expected = theme.metrics().font_base * 1.15F;
  ST_CHECK(std::abs(editor.font_size() - expected) < 0.001F);
  ST_CHECK(editor.font_size() > theme.metrics().font_base);
}

ST_TEST(code_editor_explicit_font_size_overrides_scale) {
  // 两个入口各司其职：显式像素优先；复位（负值）后重新跟主题。
  CodeEditor editor;
  const Theme theme = Theme::light();
  editor.set_font_scale(1.15F);
  editor.set_font_size(21.0F);
  editor.apply_theme(theme);
  ST_CHECK(std::abs(editor.font_size() - 21.0F) < 0.001F);

  editor.set_font_size(-1.0F);   // 复位 = 跟随主题
  editor.apply_theme(theme);
  ST_CHECK(std::abs(editor.font_size() - theme.metrics().font_base * 1.15F) < 0.001F);
}

ST_TEST(code_editor_font_scale_is_clamped_to_a_readable_range) {
  // 0 会让整屏文字消失、极大值会让一行放不下几个字——都不是“用户要的”。
  CodeEditor editor;
  editor.set_font_scale(0.0F);
  ST_CHECK(editor.font_scale() >= 0.5F);
  editor.set_font_scale(100.0F);
  ST_CHECK(editor.font_scale() <= 4.0F);
}

ST_TEST(code_editor_font_change_marks_layout_dirty) {
  // 字号变了就必须重排行高/列宽/滚动极限——不标脏会画出错位的行。
  // 本组件特意让布局脏**不冒泡**（否则每次编辑都拖出整帧重绘），
  // 所以“有没有标脏”得读它自己的 `layout_dirty()`。
  CodeEditor editor;
  const Theme theme = Theme::light();
  editor.apply_theme(theme);
  editor.arrange(st::ui::RenderContext{theme, nullptr, 0.0},
                 st::math::Rect{0.0F, 0.0F, 400.0F, 200.0F});
  editor.clear_dirty();
  ST_CHECK(!editor.layout_dirty());

  editor.set_font_scale(2.0F);
  ST_CHECK(editor.layout_dirty());
  // 显式绝对字号同样要标
  editor.clear_dirty();
  editor.set_font_size(21.0F);
  ST_CHECK(editor.layout_dirty());
}
