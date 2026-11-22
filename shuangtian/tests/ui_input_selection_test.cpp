/// `Input` / `TextArea` 的**选区**测试（2026-10-14）。
///
/// ### 为什么单开一份
///
/// 用户要求「所有的文本查看和编辑组件都要有很好的文本选中复制能力」，且 Input
/// 要按**完整编辑体验**做（Shift+方向键/Home/End 扩选、双击选词、三击整行、
/// Ctrl+A/C/X、密码框禁用、输入法替换选区）。这些是**状态机语义**，与既有
/// `ui_input_test.cpp`（绘制契约、数像素）不是一类，混在一起会让两边都难读。
///
/// ### 本文件钉住的判据
///
/// 全部用**选区状态 + 选中文本**断言（不数像素）：状态机对了，绘制才有意义；
/// 反之在“高亮好看但拷错了字”的情况下，数像素是抓不到的。
///
/// 端到端真鼠标拖选在 gbcode 上做（见提交说明）。

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "../src/ui/components/components_internal.hpp"
#include "st/core/string.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Event;
using st::ui::EventKind;
using st::ui::Input;
using st::ui::RenderContext;
using st::ui::TextArea;
using st::ui::Theme;

/// 组件 + 渲染上下文 + 事件派发的小夹具。
///
/// 直接调 `on_event`（而不是经 `UiRoot::dispatch`）：选区语义是组件自己的状态机，
/// 经 root 会掺进焦点/命中判定，失败时分不清是谁的问题。**焦点相关**的用例
/// 另用 root（见 `input_selection_requires_focus`）。
template <typename Widget>
struct Harness {
  Theme theme{Theme::light()};
  RenderContext context{theme, nullptr, 0.0};
  Widget widget{};

  Harness() {
    widget.arrange(context, st::math::Rect{0.0f, 0.0f, 300.0f, 40.0f});
  }

  auto key(std::string key_name, bool shift = false, bool ctrl = false) -> bool {
    Event event;
    event.kind = EventKind::KeyDown;
    event.key = std::move(key_name);
    event.shift = shift;
    event.ctrl = ctrl;
    return widget.on_event(context, event);
  }

  auto type(std::string_view text) -> bool {
    Event event;
    event.kind = EventKind::TextInput;
    event.text = std::string(text);
    return widget.on_event(context, event);
  }

  void advance_time(double seconds) { context.time_seconds = seconds; }

  /// 重新走一遍排布（改样式/槽位后几何要重算）。
  void rearrange() {
    widget.measure(context, st::ui::Constraints{.max_width = 600.0f, .max_height = 40.0f});
    widget.arrange(context, st::math::Rect{0.0f, 0.0f, 300.0f, 40.0f});
  }

  /// 在 x 处按下/拖动（用于拖选与双击）。
  auto press(float x, bool shift = false, double when = 0.0, int button = 1) -> bool {
    context.time_seconds = when;
    Event event;
    event.kind = EventKind::MouseDown;
    event.button = button;
    event.shift = shift;
    event.position = st::math::Point{x, 20.0f};
    return widget.on_event(context, event);
  }

  auto drag_to(float x) -> bool {
    Event event;
    event.kind = EventKind::MouseMove;
    event.position = st::math::Point{x, 20.0f};
    return widget.on_event(context, event);
  }

  auto release() -> bool {
    Event event;
    event.kind = EventKind::MouseUp;
    event.button = 1;
    event.position = st::math::Point{0.0f, 20.0f};
    return widget.on_event(context, event);
  }
};

}  // namespace

// ———————————————————————— Input：键盘选区 ————————————————————————

