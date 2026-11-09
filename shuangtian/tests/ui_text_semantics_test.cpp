#include <cstddef>
#include <format>
#include <iostream>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/components/input_box.hpp"
#include "st/ui/components/text_area.hpp"
#include "st/ui/theme.hpp"

// 这一组钉的是 `set_text` / `update_text` 的**语义分家**。
//
// 为什么需要：两者收拢前只有 `set_text` 一种语义（重置光标/滚动），而“重排后回写”
// 场景误用它 ⇒ 每敲一个字光标跳回行首、滚动回顶。`CodeEditor` 因此专门加了
// `replace_text_keep_state`，而 `Input`/`TextArea` 一直没有对应能力。
//
// 判据只能是**光标位置**：两种写法画出来的像素在“内容相同”时完全一样，
// 差别只在状态有没有被重置。

ST_TEST(input_set_text_moves_caret_to_end) {
  st::ui::Input input;
  input.set_text("hello world");
  std::cout << std::format("  [input] set_text -> cursor={}\n", input.cursor_index()) << std::flush;
  ST_CHECK_EQ(input.cursor_index(), std::size_t{11});
}

ST_TEST(input_update_text_keeps_caret) {
  st::ui::Input input;
  input.set_text("hello world");
  input.set_cursor_index(5);  // 光标停在 "hello| world"
  ST_CHECK_EQ(input.cursor_index(), std::size_t{5});

  // 内容变了但位置要保住（模拟“重新计算后回写”）。
  input.update_text("hello there world");
  std::cout << std::format("  [input] update_text -> cursor={}, text={}\n", input.cursor_index(),
                           input.value())
            << std::flush;
  ST_CHECK_EQ(input.cursor_index(), std::size_t{5});  // 没有被重置到末尾
  ST_CHECK_EQ(input.value(), std::string{"hello there world"});
}

ST_TEST(input_update_text_clamps_when_shrinking) {
  // ⚠ 单看“夹取结果”分不清“保位置”与“到末尾”——文本缩到比光标短时，两者都落在末尾
  // （本轮逆向验证实测：把实现改回“到末尾”，只验 shrink 的这条仍然绿）。
  // 所以真正的判据在下方**文本变长**的对照里。
  st::ui::Input shrink;
  shrink.set_text("0123456789");
  shrink.set_cursor_index(8);
  shrink.update_text("012");  // 比光标位置短
  ST_CHECK_EQ(shrink.cursor_index(), std::size_t{3});

  st::ui::Input keep;
  keep.set_text("0123456789");
  keep.set_cursor_index(4);
  keep.update_text("0123456789ABC");  // 仍比光标长
  std::cout << std::format("  [input] grow -> cursor={}（应保 4）\n", keep.cursor_index())
            << std::flush;
  ST_CHECK_EQ(keep.cursor_index(), std::size_t{4});
}

ST_TEST(input_update_text_same_content_is_noop) {
  // 内容没变时不应标脏：回写路径每帧调一次的话，标脏会与帧间损坏判据互相触发。
  st::ui::Input input;
  input.set_text("stable");
  input.set_cursor_index(2);
  (void)input.take_damage();  // 消耗之前的
  input.update_text("stable");
  ST_CHECK(!input.take_damage().valid);
  ST_CHECK_EQ(input.cursor_index(), std::size_t{2});
}

ST_TEST(text_area_set_text_resets_caret_and_scroll) {
  st::ui::TextArea area;
  area.set_text("line1\nline2\nline3");
  std::cout << std::format("  [textarea] set_text -> cursor={}\n", area.cursor_index()) << std::flush;
  ST_CHECK_EQ(area.cursor_index(), std::size_t{0});  // 回到文首（与 Input 相反，历史约定）
}

ST_TEST(text_area_update_text_keeps_caret) {
  st::ui::TextArea area;
  area.set_text("aaa\nbbb\nccc");
  area.set_cursor_index(6);  // 第三行行首附近
  const std::size_t before = area.cursor_index();
  area.update_text("aaa\nbbb\nccc\nddd");
  std::cout << std::format("  [textarea] update_text -> cursor={}（原 {}）\n", area.cursor_index(),
                           before)
            << std::flush;
  ST_CHECK_EQ(area.cursor_index(), before);  // 没被重置到 0
}
