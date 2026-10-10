/// 窗框单测：`ui::TitleBar`（自绘标题栏）+ `ui::resize_edge_at`（边缘判定纯函数）。
///
/// 为什么这两者必须一起测：窗框的**可用性**全押在"边缘/按钮/标题区三者的命中划分"上，
/// 而这份划分由 `resize_edge_at`（纯函数）与组件几何共同决定。只测其一都会留下
/// "代码写对了但划错了区"的缺口——而它不会报错，只表现为"拖不动/点不着"。
///
/// 重点覆盖（都是曾经在别处踩过的坑）：
/// - 边缘判定**必须有上界**：远离窗口的点不能命中（否则"鼠标在屏幕另一头、窗口却变宽"）；
/// - 按钮**不参与拖动**（两者的命中区不能重叠）；
/// - 无宿主（无窗口）时动作**如实拒绝**、但**画面照旧**（跨平台无差异的落点）；
/// - 松手位置拖出按钮 = 取消（系统标题栏同款语义）。

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/path.hpp"
#include "st/test/test.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/element.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/window_control.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::TitleBar;
using st::ui::WindowControl;
using st::ui::WindowEdge;

/// 记账用的假宿主：把每个窗口动作记下来，并可配置"可用/不可用"。
///
/// 为什么要它而不是直接冲后端跑：无头环境根本没有窗口，而**断言的正是"没有窗口时
/// 契约怎么表现"**；真窗口只用来测"有窗口时能不能用"（那由 `tools/title_bar_probe.py`
/// 与实机验证覆盖）。
struct FakeWindowControl : WindowControl {
  bool available{true};
  /// 后端是否接受 `begin_resize`（Win32 走 `WM_NCHITTEST`，这条路返回 false）。
  bool resize_accepted{true};
  int minimize_calls{0};
  int maximize_calls{0};
  int close_calls{0};
  int move_calls{0};
  int resize_calls{0};
  WindowEdge last_resize_edge{WindowEdge::None};
  bool maximized_state{false};

  [[nodiscard]] auto window_control_available() const -> bool override { return available; }
  [[nodiscard]] auto window_minimize() -> bool override {
    ++minimize_calls;
    return available;
  }
  [[nodiscard]] auto window_toggle_maximize() -> bool override {
    ++maximize_calls;
    if (!available) return false;
    maximized_state = !maximized_state;
    return true;
  }
  [[nodiscard]] auto window_request_close() -> bool override {
    ++close_calls;
    return available;
  }
  [[nodiscard]] auto window_begin_move() -> bool override {
    ++move_calls;
    return available;
  }
  [[nodiscard]] auto window_begin_resize(WindowEdge edge) -> bool override {
    ++resize_calls;
    last_resize_edge = edge;
    return available && resize_accepted;
  }
  [[nodiscard]] auto window_maximized() const -> bool override { return maximized_state; }
};

/// 夹具：标题栏放在**列容器**里（真实用法——窗框是列布局的第一行，高度由自己定）。
///
/// 为什么不能直接把它当根内容：根内容会被拉满整个视口（`UiRoot::layout` 的根矩形语义），
/// 于是标题栏变成 1280×720——按钮也跟着变成 92px 高。那不是组件的缺陷，
/// 而是"根内容"这个位置本身的语义（它已经占满窗口，不需要再画窗框）。
struct BarFixture {
  st::ui::UiRoot root{};
  TitleBar* bar{nullptr};
  FakeWindowControl control{};

  explicit BarFixture(bool with_control = true) {
    root.set_viewport(st::math::Size{1280.0f, 720.0f});
    auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    column->set_id("page");
    auto title_bar = std::make_unique<TitleBar>("renderer.cpp - gbcode");
    title_bar->set_id("titlebar");
    title_bar->set_icon("code");
    if (with_control) title_bar->set_window_control(&control);
    bar = title_bar.get();
    column->add_child(std::move(title_bar));
    // 余下区域（正文占位）：没有它的话列容器只有 40px，看不出"标题栏只占一行"。
    auto body = std::make_unique<st::ui::Panel>();
    body->set_id("body");
    body->style().grow = true;
    column->add_child(std::move(body));
    root.set_content(std::move(column));
    root.layout(true);
  }

  ~BarFixture() { root.set_content(nullptr); }

  BarFixture(const BarFixture&) = delete;
  auto operator=(const BarFixture&) -> BarFixture& = delete;

  auto send(EventKind kind, Point point, int clicks = 1) -> bool {
    Event event;
    event.kind = kind;
    event.position = point;
    event.button = 1;
    event.click_count = clicks;
    return root.dispatch(event);
  }

  /// 按钮中心（index：0=最小化 1=最大化 2=关闭）。
  [[nodiscard]] auto button_center(std::size_t index) const -> Point {
    return bar->control_button_rect(index).center();
  }
};

// ───────────────────────── 边缘判定（纯函数） ─────────────────────────

ST_TEST(title_bar_resize_edge_detects_bands_and_corners) {
  const st::math::Size size{800.0f, 600.0f};
  constexpr float kBorder = 6.0f;
  ST_CHECK(st::ui::resize_edge_at(Point{3.0f, 300.0f}, size, kBorder) == WindowEdge::Left);
  ST_CHECK(st::ui::resize_edge_at(Point{797.0f, 300.0f}, size, kBorder) == WindowEdge::Right);
  ST_CHECK(st::ui::resize_edge_at(Point{400.0f, 3.0f}, size, kBorder) == WindowEdge::Top);
  ST_CHECK(st::ui::resize_edge_at(Point{400.0f, 597.0f}, size, kBorder) == WindowEdge::Bottom);
  ST_CHECK(st::ui::resize_edge_at(Point{3.0f, 3.0f}, size, kBorder) == WindowEdge::TopLeft);
  ST_CHECK(st::ui::resize_edge_at(Point{797.0f, 3.0f}, size, kBorder) == WindowEdge::TopRight);
  ST_CHECK(st::ui::resize_edge_at(Point{3.0f, 597.0f}, size, kBorder) == WindowEdge::BottomLeft);
  ST_CHECK(st::ui::resize_edge_at(Point{797.0f, 597.0f}, size, kBorder) == WindowEdge::BottomRight);
  // 正中间不是边缘：否则窗口整块都成了"缩放区"，中间再也点不动。
  ST_CHECK(st::ui::resize_edge_at(Point{400.0f, 300.0f}, size, kBorder) == WindowEdge::None);
}

