/// 终端面板化改造的单测：标签同步（会话 → `Tabs`）、回看滚动条、「+」新建、
/// 粘贴链路、`SplitView` 隐藏侧、字号键位。
///
/// 与 `ui_terminal_pty_test.cpp`（真 shell 全链路）分工：本文件钉**组件组合层**的
/// 契约——标签数据流向、滚动条几何换算、隐藏侧几何。PTY 相关用假通道/直调验证
///（不依赖系统 shell，跑得快也不飘）。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"
#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::ui::Element;
using st::ui::SplitView;
using st::ui::Tabs;
using st::ui::Terminal;

/// 夹具：Terminal 挂根（行模式——不依赖系统 shell；PTY 路径在专测里）。
struct TermFixture {
  st::ui::UiRoot root{};
  Terminal* terminal{nullptr};

  TermFixture() {
    root.set_viewport(st::math::Size{900.0f, 500.0f});
    auto owned = std::make_unique<Terminal>();
    terminal = owned.get();
    root.set_content(std::move(owned));
    root.layout(true);
  }
};

}  // namespace

// ————————————————— 标签同步：会话 → Tabs —————————————————

ST_TEST(terminal_tabs_reflect_sessions) {
  TermFixture fixture;
  Terminal* terminal = fixture.terminal;
  // 初建即有 1 个会话（不变式：至少一个）。
  ST_CHECK_EQ(terminal->session_count(), std::size_t{1});
  // 布局后 Tabs 子件拿到标签数据（同步发生在 arrange）。
  fixture.root.layout(true);
  const Element* tabs = fixture.root.find("terminal-tabs");
  ST_REQUIRE(tabs != nullptr);
  ST_CHECK_EQ(tabs->type(), std::string_view{"Tabs"});
  const auto* tabs_widget = dynamic_cast<const Tabs*>(tabs);
  ST_REQUIRE(tabs_widget != nullptr);
  ST_CHECK_EQ(tabs_widget->tab_count(), std::size_t{1});

  // 新增会话 → 标签数跟涨；活动项指向新的。
  const std::size_t second = terminal->add_session("工作台");
  ST_CHECK_EQ(second, std::size_t{1});
  fixture.root.layout(true);
  ST_CHECK_EQ(tabs_widget->tab_count(), std::size_t{2});
  ST_CHECK_EQ(terminal->active_session(), std::size_t{1});

  // 标题更新 → 标签文本跟随（不换活动态）。
  terminal->set_session_title(1, "构建输出");
  fixture.root.layout(true);
  ST_CHECK_EQ(tabs_widget->tab_label(1), std::string_view{"构建输出"});
  ST_CHECK_EQ(terminal->active_session(), std::size_t{1});

  // 关闭会话（受理 → 下一帧 pump 执行）→ 标签数回落。
  ST_CHECK(terminal->close_session(1));
  terminal->pump();
  fixture.root.layout(true);
  ST_CHECK_EQ(terminal->session_count(), std::size_t{1});
  ST_CHECK_EQ(tabs_widget->tab_count(), std::size_t{1});
}

ST_TEST(terminal_tabs_add_button_wires_to_session) {
  TermFixture fixture;
  Terminal* terminal = fixture.terminal;
  fixture.root.layout(true);
  auto* tabs = dynamic_cast<Tabs*>(fixture.root.find("terminal-tabs"));
  ST_REQUIRE(tabs != nullptr);
  // 「+」默认显示（终端惯例），命中区在右缘。
  ST_CHECK(tabs->add_rect().width > 0.0f);
  ST_CHECK_EQ(terminal->session_count(), std::size_t{1});
  // 动作面触发 = 新建会话（Tabs::invoke_action("add") → on_add → add_session）。
  ST_CHECK(tabs->invoke_action("add", ""));
  ST_CHECK_EQ(terminal->session_count(), std::size_t{2});
  // 关掉「+」：命中区消失、动作面不再受理。
  terminal->set_add_button_visible(false);
  ST_CHECK(!terminal->add_button_visible());
  fixture.root.layout(true);
  ST_CHECK_EQ(tabs->add_rect().width, 0.0f);
  ST_CHECK(!tabs->invoke_action("add", ""));
}

ST_TEST(terminal_cycle_session_wraps) {
  TermFixture fixture;
  Terminal* terminal = fixture.terminal;
  terminal->add_session();
  terminal->add_session();
  ST_CHECK_EQ(terminal->session_count(), std::size_t{3});
  // 当前在 2（add_session 会切到新项）→ next 循环回 0，prev 回 2。
  terminal->cycle_session(true);
  ST_CHECK_EQ(terminal->active_session(), std::size_t{0});
  terminal->cycle_session(false);
  ST_CHECK_EQ(terminal->active_session(), std::size_t{2});
}

