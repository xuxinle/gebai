/// 菜单组件测试：MenuBar（标题条 + 下拉面板）与 ContextMenu（右键上下文菜单）。
///
/// 挂载契约（见 menu.hpp 文档）：
/// - MenuBar 点击标题 → `on_open_menu(index)` → 调用方 `make_panel(index)` 造面板
///   → `add_overlay(panel, Stack)`；面板 on_activate 转 on_action，on_close 清 open_index。
/// - ContextMenu 挂 FillViewport：面板外不拦截命中（点击穿透后转 on_close）、Esc 关闭。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/menu.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::ContextMenu;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::Menu;
using st::ui::MenuBar;
using st::ui::MenuItem;
using st::ui::MenuPanel;
using st::ui::UiRoot;

/// 测试专用“障碍浮层”：整视口命中点击（模拟模态屏障），但按契约**放行指针移动**。
///
/// 为何需要（2026-10-07）：真缺陷是 dsl 的 `OverlayHost` 屏障把 `MouseMove`
/// 也一起吞了，导致面板开着时菜单栏收不到 hover。那个类是 dsl.cpp 的私有实现，
/// 测试拿不到它，所以在这里复刻它的**契约**（两个命中函数分开）——
/// 一旦框架把移动也按普通命中走，本用例即变红。
class BarrierProbe : public st::ui::Element {
 public:
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "BarrierProbe"; }
  [[nodiscard]] auto hit_test(st::math::Point) const noexcept -> bool override { return true; }
  [[nodiscard]] auto hit_test_pointer_move(st::math::Point point) const noexcept -> bool override {
    return hit_test_children(point);   // 移动只看自己的子元素，不占整屏
  }
};

[[nodiscard]] auto sample_menus() -> std::vector<Menu> {
  return {
      Menu{"file", "文件",
           {MenuItem{"new", "新建"}, MenuItem{"open", "打开"},
            MenuItem{.separator = true},  // 分隔线
            MenuItem{"save", "保存", false, true}}},  // separator=false, checked=true
      Menu{"edit", "编辑", {MenuItem{"undo", "撤销"}, MenuItem{"redo", "重做"}}},
  };
}

[[nodiscard]] auto click_at(UiRoot& root, float x, float y) -> bool {
  Event event;
  event.kind = EventKind::Click;
  event.position = st::math::Point{x, y};
  return root.dispatch(event);
}

[[nodiscard]] auto press(UiRoot& root, const std::string& key) -> bool {
  Event event;
  event.kind = EventKind::KeyDown;
  event.key = key;
  return root.dispatch(event);
}

