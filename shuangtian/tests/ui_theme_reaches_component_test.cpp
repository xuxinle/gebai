/// **切换主题必须真的传到组件**（缺陷：`apply_theme` 用指针做身份比较）。
///
/// ### 缺陷原形
///
/// 为了修「声明式宿主每帧重走 `apply_theme_tree` 导致每帧整块标脏」，给
/// `CodeEditor::apply_theme` 加了"主题没换就是空操作"的早退。判据写成了
/// **指针身份**：
///
/// ```
/// if (applied_theme_ == &theme) return;   // ← 错
/// ```
///
/// 而 `UiRoot::set_theme` 是 `theme_ = std::move(theme); mark_dirty_all();`
/// ——**同一地址、内容变了**。指针比较恒等，于是切主题后：
///
/// - `theme_base_font_` 永远停在第一次套用的值 → `--ui-font-scale` 对编辑器无效；
/// - 亮/暗切换时编辑区的语法配色仍读实时 `theme`（这条侥幸没事），
///   但字号、`style_.background` 等落下来的值全部过期。
///
/// ### 判据
///
/// ① 同一 `UiRoot` 换主题（原地赋值，地址不变）→ 组件必须跟上；
/// ② 换的主题与当前**完全一致**时仍是空操作（不标脏）——这是这条早退存在的理由，
///    回退它会让"每帧整块标脏"的 CPU 缺陷复现。
///
/// ② 与 ① 必须同时成立：只验 ① 的话，"把早退整个删掉"也能过；
/// 只验 ② 的话，缺陷版本恰好能过（指针恒等 = 永远空操作）。

#include "st/test/test.hpp"

#include <cmath>
#include <memory>

#include "st/raster/canvas.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::CodeEditor;
using st::ui::Theme;

void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

/// 造一个**同一 UiRoot 下可反复套用**的主题：缩放版浅色主题。
[[nodiscard]] auto scaled_light(float factor) -> Theme {
  Theme theme = Theme::light();
  theme.metrics().scale_fonts(factor);
  return theme;
}

}  // namespace

ST_TEST(theme_switch_reaches_code_editor_through_uproot) {
  st::ui::UiRoot root{};
  root.set_theme(Theme::dark());

  auto owned = std::make_unique<CodeEditor>();
  CodeEditor* editor = owned.get();
  editor->set_id("ce-theme");
  editor->set_text("int main() { return 0; }\n");
  root.set_content(std::move(owned));
  root.set_viewport(st::math::Size{800.0F, 400.0F});

  st::raster::Canvas canvas{800, 400, 1.0F};
  const auto paint_once = [&](double time) {
    root.set_time(time);
    root.paint_frame(canvas);
    root.clear_dirty();
  };

  paint_once(1.0);
  const float before = editor->font_size();
  expect(std::abs(before - Theme::dark().metrics().font_base) < 0.01F,
         std::format("首帧字号没跟到暗色主题的 font_base（font_size={}）", before));

  // ① 同一 UiRoot 切到**放大版主题**：地址不变，内容变了。
  root.set_theme(scaled_light(1.5F));
  paint_once(1.1);
  const float after = editor->font_size();
  const float expected = scaled_light(1.5F).metrics().font_base;
  expect(std::abs(after - expected) < 0.01F,
         std::format("切主题后编辑器字号没跟上（期望 {:.2f}，实际 {:.2f}）"
                     "—— 指针身份比较吃掉了主题变更", expected, after));
  expect(after > before, "放大版主题应当让字号变大");

  // ② 再套一次**完全相同**的主题：组件自己不能因此标脏。
  //
  // 这条是早退存在的理由：声明式宿主每帧都会重走 `apply_theme`，无条件标脏会让
  // 编辑器（占视口 45%+）每帧报一整块损坏区。
  //
  // 判据用**元素自己的脏标志**：`UiRoot::clear_dirty()` 只清动画/唤醒/整帧标记，
  // 不清元素的 `dirty_`，所以“根上还脏”说明不了什么（既有行为，与本缺陷无关）。
  paint_once(1.2);
  root.set_theme(scaled_light(1.5F));   // 内容一致
  editor->clear_dirty();                // 先把元素自己的脏标志归零
  paint_once(1.3);                      // 重走 apply_theme_tree
  expect(!editor->dirty(),
         "套用完全相同的主题后编辑器仍被标脏 —— 早退失效（每帧整块标脏的 CPU 缺陷会复现）");
}