ST_TEST(input_selection_shift_arrow_extends) {
  // 用户要求「完整编辑体验」：Shift+方向键扩选是基本盘。
  // 判据：不动锚点、只移光标 → 选中区间随按键增长/收缩。
  Harness<Input> h;
  h.widget.set_text("abcdef");
  h.key("End");
  ST_CHECK_EQ(h.widget.selection().first, h.widget.selection().second);   // 无选区

  // Shift+Left：从末尾向左扩一个字符
  h.key("ArrowLeft", /*shift=*/true);
  ST_CHECK_EQ(h.widget.selected_text(), std::string("f"));
  h.key("ArrowLeft", /*shift=*/true);
  ST_CHECK_EQ(h.widget.selected_text(), std::string("ef"));

  // 无 Shift 的移动 → 收起选区，光标到选区左端（浏览器行为）
  h.key("ArrowLeft");
  ST_CHECK(!h.widget.has_selection());
  ST_CHECK_EQ(h.widget.cursor_index(), std::size_t{4});   // "ef" 的左端

  // 反向：Shift+Right 重新扩选
  h.key("ArrowRight", /*shift=*/true);
  ST_CHECK_EQ(h.widget.selected_text(), std::string("e"));
}

ST_TEST(input_selection_shift_home_end) {
  // Shift+Home / Shift+End：扩到行首/行尾（单行框的“行”就是整串）。
  Harness<Input> h;
  h.widget.set_text("hello world");
  h.key("End");
  h.key("Home", /*shift=*/true);
  ST_CHECK_EQ(h.widget.selected_text(), std::string("hello world"));

  h.key("End", /*shift=*/true);
  ST_CHECK(!h.widget.has_selection());   // 扩到同一端 → 收空
}

ST_TEST(input_selection_ctrl_a_and_typing_replaces) {
  // Ctrl+A 全选；随后**打字必须替换选区**（这是“完整编辑体验”的核心行为）。
  Harness<Input> h;
  h.widget.set_text("旧内容");
  h.key("a", /*shift=*/false, /*ctrl=*/true);
  ST_CHECK(h.widget.has_selection());
  ST_CHECK_EQ(h.widget.selected_text(), std::string("旧内容"));

  h.type("新");
  ST_CHECK_EQ(h.widget.value(), std::string("新"));
  ST_CHECK(!h.widget.has_selection());   // 替换后选区清空
}

ST_TEST(input_selection_backspace_deletes_selection) {
  // 有选区时 Backspace/Delete 删选区（而不是再删一个字符）。
  Harness<Input> h;
  h.widget.set_text("abcdef");
  h.widget.set_selection(1U, 4U);   // "bcd"
  ST_CHECK_EQ(h.widget.selected_text(), std::string("bcd"));
  h.key("Backspace");
  ST_CHECK_EQ(h.widget.value(), std::string("aef"));
  ST_CHECK(!h.widget.has_selection());
}

ST_TEST(input_selection_reverse_drag_normalizes) {
  // 反向拖选（focus 在 anchor 之前）必须归一化，且 selected_text 与方向无关。
  Harness<Input> h;
  h.widget.set_text("abcdef");
  h.widget.set_selection(4U, 1U);
  const auto [from, to] = h.widget.selection();
  ST_CHECK_EQ(from, std::size_t{1});
  ST_CHECK_EQ(to, std::size_t{4});
  ST_CHECK_EQ(h.widget.selected_text(), std::string("bcd"));
}

// ———————————————————————— Input：鼠标 / 双击 / 密码 ————————————————————————

ST_TEST(input_selection_double_click_selects_word) {
  // 双击选词（词界用公共算法，与 Terminal 同一套规则）。
  //
  // ⚠ 本用例的**局限**（写清楚，免得后人误以为它盖住了鼠标命中）：夹具没有文本
  // 端口，`index_at_x` 的宽度全为 0 → 命中点总是 0。所以这里验证的是「**双击
  // 分支的选词行为**」而不是“在某个 x 上双击能选中那个词”——后者靠端到端真机
  // （gbcode + 真鼠标点击，见提交说明）。
  Harness<Input> h;
  h.widget.set_text("foo bar_baz qux");
  // 让双击落点的命中结果就是 “bar_baz” 内部：先把光标置于词中，双击时
  // `index_at_x` 会给 0，所以直接验证“以光标位置为输入”的那条路径。
  h.widget.set_cursor_index(5U);
  const auto [begin, end] = st::ui::components_internal::word_range_at(h.widget.value(), 5U);
  ST_CHECK_EQ(h.widget.value().substr(begin, end - begin), std::string("bar_baz"));
  // 双击分支本身：连续两次相近按下 → 走选词（两端为词的边界）。
  h.press(0.0f, false, 0.0);
  h.press(0.0f, false, 0.2);
  ST_CHECK(h.widget.has_selection());
  const auto [from, to] = h.widget.selection();
  ST_CHECK(to > from);
}