/// **真实鼠标点击的完整事件序列**（`Down → Up → Click`）。
///
/// 为何不能只发一个 `Click`：真实后端在松开时补发 Click，而“按下”本身也是一个事件。
/// 只发 Click 的测试**看不见“同一手势被处理两次”**这类缺陷——菜单项正是这么漏的：
/// `MouseDown` 与 `Click` 合在一个 case 里，于是按下就执行了动作，
/// 真正的 Click 早已落在别的元素上（用户感受：“点下去菜单就没了，还透到下面”）。
/// 与协议 `input.mouse{kind:"click"}` 同序。
void dispatch_click_sequence(UiRoot& root, float x, float y) {
  const st::math::Point point{x, y};
  for (const EventKind kind : {EventKind::MouseDown, EventKind::MouseUp, EventKind::Click}) {
    Event event;
    event.kind = kind;
    event.position = point;
    event.button = 1;
    (void)root.dispatch(event);
  }
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————

ST_TEST(menu_bar_layout_and_titles) {
  UiRoot root;
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->add_child(std::move(bar));
  root.set_content(std::move(page));
  root.layout(true);

  ST_CHECK_EQ(bar_ptr->menu_count(), std::size_t{2});
  ST_CHECK_EQ(bar_ptr->menu_label(0), std::string("文件"));
  ST_CHECK_EQ(bar_ptr->menu_id(1), std::string("edit"));
  // 标题矩形：从左排起、宽度为正、高度为栏高
  const st::math::Rect first = bar_ptr->title_rect(0);
  const st::math::Rect second = bar_ptr->title_rect(1);
  ST_CHECK_EQ(first.x, 0.0f);
  ST_CHECK(first.width > 0.0f);
  ST_CHECK_EQ(first.height, 32.0f);
  ST_CHECK(second.x >= first.right() - 0.5f);
  ST_CHECK_EQ(second.height, 32.0f);
}

ST_TEST(menu_bar_highlight_does_not_touch_the_container_border) {
  // 用户报「菜单悬浮时，下边框线断开」（2026-10-07）。
  //
  // 根因：悬浮/展开的高亮块**撑满了标题矩形的全高**，而 `title_rect` 的高度等于
  // 容器高——于是高亮的下沿正好压在容器底边那条分隔线上，**圆角的抗锯齿**把线
  // 啃出一个弧形缺口（实测断口 46 逻辑 px，正好是被悬浮那一项的宽度）。
  //
  // 不变量：**高亮块必须内缩在容器内**（不与容器上下边框同位置）。
  // 回退 `highlight_rect` 成“返回 title 原样”即变红。
  UiRoot root;
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->add_child(std::move(bar));
  root.set_content(std::move(page));
  root.layout(true);

  for (std::size_t index = 0; index < bar_ptr->menu_count(); ++index) {
    const st::math::Rect title = bar_ptr->title_rect(index);
    const st::math::Rect highlight = bar_ptr->highlight_rect(title);
    // ① 上沿在标题内（下移）——不与容器顶边重合
    ST_CHECK(highlight.y > title.y);
    // ② 下沿在标题内（上收）——**不与容器底边重合**，这是“线不断”的充分条件
    ST_CHECK(highlight.bottom() < title.bottom());
    // ③ 宽度不变（只上下内缩，不改变命中/排版）
    ST_CHECK_EQ(highlight.width, title.width);
    ST_CHECK_EQ(highlight.x, title.x);
    // ④ 内缩对称且为正
    ST_CHECK(highlight.y - title.y > 0.0f);
    ST_CHECK(std::abs((highlight.y - title.y) - (title.bottom() - highlight.bottom())) < 0.01f);
  }
}

ST_TEST(menu_bar_click_opens_panel_overlay) {
  UiRoot root;
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  MenuPanel* opened = nullptr;
  bar_ptr->on_open_menu = [&](std::size_t index) {
    auto panel = bar_ptr->make_panel(index);
    opened = panel.get();
    root.add_overlay(std::move(panel));
  };
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->add_child(std::move(bar));
  root.set_content(std::move(page));
  root.layout(true);

  ST_CHECK_EQ(root.overlay_count(), std::size_t{0});
  ST_CHECK(click_at(root, 8.0f, 16.0f));  // 点第一个标题（"文件"）
  ST_CHECK_EQ(root.overlay_count(), std::size_t{1});
  ST_CHECK_EQ(bar_ptr->open_index(), std::size_t{0});
  ST_REQUIRE(opened != nullptr);
  root.layout(true);
  // 面板条目数与数据一致（含分隔线行），锚定在标题正下方
  ST_CHECK_EQ(opened->item_count(), std::size_t{4});
  ST_CHECK_EQ(opened->item_id(0), std::string("new"));
  ST_CHECK_EQ(opened->bounds().x, bar_ptr->title_rect(0).x);
  ST_CHECK(opened->bounds().y >= bar_ptr->title_rect(0).bottom() - 0.5f);
  ST_CHECK_EQ(opened->item_rect(0).height, 32.0f);
}

ST_TEST(menu_panel_keyboard_navigation) {
  MenuPanel panel({MenuItem{"a", "A"}, MenuItem{.separator = true}, MenuItem{"b", "B"},
                   MenuItem{"c", "C"}});
  st::ui::Theme theme = st::ui::Theme::light();
  st::ui::RenderContext context{theme, nullptr, 0.0};
  panel.arrange(context, st::math::Rect{0.0f, 0.0f, 200.0f, 128.0f});

  ST_CHECK_EQ(panel.highlighted(), MenuPanel::kNoIndex);
  Event down;
  down.kind = EventKind::KeyDown;
  down.key = "ArrowDown";
  ST_CHECK(panel.on_event(context, down));
  ST_CHECK_EQ(panel.highlighted(), std::size_t{0});   // 首个可激活项
  ST_CHECK(panel.on_event(context, down));
  ST_CHECK_EQ(panel.highlighted(), std::size_t{2});   // 跳过分隔线（1）
  Event up;
  up.kind = EventKind::KeyDown;
  up.key = "ArrowUp";
  ST_CHECK(panel.on_event(context, up));
  ST_CHECK_EQ(panel.highlighted(), std::size_t{0});
}

ST_TEST(menu_bar_action_callback_fires) {
  UiRoot root;
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  std::string got_menu;
  std::string got_item;
  bar_ptr->on_action = [&](const std::string& menu, const std::string& item) {
    got_menu = menu;
    got_item = item;
  };
  bar_ptr->on_open_menu = [&](std::size_t index) { root.add_overlay(bar_ptr->make_panel(index)); };
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->add_child(std::move(bar));
  root.set_content(std::move(page));
  root.layout(true);

  // 打开"文件"菜单，点击第 4 行（save）
  ST_CHECK(click_at(root, 8.0f, 16.0f));
  root.layout(true);
  const MenuPanel* panel =
      static_cast<const MenuPanel*>(root.overlay_at(root.overlay_count() - 1));
  ST_REQUIRE(panel != nullptr);
  const st::math::Rect save_row = panel->item_rect(3);
  ST_CHECK(click_at(root, save_row.x + 10.0f, save_row.y + 16.0f));
  ST_CHECK_EQ(got_menu, std::string("file"));
  ST_CHECK_EQ(got_item, std::string("save"));

  // 打开状态在面板 on_close 后清除
  ST_CHECK_EQ(bar_ptr->open_index(), MenuBar::kNoIndex);
}

ST_TEST(context_menu_position_and_viewport_clamp) {
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));

  // 常规位置：锚点右下展开
  auto menu = ContextMenu::make(st::math::Point{100.0f, 100.0f},
                                {MenuItem{"cut", "剪切"}, MenuItem{"copy", "复制"}});
  const ContextMenu* ptr = menu.get();
  root.add_overlay(std::move(menu), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);
  const st::math::Rect panel = ptr->panel()->bounds();
  ST_CHECK_EQ(panel.x, 100.0f);
  ST_CHECK_EQ(panel.y, 100.0f);
  ST_CHECK(panel.width >= MenuPanel::kMinPanelWidth);
  ST_CHECK_EQ(panel.height, 64.0f);  // 2 项 × 32

  // 右下角锚点：面板向左上翻转，完整留在视口内
  UiRoot root2;
  root2.set_viewport(st::math::Size{800.0f, 600.0f});
  root2.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  auto corner = ContextMenu::make(st::math::Point{790.0f, 590.0f},
                                  {MenuItem{"a", "A"}, MenuItem{"b", "B"}, MenuItem{"c", "C"}});
  const ContextMenu* corner_ptr = corner.get();
  root2.add_overlay(std::move(corner), UiRoot::OverlayLayout::FillViewport);
  root2.layout(true);
  const st::math::Rect flipped = corner_ptr->panel()->bounds();
  ST_CHECK(flipped.right() <= 800.0f + 0.5f);
  ST_CHECK(flipped.bottom() <= 600.0f + 0.5f);
  ST_CHECK(flipped.x >= 0.0f);
  ST_CHECK(flipped.y >= 0.0f);
}

