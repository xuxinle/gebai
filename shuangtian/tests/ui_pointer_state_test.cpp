/// 指针态（悬浮/光标）的**窗口级生命周期**：出窗、失活、松手、浮层变化。
///
/// 需求来源（用户 2026-11-02）："鼠标指针悬浮和取消状态感知和动作好好优化下——
/// 切换前台但没点击过没有对应内容的指针变化；标题栏按钮操作后没有取消悬浮状态"。
///
/// 本文件覆盖五组回归（都是修复前真实缺失的能力）：
/// ① `WindowLeave` 清态：hover 归零、光标回 Arrow、合并中的移动被丢弃、
///    元素照常收到元素级 HoverOut（不是被静默抹掉）；
/// ② 出窗后 `revalidate` 不复活残留（`has_pointer_` 失效）；
/// ③ 拖拽结束（MouseUp）后 hover/光标按松手位置重算；
/// ④ 浮层弹出/收起后 hover/光标重算（指针没动、命中链变了）；
/// ⑤ 标题栏最小化/关闭动作后 `hovered_button`/`hovered_caption` 复位（属性面可断言）。
///
/// 每条都先**证明前置状态成立**再断言修复行为——否则"本来就没人 hover"也能绿。
///
/// ⚠ 几何取**布局后的真实 bounds 中心**（不手排坐标）：`paint_frame` 会触发重排，
/// 手排矩形会被 Panel 的自动排版覆盖（实测就是这么踩的——固定坐标在重排后打空，
/// 用例假红）。动态取中心让断言跟着真实几何走。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/window_control.hpp"

namespace {

using st::math::Point;
using st::ui::Button;
using st::ui::Element;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::MouseCursor;
using st::ui::TitleBar;
using st::ui::UiRoot;
using st::ui::WindowControl;
using st::ui::WindowEdge;

/// 与 `ui_title_bar_test.cpp` 同款假宿主：记账 + 可配置。
struct FakeWindowControl : WindowControl {
  bool available{true};
  int minimize_calls{0};
  int close_calls{0};

  [[nodiscard]] auto window_control_available() const -> bool override { return available; }
  [[nodiscard]] auto window_minimize() -> bool override {
    ++minimize_calls;
    return available;
  }
  [[nodiscard]] auto window_toggle_maximize() -> bool override { return available; }
  [[nodiscard]] auto window_request_close() -> bool override {
    ++close_calls;
    return available;
  }
  [[nodiscard]] auto window_maximized() const -> bool override { return false; }
  [[nodiscard]] auto window_begin_move() -> bool override { return available; }
  [[nodiscard]] auto window_begin_resize(WindowEdge edge) -> bool override {
    (void)edge;
    return available;
  }
};

/// 一枚按钮 + 记录元素级 HoverIn/Out 次数。
/// 光标是"指针变化"的另一半断言面：hover 高亮只能看像素，光标有语义名。
struct HoverProbeButton {
  Button* element{nullptr};
  int enters{0};
  int leaves{0};

  void bind(Button* owned) {
    element = owned;
    element->set_on_hover([this](bool hovered) { (hovered ? enters : leaves) += 1; });
  }

  /// 布局后的真实中心（见文件头 ⚠：不手排坐标）。
  [[nodiscard]] auto center() const -> Point { return element->bounds().center(); }
};

/// 夹具：两个可悬浮按钮（左带手型光标、右无光标意见）+ 一个可增删的浮层。
struct PointerFixture {
  UiRoot root{};
  HoverProbeButton hand;      // 左：手型光标按钮
  HoverProbeButton plain;     // 右：Arrow 按钮（无光标意见），用于"移出后光标回落"
  MouseCursor sink_last{MouseCursor::Arrow};
  int sink_calls{0};
  st::raster::Canvas canvas{320, 160};