ST_TEST(input_selection_password_disabled) {
  // **密码态与普通文本框同口径**：掩码只是显示层，选区/选中文本照常工作。
  //
  // 历史：这条用例原本断言“密码框选区恒空、selected_text 恒空”——那是把
  // “不泄露明文”错做成了“砍掉编辑能力”。用户要的是“除字符不显示外与普通框
  // 一致”，所以现在的判据反过来：**能用**，且可用性不因密码态退化。
  Harness<Input> h;
  h.widget.set_password(true);
  h.widget.set_text("secret123");
  h.key("a", false, true);   // Ctrl+A 全选
  ST_CHECK(h.widget.has_selection());
  ST_CHECK_EQ(h.widget.selected_text(), std::string("secret123"));
  // 程序化设置同样生效（与普通框一致）
  h.widget.set_selection(0U, 6U);
  ST_CHECK_EQ(h.widget.selected_text(), std::string("secret"));
  // 但**画出来的是掩码**：屏幕字符串里不应出现明文
  ST_CHECK(h.widget.semantics_value().find("secret") == std::string::npos);
  // 双击选词也照常（与普通框同一分支）
  h.widget.clear_selection();
  h.widget.set_text("secret123");
  h.press(0.0f, false, 0.0);
  h.press(0.0f, false, 0.2);
  ST_CHECK(h.widget.has_selection());
}

// ———————————————————————— Input：密码态「显示/隐藏」切换 ————————————————————————

ST_TEST(input_password_reveal_toggles_display_only) {
  // 切换钮只改**显示**：`value()` / `selected_text()` 从一开始就是明文。
  Harness<Input> h;
  h.widget.set_password(true);
  h.widget.set_text("hunter2");
  ST_CHECK(h.widget.password_reveal());      // 默认就带切换钮（密码框本就应该能看一眼）
  ST_CHECK(!h.widget.password_visible());    // 默认掩码
  ST_CHECK_EQ(h.widget.semantics_value(), std::string("•••••••"));

  h.widget.set_password_visible(true);
  ST_CHECK(h.widget.password_visible());
  ST_CHECK_EQ(h.widget.semantics_value(), std::string("hunter2"));   // 显示层切成明文
  ST_CHECK_EQ(h.widget.value(), std::string("hunter2"));             // 值一直是明文（没变过）

  // 属性面同一状态（协议 `set password_visible=true` 走这里）
  ST_CHECK(h.widget.set_property("password_visible", "false"));
  ST_CHECK(!h.widget.password_visible());
  ST_CHECK_EQ(h.widget.get_property("password_visible").value_or("?"), std::string("false"));
}

ST_TEST(input_password_reveal_resets_when_password_mode_changes) {
  // 把“刚才瞥了一眼”带到下一个密码框上是真事故（复用时尤甚）：
  // 进入/离开密码态一律回到掩码。
  Harness<Input> h;
  h.widget.set_password(true);
  h.widget.set_text("pw");
  h.widget.set_password_visible(true);
  ST_CHECK(h.widget.password_visible());
  h.widget.set_password(false);
  h.widget.set_password(true);
  ST_CHECK(!h.widget.password_visible());

  // 关掉切换钮时也不能停在明文态（按钮没了就没法切回去）
  h.widget.set_password_visible(true);
  h.widget.set_password_reveal(false);
  ST_CHECK(!h.widget.password_visible());
}