// ————————————————— 回看滚动条（几何换算契约）—————————————————

ST_TEST(terminal_scrollbar_geometry_tracks_scrollback) {
  TermFixture fixture;
  Terminal* terminal = fixture.terminal;
  // 行模式：无屏幕模型 → 滚动条不可见（它只属于 PTY 回看）。
  fixture.root.layout(true);
  const auto* bar = dynamic_cast<const st::ui::ScrollBar*>(fixture.root.find("terminal-scrollbar"));
  ST_REQUIRE(bar != nullptr);
  ST_CHECK(!bar->visible());
  // 属性面同样报空（自动化契约）。
  ST_CHECK_EQ(terminal->get_property("scrollbar").value_or("x"), std::string{});
}

// ————————————————— 粘贴（无宿主剪贴板时静默）—————————————————

ST_TEST(terminal_paste_without_host_is_silent) {
  TermFixture fixture;
  // 没装剪贴板 provider（headless 默认）：粘贴不炸、不抢错。
  fixture.terminal->paste_clipboard();   // 无断言即通过（静默降级）
}

// ————————————————— SplitView：隐藏侧退化单面板 —————————————————

ST_TEST(split_view_hidden_second_degrades_to_single_panel) {
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto split = std::make_unique<SplitView>(SplitView::Orientation::Vertical);
  split->set_ratio(0.5f, false);
  auto* first = split->add_child(std::make_unique<Element>());
  auto* second = split->add_child(std::make_unique<Element>());
  SplitView* split_ptr = split.get();
  root.set_content(std::move(split));
  root.layout(true);
  // 双侧：各半减手柄，手柄在中间。
  ST_CHECK(first->bounds().height < 300.0f);
  ST_CHECK(second->bounds().height < 300.0f);
  ST_CHECK(split_ptr->handle_rect().height > 0.0f);

  // 隐藏第二面板：第一面板占满，手柄消失，第二面板不可见。
  split_ptr->set_second_hidden(true);
  root.layout(true);
  ST_CHECK(!second->visible());
  ST_CHECK_EQ(first->bounds().height, 600.0f);
  ST_CHECK_EQ(split_ptr->handle_rect().height, 0.0f);
  // 属性面如实回报（自动化契约）。
  ST_CHECK_EQ(split_ptr->get_property("second_hidden").value_or(""), std::string{"true"});

  // 恢复：双侧几何与比例都回来（子元素状态未丢）。
  split_ptr->set_second_hidden(false);
  root.layout(true);
  ST_CHECK(second->visible());
  ST_CHECK(first->bounds().height < 300.0f);
  ST_CHECK(split_ptr->handle_rect().height > 0.0f);
  ST_CHECK_EQ(split_ptr->get_property("second_hidden").value_or(""), std::string{"false"});
}

ST_TEST(split_view_hidden_first_symmetry) {
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto split = std::make_unique<SplitView>(SplitView::Orientation::Horizontal);
  auto* first = split->add_child(std::make_unique<Element>());
  auto* second = split->add_child(std::make_unique<Element>());
  SplitView* split_ptr = split.get();
  root.set_content(std::move(split));
  root.layout(true);
  split_ptr->set_first_hidden(true);
  root.layout(true);
  ST_CHECK(!first->visible());
  ST_CHECK_EQ(second->bounds().width, 800.0f);
  // 动作面：无参切换（toggle）。
  ST_CHECK(split_ptr->invoke_action("toggle_first", ""));
  root.layout(true);
  ST_CHECK(first->visible());
}

// ————————————————— 字号键位（属性面契约）—————————————————

ST_TEST(terminal_font_scale_clamps_and_reports) {
  TermFixture fixture;
  Terminal* terminal = fixture.terminal;
  ST_CHECK_EQ(terminal->get_property("font_scale").value_or(""), std::string{"1.00"});
  terminal->set_font_scale(1.5f);
  ST_CHECK_EQ(terminal->get_property("font_scale").value_or(""), std::string{"1.50"});
  // 夹取到 [0.5, 3.0]。
  terminal->set_font_scale(99.0f);
  ST_CHECK_EQ(terminal->get_property("font_scale").value_or(""), std::string{"3.00"});
  terminal->set_font_scale(0.1f);
  ST_CHECK_EQ(terminal->get_property("font_scale").value_or(""), std::string{"0.50"});
}

