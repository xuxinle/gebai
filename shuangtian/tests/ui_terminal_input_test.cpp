/// 终端组件的**输入翻译契约**测试（`Terminal::key_bytes`）。
///
/// 为什么单独一个文件：这里钉的是"哪个事件对应哪些字节"——终端对 shell 的**输入合约**。
/// 它此前埋在 `on_event` 里、只能靠"起真 shell 再看屏幕"间接验证，于是漏掉了一整类缺陷。
///
/// ### 本轮钉住的缺陷（2026-10-08，用户报「输入一个字符回显了 2 个」）
///
/// Win32 上一次物理按键产生 **两个**消息：`WM_KEYDOWN` + `WM_CHAR`
///（后端 `pump_messages` 调了 `TranslateMessage`），分别转成
/// `KeyDown(key="a")` 与 `TextInput(text="a")`。终端把**两条都**当文本来源
///（`KeyDown` 走裸字符回落、`TextInput` 再送一遍）⇒ 一个字符回显两次。
///
/// 框架的契约是 **`TextInput` 是文本的唯一来源**（`Input`/`CodeEditor`/`TextArea`
/// 都如此，`KeyDown` 里的裸字符一律不进字）。终端违反了它。
///
/// **为什么既有测试全绿**：控制通道的 `input.key` 只发 `KeyDown`、`input.text`
/// 只发 `TextInput`——单发各自都正常，**只有真窗口才会成对出现**。
/// 所以回归用例必须显式构造"成对"这个输入形态（见 `terminal_keydown_then_textinput_sends_one_character`）。
///
/// ### 顺带钉住的第二个缺陷
///
/// `Ctrl/Shift/Alt + 方向/Home/End` 的映射曾是**死代码**：前面的裸键分支
///（`key == "ArrowUp"`）按字符串比较、无条件抢先匹配，修饰组合永远走不到。
/// 实测（真 shell）：按一次 Ctrl+← 只左移 1 格，而 raw `ESC[1;5D` 能按词跳。
/// ⇒ 上一轮提交声称"修饰方向键已补齐"的能力**从未生效**。

#include "st/test/test.hpp"

#include <string>

#include "st/ui/components/terminal.hpp"

namespace {

using st::ui::Event;
using st::ui::EventKind;
using st::ui::Terminal;

/// 造一个 `KeyDown` 事件。
[[nodiscard]] auto key_event(std::string key, bool ctrl = false, bool shift = false,
                             bool alt = false) -> Event {
  Event event;
  event.kind = EventKind::KeyDown;
  event.key = std::move(key);
  event.ctrl = ctrl;
  event.shift = shift;
  event.alt = alt;
  return event;
}

/// 造一个 `TextInput` 事件（可打印文本的**唯一**来源）。
[[nodiscard]] auto text_event(std::string text) -> Event {
  Event event;
  event.kind = EventKind::TextInput;
  event.text = std::move(text);
  return event;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// ① 核心缺陷：一次按键只有一个字符
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(terminal_plain_character_keydown_sends_nothing) {
  // **不信则**：`KeyDown('a')` 不送字节。可打印文本归 `TextInput`。
  //
  // 逆向验证：把裸字符回落加回 `key_bytes`（`if (key.size() == 1) return key;`），
  // 本用例立刻红（返回 "a"）。
  ST_CHECK(Terminal::key_bytes(key_event("a")).empty());
  ST_CHECK(Terminal::key_bytes(key_event("Z")).empty());
  ST_CHECK(Terminal::key_bytes(key_event("5")).empty());
  ST_CHECK(Terminal::key_bytes(key_event(" ")).empty());
  ST_CHECK(Terminal::key_bytes(key_event(";")).empty());
}

ST_TEST(terminal_textinput_is_the_only_text_source) {
  // 对照面：可打印文本必须由 `TextInput` 送达（不能因为"KeyDown 不送了"而两边都不送）。
  ST_CHECK_EQ(Terminal::key_bytes(text_event("a")), std::string("a"));
  ST_CHECK_EQ(Terminal::key_bytes(text_event("hello")), std::string("hello"));
  // 非 ASCII（IME/中文）也走同一条路：按 UTF-8 原样写回 PTY。
  ST_CHECK_EQ(Terminal::key_bytes(text_event("中")), std::string("中"));
}

ST_TEST(terminal_keydown_then_textinput_sends_one_character) {
  // **真窗口的输入形态**：一次按键 = `KeyDown` + `TextInput` 成对。
  // 两条事件合起来只能产生**一个**字符——这正是用户报的"回显 2 个"。
  //
  // 这个用例是本轮缺陷的直接判据：它构造的正是真窗口的事件序列，
  // 而此前那些只单独发一条事件的用例看不到这个问题。
  const std::string from_down = Terminal::key_bytes(key_event("a"));
  const std::string from_text = Terminal::key_bytes(text_event("a"));
  ST_CHECK(from_down.empty());                 // KeyDown 不发
  ST_CHECK_EQ(from_text, std::string("a"));    // TextInput 发一次
  ST_CHECK_EQ(from_down + from_text, std::string("a"));   // 合计恰好一个字符

  // Shift+字母同理：WM_KEYDOWN(key="a", shift) + WM_CHAR(text="A")。
  const std::string shift_down = Terminal::key_bytes(key_event("a", false, true));
  const std::string shift_text = Terminal::key_bytes(text_event("A"));
  ST_CHECK(shift_down.empty());
  ST_CHECK_EQ(shift_down + shift_text, std::string("A"));
}

// ————————————————————————————————————————————————————————————————————————————
// ② 修饰 + 方向/Home/End：xterm 修饰参数（曾是死代码）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(terminal_bare_navigation_still_sends_xterm_defaults) {
  // 无修饰的方向/Home/End 送 xterm 默认序列（回归：别把裸键也改坏）。
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowUp")), std::string("\x1b[A"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowDown")), std::string("\x1b[B"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowRight")), std::string("\x1b[C"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowLeft")), std::string("\x1b[D"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("Home")), std::string("\x1b[H"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("End")), std::string("\x1b[F"));
}

