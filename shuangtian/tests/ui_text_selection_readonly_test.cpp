/// `Text` / `Heading` 的**只读选区**测试（2026-10-14）。
///
/// ### 为什么默认关闭（`set_selectable(false)`）
///
/// 静态标签遍布界面（表单说明、列表副标题、状态文字…）。若默认抢拖选手势，
/// 用户想拖窗口或划选一大片区域时会被逐个标签截断——那不是能力，是干扰。
/// 需要“能选中某一小段”的展示型文本显式打开。
///
/// ### 钉住的判据
///
/// ① 默认**不拦截**任何事件（回归防线：一旦改成默认开，这条会红）；
/// ② 开启后拖选/`Ctrl+A`/`Ctrl+C` 按**显示行**工作；
/// ③ 多行（`set_multiline`）时跨行选区正确，行间补 `\\n`；
/// ④ `max_lines_` 截断时选不到被截掉的文字（选区建在**显示行**上）。

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "../tests/support/text_port_fixtures.hpp"
#include "st/core/string.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Event;
using st::ui::EventKind;
using st::ui::RenderContext;
using st::ui::Text;
using st::ui::Theme;

/// 夹具：`Text` 挂在 `UiRoot` 下并完成布局（选区命中依赖 `bounds_`）。
///
/// 用 `UiRoot` 而不是直接 `arrange`：`Text::measure` 需要约束（决定折行宽度），
/// 而 `measure` 也是 `display_lines_` 缓存被填充的地方之一。
struct Fixture {
  Theme theme{Theme::light()};
  RenderContext context{theme, nullptr, 0.0};
  st::ui::UiRoot root{};
  Text* widget{nullptr};

  Fixture(std::string content, bool multiline, float width, float height) {
    root.set_viewport(st::math::Size{width, height});
    auto owned = std::make_unique<Text>(std::move(content));
    widget = owned.get();
    widget->set_multiline(multiline);
    root.set_content(std::move(owned));
    root.layout(true);
  }

  auto key(std::string key_name, bool ctrl = false) -> bool {
    Event event;
    event.kind = EventKind::KeyDown;
    event.key = std::move(key_name);
    event.ctrl = ctrl;
    return widget->on_event(context, event);
  }

  auto press(float x, float y) -> bool {
    Event event;
    event.kind = EventKind::MouseDown;
    event.button = 1;
    event.position = st::math::Point{x, y};
    return widget->on_event(context, event);
  }

  auto drag(float x, float y) -> bool {
    Event event;
    event.kind = EventKind::MouseMove;
    event.position = st::math::Point{x, y};
    return widget->on_event(context, event);
  }

  auto release() -> bool {
    Event event;
    event.kind = EventKind::MouseUp;
    event.button = 1;
    event.position = st::math::Point{0.0f, 0.0f};
    return widget->on_event(context, event);
  }
};

}  // namespace

ST_TEST(text_selection_default_is_off) {
  // 默认**不拦截任何事件**——静态标签不该抢拖选手势。
  Fixture fixture("普通标签文字", /*multiline=*/false, 300.0f, 40.0f);
  ST_CHECK(!fixture.widget->selectable());
  ST_CHECK(!fixture.widget->has_selection());
  // 拖选事件应被放行（返回 false = 未处理）
  ST_CHECK(!fixture.press(10.0f, 10.0f));
  ST_CHECK(!fixture.drag(80.0f, 10.0f));
  ST_CHECK(!fixture.widget->has_selection());
  // Ctrl+A 也不该生效
  ST_CHECK(!fixture.key("a", /*ctrl=*/true));
  ST_CHECK(!fixture.widget->has_selection());
}

ST_TEST(text_selection_ctrl_a_and_readback) {
  Fixture fixture("hello world", false, 300.0f, 40.0f);
  fixture.widget->set_selectable(true);
  ST_CHECK(fixture.widget->selectable());
  // Ctrl+A 全选
  ST_CHECK(fixture.key("a", true));
  ST_CHECK(fixture.widget->has_selection());
  ST_CHECK_EQ(fixture.widget->selected_text(), std::string("hello world"));
  // Esc 清选区
  ST_CHECK(fixture.key("Escape"));
  ST_CHECK(!fixture.widget->has_selection());
  ST_CHECK_EQ(fixture.widget->selected_text(), std::string{});
}

ST_TEST(text_selection_toggle_off_clears) {
  // 关掉可选应**顺手清选区**：否则会留下一个“看不见但存在”的选区，
  // 之后重新打开就凭空冒出一段高亮。
  Fixture fixture("abcdef", false, 300.0f, 40.0f);
  fixture.widget->set_selectable(true);
  fixture.key("a", true);
  ST_CHECK(fixture.widget->has_selection());
  fixture.widget->set_selectable(false);
  ST_CHECK(!fixture.widget->has_selection());
  ST_CHECK_EQ(fixture.widget->selected_text(), std::string{});
}