ST_TEST(title_bar_resize_edge_rejects_points_far_outside) {
  // 这一条是"鼠标在屏幕另一头、窗口却跟着变宽"的防线：`WM_NCHITTEST` 在拖拽期间
  // 收到的坐标可以离窗口很远，判定必须有上界。
  const st::math::Size size{800.0f, 600.0f};
  constexpr float kBorder = 6.0f;
  ST_CHECK(st::ui::resize_edge_at(Point{-500.0f, 300.0f}, size, kBorder) == WindowEdge::None);
  ST_CHECK(st::ui::resize_edge_at(Point{1300.0f, 300.0f}, size, kBorder) == WindowEdge::None);
  ST_CHECK(st::ui::resize_edge_at(Point{400.0f, -400.0f}, size, kBorder) == WindowEdge::None);
  ST_CHECK(st::ui::resize_edge_at(Point{400.0f, 900.0f}, size, kBorder) == WindowEdge::None);
  // 贴边轻微越界（差 1~2px）仍算边缘：拖动时指针越出边界是常态，
  // 一刀切在 [0,size] 上会让边缘拖动在那一帧"松手"。
  ST_CHECK(st::ui::resize_edge_at(Point{-2.0f, 300.0f}, size, kBorder) == WindowEdge::Left);
  ST_CHECK(st::ui::resize_edge_at(Point{802.0f, 300.0f}, size, kBorder) == WindowEdge::Right);
  // 带宽非法/尺寸退化为零时不报边缘（不能把"没窗口"当成"整窗都是边"）。
  ST_CHECK(st::ui::resize_edge_at(Point{0.0f, 0.0f}, size, 0.0f) == WindowEdge::None);
  ST_CHECK(st::ui::resize_edge_at(Point{0.0f, 0.0f}, st::math::Size{0.0f, 0.0f}, kBorder) ==
           WindowEdge::None);
}

ST_TEST(title_bar_resize_edge_clamps_band_on_tiny_windows) {
  // 窗口很小时带宽夹到半宽/半高：不夹的话整窗都是边缘，中间的界面再也点不着。
  // 这里用一个**能区分"夹过"与"没夹"**的坐标（x=5）：
  //   未夹（带=6）→ x<6 命中 Left；夹到 4 → 只剩 Right。
  const st::math::Size tiny{8.0f, 60.0f};
  ST_CHECK(st::ui::resize_edge_at(Point{5.0f, 30.0f}, tiny, 6.0f) == WindowEdge::Right);
  ST_CHECK(st::ui::resize_edge_at(Point{1.0f, 30.0f}, tiny, 6.0f) == WindowEdge::Left);
  ST_CHECK(st::ui::resize_edge_at(Point{7.0f, 30.0f}, tiny, 6.0f) == WindowEdge::Right);
  // 带被夹到半宽后，正中间那一点不属于任何一侧（剩下的"安全区"宽度为 0）——
  // 这是可接受的：那种尺寸下本来就没有"中间"可用。
  ST_CHECK(st::ui::resize_edge_at(Point{4.0f, 30.0f}, tiny, 6.0f) == WindowEdge::None);
}

// ───────────────────────── 组件：几何与命中 ─────────────────────────

ST_TEST(title_bar_lays_out_controls_at_right_edge) {
  BarFixture fixture;
  const Rect bounds = fixture.bar->bounds();
  ST_CHECK_EQ(bounds.x, 0.0f);
  ST_CHECK_EQ(bounds.y, 0.0f);
  ST_CHECK_EQ(bounds.width, 1280.0f);
  ST_CHECK_EQ(bounds.height, TitleBar::bar_height());

  // 三个按钮右对齐、等宽、不重叠，关闭在最右。
  const Rect minimize = fixture.bar->control_button_rect(0);
  const Rect maximize = fixture.bar->control_button_rect(1);
  const Rect close = fixture.bar->control_button_rect(2);
  ST_CHECK_EQ(close.right(), bounds.right());
  ST_CHECK_EQ(close.x, maximize.right());
  ST_CHECK_EQ(maximize.x, minimize.right());   // 三按钮紧邻不相叠，顺序：最小化 | 最大化 | 关闭
  ST_CHECK_EQ(minimize.width, TitleBar::control_button_size());
  ST_CHECK_EQ(minimize.height, bounds.height);

  // 标题**文字区**：起于图标之后（本夹具设了图标），止于按钮区左缘。
  const Rect caption = fixture.bar->caption_rect();
  ST_CHECK(caption.x > bounds.x);          // 让出左内边距 + 图标位
  ST_CHECK_EQ(caption.right(), minimize.x);
  ST_CHECK(!caption.contains(minimize.center()));
  // **拖动区**则从窗口左缘起（与系统标题栏的 `HTCAPTION` 同口径）。
  ST_CHECK_EQ(fixture.bar->drag_rect().x, bounds.x);
  ST_CHECK(fixture.bar->hits_caption(caption.center()));
  ST_CHECK(fixture.bar->hits_caption(Point{bounds.x + 2.0f, 20.0f}));
}

ST_TEST(title_bar_control_buttons_do_not_drag_window) {
  BarFixture fixture;
  // 按钮上的按下必须**只**记按压，不能顺带开始拖动窗口——
  // 否则点"关闭"会先把窗口拖走（系统标题栏里这两者也从不重叠）。
  ST_CHECK(fixture.send(EventKind::MouseDown, fixture.button_center(2)));
  ST_CHECK_EQ(fixture.control.move_calls, 0);
  ST_CHECK_EQ(fixture.control.close_calls, 0);   // 松开才生效
  ST_CHECK(fixture.send(EventKind::MouseUp, fixture.button_center(2)));
  ST_CHECK_EQ(fixture.control.close_calls, 1);
}