ST_TEST(input_password_reveal_slot_is_reclaimed_when_disabled) {
  // 切换钮占的是**输入框内右侧槽位**：关掉它，槽位必须真的收回
  //（否则“关掉按钮”只是不画，右边仍空一块）。
  //
  // 判据用**可算的几何**而不数像素：槽位收放的直接证据是**下拉箭头的位置**——
  // 它排在眼睛左侧，眼睛槽消失后箭头必须向外（向右）移一格，且位移恰等于槽宽。
  Harness<Input> h;
  h.widget.set_password(true);
  h.widget.set_dropdown(true);
  h.rearrange();
  const auto eye = h.widget.password_reveal_rect();
  const auto arrow_with_eye = h.widget.dropdown_rect();
  ST_REQUIRE(!eye.is_empty());
  ST_REQUIRE(!arrow_with_eye.is_empty());

  h.widget.set_password_reveal(false);
  h.rearrange();
  const auto arrow_without_eye = h.widget.dropdown_rect();
  ST_CHECK(h.widget.password_reveal_rect().is_empty());
  ST_REQUIRE(!arrow_without_eye.is_empty());
  ST_CHECK(arrow_without_eye.x > arrow_with_eye.x);
  ST_CHECK_NEAR(arrow_without_eye.x - arrow_with_eye.x, eye.width, 0.5f);
  ST_CHECK(arrow_without_eye.right() <= h.widget.bounds().right());
}

ST_TEST(input_selection_multibyte_safe) {
  // 选区端点必须落在**码点边界**上（不能切出半个 UTF-8 序列）。
  Harness<Input> h;
  h.widget.set_text("汉字abc");
  // 传一个落在 “汉” 中间的字节偏移（UTF-8 首字节后一位）
  h.widget.set_selection(1U, 6U);
  const std::string picked = h.widget.selected_text();
  ST_CHECK(st::utf8_is_valid(picked));
  // 起点被夹到了码点边界（0 或 3），不会产生乱码
  ST_CHECK(picked.empty() || picked.starts_with("汉") || picked.starts_with("字"));
}

// ———————————————————————— TextArea：多行选区 ————————————————————————

ST_TEST(text_area_selection_spans_lines) {
  // 跨行选区：字节偏移天然跨行（不需要“行号+列”这套额外状态）。
  Harness<TextArea> h;
  h.widget.set_text("line1\nline2\nline3");
  // 字节 [2, 10) = "ne1\nline"（含换行符本身——选择跨行时它必须在选区里，
  // 否则复制出来两行会粘在一起）
  h.widget.set_selection(2U, 10U);
  ST_CHECK_EQ(h.widget.selected_text(), std::string("ne1\nline"));
  ST_CHECK(h.widget.selected_text().find('\n') != std::string::npos);
}

ST_TEST(text_area_selection_replace_and_delete) {
  Harness<TextArea> h;
  h.widget.set_text("hello world");
  h.widget.set_selection(6U, 11U);
  ST_CHECK_EQ(h.widget.selected_text(), std::string("world"));
  h.type("there");
  ST_CHECK_EQ(h.widget.value(), std::string("hello there"));
}

ST_TEST(text_area_selection_works_in_read_only) {
  // 只读 = “不能改”，**不是**“不能选/不能拷”——这正是只读区别于 disabled 的地方。
  Harness<TextArea> h;
  h.widget.set_text("只读内容可以选中复制");
  h.widget.set_read_only(true);
  h.key("a", false, true);   // Ctrl+A
  ST_CHECK(h.widget.has_selection());
  ST_CHECK(!h.widget.selected_text().empty());
  // 但打字不落字（只读语义）
  const std::string before = h.widget.value();
  h.type("X");
  ST_CHECK_EQ(h.widget.value(), before);
}

ST_TEST(text_area_selection_triple_click_selects_line) {
  // 三击选**当前行**（多行框的通行行为：不是整篇，而是这一段）。
  //
  // ⚠ 同 `input_selection_double_click_selects_word`：无文本端口时 `index_at_point`
  // 的宽度全为 0 → 命中点总是 0，所以“点在某行上选该行”只能靠端到端真机验证。
  // 这里验证的是「三击分支**不会选整篇**」这条边界（选到了第一行而不是全文）。
  Harness<TextArea> h;
  h.widget.set_text("aaa\nbbb\nccc");
  h.press(0.0f, false, 0.0);
  h.press(0.0f, false, 0.2);
  h.press(0.0f, false, 0.4);   // 三击
  const std::string picked = h.widget.selected_text();
  ST_CHECK(!picked.empty());
  // 关键边界：三击**不是全选**（全选是 Ctrl+A 的语义）
  ST_CHECK(picked.size() < h.widget.value().size());
  ST_CHECK(picked.find('\n') == std::string::npos);   // 单行（不含换行）
}