  PointerFixture() {
    root.set_theme(st::ui::Theme::light());
    auto left = std::make_unique<Button>("手型");
    left->set_id("btn-hand");
    left->set_cursor(MouseCursor::Hand);
    hand.bind(left.get());
    auto right = std::make_unique<Button>("普通");
    right->set_id("btn-plain");
    plain.bind(right.get());
    auto row = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
    row->add_child(std::move(left));
    row->add_child(std::move(right));
    root.set_content(std::move(row));
    root.set_viewport(st::math::Size{320.0f, 160.0f});
    root.layout(true);
    root.cursor_sink = [this](MouseCursor cursor) {
      sink_last = cursor;
      ++sink_calls;
    };
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
};

}  // namespace

// ───────────────────────── ① WindowLeave 清态 ─────────────────────────

ST_TEST(window_leave_clears_hover_cursor_and_pending_moves) {
  PointerFixture fixture;
  fixture.move_to(fixture.hand.center());
  ST_CHECK(fixture.hand.element->hovered());
  ST_CHECK_EQ(fixture.hand.enters, 1);
  ST_CHECK(fixture.sink_last == MouseCursor::Hand);   // 光标确已非 Arrow

  // 预埋一条"已合并、未派发"的移动（模拟出窗前最后一瞬的 move 进了合并器）：
  Event pending;
  pending.kind = EventKind::MouseMove;
  pending.position = fixture.plain.center();   // 指向普通按钮
  (void)fixture.root.feed_pointer_move(pending);

  fixture.leave_window();

  // hover 清空 + 元素级 HoverOut 照发（元素要能收自己的悬浮视觉）
  ST_CHECK(!fixture.hand.element->hovered());
  ST_CHECK_EQ(fixture.hand.leaves, 1);
  // 光标复位 Arrow（sink 又被调了一次）
  ST_CHECK(fixture.sink_last == MouseCursor::Arrow);
  ST_CHECK(fixture.sink_calls >= 2);
  // 合并中的移动被丢弃：帧末 flush 不再派发（否则 hover 会落在普通按钮上"复活"）
  ST_CHECK_EQ(fixture.root.flush_coalesced_moves(), 0U);
  ST_CHECK(!fixture.plain.element->hovered());
}

// ───────────────────────── ② 出窗后重算不复活 ─────────────────────────

ST_TEST(window_leave_invalidates_last_pointer_position) {
  PointerFixture fixture;
  fixture.move_to(fixture.hand.center());
  ST_CHECK(fixture.hand.element->hovered());

  fixture.leave_window();
  // 出窗后浮层/内容变化触发的重算是**空操作**（指针不在窗内，旧坐标不作数）：
  // 否则刚清掉的悬浮又被"按出窗前的位置"建回来。
  fixture.root.revalidate_pointer_state();
  ST_CHECK(!fixture.hand.element->hovered());
  ST_CHECK(!fixture.plain.element->hovered());
  ST_CHECK_EQ(fixture.hand.enters, 1);   // 没有第二次 HoverIn

  // 指针真的回来（后端合成 move）才重新建立——悬停、光标一并跟上。
  fixture.move_to(fixture.plain.center());
  ST_CHECK(fixture.plain.element->hovered());
  ST_CHECK(!fixture.hand.element->hovered());
}

// ───────────────────────── ③ 拖拽结束后重算 ─────────────────────────

ST_TEST(mouse_up_revalidates_hover_after_drag) {
  PointerFixture fixture;
  // 拖拽从普通按钮起（它成为 pressed_，期间 hover/光标被锁定）……
  fixture.move_to(fixture.plain.center());
  ST_CHECK(fixture.plain.element->hovered());

  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = fixture.plain.center();
  (void)fixture.root.dispatch(down);
  // ……拖到手型按钮上方（拖拽归属：move 派发给 pressed 元素，hover 不切换——这是既有契约）。
  Event drag;
  drag.kind = EventKind::MouseMove;
  drag.position = fixture.hand.center();
  (void)fixture.root.dispatch(drag);
  ST_CHECK(fixture.plain.element->hovered());   // 仍是拖拽前的 hover
  ST_CHECK(!fixture.hand.element->hovered());

  // 松手：hover 与光标立刻按**松手位置**重算（不等下一次鼠标移动）。
  Event up;
  up.kind = EventKind::MouseUp;
  up.button = 1;
  up.position = fixture.hand.center();
  (void)fixture.root.dispatch(up);

  ST_CHECK(fixture.hand.element->hovered());     // 手型按钮接手悬浮
  ST_CHECK(!fixture.plain.element->hovered());
  ST_CHECK(fixture.root.cursor() == MouseCursor::Hand);   // 光标也跟上
}

// ───────────────────────── ④ 浮层变化后重算 ─────────────────────────

ST_TEST(overlay_changes_revalidate_pointer_state) {
  PointerFixture fixture;
  fixture.move_to(fixture.hand.center());
  ST_CHECK(fixture.hand.element->hovered());
  ST_CHECK(fixture.root.cursor() == MouseCursor::Hand);

  // 弹一个铺满视口的浮层（遮罩接管命中）：指针没动，但悬浮/光标必须落到浮层上。
  auto blocker = std::make_unique<st::ui::Panel>();
  blocker->set_id("modal-blocker");
  // 可命中（默认整块矩形命中），且不可见悬浮特效——正好验证"遮罩态光标回落 Arrow"。
  fixture.root.add_overlay(std::move(blocker), st::ui::OverlayLayout::FillViewport);
  (void)fixture.root.paint_frame(fixture.canvas);

  // 帧首消费 pointer_state_dirty_：手型按钮的悬浮被收掉（命中已属遮罩），
  // 光标回落 Arrow（遮罩没有光标意见）。
  ST_CHECK(!fixture.hand.element->hovered());
  ST_CHECK(fixture.root.cursor() == MouseCursor::Arrow);

  // 摘掉浮层：指针仍停在原处，悬浮/光标回到手型按钮上。
  if (Element* overlay = fixture.root.overlay_at(0); overlay != nullptr) {
    fixture.root.remove_overlay(overlay);
  }
  (void)fixture.root.paint_frame(fixture.canvas);
  ST_CHECK(fixture.hand.element->hovered());
  ST_CHECK(fixture.root.cursor() == MouseCursor::Hand);
}

// ───────────────────────── ⑤ 标题栏操作后悬浮复位 ─────────────────────────

namespace {

/// 标题栏夹具（与 `ui_title_bar_test.cpp` 的 BarFixture 同构，精简到本文件所需）。
struct TitleBarFixture {
  UiRoot root{};
  TitleBar* bar{nullptr};
  FakeWindowControl control{};