ST_TEST(title_bar_control_release_outside_cancels) {
  BarFixture fixture;
  ST_CHECK(fixture.send(EventKind::MouseDown, fixture.button_center(0)));
  // 按住后拖出按钮范围再松开：取消（系统标题栏同款），否则"按错了也照执行"。
  ST_CHECK(fixture.send(EventKind::MouseUp, Point{400.0f, 20.0f}));
  ST_CHECK_EQ(fixture.control.minimize_calls, 0);
  ST_CHECK_EQ(fixture.control.maximize_calls, 0);
  ST_CHECK_EQ(fixture.control.close_calls, 0);
}

ST_TEST(title_bar_caption_press_starts_move) {
  BarFixture fixture;
  ST_CHECK(fixture.send(EventKind::MouseDown, Point{300.0f, 20.0f}));
  ST_CHECK_EQ(fixture.control.move_calls, 1);
  // 标题栏上的按下**必须被消费**：不消费就穿透到下层界面（点标题栏把下面的按钮点了）。
  ST_CHECK(fixture.send(EventKind::MouseDown, Point{300.0f, 20.0f}));
}

ST_TEST(title_bar_double_click_toggles_maximize) {
  BarFixture fixture;
  ST_CHECK(fixture.send(EventKind::DoubleClick, Point{300.0f, 20.0f}, 2));
  ST_CHECK_EQ(fixture.control.maximize_calls, 1);
  ST_CHECK(fixture.control.maximized_state);
  // 双击按钮区**不**触发最大化：那是按钮自己的地盘（双击"关闭"等于连点两下）。
  // 返回 false = 未消费（继续冒泡）——这一格不由标题栏认领。
  ST_CHECK(!fixture.send(EventKind::DoubleClick, fixture.button_center(0), 2));
  ST_CHECK_EQ(fixture.control.maximize_calls, 1);
}

ST_TEST(title_bar_edge_press_starts_resize_when_supported) {
  BarFixture fixture;
  // 左下角：缩放带内（按钮区在右上，互不干扰）。
  ST_CHECK(fixture.send(EventKind::MouseDown, Point{1.0f, 39.0f}));
  ST_CHECK_EQ(fixture.control.resize_calls, 1);
  ST_CHECK(fixture.control.last_resize_edge == WindowEdge::BottomLeft);
  ST_CHECK_EQ(fixture.control.move_calls, 0);
}

ST_TEST(title_bar_edge_falls_back_to_move_when_resize_unsupported) {
  BarFixture fixture;
  // Win32 的缩放由窗口层 `WM_NCHITTEST` 接管，组件这条路返回 false——
  // 此时**必须落回拖动**而不是就此吞掉：吞掉的话边缘成了"死区"，窗口反而更难拖。
  fixture.control.resize_accepted = false;
  ST_CHECK(fixture.send(EventKind::MouseDown, Point{2.0f, 20.0f}));
  ST_CHECK_EQ(fixture.control.resize_calls, 1);
  ST_CHECK_EQ(fixture.control.move_calls, 1);
}

// ───────────────────────── 组件：无宿主（跨平台无差异） ─────────────────────────

ST_TEST(title_bar_without_host_paints_but_refuses_actions) {
  BarFixture fixture(/*with_control=*/false);
  // ① 画面照旧：**不能**因为"没有窗口"就少画一个按钮。
  //    （无头截图与实机窗口必须可比；`cross_platform.md` §7 的一致性表。）
  ST_CHECK(fixture.bar->show_controls());
  ST_CHECK(!fixture.bar->control_button_rect(2).is_empty());
  st::raster::Canvas canvas{1280, 40};
  auto context = fixture.root.render_context();
  fixture.bar->paint(context, canvas);  // ② 动作如实拒绝：按钮按了、拖动按了，但没有任何动作被"假成功"。
  ST_CHECK(fixture.send(EventKind::MouseDown, fixture.button_center(1)));
  ST_CHECK(fixture.send(EventKind::MouseUp, fixture.button_center(1)));
  ST_CHECK(fixture.send(EventKind::MouseDown, Point{400.0f, 20.0f}));
  ST_CHECK(fixture.send(EventKind::DoubleClick, Point{400.0f, 20.0f}, 2));
  // 属性面如实回答"这个环境没有窗口控制"（自动化据此区分两种"没反应"）。
  ST_CHECK_EQ(*fixture.bar->get_property("window_control_available"), std::string("false"));
  ST_CHECK_EQ(*fixture.bar->get_property("maximized"), std::string("false"));
}

ST_TEST(title_bar_action_surface_reports_capability) {
  BarFixture fixture(/*with_control=*/false);
  // `invoke` 的返回值就是"这一下生效了吗"：无宿主时一律 false（协议层据此如实上报）。
  ST_CHECK(!fixture.bar->invoke_action("minimize", ""));
  ST_CHECK(!fixture.bar->invoke_action("maximize", ""));
  ST_CHECK(!fixture.bar->invoke_action("close", ""));

  BarFixture hosted;
  ST_CHECK(hosted.bar->invoke_action("minimize", ""));
  ST_CHECK_EQ(hosted.control.minimize_calls, 1);
  // 未知动作不冒充已处理（协议层要能报 unsupported）。
  ST_CHECK(!hosted.bar->invoke_action("nonsense", ""));
}

ST_TEST(title_bar_unavailable_host_refuses_actions) {
  BarFixture fixture;
  fixture.control.available = false;   // 后端在，但明确报"不支持窗口控制"
  ST_CHECK(!fixture.bar->invoke_action("minimize", ""));
  ST_CHECK(!fixture.bar->invoke_action("close", ""));
  ST_CHECK_EQ(*fixture.bar->get_property("window_control_available"), std::string("false"));
}

// ───────────────────────── 组件：属性面与标题联动 ─────────────────────────

ST_TEST(title_bar_property_surface_round_trip) {
  BarFixture fixture;
  ST_CHECK_EQ(*fixture.bar->get_property("title"), std::string("renderer.cpp - gbcode"));
  ST_CHECK_EQ(*fixture.bar->get_property("text"), std::string("renderer.cpp - gbcode"));
  ST_CHECK_EQ(*fixture.bar->get_property("icon"), std::string("code"));
  ST_CHECK_EQ(*fixture.bar->get_property("show_controls"), std::string("true"));

  ST_CHECK(fixture.bar->set_property("title", "deploy.py - gbcode"));
  ST_CHECK_EQ(fixture.bar->title(), std::string("deploy.py - gbcode"));
  ST_CHECK(fixture.bar->set_property("show_controls", "false"));
  ST_CHECK(!fixture.bar->show_controls());
  // 关掉按钮后：标题带铺到最右，且**不再**有按钮命中区。
  // （这里刻意**不**重排：几何按需算，属性改完立即查就应对——
  //   否则自动化"改属性→查几何"会拿到旧值，实测就是这么撞出来的。）
  ST_CHECK_EQ(fixture.bar->caption_rect().right(), fixture.bar->bounds().right());
  ST_CHECK(!fixture.bar->set_property("nonsense", "x"));
}