ST_TEST(text_selection_multiline_spans_lines) {
  // 多行：选区按**显示行**工作，跨行时行间补 `\n`。
  Fixture fixture("first\nsecond\nthird", /*multiline=*/true, 300.0f, 200.0f);
  fixture.widget->set_selectable(true);
  fixture.widget->select_all();
  const std::string all = fixture.widget->selected_text();
  ST_CHECK(!all.empty());
  // 三行都在（折行宽度够时每段一行）
  ST_CHECK(all.find("first") != std::string::npos);
  ST_CHECK(all.find("third") != std::string::npos);
  ST_CHECK(all.find('\n') != std::string::npos);
}

ST_TEST(text_selection_mouse_drag_within_line) {
  // 真鼠标拖选（单行）：从行内左端拖到右端，选中区间非空且是原文的子串。
  //
  // 本用例用**等宽文本桩**（`FixedAdvanceTextPort`）——与前面几条不同：
  // 无端口时 `measure_width` 恒为 0，`caret_at` 的行内命中会退化成同一个偏移，
  // 拖多远都选不出东西。堆等宽桩才能验证「坐标 → 偏移」这段映射。
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};   // 等宽桩：每字符 kAdvance=8px
  RenderContext context{theme, &port, 0.0};
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{300.0f, 40.0f});
  auto owned = std::make_unique<Text>("abcdefghij");
  Text* widget = owned.get();
  widget->set_selectable(true);
  root.set_content(std::move(owned));
  root.layout(true);

  const st::math::Rect bounds = widget->bounds();
  ST_REQUIRE(bounds.width > 0.0f);
  const float mid_y = bounds.y + bounds.height * 0.5f;
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = st::math::Point{bounds.x + 1.0f, mid_y};
  ST_CHECK(widget->on_event(context, down));
  Event move = down;
  move.kind = EventKind::MouseMove;
  move.position = st::math::Point{bounds.x + 61.0f, mid_y};
  ST_CHECK(widget->on_event(context, move));
  const std::string picked = widget->selected_text();
  ST_CHECK(!picked.empty());
  ST_CHECK(widget->content().find(picked) != std::string::npos);
  // 等宽桩每字符 8px：起点在 x+1（≈字符 0）、终点在 x+61（≈字符 7.6 → 取最近边界 8）
  // ⇒ 选中前 8 个字符。注意不是“按 bounds 宽度算”——文本只占 `measure_width`
  // 那么宽，拖过文本末尾后偏移就夹在串尾。
  ST_CHECK_EQ(picked, std::string("abcdefgh"));
}

ST_TEST(text_selection_multiline_drag_across_lines) {
  // 多行拖选跨行：拖到下一行时应选中行间内容（行间补 `\n`）。
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};   // 等宽桩：每字符 kAdvance=8px
  RenderContext context{theme, &port, 0.0};
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{300.0f, 120.0f});
  auto owned = std::make_unique<Text>("aaaa\nbbbb\ncccc");
  Text* widget = owned.get();
  widget->set_multiline(true);
  widget->set_selectable(true);
  root.set_content(std::move(owned));
  root.layout(true);

  const st::math::Rect bounds = widget->bounds();
  ST_REQUIRE(bounds.height > 0.0f);
  // 从第一行中部拖到第二行中部
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = st::math::Point{bounds.x + 20.0f, bounds.y + 2.0f};
  ST_CHECK(widget->on_event(context, down));
  Event move = down;
  move.kind = EventKind::MouseMove;
  move.position = st::math::Point{bounds.x + 20.0f, bounds.y + bounds.height * 0.5f};
  ST_CHECK(widget->on_event(context, move));
  const std::string picked = widget->selected_text();
  ST_CHECK(!picked.empty());
  ST_CHECK(picked.find('\n') != std::string::npos);   // 跨行 ⇒ 含换行
}

ST_TEST(heading_inherits_text_selection) {
  // `Heading` 继承 `Text`：选区能力应自动获得（不需要另写一遍）。
  Theme theme{Theme::light()};
  RenderContext context{theme, nullptr, 0.0};
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{400.0f, 60.0f});
  auto owned = std::make_unique<st::ui::Heading>("标题文字", 2U);
  st::ui::Heading* heading = owned.get();
  root.set_content(std::move(owned));
  root.layout(true);

  ST_CHECK(!heading->selectable());   // 默认同样关着
  heading->set_selectable(true);
  Event select_all;
  select_all.kind = EventKind::KeyDown;
  select_all.key = "a";
  select_all.ctrl = true;
  ST_CHECK(heading->on_event(context, select_all));
  ST_CHECK(heading->has_selection());
  ST_CHECK_EQ(heading->selected_text(), std::string("标题文字"));
}