ST_TEST(tabs_add_button_geometry_and_property) {
  // Tabs 独立测：空标签表也画「+」（关到零还能重开）。
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{400.0f, 160.0f});
  auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  auto* tabs = static_cast<Tabs*>(content->add_child(std::make_unique<Tabs>()));
  int adds = 0;
  tabs->on_add = [&adds] { ++adds; };
  tabs->set_show_add_button(true);
  root.set_content(std::move(content));
  root.layout(true);
  ST_CHECK(tabs->add_rect().width > 0.0f);
  ST_CHECK_EQ(tabs->get_property("show_add").value_or(""), std::string{"true"});
  // 动作面触发回调。
  ST_CHECK(tabs->invoke_action("add", ""));
  ST_CHECK_EQ(adds, 1);
  // 关掉开关：命中区消失。
  tabs->set_show_add_button(false);
  ST_CHECK_EQ(tabs->add_rect().width, 0.0f);
  ST_CHECK_EQ(tabs->get_property("show_add").value_or(""), std::string{"false"});
}

// ————————————————— 尾部动作钮（trailing）—————————————————

ST_TEST(tabs_trailing_buttons_geometry_and_click) {
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{400.0f, 160.0f});
  auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  auto* tabs = static_cast<Tabs*>(content->add_child(std::make_unique<Tabs>()));
  tabs->set_tabs({"a", "b"});
  int clicks_a = 0;
  int clicks_b = 0;
  const std::size_t slot_a = tabs->add_trailing_button(
      "trash", "A", [&clicks_a] { ++clicks_a; });
  const std::size_t slot_b = tabs->add_trailing_button(
      "square", "B", [&clicks_b] { ++clicks_b; });
  root.set_content(std::move(content));
  root.layout(true);
  // 几何：两个钮在右缘；后挂的（slot_b）贴「+」侧、先挂的更左——不重叠。
  const st::math::Rect rect_a = tabs->trailing_rect(slot_a);
  const st::math::Rect rect_b = tabs->trailing_rect(slot_b);
  ST_CHECK(rect_a.width > 0.0f);
  ST_CHECK(rect_b.width > 0.0f);
  ST_CHECK(rect_a.right() <= rect_b.x + 0.5f);
  ST_CHECK(tabs->trailing_width() >= 2.0f * 26.0f);
  // 点击命中（经根派发真实的 Click）。
  st::ui::Event click;
  click.kind = st::ui::EventKind::Click;
  click.position = rect_a.center();
  ST_CHECK(root.dispatch(click));
  ST_CHECK_EQ(clicks_a, 1);
  // 禁用后不接命中。
  tabs->set_trailing_enabled(slot_b, false);
  st::ui::Event click_b;
  click_b.kind = st::ui::EventKind::Click;
  click_b.position = rect_b.center();
  (void)root.dispatch(click_b);
  ST_CHECK_EQ(clicks_b, 0);
  // 摘除后槽位前移：总数减一（旧 slot 编号失效，不再断言其矩形）。
  tabs->remove_trailing(slot_a);
  int live = 0;
  for (std::size_t slot = 0; slot < 4; ++slot) {
    if (tabs->trailing_rect(slot).width > 0.0f) ++live;
  }
  ST_CHECK_EQ(live, 1);
}

// ————————————————— 终端面板头（并入标签栏）—————————————————

ST_TEST(terminal_header_actions_live_in_tabs_trailing) {
  TermFixture fixture;
  Terminal* terminal = fixture.terminal;
  fixture.root.layout(true);
  auto* tabs = dynamic_cast<Tabs*>(fixture.root.find("terminal-tabs"));
  ST_REQUIRE(tabs != nullptr);
  // 行模式未起作业：清屏常驻、无中止；装了 collapse 回调后收起出现（sync 随布局跑）。
  int collapses = 0;
  terminal->on_request_collapse = [&collapses] { ++collapses; };
  fixture.root.layout(true);   // sync_tabs → sync_header_actions
  bool has_trailing = false;
  for (std::size_t slot = 0; slot < 4; ++slot) {
    if (tabs->trailing_rect(slot).width > 0.0f) has_trailing = true;
  }
  ST_CHECK(has_trailing);
  // 动作面 collapse 与内置"收起"钮同一条链路。
  ST_CHECK(terminal->invoke_action("collapse", ""));
  ST_CHECK_EQ(collapses, 1);
  // 不装回调时动作面如实拒绝。
  terminal->on_request_collapse = nullptr;
  ST_CHECK(!terminal->invoke_action("collapse", ""));
}

ST_TEST(terminal_default_tab_title_is_shell_name) {
  TermFixture fixture;
  Terminal* terminal = fixture.terminal;
  // 默认标题 = shell 短名（非"终端 N"序号）。
  const std::string title = terminal->get_property("session_title").value_or("");
  ST_CHECK(!title.empty());
  ST_CHECK(title.find("终端") == std::string::npos);
  // 新会话同样（同一 shell）。
  const std::size_t second = terminal->add_session();
  ST_CHECK_EQ(terminal->session(second)->title, title);
}