// ───────────────────────── TitleBar：附属槽（窗框是框架） ─────────────────────────

ST_TEST(title_bar_accessory_slots_arrange_inside_the_row) {
  BarFixture fixture;
  // 尾部槽：菜单/主题这类宿主控件挂在控制按钮**左侧**。
  auto button = std::make_unique<st::ui::Button>("暗色");
  button->set_id("slot-theme");
  auto* slot = fixture.bar->add_trailing(std::move(button));
  fixture.root.layout(true);
  const Rect slot_rect = slot->bounds();
  const Rect minimize = fixture.bar->control_button_rect(0);
  ST_CHECK(slot_rect.right() <= minimize.x + 0.01f);   // 不与控制按钮重叠
  ST_CHECK(slot_rect.width > 0.0f);                    // 真被测量/排位了（不 measure 子树则为 0）
  ST_CHECK_EQ(slot_rect.height, fixture.bar->bounds().height);
  // 槽位不算拖动区：否则点主题按钮会把窗口拖走。
  ST_CHECK(!fixture.bar->hits_caption(slot_rect.center()));
  // 槽位是标题栏的**子元素**（内容子元素口径一致，声明式才能正确对齐/裁剪）。
  ST_CHECK_EQ(fixture.bar->children().size(), 1U);
  ST_CHECK_EQ(fixture.bar->content_child_count(), 1U);
}

ST_TEST(title_bar_leading_slot_makes_room_for_title_text) {
  BarFixture fixture;
  auto brand = std::make_unique<st::ui::Heading>("霜天 · 组件画廊", 3);
  brand->style().font_size = 14.0f;
  auto* leading = fixture.bar->add_leading(std::move(brand));
  fixture.root.layout(true);
  // 标题文字区必须在**前部槽之后**：否则品牌名与标题文字会叠在一起
  // （实测踩到：两层字画在同一个位置）。
  ST_CHECK(fixture.bar->caption_rect().x >= leading->bounds().right() - 0.01f);
  // 但**拖动区**仍从窗口左缘起（与系统标题栏的 `HTCAPTION` 一致）。
  ST_CHECK_EQ(fixture.bar->drag_rect().x, fixture.bar->bounds().x);
  ST_CHECK(fixture.bar->hits_caption(Point{4.0f, 20.0f}));   // 左侧空当也算拖动区
}

/// 挂在 `leading`/`trailing` 槽里的控件**不得**改到窗口状态。
///
/// 这不是理论问题：gbcode 把菜单栏挂进标题栏的 `leading` 槽之后，在
/// **文件**菜单上双击本应只是“点两下菜单”，实际会把窗口最大化——
/// 因为菜单自己消费了按下与单击，唯独双击无人认领，于是冒泡到标题栏，
/// 落进“双击标题区 = 最大化/还原”。
///
/// 根因是 `drag_rect()` 只排除了**尾部槽**：`leading` 槽与它左侧的空当一起
/// 被算成拖动区（`hits_caption` 因此为真），而头文件写着“附属槽不参与拖动
/// （它们是控件，不是拖动区）”——注释与实现自相矛盾。
///
/// 判据用**窗口动作调用次数**而不是像素：像素只说明“重画了”，而这里要的是
/// “系统动作有没有被触发”（无头下它会静默变成一次无害调用）。
ST_TEST(title_bar_accessory_slots_do_not_trigger_window_actions) {
  BarFixture fixture;
  // 菜单栏形态的宿主控件（MenuBar 自己会消费按下/单击，正是“只有双击漏出来”的场景）。
  auto menu = std::make_unique<st::ui::MenuBar>();
  menu->set_id("menubar");
  menu->set_menus({st::ui::Menu{"file", "文件", {st::ui::MenuItem{"new", "新建文件"}}}});
  auto* leading = fixture.bar->add_leading(std::move(menu));
  fixture.root.layout(true);
  const Rect slot = leading->bounds();
  ST_REQUIRE(slot.width > 0.0f);
  ST_REQUIRE(slot.width > 8.0f);   // 探针点必须落在控件上（贴边那一两像素不算）

  // ① 双击槽位：**不得**触发最大化。
  (void)fixture.send(EventKind::DoubleClick, Point{slot.x + slot.width * 0.5f, slot.center().y}, 2);
  ST_CHECK_EQ(fixture.control.maximize_calls, 0);
  ST_CHECK(!fixture.control.maximized_state);
  ST_CHECK(!fixture.bar->hits_caption(Point{slot.x + slot.width * 0.5f, slot.center().y}));

  // ② 按住槽位：**不得**开始拖动窗口（否则点菜单会把窗口拖走）。
  (void)fixture.send(EventKind::MouseDown, Point{slot.x + slot.width * 0.5f, slot.center().y});
  ST_CHECK_EQ(fixture.control.move_calls, 0);
  (void)fixture.send(EventKind::MouseUp, Point{slot.x + slot.width * 0.5f, slot.center().y});

  // ③ 槽位之后的标题带仍照旧可拖/可双击（改的是槽位，不是整条栏）。
  const Rect caption = fixture.bar->caption_rect();
  ST_REQUIRE(caption.width > 16.0f);
  ST_CHECK(fixture.bar->hits_caption(caption.center()));
  const std::size_t moves_before = static_cast<std::size_t>(fixture.control.move_calls);
  (void)fixture.send(EventKind::MouseDown, caption.center());
  ST_CHECK_EQ(static_cast<std::size_t>(fixture.control.move_calls), moves_before + 1U);
  (void)fixture.send(EventKind::DoubleClick, caption.center(), 2);
  ST_CHECK_EQ(fixture.control.maximize_calls, 1);
}

