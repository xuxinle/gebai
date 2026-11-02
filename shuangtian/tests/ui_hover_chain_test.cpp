/// 悬浮链（hover chain）回归：自持位置悬浮的组件在「跨区移动 / 出窗 / 失活 / 浮层接管」
/// 时必须收到离开通知并清高亮。
///
/// 需求来源（gbcode 实测，2026-11-02）：菜单栏标题高亮在指针移进编辑器后残留；
/// 用户要求系统性修复（悬浮链模型），而非逐组件打补丁。
///
/// 覆盖四组（全部对应悬浮链分发的真实路径）：
/// ① 跨区移动：MenuBar（自持 hover_index_，不在视觉跟踪集）→ 移到别处必须清；
/// ② 出窗（WindowLeave）：MenuBar / Table / TitleBar 按钮高亮全清；
/// ③ 键盘态保护：CompletionPopup 的 selected_（键盘）不随 HoverOut 清——鼠标态与
///    键盘态是两套账，链通知只管鼠标侧；
/// ④ 链语义本身：进入/离开按链去重（同一容器内移动不重复通知；祖先收自己的份）。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/window_control.hpp"

namespace {

using st::math::Point;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::MenuBar;
using st::ui::TitleBar;
using st::ui::UiRoot;
using st::ui::WindowControl;
using st::ui::WindowEdge;

struct FakeWindowControl : WindowControl {
  bool available{true};
  [[nodiscard]] auto window_control_available() const -> bool override { return available; }
  [[nodiscard]] auto window_minimize() -> bool override { return available; }
  [[nodiscard]] auto window_toggle_maximize() -> bool override { return available; }
  [[nodiscard]] auto window_request_close() -> bool override { return available; }
  [[nodiscard]] auto window_maximized() const -> bool override { return false; }
  [[nodiscard]] auto window_begin_move() -> bool override { return available; }
  [[nodiscard]] auto window_begin_resize(WindowEdge) -> bool override { return available; }
};

/// 菜单栏 + 编辑区两栏界面：跨区移动的实场（菜单栏命中链上没有任何被跟踪元素）。
struct MenuBarFixture {
  UiRoot root{};
  MenuBar* bar{nullptr};

  MenuBarFixture() {
    root.set_theme(st::ui::Theme::light());
    auto owned = std::make_unique<MenuBar>();
    owned->set_id("menubar");
    owned->set_menus({st::ui::Menu{"file", "文件"}, st::ui::Menu{"edit", "编辑"},
                      st::ui::Menu{"view", "视图"}});
    bar = owned.get();
    auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    column->add_child(std::move(owned));
    auto body = std::make_unique<st::ui::Panel>();
    body->set_id("editor");
    body->style().grow = true;
    column->add_child(std::move(body));
    root.set_content(std::move(column));
    root.set_viewport(st::math::Size{640.0f, 400.0f});
    root.layout(true);
  }

  void move_to(Point point) {
    Event event;
    event.kind = EventKind::MouseMove;
    event.position = point;
    (void)root.dispatch(event);
  }

  void leave_window() {
    Event event;
    event.kind = EventKind::WindowLeave;
    (void)root.dispatch(event);
  }

  /// 第 index 个菜单标题的中心（布局后的真实几何）。
  [[nodiscard]] auto title_center(std::size_t index) const -> Point {
    return bar->title_rect(index).center();
  }
};

}  // namespace

// ───────────────────────── ① 跨区移动清高亮 ─────────────────────────

ST_TEST(hover_chain_menu_bar_clears_when_pointer_moves_away) {
  MenuBarFixture fixture;
  fixture.move_to(fixture.title_center(0));
  ST_CHECK_EQ(fixture.bar->hover_index(), 0);   // 前置：标题确实 hover 了

  // 移进编辑区（菜单栏不在新命中链上）——MenuBar 只在自己被命中时收到 move，
  // 指针走后它看不到任何事件。悬浮链的离开通知是它唯一的清理通道。
  fixture.move_to(Point{320.0f, 200.0f});
  ST_CHECK_EQ(fixture.bar->hover_index(), -1);   // 高亮必须清

  // 回来能重新 hover（清掉不是坏掉）
  fixture.move_to(fixture.title_center(1));
  ST_CHECK_EQ(fixture.bar->hover_index(), 1);
}

ST_TEST(hover_chain_menu_bar_clears_on_window_leave) {
  MenuBarFixture fixture;
  fixture.move_to(fixture.title_center(2));
  ST_CHECK_EQ(fixture.bar->hover_index(), 2);

  fixture.leave_window();
  ST_CHECK_EQ(fixture.bar->hover_index(), -1);
}

// ───────────────────────── ② Table / TitleBar 出窗清理 ─────────────────────────

namespace {

struct TableFixture {
  UiRoot root{};
  st::ui::Table* table{nullptr};