ST_TEST(context_menu_outside_click_closes) {
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));

  int closed = 0;
  auto menu = ContextMenu::make(st::math::Point{400.0f, 300.0f}, {MenuItem{"x", "X"}});
  menu->on_close = [&closed]() { ++closed; };
  const ContextMenu* ptr = menu.get();
  root.add_overlay(std::move(menu), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  // 命中：面板内即菜单本体（dismiss barrier 形态）；面板内命中返回菜单自己
  ST_CHECK(root.hit_test(st::math::Point{410.0f, 310.0f}) == ptr);

  // 面板外点击：dismiss barrier 拦下并触发 on_close
  ST_CHECK(click_at(root, 10.0f, 10.0f));
  ST_CHECK_EQ(closed, 1);
  root.remove_overlay(root.overlay_at(0));
  ST_CHECK_EQ(root.overlay_count(), std::size_t{0});
}

ST_TEST(context_menu_escape_closes) {
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));

  int closed = 0;
  auto menu = ContextMenu::make(st::math::Point{400.0f, 300.0f}, {MenuItem{"x", "X"}});
  menu->on_close = [&closed]() { ++closed; };
  root.add_overlay(std::move(menu), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  ST_CHECK(press(root, "Escape"));  // 浮层优先吃 Esc
  ST_CHECK_EQ(closed, 1);
}