/// 尾部槽同样不得触发窗口动作（与上一条同一契约的另一侧）。
ST_TEST(title_bar_trailing_slot_does_not_trigger_window_actions) {
  BarFixture fixture;
  auto button = std::make_unique<st::ui::Button>("主题");
  button->set_id("slot-theme");
  auto* slot = fixture.bar->add_trailing(std::move(button));
  fixture.root.layout(true);
  const Point center = slot->bounds().center();
  (void)fixture.send(EventKind::DoubleClick, center, 2);
  ST_CHECK_EQ(fixture.control.maximize_calls, 0);
  (void)fixture.send(EventKind::MouseDown, center);
  ST_CHECK_EQ(fixture.control.move_calls, 0);
  // 槽位的**左侧空当**仍属拖动区（尾部槽只切掉自己那一块）。
  ST_CHECK(fixture.bar->hits_caption(Point{slot->bounds().x - 4.0f, slot->bounds().center().y}));
}

// ───────────────────────── WindowFrame：组件化的窗口 ─────────────────────────

/// 窗框夹具：窗框作根内容，内容区挂一个具名面板，其余交给组件自己。
struct FrameFixture {
  st::ui::UiRoot root{};
  st::ui::WindowFrame* frame{nullptr};
  FakeWindowControl control{};

  explicit FrameFixture(bool with_control = true, std::string title = "霜天 · 组件画廊") {
    root.set_viewport(st::math::Size{1280.0f, 800.0f});
    auto window_frame = std::make_unique<st::ui::WindowFrame>(std::move(title));
    window_frame->set_id("window-frame");
    if (with_control) window_frame->set_window_control(&control);
    frame = window_frame.get();
    auto body = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    body->set_id("body");
    frame->add_child(std::move(body));
    root.set_content(std::move(window_frame));
    root.layout(true);
  }

  ~FrameFixture() { root.set_content(nullptr); }

  FrameFixture(const FrameFixture&) = delete;
  auto operator=(const FrameFixture&) -> FrameFixture& = delete;

  auto send(EventKind kind, Point point) -> bool {
    Event event;
    event.kind = kind;
    event.position = point;
    event.button = 1;
    return root.dispatch(event);
  }
};

ST_TEST(window_frame_holds_title_bar_and_content_slot) {
  FrameFixture fixture;
  const Rect bounds = fixture.frame->bounds();
  ST_CHECK_EQ(bounds.width, 1280.0f);
  ST_CHECK_EQ(bounds.height, 800.0f);
  ST_CHECK(fixture.frame->title_bar() != nullptr);
  ST_CHECK_EQ(fixture.frame->title_bar()->bounds().height, TitleBar::bar_height());
  // 内容区在标题栏**下方**，且铺满剩余高度（否则界面会在底部留一条空白）。
  const Rect content = fixture.frame->content()->bounds();
  ST_CHECK_EQ(content.y, bounds.y + TitleBar::bar_height());
  ST_CHECK_EQ(content.height, bounds.height - TitleBar::bar_height());
  ST_CHECK_EQ(content.width, bounds.width);
  // 宿主挂的节点进的是**内容槽**（不是标题栏）。
  ST_CHECK_EQ(fixture.frame->content()->child_count(), 1U);
  ST_CHECK(fixture.root.find("body") != nullptr);
  // 载体子元素口径：内容槽里的那些，标题栏/边缘条不算（否则声明式会对它们做对齐/裁剪）。
  ST_CHECK_EQ(fixture.frame->content_child_count(), 1U);
}

ST_TEST(window_frame_edges_cover_all_directions) {
  FrameFixture fixture;
  const Rect bounds = fixture.frame->bounds();
  // 八向边缘条各自落在正确的边上（与后端 `resize_edge_at` 同一判定）。
  ST_CHECK(fixture.frame->edge_at(Point{bounds.x + 1.0f, bounds.y + 300.0f}) == WindowEdge::Left);
  ST_CHECK(fixture.frame->edge_at(Point{bounds.right() - 1.0f, 300.0f}) == WindowEdge::Right);
  ST_CHECK(fixture.frame->edge_at(Point{600.0f, bounds.y + 1.0f}) == WindowEdge::Top);
  ST_CHECK(fixture.frame->edge_at(Point{600.0f, bounds.bottom() - 1.0f}) == WindowEdge::Bottom);
  ST_CHECK(fixture.frame->edge_at(Point{bounds.x + 1.0f, bounds.y + 1.0f}) == WindowEdge::TopLeft);
  ST_CHECK(fixture.frame->edge_at(Point{bounds.right() - 1.0f, bounds.bottom() - 1.0f}) ==
           WindowEdge::BottomRight);
  // 内容区中间不是边缘（否则整窗都成了缩放区，界面点不动）。
  ST_CHECK(fixture.frame->edge_at(Point{640.0f, 400.0f}) == WindowEdge::None);
  // 边缘条矩形不越界（画在窗口**内侧**）。
  for (std::size_t index = 0; index < st::ui::WindowFrame::kEdgeCount; ++index) {
    const Rect edge = fixture.frame->edge_rect(index);
    ST_CHECK(!edge.is_empty());
    ST_CHECK(edge.x >= bounds.x - 0.01f && edge.right() <= bounds.right() + 0.01f);
    ST_CHECK(edge.y >= bounds.y - 0.01f && edge.bottom() <= bounds.bottom() + 0.01f);
  }
  // 关掉边缘后一律 `None`（"确实不想让用户缩放"的场合）。
  fixture.frame->set_show_resize_edges(false);
  ST_CHECK(fixture.frame->edge_at(Point{bounds.x + 1.0f, 300.0f}) == WindowEdge::None);
  ST_CHECK(fixture.frame->edge_rect(0).is_empty());
}

ST_TEST(window_frame_edge_press_starts_resize_or_swallows) {
  FrameFixture fixture;
  const Rect bounds = fixture.frame->bounds();
  // 边缘按下：请求后端开始缩放（Win32 由 `WM_NCHITTEST` 接管 → 后端返回 false），
  // 但事件**仍要被消费**：边缘上的按下不该穿透到内容区。
  ST_CHECK(fixture.send(EventKind::MouseDown, Point{bounds.x + 1.0f, 400.0f}));
  ST_CHECK_EQ(fixture.control.resize_calls, 1);
  ST_CHECK(fixture.control.last_resize_edge == WindowEdge::Left);
  ST_CHECK(fixture.send(EventKind::MouseUp, Point{bounds.x + 1.0f, 400.0f}));
  // 内容区按下：**不消费**（继续冒泡，由内容/标题栏自己处理）。
  ST_CHECK(!fixture.send(EventKind::MouseDown, Point{640.0f, 400.0f}));
  ST_CHECK_EQ(fixture.control.resize_calls, 1);
}