  TitleBarFixture() {
    root.set_viewport(st::math::Size{1280.0f, 720.0f});
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

  [[nodiscard]] auto hovered_button() const -> int {
    const auto value = bar->get_property("hovered_button");
    return value.has_value() ? std::stoi(*value) : -2;
  }
  [[nodiscard]] auto hovered_caption() const -> int {
    const auto value = bar->get_property("hovered_caption");
    return value.has_value() ? std::stoi(*value) : -2;
  }
};

}  // namespace

ST_TEST(title_bar_action_resets_button_hover_state) {
  TitleBarFixture fixture;
  // 悬浮在最小化按钮上（标题栏自绘按钮级高亮，元素级 hovered 不覆盖它）
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = fixture.bar->control_button_rect(0).center();
  (void)fixture.root.dispatch(move);
  ST_CHECK_EQ(fixture.hovered_button(), 0);   // 前置成立：悬浮确实在

  // 触发最小化：动作生效 + 悬浮态复位（恢复回来时不带着旧高亮）。
  ST_CHECK(fixture.bar->invoke_action("minimize", ""));
  ST_CHECK_EQ(fixture.control.minimize_calls, 1);
  ST_CHECK_EQ(fixture.hovered_button(), -1);
  ST_CHECK_EQ(fixture.hovered_caption(), -1);
}

ST_TEST(title_bar_close_action_resets_button_hover_state) {
  TitleBarFixture fixture;
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = fixture.bar->control_button_rect(2).center();   // 关闭按钮
  (void)fixture.root.dispatch(move);
  ST_CHECK_EQ(fixture.hovered_button(), 2);

  ST_CHECK(fixture.bar->invoke_action("close", ""));
  ST_CHECK_EQ(fixture.control.close_calls, 1);
  ST_CHECK_EQ(fixture.hovered_button(), -1);
  ST_CHECK_EQ(fixture.hovered_caption(), -1);
}

ST_TEST(title_bar_caption_hover_reads_back_through_property) {
  TitleBarFixture fixture;
  // 命令按钮（主题按钮，caption 0）悬浮可读回——属性面与真实悬浮一致。
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = fixture.bar->caption_button_rect(0).center();
  (void)fixture.root.dispatch(move);
  ST_CHECK_EQ(fixture.hovered_caption(), 0);
  // 移开即清（既有 hover 命中逻辑不动，属性面跟着走）。
  move.position = Point{300.0f, 20.0f};
  (void)fixture.root.dispatch(move);
  ST_CHECK_EQ(fixture.hovered_caption(), -1);
}