ST_TEST(context_menu_item_click_activates) {
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));

  std::string picked;
  int closed = 0;
  auto menu = ContextMenu::make(st::math::Point{50.0f, 50.0f},
                                {MenuItem{"rename", "重命名"}, MenuItem{"remove", "删除"}});
  menu->panel()->on_activate = [&picked](std::size_t index) {
    picked = index == 0 ? "rename" : "remove";
  };
  menu->on_close = [&closed]() { ++closed; };
  const ContextMenu* ptr = menu.get();
  root.add_overlay(std::move(menu), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  // 点击第 2 行：激活 + 关闭
  const st::math::Rect row = ptr->panel()->item_rect(1);
  ST_CHECK(click_at(root, row.x + 20.0f, row.y + 16.0f));
  ST_CHECK_EQ(picked, std::string("remove"));
  ST_CHECK_EQ(closed, 1);
}

// ————————————————————————————————————————————————————————————————————————————
// 面板生命周期：关闭必须通知调用方（否则 overlay 留在屏上）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(menu_bar_panel_close_notifies_caller) {
  // 回归用例：`make_panel` 曾把面板的 `on_close` 接到自己的 `set_open_index` 上，
  // 于是激活条目 / Esc 只改了菜单栏内部状态，**调用方收不到通知** →
  // 挂上去的 overlay 永远摘不掉（实测：菜单选完之后面板叠在界面上不消失）。
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  root.set_content(std::move(bar));
  root.layout(true);

  int closed = 0;
  std::string action;
  bar_ptr->on_action = [&action](const std::string&, const std::string& item) { action = item; };
  bar_ptr->on_menu_close = [&closed]() { ++closed; };

  auto panel = bar_ptr->make_panel(0);
  ST_CHECK(panel != nullptr);
  MenuPanel* panel_ptr = panel.get();
  root.add_overlay(std::move(panel), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  // 激活“保存”（第 4 项）→ 动作派发 + 关闭通知
  const st::math::Rect row = panel_ptr->item_rect(3);
  ST_CHECK(click_at(root, row.x + 20.0f, row.y + 16.0f));
  ST_CHECK_EQ(action, std::string("save"));
  ST_CHECK_EQ(closed, 1);
  ST_CHECK_EQ(bar_ptr->open_index(), MenuBar::kNoIndex);
}