ST_TEST(window_frame_forwards_window_actions) {
  FrameFixture fixture;
  // 动作转发到标题栏（唯一实现处）：三个动作都生效。
  ST_CHECK(fixture.frame->invoke_action("minimize", ""));
  ST_CHECK(fixture.frame->invoke_action("maximize", ""));
  ST_CHECK(fixture.frame->invoke_action("close", ""));
  ST_CHECK_EQ(fixture.control.minimize_calls, 1);
  ST_CHECK_EQ(fixture.control.maximize_calls, 1);
  ST_CHECK_EQ(fixture.control.close_calls, 1);
  // 属性面：标题转发给标题栏（一处定义，两处可见）。
  ST_CHECK(fixture.frame->set_property("title", "概览"));
  ST_CHECK_EQ(fixture.frame->title_bar()->title(), std::string("概览"));
  ST_CHECK_EQ(*fixture.frame->get_property("title"), std::string("概览"));
  // 未知动作不冒充已处理。
  ST_CHECK(!fixture.frame->invoke_action("nonsense", ""));
  // 未注入端口：动作**如实拒绝**（"点了没反应"与"环境没这个能力"是两件事）。
  FrameFixture bare(/*with_control=*/false);
  ST_CHECK(!bare.frame->invoke_action("minimize", ""));
}

ST_TEST(restore_icon_differs_from_maximize_icon) {
  // 「最大化 / 还原」两种形态必须**看得出来不一样**：
  // 早期两者共用 `square`，用户在像素上分不出按钮此刻是哪个动作。
  ST_CHECK(st::ui::Icon::has("restore"));
  const Rect box{0.0f, 0.0f, 16.0f, 16.0f};
  const auto restore = st::ui::Icon::path("restore", box, 2.0f);
  const auto maximize = st::ui::Icon::path("square", box, 2.0f);

  // 判据必须落在**“多出来的那个叠框”**上，而不是包围盒尺寸。
  //
  // ⚠ 旧写法的漏洞（实测拓到）：它比的是两者的 `flattened_bounds` 宽/高不等。
  // 而 `restore` 当时有一个真 bug（后窗竖线 `L16 16` 伸进了前窗内部）——
  // 那多出来的一截把包围盒撞大了，于是**断言靠 bug 才能通过**：
  // 修好几何后两者尺寸完全一致（都是 17px 归一化尺寸），这条就红了。
  // 一个“因为缺陷而绿”的护栏比没有护栏更坑：它会在修缺陷时把人指向错的方向。
  //
  // 正确的量是**路径本身的几何差**：两者应当不是同一条路径，
  // 且 `restore` 应当比 `square` **多出一段子路径**（叠在后面那个方框）。
  const auto restore_polylines = restore.flatten(0.25f);
  const auto maximize_polylines = maximize.flatten(0.25f);
  ST_CHECK(restore_polylines.size() > maximize_polylines.size());
  ST_CHECK_EQ(maximize_polylines.size(), 1U);
  ST_CHECK_EQ(restore_polylines.size(), 2U);
}

/// `restore` 的**视觉重心**必须真的偏——两个叠框与单个方框不能长得一样。
///
/// 上一条只证“结构不同”；这一条盯“画出来看得不一样”：
/// 两者都在各自包围盒内居中的话，像素分布必然不同（`restore` 有左上一个角）。
ST_TEST(restore_icon_ink_differs_from_maximize_icon) {
  constexpr int kSize = 32;
  const auto render = [](std::string_view name) {
    st::raster::Canvas canvas{kSize, kSize, 1.0f};
    canvas.clear(st::math::Color::rgb(0xFF, 0xFF, 0xFF));
    st::ui::Icon::draw(canvas, name, Rect{0.0f, 0.0f, 32.0f, 32.0f},
                       st::math::Color::rgb(0x00, 0x00, 0x00), 2.0f);
    return canvas;
  };
  const st::raster::Canvas restore = render("restore");
  const st::raster::Canvas maximize = render("square");
  std::size_t differing = 0;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      if (!(restore.pixel_at(x, y) == maximize.pixel_at(x, y))) ++differing;
    }
  }
  st::print("[restore-vs-maximize] 不同像素 {} / {}\n", differing, kSize * kSize);
  // “看得出不一样”：至少要有可观的一片像素不同（不是抗锯齿的单像素抖动）
  ST_CHECK(differing > 60U);
}

ST_TEST(title_bar_maximized_state_drives_button_icon) {
  BarFixture fixture;
  // 按钮形态跟随窗口状态：最大化时"最大化"按钮应显示"还原"图标。
  // 这里不断言图标名（那是视觉细节），断言的是**状态可读且会变**：
  // 自动化若读不到它，就只能靠截图猜按钮此刻是什么意思。
  ST_CHECK_EQ(*fixture.bar->get_property("maximized"), std::string("false"));
  (void)fixture.bar->invoke_action("maximize", "");
  ST_CHECK_EQ(*fixture.bar->get_property("maximized"), std::string("true"));
  (void)fixture.bar->invoke_action("maximize", "");
  ST_CHECK_EQ(*fixture.bar->get_property("maximized"), std::string("false"));
}

// ───────────────────── 窗口按钮的悬停/按下反馈（像素级） ─────────────────────

/// 渲染一帧标题栏（`hovered`/`pressed` 指定哪个按钮处于那个态，-1 = 都没有）。
///
/// ⚠ `-1` 是"无"的哨兵，**不能图省事传 0**——0 是"最小化"的合法按钮号，
/// 传 0 会静默变成"按下最小化"（实测："悬停"那一路因此测得与"按下"逐位相同）。
struct ControlButtonShot {
  st::raster::Canvas canvas;
  Rect button{};
};