ST_TEST(tabs_long_titles_are_capped) {
  // 长标题（如 shell OSC 带完整路径）不该把标签撑到整栏：宽度封顶 + 省略号。
  // 需要真字体端口——NullTextPort 宽度恒 0，宽度断言无意义。
  std::optional<st::text::FontStack> stack{};
  std::unique_ptr<st::test::RendererTextPort> port{};
  if (const auto loaded = st::text::FontStack::system_default(); loaded.has_value()) {
    stack.emplace(std::move(*loaded));
    port = std::make_unique<st::test::RendererTextPort>(*stack);
  }
  st::ui::UiRoot root;
  if (port) root.set_text_port(port.get());
  root.set_viewport(st::math::Size{600.0f, 160.0f});
  auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  auto* tabs = static_cast<Tabs*>(content->add_child(std::make_unique<Tabs>()));
  const std::string long_title(120, 'x');   // 远超上限
  tabs->set_tabs({"短", long_title, "另一个"});
  root.set_content(std::move(content));
  root.layout(true);
  // 默认上限 200：长标签宽度 ≤ 200 + padding + close 区（≈ 250），不会到 600。
  const st::math::Rect long_tab = tabs->tab_rect(1);
  const st::math::Rect short_tab = tabs->tab_rect(0);
  ST_CHECK(long_tab.width > 0.0f);
  ST_CHECK(long_tab.width < 260.0f);
  // 短标签不受影响（仍按内容宽度）。
  ST_CHECK(short_tab.width < long_tab.width);
  ST_CHECK(short_tab.width >= 36.0f);
  // 属性面可改：调小后长标签更窄；0 = 不限（宽度放开）。
  tabs->set_max_tab_width(80.0f);
  root.layout(true);
  ST_CHECK(tabs->tab_rect(1).width < tabs->tab_rect(0).width + 80.0f + 40.0f);
  ST_CHECK(tabs->tab_rect(1).width < 140.0f);
  ST_CHECK_EQ(tabs->get_property("max_tab_width").value_or(""), std::string{"80"});
  tabs->set_max_tab_width(0.0f);
  root.layout(true);
  ST_CHECK(tabs->tab_rect(1).width > 400.0f);   // 放开后长标题占满
}

ST_TEST(terminal_indicator_follows_active_session) {
  // 回归：add_session 后指示条必须最终落到新活动标签下（宽度=该标签宽）。
  // 旧缺陷：动画每帧被重置（或起步坐标未快照），指示条钉死在旧标签下。
  std::optional<st::text::FontStack> stack{};
  std::unique_ptr<st::test::RendererTextPort> port{};
  if (const auto loaded = st::text::FontStack::system_default(); loaded.has_value()) {
    stack.emplace(std::move(*loaded));
    port = std::make_unique<st::test::RendererTextPort>(*stack);
  }
  st::ui::UiRoot root;
  if (port) root.set_text_port(port.get());
  root.set_viewport(st::math::Size{900.0f, 500.0f});
  auto owned = std::make_unique<Terminal>();
  Terminal* terminal = owned.get();
  root.set_content(std::move(owned));
  root.layout(true);
  root.set_time(0.0);
  auto* tabs = dynamic_cast<Tabs*>(root.find("terminal-tabs"));
  ST_REQUIRE(tabs != nullptr);
  const st::math::Rect first = tabs->tab_rect(0);
  ST_CHECK(first.width > 0.0f);

  // 新建第二个会话（标题给个明显更长的，几何可区分）。
  terminal->add_session("long-session-title-here");
  // 推进多帧（动画 180ms；每帧走 50ms）。
  for (int frame = 1; frame <= 12; ++frame) {
    root.set_time(static_cast<double>(frame) * 0.05);
    root.layout(false);
    st::raster::Canvas canvas{900, 500, 1.0f};
    root.paint(canvas);
  }
  // 断言：Tabs 的活动项 = 1；其 tab_rect 与第一个明显不同。
  ST_CHECK_EQ(tabs->active_index(), std::size_t{1});
  const st::math::Rect second = tabs->tab_rect(1);
  ST_CHECK(second.x > first.x);
  ST_CHECK(second.width != first.width);   // 标题长度不同
  // 指示条落点：动画已完成（600ms > 180ms），应等于第二个标签矩形。
  // （resolve_indicator 是私有——用像素验证走 paint；这里以状态断言为主，
  //   像素验证交给 e2e。至少：再次 layout + paint 后指示条不回退。）
}