/// 一次物理点击只能**激活一次**，且面板在**松开前**不能没。
///
/// 回归（真实鼠标实测，2026-10-07）：`MenuPanel::on_event` 把 `Click` 与 `MouseDown`
/// 合在一个 case 里，于是**按下那一刻**就执行了动作并关闭面板——等真正的 `Click`
/// 到达时面板早已不在了，那个 Click 与 `MouseUp` 就落到**下面那个元素**上。
/// 用户看到的就是“点菜单秒退，还透到下层”。
///
/// 同一个文件里 `MenuBar` 的注释早就记过这个坑（“曾经把 Click 与 MouseDown 合在
/// 一个 case 里——一次物理点击会触发两次回调”）——当时只修了 MenuBar，同族的
/// `MenuPanel` 漏了。所以这条用例两件事一起钉：
/// ① 按下**不激活**；② 完整序列下来只激活**一次**。
ST_TEST(menu_panel_item_activates_once_per_physical_click) {
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  root.set_content(std::move(bar));
  root.layout(true);

  int activated = 0;
  int closed = 0;
  std::string action;
  bar_ptr->on_action = [&action, &activated](const std::string&, const std::string& item) {
    ++activated;
    action = item;
  };
  bar_ptr->on_menu_close = [&closed]() { ++closed; };

  auto panel = bar_ptr->make_panel(0);
  MenuPanel* panel_ptr = panel.get();
  root.add_overlay(std::move(panel), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  const st::math::Rect row = panel_ptr->item_rect(0);
  const float x = row.x + 20.0f;
  const float y = row.y + row.height * 0.5f;

  // ① 按下：**不得**激活（否则面板会在松开前就关掉，点击随即透到下层）
  {
    Event down;
    down.kind = EventKind::MouseDown;
    down.position = st::math::Point{x, y};
    down.button = 1;
    (void)root.dispatch(down);
  }
  ST_CHECK_EQ(activated, 0);
  ST_CHECK_EQ(closed, 0);
  ST_CHECK(panel_ptr->bounds().contains(st::math::Point{x, y}));   // 面板还在原位

  // ② 松开 + Click：这一次才激活，而且**只有一次**
  dispatch_click_sequence(root, x, y);
  ST_CHECK_EQ(activated, 1);
  ST_CHECK_EQ(action, std::string("new"));
  ST_CHECK_EQ(closed, 1);
}

ST_TEST(menu_bar_click_on_open_title_toggles_closed_and_never_leaks_double_click) {
  // 两条契约（都来自真实窗口实测的“点菜单秒退”）：
  //
  // ① **再点已打开的标题 = 关闭**（toggle 手感，与浏览器/VSCode 菜单栏一致）。
  // ② 双击序列不得把窗口动作带出来：菜单栏长在标题栏附属槽里，
  //    `DoubleClick` 漏出去就会被当成“双击标题区”而最大化窗口。
  //    此前 `MenuBar` 根本没有 `DoubleClick` 分支——漏出去的正是它。
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  root.set_content(std::move(bar));
  root.layout(true);

  int opened = 0;
  int closed = 0;
  bar_ptr->on_open_menu = [&opened](std::size_t) { ++opened; };
  bar_ptr->on_menu_close = [&closed]() { ++closed; };
  bar_ptr->on_action = [](const std::string&, const std::string&) {};

  const st::math::Rect title = bar_ptr->title_rect(0);
  const float x = title.x + title.width * 0.5f;
  const float y = title.y + title.height * 0.5f;
  const auto click = [&]() {
    Event event;
    event.kind = EventKind::Click;
    event.position = st::math::Point{x, y};
    event.button = 1;
    return root.dispatch(event);
  };

  ST_CHECK(click());                       // 开
  ST_CHECK_EQ(opened, 1);
  ST_CHECK_EQ(bar_ptr->open_index(), std::size_t{0});
  ST_CHECK(click());                       // 同一个标题再点：关
  ST_CHECK_EQ(closed, 1);
  ST_CHECK_EQ(bar_ptr->open_index(), MenuBar::kNoIndex);
  ST_CHECK(click());                       // 又开
  ST_CHECK_EQ(opened, 2);

  // 双击：必须被菜单栏**收下**（返回 true），否则会冒泡到标题栏变成最大化。
  Event dbl;
  dbl.kind = EventKind::DoubleClick;
  dbl.position = st::math::Point{x, y};
  dbl.button = 1;
  dbl.click_count = 2;
  ST_CHECK(root.dispatch(dbl));
}

ST_TEST(menu_bar_escape_notifies_close_but_does_not_swallow) {  // Esc 的两条约定：① 通知调用方（走 on_close → on_menu_close）；
  // ② **不吞键**（返回 false）——调用方可能还有自己的 Esc 语义要处理。
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  root.set_content(std::move(bar));
  root.layout(true);

  int closed = 0;
  bar_ptr->on_menu_close = [&closed]() { ++closed; };
  auto panel = bar_ptr->make_panel(1);
  root.add_overlay(std::move(panel), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  (void)press(root, "Escape");
  ST_CHECK_EQ(closed, 1);
  ST_CHECK_EQ(bar_ptr->open_index(), MenuBar::kNoIndex);
}

ST_TEST(menu_bar_hover_switches_open_menu) {
  // 已打开面板时悬停到另一标题：应当**发起切换**（on_open_menu 被再次调用），
  // 而不是什么都不做——否则用户从“文件”滑到“编辑”时面板不换，
  // 再点一下就把两张面板叠在一起（实测缺陷）。
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  root.set_content(std::move(bar));
  root.layout(true);

  std::vector<std::size_t> opened;
  bar_ptr->on_open_menu = [&opened](std::size_t index) { opened.push_back(index); };
  (void)bar_ptr->make_panel(0);
  ST_CHECK_EQ(bar_ptr->open_index(), static_cast<std::size_t>(0));

  const st::math::Rect second = bar_ptr->title_rect(1);
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = st::math::Point{second.x + second.width * 0.5f, second.y + second.height * 0.5f};
  (void)root.dispatch(move);

  ST_CHECK_EQ(opened.size(), static_cast<std::size_t>(1));
  ST_CHECK_EQ(opened[0], static_cast<std::size_t>(1));
  ST_CHECK_EQ(bar_ptr->open_index(), static_cast<std::size_t>(1));
}

ST_TEST(menu_bar_hover_reaches_the_bar_through_the_open_panel_barrier) {
  // 用户报「点击其他按钮直接切换列表」（2026-10-07）。
  //
  // 根因不在菜单栏，而在 **overlay 屏障把 `MouseMove` 一并吞了**：
  // 面板开着时 `menubar.hovered` 恒为 false，菜单栏根本收不到移动，
  // 于是“移到别的标题就切换”形同不存在（真机实测）。
  //
  // 不变量：**屏障只拦“点”，放行“移动”**（否则任何“浮层开着仍需 hover 的宿主”都会坏）。
  // 回退 `OverlayHost::on_event` 的 MouseMove 分支为 `return true`、
  // 或回退 `UiRoot` 的移动命中为 `hit_test`，本用例即变红。
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  root.set_content(std::move(bar));
  root.layout(true);

  std::vector<std::size_t> opened;
  bar_ptr->on_open_menu = [&opened](std::size_t index) { opened.push_back(index); };
  // **前提：先打开第 0 项**——切换只在“已有菜单开着”时才发生（否则移动只是普通 hover）。
  // `make_panel` 会置 `open_index_`；这里不要那个面板，接住返回值后丢弃。
  (void)bar_ptr->make_panel(0);
  ST_CHECK_EQ(bar_ptr->open_index(), static_cast<std::size_t>(0));
  // 挂一个**屏障形态**的浮层：整个视口命中（模拟 `menu_panel_overlay` 的 barrier），
  // 但按契约它只该拦“点”、放行“移动”。
  root.add_overlay(std::make_unique<BarrierProbe>(), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  const st::math::Rect second = bar_ptr->title_rect(1);
  const st::math::Point over_second{second.x + second.width * 0.5f,
                                    second.y + second.height * 0.5f};
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = over_second;
  (void)root.dispatch(move);

  // ① 菜单栏确实收到了 hover（屏障没把它挡在外面）
  ST_CHECK_EQ(bar_ptr->hover_index(), static_cast<std::size_t>(1));
  // ② 并因此发起了切换
  ST_CHECK_EQ(opened.size(), static_cast<std::size_t>(1));
  ST_CHECK_EQ(bar_ptr->open_index(), static_cast<std::size_t>(1));
}

ST_TEST(menu_panel_anchor_follows_the_switched_title) {
  // 用户报「菜单激活列表要跟随菜单按钮的位置，不再都是在最左边弹出」（2026-10-07）。
  //
  // 根因：锚点在**声明式重建时**就拍死了，而那时 `MenuBar::title_widths_` 还是空的
  //（测量在重建之后）——`title_rect()` 返回 `x=栏左, width=0`，面板永远停在最左边。
  //（真机实测锚点就是 `36,0,0,40`。）
  //
  // 修法：锚点改为**惰性求值**（`set_anchor_source`），布局时才算。
  // 本用例钉住：切换后锚点等于**目标标题**的矩形，且锚点宽度非零。
  UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto bar = std::make_unique<MenuBar>();
  bar->set_menus(sample_menus());
  MenuBar* bar_ptr = bar.get();
  root.set_content(std::move(bar));
  root.layout(true);

  auto panel = bar_ptr->make_panel(0);
  MenuPanel* panel_ptr = panel.get();
  // 与真实用法一致（`dsl::menu_panel_overlay` 也走 FillViewport）：面板自己按锚点定位，
  // 不靠调用方分给它的矩形。
  root.add_overlay(std::move(panel), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  for (std::size_t index = 0; index < bar_ptr->menu_count(); ++index) {
    const st::math::Rect title = bar_ptr->title_rect(index);
    ST_CHECK(title.width > 0.0f);   // 测量已跑：标题矩形是真实几何
    bar_ptr->bind_panel(*panel_ptr, index);
    root.layout(true);
    const st::math::Rect anchor = panel_ptr->anchor_rect();
    ST_CHECK_EQ(anchor.x, title.x);
    ST_CHECK_EQ(anchor.width, title.width);
    ST_CHECK(anchor.width > 0.0f);            // 非零 = 不是“拍死时的空矩形”
    // 面板的 `bounds_` 必须落在标题下方（`arrange` 走锚点分支的产物）。
    // 注：用 FillViewport 挂才与真实用法一致（`menu_panel_overlay` 就是那样挂）。
    ST_CHECK_EQ(panel_ptr->bounds().x, title.x);
  }
}