[[nodiscard]] auto render_control_button(BarFixture& fixture, int hovered, int pressed,
                                         std::size_t probe_index) -> ControlButtonShot {
  // 走**真实事件路径**（`root.dispatch` + `root.paint`）：按钮的悬停/按压态由
  // `TitleBar::on_event` 维护在组件内部（不是 `Element` 的通用悬停），
  // 直接调 `paint` 不会有任何高亮——那会让本用例恒红，而不是测出缺陷。
  if (pressed >= 0) {
    (void)fixture.send(EventKind::MouseDown, fixture.button_center(
                                                 static_cast<std::size_t>(pressed)));
  } else if (hovered >= 0) {
    (void)fixture.send(EventKind::MouseMove, fixture.button_center(
                                                  static_cast<std::size_t>(hovered)));
  }
  st::raster::Canvas canvas{1280, 120, 1.0f};
  canvas.clear(fixture.root.theme().colors().bg);
  fixture.root.paint(canvas);
  // 用完**清掉**状态，否则后面的调用会带着上一个按钮的高亮拼图。
  if (pressed >= 0) {
    (void)fixture.send(EventKind::MouseUp, Point{-10.0f, -10.0f});
  }
  Event out;
  out.kind = EventKind::HoverOut;
  out.position = Point{-10.0f, -10.0f};
  (void)fixture.root.dispatch(out);
  return ControlButtonShot{std::move(canvas), fixture.bar->control_button_rect(probe_index)};
}

/// 采样点：按钮内的**空白处**。
///
/// ⚠ 纵向取 `center().y` 会落到字形上——`minus` 就是一条压在竖直中线的横线，
/// 于是"悬停底色"被量成了墨迹（实测：按钮 0 报 1.3831，而按钮 1/2 报 1.0000）。
/// 取靠近块上沿的位置：块纵向内缩 4、字形只在中心 14px 内，`y+6` 两边都避开。
[[nodiscard]] auto probe_pixel(const ControlButtonShot& shot) -> Color {
  return shot.canvas.pixel_at(static_cast<int>(shot.button.x) + 5,
                              static_cast<int>(shot.button.y) + 6);
}

/// 按钮的悬停/按下**必须看得见**：这是本轮修的真缺陷。
///
/// 旧实现用 `surface_pressed.with_alpha_f(0.10)` **叠层**，而标题栏底色
/// `surface_alt` 本身就是不透明的——alpha 罩层在上面不产生任何差异，
/// 实测只差 **1/255**（对比度 1.0055），等于悬停完全没有反馈。
/// 关闭按钮之所以看得见（1.0564），纯粹因为它用的是危险色相。
///
/// 判据用**对比度**（与主题自己的可断言契约同一套口径）：
/// 普通按钮不低于主题给的悬浮档实际能拉开的量级，关闭按钮必须明显更强
/// （它是全窗唯一不可逆动作，用实心 danger 底 + 白字）。
ST_TEST(title_bar_control_button_states_are_visible) {
  BarFixture fixture;
  const Color bar_bg = fixture.root.theme().colors().surface_alt;
  const Color danger = fixture.root.theme().colors().danger;

  const auto probe = [](const ControlButtonShot& shot) -> Color { return probe_pixel(shot); };
  const auto contrast = [](Color a, Color b) -> float {
    return st::math::contrast_ratio(a, b);
  };

  // 静止帧：没有任何按钮处于悬停/按下（probe_index 只用来取几何）。
  const ControlButtonShot rest = render_control_button(fixture, -1, -1, 0);
  for (int index = 0; index < 3; ++index) {
    const auto slot = static_cast<std::size_t>(index);
    const ControlButtonShot hovered = render_control_button(fixture, index, -1, slot);
    const ControlButtonShot pressed = render_control_button(fixture, -1, index, slot);
    const float hover_ratio = contrast(probe(hovered), bar_bg);
    const float press_ratio = contrast(probe(pressed), bar_bg);
    st::print("[wbtn] 按钮 {} 悬停对比度 {:.4f} · 按下 {:.4f}（底色 #{:02X}{:02X}{:02X}）\n", index,
              static_cast<double>(hover_ratio), static_cast<double>(press_ratio),
              static_cast<unsigned>(bar_bg.r), static_cast<unsigned>(bar_bg.g),
              static_cast<unsigned>(bar_bg.b));
    // ① 悬停真的落像素（不能等于底色）。
    ST_CHECK(!(probe(hovered) == bar_bg));
    if (index == 2) {
      // ② 关闭：**实心 danger 底**（与底色反差 2.11），不是一抹淡红。
      ST_CHECK(probe(hovered) == danger);
      ST_CHECK(hover_ratio >= 1.8f);
    } else {
      // ③ 普通按钮：中性档（与底色同向、可辨）；不写死具体色值——
      //    取哪一档由 `neutral_hover_tint` 按底色亮度选，深色主题下方向相反。
      ST_CHECK(hover_ratio >= 1.03f);
    }
    // ④ 按下必须比静止更重（否则"按下去了"看不出来）。
    //    关闭按钮按下与悬停同色（都是实心 danger），所以只要求"不比悬停更轻"。
    ST_CHECK(press_ratio >= hover_ratio);
    ST_CHECK(!(probe(pressed) == probe(rest)));
  }
}