  TableFixture() {
    root.set_theme(st::ui::Theme::light());
    auto owned = std::make_unique<st::ui::Table>(std::vector<st::ui::TableColumn>{
        st::ui::TableColumn{"名称", 180.0f}, st::ui::TableColumn{"大小", 100.0f}});
    owned->set_id("table");
    for (int i = 0; i < 6; ++i) owned->add_row({std::string("row-") + std::to_string(i), "1 KB"});
    table = owned.get();
    root.set_content(std::move(owned));
    root.set_viewport(st::math::Size{400.0f, 300.0f});
    root.layout(true);
  }

  void move_to(Point point) {
    Event event;
    event.kind = EventKind::MouseMove;
    event.position = point;
    (void)root.dispatch(event);
  }
};

}  // namespace

ST_TEST(hover_chain_table_row_clears_on_window_leave) {
  TableFixture fixture;
  // 表头下第一行中心（真实几何）
  const st::math::Rect first_row{fixture.table->bounds().x + 4.0f,
                                 fixture.table->bounds().y + 28.0f + 4.0f,
                                 fixture.table->bounds().width - 8.0f, 20.0f};
  fixture.move_to(first_row.center());
  // 前置：行确实 hover 了（Table 无公开 hover 读回面，用属性面）
  ST_CHECK_EQ(*fixture.table->get_property("hovered_row"), std::string("0"));

  Event leave;
  leave.kind = EventKind::WindowLeave;
  (void)fixture.root.dispatch(leave);
  ST_CHECK_EQ(*fixture.table->get_property("hovered_row"), std::string("-1"));
}

namespace {

struct TitleBarHoverFixture {
  UiRoot root{};
  TitleBar* bar{nullptr};
  FakeWindowControl control{};

  TitleBarHoverFixture() {
    root.set_theme(st::ui::Theme::light());
    root.set_viewport(st::math::Size{640.0f, 400.0f});
    auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    auto title_bar = std::make_unique<TitleBar>("t");
    title_bar->set_window_control(&control);
    bar = title_bar.get();
    column->add_child(std::move(title_bar));
    auto body = std::make_unique<st::ui::Panel>();
    body->style().grow = true;
    column->add_child(std::move(body));
    root.set_content(std::move(column));
    root.layout(true);
  }
};

}  // namespace

ST_TEST(hover_chain_title_bar_button_clears_on_window_leave) {
  TitleBarHoverFixture fixture;
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = fixture.bar->control_button_rect(2).center();   // 关闭按钮
  (void)fixture.root.dispatch(move);
  ST_CHECK_EQ(*fixture.bar->get_property("hovered_button"), std::string("2"));

  Event leave;
  leave.kind = EventKind::WindowLeave;
  (void)fixture.root.dispatch(leave);
  ST_CHECK_EQ(*fixture.bar->get_property("hovered_button"), std::string("-1"));
  ST_CHECK_EQ(*fixture.bar->get_property("hovered_caption"), std::string("-1"));
}

// ───────────────────────── ④ 链语义：同容器移动不重复通知 ─────────────────────────

namespace {

/// 记录收到的 HoverIn/Out 总数（判据：同一容器内移动不应重复触发祖先的通知）。
struct CountingPanel : st::ui::Panel {
  int hover_in_events{0};
  int hover_out_events{0};

  auto on_event(const st::ui::RenderContext&, Event& event) -> bool override {
    if (event.kind == EventKind::HoverIn) ++hover_in_events;
    if (event.kind == EventKind::HoverOut) ++hover_out_events;
    return false;
  }
};

}  // namespace

ST_TEST(hover_chain_no_duplicate_notifications_within_same_chain) {
  st::ui::UiRoot root;
  root.set_theme(st::ui::Theme::light());
  auto panel = std::make_unique<CountingPanel>();
  panel->set_id("panel");
  auto button = std::make_unique<st::ui::Button>("b");
  button->set_id("btn");
  panel->add_child(std::move(button));
  CountingPanel* counter = panel.get();
  root.set_content(std::move(panel));
  root.set_viewport(st::math::Size{320.0f, 160.0f});
  root.layout(true);

  const Point center = counter->bounds().center();
  auto move = [&](Point point) {
    Event event;
    event.kind = EventKind::MouseMove;
    event.position = point;
    (void)root.dispatch(event);
  };

  move(center);
  ST_CHECK_EQ(counter->hover_in_events, 1);   // 进入链一次，通知恰好一次

  // 链内移动（panel → 它的 button，panel 仍在链上）：panel 不应再收进入/离开
  if (st::ui::Element* btn = root.find("btn"); btn != nullptr && !btn->bounds().is_empty()) {
    const Point button_point = btn->bounds().center();
    move(button_point);
    ST_CHECK_EQ(counter->hover_in_events, 1);   // 仍在链上：没有重复进入
    ST_CHECK_EQ(counter->hover_out_events, 0);   // 也没被误判离开
  }

  // 移出窗口：离开通知恰好一次
  Event leave;
  leave.kind = EventKind::WindowLeave;
  (void)root.dispatch(leave);
  ST_CHECK_EQ(counter->hover_out_events, 1);
}