ST_TEST(terminal_modified_navigation_sends_xterm_modifier_parameter) {
  // **死代码判据**：修饰版必须与裸版**不同**。旧实现下两者恒等
  //（Ctrl+← 落到 `key == "ArrowLeft"` 分支 → `ESC[D`），本用例因此会红。
  //
  // 逆向验证：把修饰分支挪回裸键分支**之后**，下面这些断言立刻失败。
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowLeft", true)), std::string("\x1b[1;5D"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowRight", true)), std::string("\x1b[1;5C"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowUp", true)), std::string("\x1b[1;5A"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowDown", true)), std::string("\x1b[1;5B"));
  // Ctrl+←/→ = readline 的按词跳；必须**不是**裸序列（否则只是移一格）。
  ST_CHECK(Terminal::key_bytes(key_event("ArrowLeft", true)) != std::string("\x1b[D"));

  // Shift（参数 2）：vim 选择 / less 搜索。
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowLeft", false, true)), std::string("\x1b[1;2D"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("Home", false, true)), std::string("\x1b[1;2H"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("End", false, true)), std::string("\x1b[1;2F"));

  // Alt（参数 3）。
  ST_CHECK_EQ(Terminal::key_bytes(key_event("ArrowUp", false, false, true)),
              std::string("\x1b[1;3A"));

  // 组合（xterm 各份相加）：Shift+Ctrl=6、Alt+Ctrl=7。
  Event both = key_event("ArrowRight", true, true);
  ST_CHECK_EQ(Terminal::key_bytes(both), std::string("\x1b[1;6C"));
  Event alt_ctrl = key_event("ArrowLeft", true, false, true);
  ST_CHECK_EQ(Terminal::key_bytes(alt_ctrl), std::string("\x1b[1;7D"));
}

// ————————————————————————————————————————————————————————————————————————————
// ③ 控制键 / Alt / Ctrl：不因本轮改动而倒退
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(terminal_control_keys_translate_to_bytes) {
  ST_CHECK_EQ(Terminal::key_bytes(key_event("Enter")), std::string("\r"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("NumpadEnter")), std::string("\r"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("Backspace")), std::string("\x7f"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("Tab")), std::string("\t"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("Escape")), std::string("\x1b"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("Delete")), std::string("\x1b[3~"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("PageUp")), std::string("\x1b[5~"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("PageDown")), std::string("\x1b[6~"));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("Insert")), std::string("\x1b[2~"));
}

ST_TEST(terminal_alt_and_ctrl_letters_are_the_only_source_of_their_bytes) {
  // 这两类组合**不产生 `TextInput`**（后端把控制字符滤掉；Alt 走 WM_SYSKEYDOWN），
  // 所以它们必须由 `KeyDown` 送——不能因为"KeyDown 不再送裸字符"一起砍掉。
  ST_CHECK_EQ(Terminal::key_bytes(key_event("b", false, false, true)), std::string("\x1b") + "b");
  ST_CHECK_EQ(Terminal::key_bytes(key_event("f", false, false, true)), std::string("\x1b") + "f");
  ST_CHECK_EQ(Terminal::key_bytes(key_event("c", true)), std::string(1, '\x03'));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("d", true)), std::string(1, '\x04'));
  ST_CHECK_EQ(Terminal::key_bytes(key_event("a", true)), std::string(1, '\x01'));
  // Ctrl+[ 是 ESC 的另一种写法（vim 用户常用）。
  ST_CHECK_EQ(Terminal::key_bytes(key_event("[", true)), std::string("\x1b"));
  // 未映射的控制组合不产生字节（不是"随便送点什么"）。
  ST_CHECK(Terminal::key_bytes(key_event("F5")).empty());
  ST_CHECK(Terminal::key_bytes(key_event("Shift")).empty());
}