/// 悬停块的**形状**：横向贴满按钮，纵向各缩一档（块高 = 32 = 系统按钮高）。
///
/// 不缩的话块与栏的上下分界线贴死，看着像"栏被切了一刀"；
/// 而块与**命中区**（整个按钮）不重合是**有意为之**——这正是桌面外壳的常规做法，
/// 系统的悬停块同样不等于命中区。
ST_TEST(title_bar_control_hover_block_is_inset_vertically) {
  BarFixture fixture;
  // ⚠ 不能拿"与栏底色不同"当"块存在"的判据：标题栏自己有一条 1px 分界线
  //   （`style_.border_width`），它同样不是底色——扫描会把上下两条线算进去，
  //   于是任何块都被量成"内缩 0/0、块高 40"（实测踩到）。
  //   正确口径是**与静止帧比对**：只有块会让像素变。
  const ControlButtonShot rest = render_control_button(fixture, -1, -1, 2);
  const ControlButtonShot shot = render_control_button(fixture, 2, -1, 2);
  const Rect button = shot.button;
  // 沿按钮**竖直中线**扫：块的上下边界（相对按钮框）。
  const int x = static_cast<int>(button.center().x);
  int first = -1;
  int last = -1;
  for (int y = static_cast<int>(button.y); y < static_cast<int>(button.bottom()); ++y) {
    if (!(shot.canvas.pixel_at(x, y) == rest.canvas.pixel_at(x, y))) {
      if (first < 0) first = y;
      last = y;
    }
  }
  ST_REQUIRE(first >= 0);
  const int inset_top = first - static_cast<int>(button.y);
  const int inset_bottom = static_cast<int>(button.bottom()) - 1 - last;
  st::print("[wbtn] 悬停块纵向内缩 {}/{} · 块高 {}\n", inset_top, inset_bottom, last - first + 1);
  ST_CHECK(inset_top > 0);
  ST_CHECK(inset_bottom > 0);
  ST_CHECK_EQ(inset_top, inset_bottom);
  // 横向贴满：块在按钮左缘内 1px 就应已存在（块只纵向内缩，横向不缩）。
  const int probe_y = first + 2;
  ST_CHECK(!(shot.canvas.pixel_at(static_cast<int>(button.x) + 1, probe_y) ==
             rest.canvas.pixel_at(static_cast<int>(button.x) + 1, probe_y)));
}

/// **按住标题栏空白处不得把整条栏压暗**（拖动窗口的第一步就是按在栏上）。
///
/// 症状：按住拖动时整条标题栏从 `#EDEDF2` 变 `#DADADF`，看着像窗框闪了一下。
/// 根因两层：① 标题栏自己没关基类的悬浮/按下回放；
/// ② 更隐蔽的是——`Element::paint_box` 里那条"按下了但 hover_t 尚未起来"的
/// 回退分支**只查了 `hover_effect_.background`、没查 `enabled`**，
/// 于是即使组件显式 `set_hover_effect({.enabled=false})`，它依旧会把
/// 整条栏的底色压暗（实测：关掉特效后仍然变暗，修掉这一支才真正生效）。
ST_TEST(title_bar_press_does_not_darken_the_whole_bar) {
  BarFixture fixture;

  const auto render_frame = [&]() {
    st::raster::Canvas canvas{1280, 120, 1.0f};
    canvas.clear(fixture.root.theme().colors().bg);
    fixture.root.paint(canvas);
    return canvas;
  };
  const st::raster::Canvas before_frame = render_frame();
  // 按在**空白拖动区**（caption 中段，远离任何按钮）。
  ST_CHECK(fixture.send(EventKind::MouseDown, Point{400.0f, 20.0f}));
  const st::raster::Canvas after_frame = render_frame();
  fixture.send(EventKind::MouseUp, Point{400.0f, 20.0f});

  std::size_t differing = 0;
  // 只比标题栏那 40 行（避免把下方正文的差异算进来）。
  for (int y = 0; y < 40; ++y) {
    for (int x = 0; x < 1280; ++x) {
      if (!(before_frame.pixel_at(x, y) == after_frame.pixel_at(x, y))) ++differing;
    }
  }
  st::print("[wbtn] 按住标题栏空白处：整栏差异 {} px\n", differing);
  ST_CHECK_EQ(static_cast<int>(differing), 0);
  ST_CHECK(fixture.bar->bounds().height == TitleBar::bar_height());
}

}  // namespace

/// **前置图标与 `leading` 槽并存**：图标画在槽之前，且两者都不重叠。
///
/// 旧契约是"有 `leading` 槽就不画图标"（注释写的是"图标让位给槽"），
/// 而 `arrange` 里图标宽度始终占着——于是挂了菜单栏的宿主（gbcode）看到的是
/// "左边距 + 两倍图标宽的空白，图标根本没画"：**图形与布局互相矛盾**。
/// 判据分两半：几何（图标盒与槽盒不重叠）+ 像素（槽左侧真的出现了非底色墨迹）。
ST_TEST(title_bar_icon_and_leading_slot_coexist) {
  BarFixture fixture;
  auto menu = std::make_unique<st::ui::MenuBar>();
  menu->set_id("menubar");
  menu->set_menus({st::ui::Menu{"file", "文件", {st::ui::MenuItem{"new", "新建"}}}});
  auto* slot = fixture.bar->add_leading(std::move(menu));
  fixture.root.layout(true);
  const Rect slot_rect = slot->bounds();
  ST_REQUIRE(slot_rect.width > 0.0f);

  // ① 几何：图标占据左边距之后的那一格（16px），槽必须在它右侧。
  constexpr float kPadding = 12.0f;
  constexpr float kIcon = 16.0f;
  // 图标盒**纵向居中**（与 `paint_content` 同一算式）：只取盒的上沿会把探针
  // 采样区落到图形之外——而图形本身又只占盒的中间一部分（光学归一化留白），
  // 于是“几何对了”但“数不到墨”。
  const float icon_y = (fixture.bar->bounds().height - kIcon) * 0.5f;
  const Rect icon_box{kPadding, icon_y, kIcon, kIcon};
  ST_CHECK(slot_rect.x >= icon_box.right() - 0.01f);

  // ② 像素：图标盒那一格必须出现**非底色**墨迹（否则"几何留了位、图形没画"，
  //    正是旧实现的表现）。
  st::raster::Canvas canvas{1280, 120, 1.0f};
  const Color background = fixture.root.theme().colors().surface_alt;
  canvas.clear(background);
  fixture.root.paint(canvas);
  std::size_t ink = 0;
  for (int y = static_cast<int>(icon_box.y) + 1;
       y < static_cast<int>(icon_box.bottom()) - 1; ++y) {
    for (int x = static_cast<int>(icon_box.x) + 1;
         x < static_cast<int>(icon_box.right()) - 1; ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const int delta = std::abs(static_cast<int>(pixel.r) - static_cast<int>(background.r)) +
                        std::abs(static_cast<int>(pixel.g) - static_cast<int>(background.g)) +
                        std::abs(static_cast<int>(pixel.b) - static_cast<int>(background.b));
      if (delta > 30) ++ink;
    }
  }
  st::print("[brand] 前置图标盒内墨迹 {} px（槽左缘 x={:.1f}）\n", ink, slot_rect.x);
  ST_CHECK(ink > 20U);
}
